#!/usr/bin/env python3
"""Convert Mahimahi, Lumos, and LeoCC traces to unified CSV lines files."""
from __future__ import annotations

import argparse
import csv
import itertools
import json
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
CAPACITY_DECIMALS = 3  # Decimal precision for bps/kbps/mbps/gbps capacity tokens (e.g. 2.135mbps, 307kbps, 12mbps).

_END = '__END__'

# Official Pantheon calibrated real-world Internet paths (Yan et al., USENIX ATC'18 / pantheon.stanford.edu).
# Maps source .trace filename -> (output_stem, one_way_delay_ms, steady_cap_token_or_None, description).
# For the 3 Poisson cellular traces, steady_cap_token is None so every stochastic millisecond event is preserved.
# For the 3 steady-rate wired WAN traces (deterministic 60 s Bresenham pacing), exact Mbps token avoids 55k-row bloat.
PANTHEON_CALIBRATED: dict[str, tuple[str, int, str | None, str]] = {
    '0.57mbps-poisson.trace': (
        'nepal_to_aws_india',
        28,
        None,
        'Pantheon calibrated: Nepal cellular to AWS India (0.57mbps-poisson, mm-delay 28ms)',
    ),
    '2.64mbps-poisson.trace': (
        'mexico_cell_to_aws_ca',
        88,
        None,
        'Pantheon calibrated: Mexico cellular to AWS California (2.64mbps-poisson, mm-delay 88ms)',
    ),
    '3.04mbps-poisson.trace': (
        'aws_brazil_to_colombia_cell',
        130,
        None,
        'Pantheon calibrated: AWS Brazil to Colombia cellular (3.04mbps-poisson, mm-delay 130ms)',
    ),
    '77.72mbps.trace': (
        'aws_korea_to_china',
        51,
        '77.72mbps',
        'Pantheon calibrated: AWS Korea to China wired (77.72mbps, mm-delay 51ms)',
    ),
    '100.42mbps.trace': (
        'india_to_aws_india',
        27,
        '100.42mbps',
        'Pantheon calibrated: India wired to AWS India (100.42mbps, mm-delay 27ms)',
    ),
    '114.68mbps.trace': (
        'aws_ca_to_mexico',
        45,
        '114.68mbps',
        'Pantheon calibrated: AWS California to Mexico wired (114.68mbps, mm-delay 45ms)',
    ),
}


def format_capacity_bps(bits_per_sec: float, *, decimals: int = CAPACITY_DECIMALS) -> str:
    """Format link capacity bitrate using bps/kbps/mbps/gbps (0 for zero)."""
    if not math.isfinite(bits_per_sec) or bits_per_sec < 0:
        raise ValueError(f'invalid capacity bps: {bits_per_sec!r}')
    if decimals < 0:
        raise ValueError(f'decimals must be nonnegative: {decimals}')
    if bits_per_sec == 0:
        return '0'

    def fmt(val: float, unit: str) -> str:
        rounded = round(val, decimals)
        text = f'{rounded:.{decimals}f}'
        if '.' in text:
            text = text.rstrip('0').rstrip('.')
        return f'{text}{unit}'

    if bits_per_sec >= 1e9 or round(bits_per_sec / 1e6, decimals) >= 1000:
        return fmt(bits_per_sec / 1e9, 'gbps')
    if bits_per_sec >= 1e6 or round(bits_per_sec / 1e3, decimals) >= 1000:
        return fmt(bits_per_sec / 1e6, 'mbps')
    if bits_per_sec >= 1e3 or round(bits_per_sec, decimals) >= 1000:
        return fmt(bits_per_sec / 1e3, 'kbps')
    return fmt(bits_per_sec, 'bps')


