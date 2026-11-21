#!/usr/bin/env python3
"""Shared parser, discovery, workload, and integration helpers for datasets/lines."""
from __future__ import annotations

import hashlib
import math
import os
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ONE_WAY_DELAY_MS = 10.0
LINES_DATASETS = ('basic', 'leocc', 'cellular-traces-2018', 'lumos')


def parse_line(path: Path, delay: bool = False) -> tuple[int, list[tuple[int, float]]]:
    points: list[tuple[int, float]] = []
    end: int | None = None
    with path.open(encoding='utf-8') as stream:
        for number, raw in enumerate(stream, 1):
            text = raw.strip()
            if not text:
                continue
            if text.startswith('#'):
                if text.startswith('# end_ms='):
                    if end is not None:
                        raise ValueError(f'{path}:{number}: duplicate end marker')
                    try:
                        end = int(text[len('# end_ms='):])
                    except ValueError:
                        raise ValueError(f'{path}:{number}: invalid end marker') from None
                continue
            try:
                t_raw, raw_value = text.split(',')
                raw_value = raw_value.strip()
                mbps = not delay and raw_value.endswith('m')
                if mbps and raw_value[:-1] != raw_value[:-1].rstrip():
                    raise ValueError
                t, value = int(t_raw), float(raw_value[:-1] if mbps else raw_value)
                if (
                    t < 0
                    or not math.isfinite(value)
                    or value < 0
                    or (
                        not delay
                        and (
                            (not mbps and not value.is_integer())
                            or value > (12_000_000 if mbps else 1_000_000)
                        )
                    )
                ):
                    raise ValueError
                if mbps:
                    value /= 12.0
                if (not points and t != 0) or (points and t <= points[-1][0]):
                    raise ValueError
            except ValueError:
                raise ValueError(f'{path}:{number}: invalid time point or value') from None
            points.append((t, value))
    if not points:
        raise ValueError(f'{path}: no data')
    if delay:
        if end is None or end <= points[-1][0]:
            raise ValueError(f'{path}: missing or invalid # end_ms marker')
    else:
        if len(points) < 2 or points[-1][1] != 0 or points[-1][0] <= 0:
            raise ValueError(f'{path}: capacity must end with an explicit time,0')
        end = points[-1][0]
    return end, points


def peek_line_end_ms(path: Path, delay: bool = False) -> int:
    """Fast end-time lookup from the tail of a lines file (falls back to parse_line)."""
    try:
        size = path.stat().st_size
        read_len = min(size, 4096)
        with path.open('rb') as stream:
            stream.seek(size - read_len)
            tail = stream.read(read_len).decode('utf-8', errors='strict')
        lines = [line.strip() for line in tail.splitlines() if line.strip()]
        if not lines:
            return parse_line(path, delay=delay)[0]
        if delay:
            for line in reversed(lines):
                if line.startswith('# end_ms='):
                    end = int(line[len('# end_ms='):])
                    if end > 0:
                        return end
            return parse_line(path, delay=True)[0]
        for line in reversed(lines):
            if line.startswith('#'):
                continue
            t_raw, val_raw = line.split(',', 1)
            t = int(t_raw)
            val_s = val_raw.strip()
            if val_s.endswith('m'):
                val = float(val_s[:-1])
            else:
                val = float(val_s)
            if t > 0 and val == 0:
                return t
            break
    except (OSError, ValueError):
        pass
    return parse_line(path, delay=delay)[0]


def integrate_piecewise(
    points: list[tuple[int, float]],
    end_ms: int,
    period_ms: int | None = None,
) -> float:
    """Integrate piecewise-constant [(t_ms, value), ...] over [0, end_ms)."""
    if end_ms <= 0 or not points:
        return 0.0

    def integrate_one(limit: int) -> float:
        total = 0.0
        for idx, (t, val) in enumerate(points):
            if t >= limit:
                break
            nxt = points[idx + 1][0] if idx + 1 < len(points) else limit
            seg_end = min(nxt, limit)
            if seg_end > t:
                total += (seg_end - t) * val
        return total

    if not period_ms or period_ms <= 0 or end_ms <= period_ms:
        return integrate_one(end_ms)
    full_cycles, rem = divmod(end_ms, period_ms)
    return full_cycles * integrate_one(period_ms) + integrate_one(rem)


