#!/usr/bin/env python3
"""Shared CSV lines parser, discovery, workload, and integration helpers for datasets/lines."""
from __future__ import annotations

import csv
import hashlib
import io
import math
import os
import re
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ONE_WAY_DELAY_MS = 10.0
LINES_DATASETS = (
    'basic', 'leocc', 'cellular-traces-2018', 'cellular-traces-nyc',
    'mahimahi', 'lumos', 'pantheon',
    'opennetlab-mmwave-5g', 'opennetlab-fcc-wired', 'opennetlab-norway-3g',
)
CSV_ALLOWED_COLUMNS = frozenset({
    'time', 'cap', 'up_cap', 'down_cap', 'delay', 'up_delay', 'down_delay', 'comment',
})
WORKLOAD_ALLOWED_COLUMNS = frozenset({'time', 'size', 'comment'})


def parse_time_ms(raw_value: str) -> int:
    """Parse a lines timestamp token into a non-negative integer millisecond.

    Supported formats (case-insensitive unit, no whitespace before unit):
      - unsuffixed integer : milliseconds (default, e.g. ``0``, ``1000``)
      - ``<num>ms``        : milliseconds (must resolve to an exact integer ms)
      - ``<num>s``         : seconds (``num * 1000`` ms; must resolve to an exact integer ms)
    """
    s = raw_value.strip()
    if not s:
        raise ValueError('empty time value')
    lower = s.lower()
    if lower.endswith('ms'):
        num_s, mult = s[:-2], 1.0
    elif lower.endswith('s'):
        num_s, mult = s[:-1], 1000.0
    else:
        num_s, mult = s, None

    if not num_s or num_s != num_s.strip():
        raise ValueError(f'invalid time value: {raw_value!r}')
    if mult is None:
        if not num_s.isdigit():
            raise ValueError(f'invalid time value: {raw_value!r}')
        t_ms = int(num_s)
    else:
        val = float(num_s)
        if not math.isfinite(val) or val < 0:
            raise ValueError(f'invalid time value: {raw_value!r}')
        scaled = val * mult
        rounded = round(scaled)
        if not math.isfinite(scaled) or abs(scaled - rounded) > 1e-6:
            raise ValueError(f'time must resolve to whole milliseconds: {raw_value!r}')
        t_ms = int(rounded)
    if t_ms < 0 or t_ms > 10**12:
        raise ValueError(f'time out of range: {raw_value!r}')
    return t_ms


def parse_delay_ms(raw_value: str) -> float:
    """Parse a one-way delay token into non-negative float milliseconds.

    Supported formats (case-insensitive unit, no whitespace before unit):
      - unsuffixed number or ``<num>ms`` : milliseconds (default)
      - ``<num>us``                      : microseconds (``num / 1000.0`` ms)
      - ``<num>s``                       : seconds (``num * 1000.0`` ms)
    """
    s = raw_value.strip()
    if not s:
        raise ValueError('empty delay value')
    lower = s.lower()
    if lower.endswith('ms'):
        num_s, mult = s[:-2], 1.0
    elif lower.endswith('us'):
        num_s, mult = s[:-2], 1e-3
    elif lower.endswith('s'):
        num_s, mult = s[:-1], 1000.0
    else:
        num_s, mult = s, 1.0

    if not num_s or num_s != num_s.strip():
        raise ValueError(f'invalid delay value: {raw_value!r}')
    val = float(num_s)
    if not math.isfinite(val) or val < 0:
        raise ValueError(f'invalid delay value: {raw_value!r}')
    ms = val * mult
    if not math.isfinite(ms):
        raise ValueError(f'invalid delay value: {raw_value!r}')
    return ms