def format_capacity(packet_count: int, *, decimals: int = CAPACITY_DECIMALS) -> str:
    """Format a 1500-byte packet-opportunity count per ms into mbps/gbps notation."""
    if packet_count < 0:
        raise ValueError(f'invalid packet count: {packet_count}')
    return format_capacity_bps(packet_count * PACKET_BYTES * 8.0 * 1000.0, decimals=decimals)


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
    constant_delay_ms: float | None = None,
    steady_cap_token: str | None = None,
    comment: str | None = None,
) -> int:
    """Stream-merge capacity (and optional delay) traces into a unified single-file CSV."""
    symmetric_cap = (up_path == down_path)
    has_delay = (delay_path is not None) or (constant_delay_ms is not None)
    has_comment = bool(comment)

    header = ['time']
    header.extend(['cap'] if symmetric_cap else ['up_cap', 'down_cap'])
    if has_delay:
        header.append('delay')
    if has_comment:
        header.append('comment')

    writer = csv.writer(output, lineterminator='\n')
    writer.writerow(header)

    # Compact exact-rate output for deterministic steady-rate traces (e.g. Pantheon calibrated wired WAN).
    if steady_cap_token is not None:
        end_ms = 0
        for t, tok in iter_capacity_events(up_path, decimals=decimals):
            if tok == _END:
                end_ms = t
        row0 = ['0', steady_cap_token] if symmetric_cap else ['0', steady_cap_token, steady_cap_token]
        end_row = [str(end_ms), '0'] if symmetric_cap else [str(end_ms), '0', '0']
        if has_delay:
            assert constant_delay_ms is not None
            row0.append(f'{constant_delay_ms:g}')
            end_row.append('')
        if has_comment:
            row0.append(comment or '')
            end_row.append('')
        writer.writerow(row0)
        writer.writerow(end_row)
        return end_ms

    # Fast streaming path for symmetric capacity with no dynamic delay trace (e.g. Lumos 5G, NYC 4G, Pantheon Poisson).
    if symmetric_cap and delay_path is None:
        end_ms = 0
        first_row = True
        for t, tok in iter_capacity_events(up_path, decimals=decimals):
            if tok == _END:
                end_row = [str(t), '0']
                if has_delay:
                    end_row.append('')
                if has_comment:
                    end_row.append('')
                writer.writerow(end_row)
                end_ms = t
                break
            row = [str(t), tok]
            if has_delay:
                row.append(f'{constant_delay_ms:g}' if first_row else '')
            if has_comment:
                row.append((comment or '') if first_row else '')
            writer.writerow(row)
            first_row = False
        return end_ms

    streams: list[Iterator[tuple[int, str]]] = [iter_capacity_events(up_path, decimals=decimals)]
    if not symmetric_cap:
        streams.append(iter_capacity_events(down_path, decimals=decimals))
    if delay_path is not None:
        streams.append(iter_delay_events(delay_path))

    heads: list[tuple[int, str] | None] = [next(it, None) for it in streams]
    ncols = len(streams)
    end_ms = 0
    first_row = True

    while True:
        min_t = min(h[0] for h in heads if h is not None)
        if any(h is not None and h[0] == min_t and h[1] == _END for h in heads):
            end_ms = min_t
            end_row = [str(end_ms)] + (['0'] if symmetric_cap else ['0', '0'])
            if has_delay:
                end_row.append('')
            if has_comment:
                end_row.append('')
            writer.writerow(end_row)
            break

        vals = [''] * ncols
        for idx, h in enumerate(heads):
            if h is not None and h[0] == min_t:
                vals[idx] = h[1]
                heads[idx] = next(streams[idx], None)
        row = [str(min_t), *vals]
        if delay_path is None and constant_delay_ms is not None:
            row.append(f'{constant_delay_ms:g}' if first_row else '')
        if has_comment:
            row.append((comment or '') if first_row else '')
        writer.writerow(row)
        first_row = False

    # Drain remaining iterators so invalid trailing inputs are still caught.
    for it in streams:
        for _ in it:
            pass
    return end_ms


