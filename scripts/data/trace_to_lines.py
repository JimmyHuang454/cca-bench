#!/usr/bin/env python3
"""Convert Mahimahi, Lumos, and LeoCC traces to unified CSV lines files."""
from __future__ import annotations

import argparse
import itertools
import math
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Iterator, NamedTuple, TextIO

ROOT = Path(__file__).resolve().parents[2]

# Edit these settings, then run `python3 scripts/data/trace_to_lines.py`.
# Directory mode: convert all supported trace pairs into unified CSV files under datasets/lines/<dataset>/.
DATASET_DIR: Path | None = ROOT / 'datasets/traces/lumos'

# Single-pair mode: set DATASET_DIR = None and specify these files.
UPLINK_TRACE: Path | None = None
DOWNLINK_TRACE: Path | None = None
DELAY_TRACE: Path | None = None  # Optional delay trace (omit when no measured delay exists; benchmark uses its default).
PACKET_BYTES = 1500
CAPACITY_DECIMALS = 2  # Decimal precision for kb/mb capacity tokens (e.g. 10.33kb, 1.43mb).

_END = '__END__'


def format_capacity_bytes(bytes_per_ms: float, *, decimals: int = CAPACITY_DECIMALS) -> str:
    """Format per-ms byte capacity preferentially using kb/mb with decimal precision."""
    if not math.isfinite(bytes_per_ms) or bytes_per_ms < 0:
        raise ValueError(f'invalid per-ms capacity bytes: {bytes_per_ms!r}')
    if decimals < 0:
        raise ValueError(f'decimals must be nonnegative: {decimals}')
    if bytes_per_ms == 0:
        return '0'

    def fmt(val: float, unit: str) -> str:
        rounded = round(val, decimals)
        text = f'{rounded:.{decimals}f}'
        if '.' in text:
            text = text.rstrip('0').rstrip('.')
        return f'{text}{unit}'

    if bytes_per_ms >= 1024 * 1024 or round(bytes_per_ms / 1024.0, decimals) >= 1024:
        return fmt(bytes_per_ms / (1024.0 * 1024.0), 'mb')
    if bytes_per_ms >= 1024:
        return fmt(bytes_per_ms / 1024.0, 'kb')
    return fmt(bytes_per_ms, 'b')


def format_capacity(packet_count: int, *, decimals: int = CAPACITY_DECIMALS) -> str:
    """Format a 1500-byte packet-opportunity count per ms into kb/mb notation."""
    if packet_count < 0:
        raise ValueError(f'invalid packet count: {packet_count}')
    return format_capacity_bytes(packet_count * PACKET_BYTES, decimals=decimals)


def numbers(path: Path, *, integer: bool) -> Iterator[int | float]:
    found = False
    with path.open(encoding='utf-8') as stream:
        for line_no, line in enumerate(stream, 1):
            value = line.strip()
            if not value or value.startswith('#'):
                continue
            try:
                if integer:
                    if not re.fullmatch(r'\d+', value):
                        raise ValueError
                    number = int(value)
                else:
                    number = float(value)
                    if not math.isfinite(number) or number < 0:
                        raise ValueError
            except ValueError:
                kind = 'integer timestamp' if integer else 'delay'
                raise ValueError(f'{path}:{line_no}: invalid nonnegative {kind}: {value!r}') from None
            found = True
            yield number
    if not found:
        raise ValueError(f'{path}: no samples')


def iter_capacity_events(path: Path, *, decimals: int = CAPACITY_DECIMALS) -> Iterator[tuple[int, str]]:
    """Yield (t_ms, token) capacity changes on [0, end_ms), ending with (end_ms, _END)."""
    previous_time = -1
    previous_token: str | None = None
    for raw_time, group in itertools.groupby(numbers(path, integer=True)):
        at = int(raw_time)
        if at <= previous_time:
            raise ValueError(f'{path}: timestamps must be nondecreasing')
        count = sum(1 for _ in group)
        token = format_capacity(count, decimals=decimals)
        if previous_time == -1 and at > 0:
            yield 0, '0'
            previous_token = '0'
        elif previous_time >= 0 and at > previous_time + 1:
            if previous_token != '0':
                yield previous_time + 1, '0'
                previous_token = '0'
        if token != previous_token:
            yield at, token
            previous_token = token
        previous_time = at
    yield previous_time + 1, _END


def iter_delay_events(path: Path) -> Iterator[tuple[int, str]]:
    """Yield (t_ms, delay_str) 10 ms delay changes on [0, end_ms), ending with (end_ms, _END)."""
    previous: float | None = None
    count = 0
    for count, value in enumerate(numbers(path, integer=False), 1):
        fval = float(value)
        if fval != previous:
            yield (count - 1) * 10, f'{fval:g}'
            previous = fval
    yield count * 10, _END