def parse_capacity_value(raw_value: str) -> float:
    """Parse a lines capacity token into 1500-byte opportunities per millisecond.

    Supported formats (case-insensitive, no whitespace allowed between number and unit):
      - ``0``          : zero capacity (outage / end-of-trace sentinel)
      - ``<num>bps``   : bits / s (``num / 12_000_000.0`` opportunities / ms)
      - ``<num>kbps``  : kilobits / s (``num / 12_000.0`` opportunities / ms)
      - ``<num>mbps``  : megabits / s (``num / 12.0`` opportunities / ms)
      - ``<num>gbps``  : gigabits / s (``num * 1000.0 / 12.0`` opportunities / ms)
    """
    s = raw_value.strip()
    if not s:
        raise ValueError('empty capacity value')
    if s == '0':
        return 0.0
    lower = s.lower()
    if lower.endswith('gbps'):
        num_s, mult = s[:-4], 1000.0 / 12.0
    elif lower.endswith('mbps'):
        num_s, mult = s[:-4], 1.0 / 12.0
    elif lower.endswith('kbps'):
        num_s, mult = s[:-4], 1.0 / 12_000.0
    elif lower.endswith('bps'):
        num_s, mult = s[:-3], 1.0 / 12_000_000.0
    else:
        raise ValueError(
            f'invalid capacity value {raw_value!r}: expected 0 or <num>(bps|kbps|mbps|gbps)'
        )

    if not num_s or num_s != num_s.strip() or not re.fullmatch(r'(?:\d+(?:\.\d*)?|\.\d+)', num_s):
        raise ValueError(f'invalid capacity value: {raw_value!r}')
    val = float(num_s)
    if not math.isfinite(val) or val < 0:
        raise ValueError(f'invalid capacity value: {raw_value!r}')
    opps = val * mult
    if not math.isfinite(opps) or opps > 1_000_000:
        raise ValueError(f'capacity out of range: {raw_value!r}')
    return opps


def parse_size_bytes(raw_value: str) -> int:
    """Parse a workload payload size into positive integer bytes (1 .. 100_000_000).

    Supported formats (case-insensitive, no whitespace before unit):
      - unsuffixed integer or ``<num>b`` : bytes
      - ``<num>k`` / ``<num>kb``         : KiB (1024 B)
      - ``<num>m`` / ``<num>mb``         : MiB (1024^2 B)
    """
    s = raw_value.strip()
    if not s:
        raise ValueError('empty size value')
    lower = s.lower()
    if lower.endswith('mb'):
        num_s, mult = s[:-2], 1024.0 * 1024.0
    elif lower.endswith('kb'):
        num_s, mult = s[:-2], 1024.0
    elif lower.endswith('m'):
        num_s, mult = s[:-1], 1024.0 * 1024.0
    elif lower.endswith('k'):
        num_s, mult = s[:-1], 1024.0
    elif lower.endswith('b'):
        num_s, mult = s[:-1], 1.0
    else:
        num_s, mult = s, None

    if not num_s or num_s != num_s.strip():
        raise ValueError(f'invalid size value: {raw_value!r}')
    if mult is None:
        if not num_s.isdigit():
            raise ValueError(f'invalid size value: {raw_value!r}')
        size = int(num_s)
    else:
        val = float(num_s)
        if not math.isfinite(val) or val <= 0:
            raise ValueError(f'invalid size value: {raw_value!r}')
        size = int(round(val * mult))
    if size <= 0 or size > 100_000_000:
        raise ValueError(f'size out of range: {raw_value!r}')
    return size


def _parse_csv_header(raw_header: str, path: Path, line_no: int) -> dict[str, int]:
    try:
        fields = next(csv.reader([raw_header], strict=True))
    except csv.Error:
        raise ValueError(f'{path}:{line_no}: malformed CSV header') from None
    cols: dict[str, int] = {}
    for idx, raw in enumerate(fields):
        name = raw.strip().lower()
        if not name or name not in CSV_ALLOWED_COLUMNS or name in cols:
            raise ValueError(f'{path}:{line_no}: invalid or duplicate CSV column {raw!r}')
        cols[name] = idx
    if 'time' not in cols:
        raise ValueError(f'{path}:{line_no}: missing required time column')
    has_up_cap_source = ('up_cap' in cols) or ('cap' in cols)
    has_down_cap_source = ('down_cap' in cols) or ('cap' in cols)
    if not (has_up_cap_source and has_down_cap_source):
        raise ValueError(f'{path}:{line_no}: must provide cap or both up_cap and down_cap')
    return cols