def _parse_opennetlab_pattern(
    pattern: list[dict],
    *,
    decimals: int,
    source: Path,
) -> tuple[list[tuple[int, str]], list[tuple[int, str]] | None, int]:
    """Parse an OpenNetLab trace_pattern list into (cap_events, delay_events_or_None, end_ms).

    OpenNetLab unit semantics (verified from onl-emu/emulator_call/tc_setup.py):
      - duration : segment duration in ms
      - capacity : kbps -> bytes/ms = capacity / 8.0
      - rtt      : full RTT in ms -> symmetric one-way delay = rtt / 2.0 ms
    """
    if not isinstance(pattern, list) or not pattern:
        raise ValueError(f'{source}: trace_pattern must be a non-empty list')
    cap_events: list[tuple[int, str]] = []
    delay_events: list[tuple[int, str]] = []
    has_any_rtt = False
    elapsed_ms = 0.0
    prev_cap: str | None = None
    prev_delay: str | None = None

    for idx, seg in enumerate(pattern):
        if not isinstance(seg, dict):
            raise ValueError(f'{source}: segment {idx} must be a JSON object')
        dur = float(seg.get('duration', 0.0))
        if not math.isfinite(dur) or dur <= 0.0:
            raise ValueError(f'{source}: segment {idx} has invalid duration {dur!r}')
        cap_kbps = float(seg.get('capacity', 0.0))
        if not math.isfinite(cap_kbps) or cap_kbps < 0.0:
            raise ValueError(f'{source}: segment {idx} has invalid capacity {cap_kbps!r}')

        t_start = int(round(elapsed_ms))
        elapsed_ms += dur
        t_end = int(round(elapsed_ms))
        if t_end <= t_start:
            continue

        cap_tok = format_capacity_bps(cap_kbps * 1000.0, decimals=decimals)
        if not cap_events or cap_tok != prev_cap:
            cap_events.append((t_start, cap_tok))
            prev_cap = cap_tok

        if 'rtt' in seg and seg['rtt'] is not None:
            rtt_ms = float(seg['rtt'])
            if not math.isfinite(rtt_ms) or rtt_ms < 0.0:
                raise ValueError(f'{source}: segment {idx} has invalid rtt {rtt_ms!r}')
            if rtt_ms > 0.0:
                has_any_rtt = True
                delay_tok = f'{round(rtt_ms / 2.0, 3):g}'
                if not delay_events or delay_tok != prev_delay:
                    delay_events.append((t_start, delay_tok))
                    prev_delay = delay_tok

    end_ms = int(round(elapsed_ms))
    if not cap_events or end_ms <= 0:
        raise ValueError(f'{source}: trace_pattern produced no valid intervals')
    if cap_events[0][0] != 0:
        cap_events.insert(0, (0, '0'))
    if has_any_rtt and delay_events and delay_events[0][0] != 0:
        delay_events.insert(0, (0, delay_events[0][1]))
    return cap_events, (delay_events if has_any_rtt else None), end_ms


def write_opennetlab_json_csv(
    json_path: Path,
    output: TextIO,
    *,
    decimals: int = CAPACITY_DECIMALS,
) -> int:
    """Convert an OpenNetLab/AlphaRTC JSON trace into a unified single-file CSV."""
    try:
        data = json.loads(json_path.read_text(encoding='utf-8'))
    except json.JSONDecodeError as exc:
        raise ValueError(f'{json_path}: invalid JSON: {exc}') from None
    if not isinstance(data, dict):
        raise ValueError(f'{json_path}: expected top-level JSON object')

    ul_pat = (data.get('uplink') or {}).get('trace_pattern')
    dl_pat = (data.get('downlink') or {}).get('trace_pattern')
    if not ul_pat:
        raise ValueError(f'{json_path}: missing uplink.trace_pattern')

    ul_cap, ul_delay, ul_end = _parse_opennetlab_pattern(ul_pat, decimals=decimals, source=json_path)
    symmetric = not bool(dl_pat)
    if symmetric:
        end_ms = ul_end
        cap_cols = [('cap', dict(ul_cap))]
        delay_cols = [('delay', dict(ul_delay))] if ul_delay is not None else []
    else:
        dl_cap, dl_delay, dl_end = _parse_opennetlab_pattern(dl_pat, decimals=decimals, source=json_path)
        end_ms = min(ul_end, dl_end)
        cap_cols = [('up_cap', dict(ul_cap)), ('down_cap', dict(dl_cap))]
        delay_cols = []
        if ul_delay is not None:
            delay_cols.append(('up_delay', dict(ul_delay)))
        if dl_delay is not None:
            delay_cols.append(('down_delay', dict(dl_delay)))

    src_parts = []
    if data.get('from'):
        src_parts.append(f"from={data['from']}")
    if data.get('delay_from'):
        src_parts.append(f"delay_from={data['delay_from']}")
    comment = f"OpenNetLab ({'; '.join(src_parts)})" if src_parts else None

    header = ['time'] + [c for c, _ in cap_cols] + [c for c, _ in delay_cols] + (['comment'] if comment else [])
    writer = csv.writer(output, lineterminator='\n')
    writer.writerow(header)

    all_times = sorted({t for _, mp in (*cap_cols, *delay_cols) for t in mp if t < end_ms})
    for idx, t in enumerate(all_times):
        row = [str(t)]
        for _, mp in cap_cols:
            row.append(mp.get(t, ''))
        for _, mp in delay_cols:
            row.append(mp.get(t, ''))
        if comment:
            row.append(comment if idx == 0 else '')
        writer.writerow(row)

    end_row = [str(end_ms)] + ['0'] * len(cap_cols) + [''] * len(delay_cols) + ([''] if comment else [])
    writer.writerow(end_row)
    return end_ms