def write_scene_csv(
    up_path: Path,
    down_path: Path,
    delay_path: Path | None,
    output: TextIO,
    *,
    decimals: int = CAPACITY_DECIMALS,
) -> int:
    """Stream-merge capacity (and optional delay) traces into a unified single-file CSV."""
    symmetric_cap = (up_path == down_path)
    has_delay = (delay_path is not None)

    header = ['time']
    header.extend(['cap'] if symmetric_cap else ['up_cap', 'down_cap'])
    if has_delay:
        header.append('delay')
    output.write(','.join(header) + '\n')

    # Fast streaming path for symmetric capacity with no delay (e.g. Lumos 5G).
    if symmetric_cap and not has_delay:
        end_ms = 0
        for t, tok in iter_capacity_events(up_path, decimals=decimals):
            if tok == _END:
                output.write(f'{t},0\n')
                end_ms = t
                break
            output.write(f'{t},{tok}\n')
        return end_ms

    streams: list[Iterator[tuple[int, str]]] = [iter_capacity_events(up_path, decimals=decimals)]
    if not symmetric_cap:
        streams.append(iter_capacity_events(down_path, decimals=decimals))
    if has_delay and delay_path is not None:
        streams.append(iter_delay_events(delay_path))

    heads: list[tuple[int, str] | None] = [next(it, None) for it in streams]
    ncols = len(streams)
    end_ms = 0

    while True:
        min_t = min(h[0] for h in heads if h is not None)
        if any(h is not None and h[0] == min_t and h[1] == _END for h in heads):
            end_ms = min_t
            end_row = [str(end_ms)] + (['0'] if symmetric_cap else ['0', '0'])
            if has_delay:
                end_row.append('')
            output.write(','.join(end_row) + '\n')
            break

        vals = [''] * ncols
        for idx, h in enumerate(heads):
            if h is not None and h[0] == min_t:
                vals[idx] = h[1]
                heads[idx] = next(streams[idx], None)
        output.write(f'{min_t},' + ','.join(vals) + '\n')

    # Drain remaining iterators so invalid trailing inputs are still caught.
    for it in streams:
        for _ in it:
            pass
    return end_ms


class SceneJob(NamedTuple):
    up: Path
    down: Path
    delay: Path | None
    target: Path


def directory_jobs(directory: Path, dest: Path) -> list[SceneJob]:
    jobs: list[SceneJob] = []
    for down in sorted(directory.glob('*.down')):
        up = down.with_suffix('.up')
        name = down.stem
        lumos = name.startswith(('5g_trace_', '4g_trace_'))
        if lumos:
            # Lumos has measured downlink only; use symmetric capacity and omit delay column.
            src_up, src_down = down, down
        elif up.is_file():
            src_up, src_down = up, down
        else:
            raise ValueError(f'missing uplink file: {up}')
        delay_cand = down.with_suffix('.delay')
        delay = delay_cand if delay_cand.is_file() else None
        jobs.append(SceneJob(src_up, src_down, delay, dest / f'{name}.csv'))

    # LeoCC examples use one bandwidth trace in both directions.
    leo_files = sorted(directory.glob('bw_*.txt'))
    if (directory / 'leo-bw.txt').is_file():
        leo_files.append(directory / 'leo-bw.txt')
    for bw in leo_files:
        delay_cand = (directory / 'leo-delay.txt' if bw.name == 'leo-bw.txt'
                      else directory / f'delay_{bw.name[len("bw_"):]}')
        delay = delay_cand if delay_cand.is_file() else None
        name = bw.stem
        jobs.append(SceneJob(bw, bw, delay, dest / f'{name}.csv'))

    if not jobs:
        raise ValueError(f'{directory}: no .up/.down pairs or bw_*.txt traces found')
    return jobs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('uplink', nargs='?', type=Path, help='uplink trace or dataset directory')
    parser.add_argument('downlink', nargs='?', type=Path)
    parser.add_argument('delay', nargs='?', type=Path)
    parser.add_argument('--decimals', type=int, default=CAPACITY_DECIMALS,
                        help=f'decimal places for kb/mb capacity values (default: {CAPACITY_DECIMALS})')
    args = parser.parse_args()
    if args.decimals < 0:
        raise SystemExit('error: --decimals must be nonnegative')

    created: list[Path] = []
    try:
        if args.uplink is None:
            if DATASET_DIR is not None:
                if not DATASET_DIR.is_dir():
                    raise ValueError(f'dataset directory not found: {DATASET_DIR}')
                args.uplink = DATASET_DIR
            else:
                if UPLINK_TRACE is None or DOWNLINK_TRACE is None:
                    raise ValueError('set DATASET_DIR or both UPLINK_TRACE and DOWNLINK_TRACE')
                args.uplink, args.downlink, args.delay = UPLINK_TRACE, DOWNLINK_TRACE, DELAY_TRACE
        if args.uplink.is_dir():
            if args.downlink or args.delay:
                raise ValueError('directory mode accepts only one argument')
            dest = ROOT / 'datasets' / 'lines' / args.uplink.resolve().name
            jobs = directory_jobs(args.uplink, dest)
        else:
            if args.downlink is None:
                raise ValueError('downlink trace is required')
            name = args.downlink.stem
            dest = ROOT / 'datasets' / 'lines' / 'converted'
            jobs = [SceneJob(args.uplink, args.downlink, args.delay, dest / f'{name}.csv')]

        targets = [job.target for job in jobs]
        if len(set(targets)) != len(targets):
            raise ValueError('multiple inputs map to the same output filename')
        for job in jobs:
            target = job.target
            if target.exists():
                raise ValueError(f'output already exists: {target}')
            scene_dir = target.parent / target.stem
            if (scene_dir / 'line.csv').exists():
                raise ValueError(f'output already exists: {scene_dir / "line.csv"}')

        dest.mkdir(parents=True, exist_ok=True)
        # Convert and validate the entire batch before publishing any files.
        with tempfile.TemporaryDirectory(prefix='.trace-to-lines-', dir=dest) as temp:
            staged: list[tuple[Path, Path]] = []
            for index, job in enumerate(jobs):
                tmp = Path(temp) / f'{index}.csv'
                with tmp.open('w', encoding='utf-8') as output:
                    write_scene_csv(job.up, job.down, job.delay, output, decimals=args.decimals)
                staged.append((tmp, job.target))
            for tmp, target in staged:
                target.parent.mkdir(parents=True, exist_ok=True)
                os.link(tmp, target)  # exclusive: never replace an existing file
                created.append(target)
        for target in created:
            print(target)
    except (OSError, ValueError) as error:
        for target in created:
            target.unlink(missing_ok=True)
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