def parse_line_csv(path: Path) -> dict:
    """Parse a unified single-file CSV lines scenario.

    Supported columns (case-insensitive header, arbitrary column order):
      - ``time`` (required): strictly increasing time (default ms, or ``ms``/``s`` suffix)
      - ``cap``, ``up_cap``, ``down_cap``: link bitrate capacity (``0`` or ``bps``/``kbps``/``mbps``/``gbps``, case-insensitive);
        direction-specific columns override ``cap`` on each row (``up_cap > cap``, ``down_cap > cap``).
      - ``delay``, ``up_delay``, ``down_delay`` (optional): one-way base delay (default ms, or ``us``/``ms``/``s`` suffix);
        direction-specific columns override ``delay`` on each row (``up_delay > delay``, ``down_delay > delay``).
      - ``comment`` (optional): free-text row annotation (ignored during simulation).

    Empty resolved cells carry forward the previous value; the final row sets both capacities to 0 and defines end_ms.
    """
    cols: dict[str, int] | None = None
    ncols = 0
    up_pts: list[tuple[int, float]] = []
    down_pts: list[tuple[int, float]] = []
    up_delay_pts: list[tuple[int, float]] = []
    down_delay_pts: list[tuple[int, float]] = []
    has_up_delay = False
    has_down_delay = False
    prev_t = -1
    last_up_cap_zero = False
    last_down_cap_zero = False
    last_row_up_delay_t: int | None = None
    last_row_down_delay_t: int | None = None
    row_count = 0

    with path.open(encoding='utf-8-sig', newline='') as stream:
        for line_no, raw in enumerate(stream, 1):
            stripped = raw.strip()
            if not stripped or stripped.startswith('#'):
                continue
            if cols is None:
                cols = _parse_csv_header(raw, path, line_no)
                ncols = len(cols)
                t_idx = cols['time']
                cap_idx = cols.get('cap')
                up_cap_idx = cols.get('up_cap')
                down_cap_idx = cols.get('down_cap')
                delay_idx = cols.get('delay')
                up_d_idx = cols.get('up_delay')
                down_d_idx = cols.get('down_delay')
                continue

            try:
                fields = next(csv.reader([raw], strict=True))
            except csv.Error:
                raise ValueError(f'{path}:{line_no}: malformed CSV row') from None
            if len(fields) > ncols:
                raise ValueError(f'{path}:{line_no}: too many CSV columns')
            if len(fields) < ncols:
                fields.extend([''] * (ncols - len(fields)))

            try:
                t = parse_time_ms(fields[t_idx])
            except ValueError:
                raise ValueError(f'{path}:{line_no}: invalid time {fields[t_idx]!r}') from None
            if row_count == 0:
                if t != 0:
                    raise ValueError(f'{path}:{line_no}: first row must start at time=0')
            elif t <= prev_t:
                raise ValueError(f'{path}:{line_no}: time must be strictly increasing')

            try:
                cap_raw = fields[cap_idx].strip() if cap_idx is not None else ''
                up_cap_raw = fields[up_cap_idx].strip() if up_cap_idx is not None else ''
                down_cap_raw = fields[down_cap_idx].strip() if down_cap_idx is not None else ''

                cap_val = parse_capacity_value(cap_raw) if cap_raw else None
                up_val = parse_capacity_value(up_cap_raw) if up_cap_raw else cap_val
                down_val = parse_capacity_value(down_cap_raw) if down_cap_raw else cap_val

                if row_count == 0 and (up_val is None or down_val is None):
                    raise ValueError('initial t=0 capacity must be specified')

                if up_val is not None:
                    if not up_pts or up_val != up_pts[-1][1]:
                        up_pts.append((t, up_val))
                    last_up_cap_zero = (up_val == 0.0)
                else:
                    last_up_cap_zero = False

                if down_val is not None:
                    if not down_pts or down_val != down_pts[-1][1]:
                        down_pts.append((t, down_val))
                    last_down_cap_zero = (down_val == 0.0)
                else:
                    last_down_cap_zero = False

                delay_raw = fields[delay_idx].strip() if delay_idx is not None else ''
                up_d_raw = fields[up_d_idx].strip() if up_d_idx is not None else ''
                down_d_raw = fields[down_d_idx].strip() if down_d_idx is not None else ''

                common_d = parse_delay_ms(delay_raw) if delay_raw else None
                up_d_val = parse_delay_ms(up_d_raw) if up_d_raw else common_d
                down_d_val = parse_delay_ms(down_d_raw) if down_d_raw else common_d

                if row_count == 0:
                    if up_d_val is not None:
                        has_up_delay = True
                        up_delay_pts.append((t, up_d_val))
                    if down_d_val is not None:
                        has_down_delay = True
                        down_delay_pts.append((t, down_d_val))
                else:
                    if up_d_val is not None:
                        if not has_up_delay:
                            raise ValueError('up delay missing at t=0')
                        if not up_delay_pts or up_d_val != up_delay_pts[-1][1]:
                            up_delay_pts.append((t, up_d_val))
                            last_row_up_delay_t = t
                        else:
                            last_row_up_delay_t = None
                    else:
                        last_row_up_delay_t = None

                    if down_d_val is not None:
                        if not has_down_delay:
                            raise ValueError('down delay missing at t=0')
                        if not down_delay_pts or down_d_val != down_delay_pts[-1][1]:
                            down_delay_pts.append((t, down_d_val))
                            last_row_down_delay_t = t
                        else:
                            last_row_down_delay_t = None
                    else:
                        last_row_down_delay_t = None
            except ValueError:
                raise ValueError(f'{path}:{line_no}: invalid CSV value') from None

            prev_t = t
            row_count += 1

    if cols is None or row_count < 2 or prev_t <= 0:
        raise ValueError(f'{path}: CSV lines file must have header and at least 2 rows ending at end_ms > 0')
    if not (last_up_cap_zero and last_down_cap_zero):
        raise ValueError(f'{path}: final row must explicitly end capacity with 0')

    end_ms = prev_t
    if up_pts[-1][0] != end_ms:
        up_pts.append((end_ms, 0.0))
    if down_pts[-1][0] != end_ms:
        down_pts.append((end_ms, 0.0))

    if has_up_delay and last_row_up_delay_t == end_ms and up_delay_pts and up_delay_pts[-1][0] == end_ms:
        up_delay_pts.pop()
    if has_down_delay and last_row_down_delay_t == end_ms and down_delay_pts and down_delay_pts[-1][0] == end_ms:
        down_delay_pts.pop()

    return {
        'end_ms': end_ms,
        'up': up_pts,
        'down': down_pts,
        'up_delay': up_delay_pts if has_up_delay else None,
        'down_delay': down_delay_pts if has_down_delay else None,
    }