class SceneJob(NamedTuple):
    up: Path
    down: Path
    delay: Path | None
    target: Path
    constant_delay_ms: float | None = None
    steady_cap_token: str | None = None
    comment: str | None = None


def directory_jobs(directory: Path, dest: Path) -> list[SceneJob]:
    jobs: list[SceneJob] = []
    # Official StanfordSNR/pantheon-traces directory: emit the 6 calibrated 60s real-world paths
    # with their official mm-delay values and skip Mahimahi 1-ms repeating stubs (e.g. 12mbps.trace).
    pantheon_hits = [fn for fn in PANTHEON_CALIBRATED if (directory / fn).is_file()]
    if pantheon_hits and (directory.resolve().name.lower().startswith('pantheon') or len(pantheon_hits) >= 2):
        for fn in pantheon_hits:
            stem, delay_ms, steady_tok, desc = PANTHEON_CALIBRATED[fn]
            src = directory / fn
            jobs.append(SceneJob(
                src, src, None, dest / f'{stem}.csv',
                constant_delay_ms=float(delay_ms),
                steady_cap_token=steady_tok,
                comment=desc,
            ))
        return jobs

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

    # Single-file Mahimahi traces (e.g. Soheil-ab/Cellular-Traces-NYC *.trace).
    for trace in sorted(directory.glob('*.trace')):
        delay_cand = trace.with_suffix('.delay')
        delay = delay_cand if delay_cand.is_file() else None
        jobs.append(SceneJob(trace, trace, delay, dest / f'{trace.stem}.csv'))

    # OpenNetLab / AlphaRTC JSON network traces (*.json).
    for jfile in sorted(directory.glob('*.json')):
        jobs.append(SceneJob(jfile, jfile, None, dest / f'{jfile.stem}.csv'))

    if not jobs:
        raise ValueError(f'{directory}: no .up/.down pairs, *.trace, *.json, or bw_*.txt traces found')
    return jobs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('uplink', nargs='?', type=Path, help='uplink trace or dataset directory')
    parser.add_argument('downlink', nargs='?', type=Path)
    parser.add_argument('delay', nargs='?', type=Path)
    parser.add_argument('--decimals', type=int, default=CAPACITY_DECIMALS,
                        help=f'decimal places for bps/kbps/mbps/gbps capacity values (default: {CAPACITY_DECIMALS})')
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
                if args.uplink.suffix in ('.trace', '.json'):
                    args.downlink = args.uplink
                else:
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
                    if job.up.suffix == '.json':
                        write_opennetlab_json_csv(job.up, output, decimals=args.decimals)
                    else:
                        write_scene_csv(
                            job.up, job.down, job.delay, output,
                            decimals=args.decimals,
                            constant_delay_ms=job.constant_delay_ms,
                            steady_cap_token=job.steady_cap_token,
                            comment=job.comment,
                        )
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