def discover(directory: Path, *, fast_duration: bool = False) -> list[dict]:
    if not directory.is_dir():
        raise ValueError(f'dataset directory not found: {directory}')
    up = {(p.parent, p.name[:-len('.up.txt')]): p for p in directory.rglob('*.up.txt')}
    down = {(p.parent, p.name[:-len('.down.txt')]): p for p in directory.rglob('*.down.txt')}
    for direction, files in (('up', up), ('down', down)):
        for p in directory.rglob(f'{direction}.txt'):
            key = (p.parent, p.parent.name)
            if key in files:
                raise ValueError(f'ambiguous {direction} capacity files in {p.parent}')
            files[key] = p
    missing = set(up) ^ set(down)
    if missing:
        raise ValueError(
            f'missing up/down capacity for: {", ".join(str(parent / name) for parent, name in sorted(missing))}'
        )
    if not up:
        raise ValueError(f'no line scenarios found in {directory}')
    scenarios = []
    seen = set()
    end_fn = peek_line_end_ms if fast_duration else (lambda p, delay: parse_line(p, delay=delay)[0])
    for parent, name in sorted(up):
        if name in seen:
            raise ValueError(f'duplicate scenario name: {name}')
        seen.add(name)
        files = {'up': up[parent, name], 'down': down[parent, name]}
        if (files['up'].name == 'up.txt') != (files['down'].name == 'down.txt'):
            raise ValueError(f'mixed capacity file naming in {parent}')
        for direction in ('up', 'down'):
            fixed_names = files[direction].name == f'{direction}.txt'
            p = parent / (f'{direction}.delay.txt' if fixed_names else f'{name}.{direction}.delay.txt')
            if p.is_file():
                files[direction + '_delay'] = p
        ends = [end_fn(p, key.endswith('_delay')) for key, p in files.items()]
        period_ms = min(ends)
        scenarios.append({
            'name': name,
            'files': files,
            'duration_s': period_ms / 1000.0,
            'period_ms': period_ms,
        })
    return scenarios


def discover_dataset(
    dataset: str,
    scenarios_filter: list[str] | None = None,
    *,
    root: Path = ROOT,
    fast_duration: bool = False,
) -> list[dict]:
    """Discover lines scenarios by dataset name ('basic', 'leocc', 'lumos', 'all', or path)."""
    lines_root = root / 'datasets' / 'lines'
    spec = dataset.strip()
    if not spec:
        raise ValueError('dataset must not be empty')

    scenarios: list[dict] = []
    if spec == 'all':
        candidate_dirs = [lines_root / name for name in LINES_DATASETS if (lines_root / name).is_dir()]
        for extra in sorted(lines_root.iterdir()):
            if extra.is_dir() and extra not in candidate_dirs:
                try:
                    found = discover(extra, fast_duration=fast_duration)
                except ValueError:
                    continue
                for s in found:
                    s['dataset'] = extra.name
                scenarios.extend(found)
        for d in candidate_dirs:
            found = discover(d, fast_duration=fast_duration)
            for s in found:
                s['dataset'] = d.name
            scenarios.extend(found)
    else:
        direct = Path(spec)
        if direct.is_dir():
            target = direct
            ds_name = direct.name
        elif (lines_root / spec).is_dir():
            target = lines_root / spec
            ds_name = spec.split('/', 1)[0]
        else:
            raise ValueError(f'dataset not found: {spec} (checked {lines_root / spec})')
        found = discover(target, fast_duration=fast_duration)
        for s in found:
            s['dataset'] = ds_name
        scenarios.extend(found)

    if scenarios_filter:
        wanted = [item.strip() for item in scenarios_filter if item.strip()]
        by_name = {s['name']: s for s in scenarios}
        by_qual = {f"{s.get('dataset', '')}/{s['name']}": s for s in scenarios}
        selected: list[dict] = []
        seen: set[str] = set()
        missing = []
        for token in wanted:
            match = by_qual.get(token) or by_name.get(token)
            if match is None:
                missing.append(token)
                continue
            if match['name'] not in seen:
                seen.add(match['name'])
                selected.append(match)
        if missing:
            raise ValueError(f'unknown scenarios in dataset {spec!r}: {", ".join(missing)}')
        scenarios = selected

    if not scenarios:
        raise ValueError(f'no scenarios selected for dataset {spec!r}')
    return scenarios


def workload(path: Path) -> tuple[int, float]:
    elapsed, count = 0.0, 0
    for number, raw in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        try:
            gap_raw, size_raw = line.split()
            gap, size = float(gap_raw), int(size_raw)
            if not math.isfinite(gap) or gap < 0 or size <= 0 or size > 100_000_000:
                raise ValueError
        except ValueError:
            raise ValueError(f'{path}:{number}: invalid workload entry') from None
        elapsed += gap / 1000.0
        count += 1
    if not count:
        raise ValueError(f'{path}: empty workload')
    return count, elapsed


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for part in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(part)
    return h.hexdigest()


def atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode='w',
        encoding='utf-8',
        dir=path.parent,
        prefix=f'.{path.name}.',
        delete=False,
    ) as stream:
        temp = Path(stream.name)
        try:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            temp.unlink(missing_ok=True)
            raise
    try:
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)