def peek_line_csv_info(path: Path) -> tuple[int, bool, bool]:
    """Fast (end_ms, has_up_delay, has_down_delay) lookup for large CSV lines files."""
    try:
        cols: dict[str, int] | None = None
        first_row: list[str] | None = None
        with path.open(encoding='utf-8-sig', newline='') as stream:
            for line_no, raw in enumerate(stream, 1):
                stripped = raw.strip()
                if not stripped or stripped.startswith('#'):
                    continue
                if cols is None:
                    cols = _parse_csv_header(raw, path, line_no)
                else:
                    first_row = next(csv.reader([raw], strict=True))
                    break
        if cols is None or first_row is None:
            parsed = parse_line_csv(path)
            return parsed['end_ms'], parsed['up_delay'] is not None, parsed['down_delay'] is not None
        ncols = len(cols)
        if len(first_row) < ncols:
            first_row.extend([''] * (ncols - len(first_row)))
        delay_idx = cols.get('delay')
        up_d_idx = cols.get('up_delay')
        down_d_idx = cols.get('down_delay')
        has_common_d = delay_idx is not None and bool(first_row[delay_idx].strip())
        has_up_delay = (up_d_idx is not None and bool(first_row[up_d_idx].strip())) or has_common_d
        has_down_delay = (down_d_idx is not None and bool(first_row[down_d_idx].strip())) or has_common_d

        size = path.stat().st_size
        read_len = min(size, 4096)
        with path.open('rb') as stream:
            stream.seek(size - read_len)
            tail = stream.read(read_len).decode('utf-8', errors='strict')
        t_idx = cols['time']
        cap_idx = cols.get('cap')
        up_cap_idx = cols.get('up_cap')
        down_cap_idx = cols.get('down_cap')
        for raw in reversed(tail.splitlines()):
            stripped = raw.strip()
            if not stripped or stripped.startswith('#'):
                continue
            fields = next(csv.reader([raw], strict=True))
            if len(fields) < ncols:
                fields.extend([''] * (ncols - len(fields)))
            t = parse_time_ms(fields[t_idx])
            cap_raw = fields[cap_idx].strip() if cap_idx is not None else ''
            up_raw = (fields[up_cap_idx].strip() if up_cap_idx is not None else '') or cap_raw
            down_raw = (fields[down_cap_idx].strip() if down_cap_idx is not None else '') or cap_raw
            if t > 0 and up_raw and down_raw and parse_capacity_value(up_raw) == 0 and parse_capacity_value(down_raw) == 0:
                return t, has_up_delay, has_down_delay
            break
    except (OSError, ValueError, csv.Error):
        pass
    parsed = parse_line_csv(path)
    return parsed['end_ms'], parsed['up_delay'] is not None, parsed['down_delay'] is not None


def load_scenario_lines(scenario: dict, *, default_one_way_delay_ms: float | None = None) -> dict:
    """Load capacity and delay schedules from a CSV lines scenario."""
    files = scenario.get('files') or {}
    if 'line' not in files:
        raise ValueError(f'scenario missing CSV line file: {scenario.get("name", "<unknown>")}')
    data = parse_line_csv(Path(files['line']))
    up_delay = data['up_delay']
    down_delay = data['down_delay']
    has_up_delay = up_delay is not None
    has_down_delay = down_delay is not None

    if default_one_way_delay_ms is not None:
        if up_delay is None:
            up_delay = [(0, float(default_one_way_delay_ms))]
        if down_delay is None:
            down_delay = [(0, float(default_one_way_delay_ms))]

    return {
        'end_ms': data['end_ms'],
        'up': data['up'],
        'down': data['down'],
        'up_delay': up_delay,
        'down_delay': down_delay,
        'has_up_delay': has_up_delay,
        'has_down_delay': has_down_delay,
    }


def write_line_csv(
    path: Path,
    *,
    up_tokens: list[tuple[int, str]],
    down_tokens: list[tuple[int, str]],
    end_ms: int,
    up_delay_tokens: list[tuple[int, str]] | None = None,
    down_delay_tokens: list[tuple[int, str]] | None = None,
    comments: dict[int, str] | None = None,
    include_comment_col: bool = False,
) -> None:
    """Write a canonical single-file CSV lines scenario with unitless headers."""
    if end_ms <= 0:
        raise ValueError(f'end_ms must be > 0, got {end_ms}')

    def norm_cap(pts: list[tuple[int, str]]) -> dict[int, str]:
        out: dict[int, str] = {}
        prev: str | None = None
        for t, tok in pts:
            if t < 0 or t >= end_ms:
                continue
            if not out and t != 0:
                raise ValueError('capacity schedule must start at t=0')
            if tok != prev:
                out[t] = tok
                prev = tok
        if 0 not in out:
            raise ValueError('capacity schedule must start at t=0')
        return out

    def norm_delay(pts: list[tuple[int, str]] | None) -> dict[int, str] | None:
        if pts is None:
            return None
        out: dict[int, str] = {}
        prev: str | None = None
        for t, tok in pts:
            if t < 0 or t >= end_ms:
                continue
            if not out and t != 0:
                raise ValueError('delay schedule must start at t=0')
            if tok != prev:
                out[t] = tok
                prev = tok
        if 0 not in out:
            raise ValueError('delay schedule must start at t=0')
        return out

    up_map = norm_cap(up_tokens)
    down_map = norm_cap(down_tokens)
    ud_map = norm_delay(up_delay_tokens)
    dd_map = norm_delay(down_delay_tokens)
    cmt_map = {t: c for t, c in (comments or {}).items() if 0 <= t <= end_ms and c}

    sym_cap = (up_map == down_map)
    has_ud = ud_map is not None
    has_dd = dd_map is not None
    sym_delay = has_ud and has_dd and (ud_map == dd_map)
    has_comment = include_comment_col or bool(cmt_map)

    header = ['time']
    header.extend(['cap'] if sym_cap else ['up_cap', 'down_cap'])
    if sym_delay:
        header.append('delay')
    else:
        if has_ud:
            header.append('up_delay')
        if has_dd:
            header.append('down_delay')
    if has_comment:
        header.append('comment')

    times = set(up_map) | set(down_map) | set(cmt_map)
    if ud_map:
        times |= set(ud_map)
    if dd_map:
        times |= set(dd_map)
    times.discard(end_ms)

    buf = io.StringIO()
    writer = csv.writer(buf, lineterminator='\n')
    writer.writerow(header)

    for t in sorted(times):
        row = [str(t)]
        if sym_cap:
            row.append(up_map.get(t, ''))
        else:
            row.append(up_map.get(t, ''))
            row.append(down_map.get(t, ''))
        if sym_delay:
            row.append(ud_map.get(t, '') if ud_map else '')
        else:
            if has_ud:
                row.append(ud_map.get(t, '') if ud_map else '')
            if has_dd:
                row.append(dd_map.get(t, '') if dd_map else '')
        if has_comment:
            row.append(cmt_map.get(t, ''))
        writer.writerow(row)

    end_row = [str(end_ms)]
    end_row.extend(['0'] if sym_cap else ['0', '0'])
    if sym_delay:
        end_row.append('')
    else:
        if has_ud:
            end_row.append('')
        if has_dd:
            end_row.append('')
    if has_comment:
        end_row.append(cmt_map.get(end_ms, ''))
    writer.writerow(end_row)

    atomic_write(path, buf.getvalue())


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
    if directory.is_file():
        if directory.suffix.lower() != '.csv':
            raise ValueError(f'unsupported scenario file: {directory}')
        name = directory.parent.name if directory.stem == 'line' else directory.stem
        if fast_duration:
            period_ms, has_ud, has_dd = peek_line_csv_info(directory)
        else:
            parsed = parse_line_csv(directory)
            period_ms = parsed['end_ms']
            has_ud = parsed['up_delay'] is not None
            has_dd = parsed['down_delay'] is not None
        return [{
            'name': name,
            'files': {'line': directory},
            'duration_s': period_ms / 1000.0,
            'period_ms': period_ms,
            'has_up_delay': has_ud,
            'has_down_delay': has_dd,
        }]
    if not directory.is_dir():
        raise ValueError(f'dataset directory not found: {directory}')

    csv_files: dict[tuple[Path, str], Path] = {}
    for p in directory.rglob('*.csv'):
        if p.name.startswith('.'):
            continue
        key = (p.parent, p.parent.name) if p.name == 'line.csv' else (p.parent, p.stem)
        if key in csv_files:
            raise ValueError(f'ambiguous CSV scenario files in {p.parent}')
        csv_files[key] = p

    if not csv_files:
        raise ValueError(f'no CSV line scenarios found in {directory}')

    scenarios: list[dict] = []
    seen: set[str] = set()
    for parent, name in sorted(csv_files, key=lambda item: (item[1], item[0])):
        if name in seen:
            raise ValueError(f'duplicate scenario name: {name}')
        seen.add(name)
        csv_path = csv_files[parent, name]
        if fast_duration:
            period_ms, has_ud, has_dd = peek_line_csv_info(csv_path)
        else:
            parsed = parse_line_csv(csv_path)
            period_ms = parsed['end_ms']
            has_ud = parsed['up_delay'] is not None
            has_dd = parsed['down_delay'] is not None
        scenarios.append({
            'name': name,
            'files': {'line': csv_path},
            'duration_s': period_ms / 1000.0,
            'period_ms': period_ms,
            'has_up_delay': has_ud,
            'has_down_delay': has_dd,
        })
    return scenarios


def discover_dataset(
    dataset: str,
    scenarios_filter: list[str] | None = None,
    *,
    root: Path = ROOT,
    fast_duration: bool = False,
) -> list[dict]:
    """Discover CSV lines scenarios by dataset name ('basic', 'leocc', 'lumos', 'all', or path)."""
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
        if direct.is_dir() or (direct.is_file() and direct.suffix.lower() == '.csv'):
            target = direct
            ds_name = direct.parent.name if direct.is_file() else direct.name
        elif (lines_root / spec).is_dir() or ((lines_root / spec).is_file() and (lines_root / spec).suffix.lower() == '.csv'):
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


def parse_workload(path: Path) -> list[tuple[float, int]]:
    """Parse a standard RFC 4180 CSV workload file into [(t_ms, size_bytes), ...].

    Supported columns (case-insensitive header, arbitrary column order):
      - ``time`` (required): non-negative monotonically non-decreasing arrival time
        (default ms, or ``us``/``ms``/``s`` suffix, no space before unit).
      - ``size`` (required): positive payload size in ``1 .. 100_000_000`` bytes
        (default int bytes, or ``b``/``k``/``kb``/``m``/``mb`` suffix, binary 1024).
      - ``comment`` (optional): free-text row annotation (ignored during simulation).
    """
    cols: dict[str, int] | None = None
    ncols = 0
    items: list[tuple[float, int]] = []
    with path.open(encoding='utf-8-sig', newline='') as stream:
        for line_no, raw in enumerate(stream, 1):
            stripped = raw.strip()
            if not stripped or stripped.startswith('#'):
                continue
            try:
                fields = next(csv.reader([raw], strict=True))
            except csv.Error:
                raise ValueError(f'{path}:{line_no}: malformed CSV row') from None
            if cols is None:
                cols = {}
                for idx, h in enumerate(fields):
                    name = h.strip().lower()
                    if not name or name not in WORKLOAD_ALLOWED_COLUMNS or name in cols:
                        raise ValueError(f'{path}:{line_no}: invalid or duplicate workload column {h!r}')
                    cols[name] = idx
                if 'time' not in cols or 'size' not in cols:
                    raise ValueError(f'{path}:{line_no}: workload CSV requires time and size columns')
                ncols = len(cols)
                t_idx = cols['time']
                s_idx = cols['size']
                continue
            if len(fields) > ncols:
                raise ValueError(f'{path}:{line_no}: too many CSV columns')
            if len(fields) < ncols:
                fields.extend([''] * (ncols - len(fields)))
            try:
                t_ms = parse_delay_ms(fields[t_idx])
                size = parse_size_bytes(fields[s_idx])
                if items and t_ms < items[-1][0]:
                    raise ValueError('time must be non-decreasing')
            except ValueError:
                raise ValueError(f'{path}:{line_no}: invalid workload entry') from None
            items.append((t_ms, size))
    if cols is None or not items:
        raise ValueError(f'{path}: empty workload')
    return items


def format_size_bytes(size_bytes: int) -> str:
    """Losslessly format exact byte sizes with binary mb/kb suffixes when divisible."""
    if size_bytes % (1024 * 1024) == 0:
        return f'{size_bytes // (1024 * 1024)}mb'
    if size_bytes % 1024 == 0:
        return f'{size_bytes // 1024}kb'
    return str(size_bytes)


def write_workload_csv(
    path: Path,
    items: list[tuple[float, int]],
    *,
    comments: dict[int, str] | None = None,
    include_comment_col: bool = False,
) -> None:
    """Write a canonical RFC 4180 CSV workload file (time,size[,comment])."""
    if not items:
        raise ValueError('cannot write empty workload')
    cmt_map = {idx: c for idx, c in (comments or {}).items() if c}
    has_comment = include_comment_col or bool(cmt_map)
    buf = io.StringIO()
    writer = csv.writer(buf, lineterminator='\n')
    writer.writerow(['time', 'size', 'comment'] if has_comment else ['time', 'size'])
    prev_t = -1.0
    for idx, (t_ms, size) in enumerate(items):
        if t_ms < 0 or (prev_t >= 0 and t_ms < prev_t):
            raise ValueError(f'workload timestamps must be non-decreasing at index {idx}')
        t_s = str(int(round(t_ms))) if abs(t_ms - round(t_ms)) < 1e-9 else f'{t_ms:g}'
        row = [t_s, format_size_bytes(size)]
        if has_comment:
            row.append(cmt_map.get(idx, ''))
        writer.writerow(row)
        prev_t = t_ms
    atomic_write(path, buf.getvalue())


def workload(path: Path) -> tuple[int, float]:
    items = parse_workload(path)
    return len(items), items[-1][0] / 1000.0


def write_mortise_workload(src: Path, dst: Path) -> None:
    """Materialize a CSV workload into legacy Mortise ``<gap_ms>\\t<size_bytes>`` for upstream client."""
    items = parse_workload(src)
    prev_t = 0.0
    lines: list[str] = []
    for t_ms, size in items:
        gap = max(0.0, t_ms - prev_t)
        gap_s = str(int(round(gap))) if abs(gap - round(gap)) < 1e-6 else f'{gap:g}'
        lines.append(f'{gap_s}\t{size}')
        prev_t = t_ms
    atomic_write(dst, '\n'.join(lines) + '\n')


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
