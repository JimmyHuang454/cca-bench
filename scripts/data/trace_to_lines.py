#!/usr/bin/env python3
"""Convert Mahimahi and LeoCC traces to compact, piecewise-constant text files."""
from __future__ import annotations

import argparse
import itertools
import math
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Iterator, NamedTuple, TextIO

ROOT = Path(__file__).resolve().parents[2]

# Edit these settings, then run `python3 scripts/data/trace_to_lines.py`.
# Directory mode: convert all supported trace pairs into a matching lines folder.
DATASET_DIR: Path | None = ROOT / 'datasets/traces/lumos'

# Single-pair mode: set DATASET_DIR = None and specify these files.
UPLINK_TRACE: Path | None = None
DOWNLINK_TRACE: Path | None = None
DELAY_TRACE: Path | None = None  # Optional LeoCC delay file, used for both directions.
LUMOS_ONE_WAY_DELAY_MS = 20.0  # Assumed delay in each direction when no delay trace exists.


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


def write_capacity(path: Path, output: TextIO) -> int:
    """Write each 1 ms bin and finish at last timestamp + 1; do not loop."""
    previous_time = -1
    previous_count = None
    for raw_time, group in itertools.groupby(numbers(path, integer=True)):
        at = int(raw_time)
        if at <= previous_time:
            raise ValueError(f'{path}: timestamps must be nondecreasing')
        count = sum(1 for _ in group)
        if previous_time == -1 and at > 0:
            output.write('0,0\n')
            previous_count = 0
        elif previous_time >= 0 and at > previous_time + 1:
            output.write(f'{previous_time + 1},0\n')
            previous_count = 0
        if count != previous_count:
            output.write(f'{at},{count}\n')
        previous_count = count
        previous_time = at
    output.write(f'{previous_time + 1},0\n')
    return previous_time + 1


def write_delay(path: Path, output: TextIO) -> None:
    """Compress 10 ms LeoCC samples; the end marker is not a delay value."""
    previous = None
    count = 0
    for count, value in enumerate(numbers(path, integer=False), 1):
        if value != previous:
            output.write(f'{(count - 1) * 10},{value:g}\n')
            previous = value
    output.write(f'# end_ms={count * 10}\n')


class ConversionJob(NamedTuple):
    source: Path
    target: Path
    kind: str


def directory_jobs(directory: Path, dest: Path) -> list[ConversionJob]:
    jobs = []
    for down in sorted(directory.glob('*.down')):
        up = down.with_suffix('.up')
        name = down.stem
        scene_dir = dest / name
        lumos = name.startswith(('5g_trace_', '4g_trace_'))
        if lumos:
            # Lumos has measured downlink only; use that same schedule in both
            # directions as an explicit symmetric approximation.
            jobs.extend([ConversionJob(down, scene_dir / 'down.txt', 'capacity'),
                         ConversionJob(down, scene_dir / 'up.txt', 'copy')])
        elif up.is_file():
            jobs.extend([ConversionJob(up, scene_dir / 'up.txt', 'capacity'),
                         ConversionJob(down, scene_dir / 'down.txt', 'capacity')])
        else:
            raise ValueError(f'missing uplink file: {up}')
        delay = down.with_suffix('.delay')
        if delay.is_file():
            jobs.extend([ConversionJob(delay, scene_dir / 'up.delay.txt', 'delay'),
                         ConversionJob(delay, scene_dir / 'down.delay.txt', 'delay')])
        elif lumos:
            jobs.extend([ConversionJob(down, scene_dir / 'up.delay.txt', 'constant-delay'),
                         ConversionJob(down, scene_dir / 'down.delay.txt', 'constant-delay')])
    # LeoCC examples use one bandwidth trace in both directions.
    leo_files = sorted(directory.glob('bw_*.txt'))
    if (directory / 'leo-bw.txt').is_file():
        leo_files.append(directory / 'leo-bw.txt')
    for bw in leo_files:
        delay = (directory / 'leo-delay.txt' if bw.name == 'leo-bw.txt'
                 else directory / f'delay_{bw.name[len("bw_"):]}')
        if not delay.is_file():
            raise ValueError(f'missing LeoCC delay file: {delay}')
        name = bw.stem
        scene_dir = dest / name
        jobs.extend([ConversionJob(bw, scene_dir / 'up.txt', 'capacity'),
                     ConversionJob(bw, scene_dir / 'down.txt', 'capacity'),
                     ConversionJob(delay, scene_dir / 'up.delay.txt', 'delay'),
                     ConversionJob(delay, scene_dir / 'down.delay.txt', 'delay')])
    if not jobs:
        raise ValueError(f'{directory}: no .up/.down pairs or bw_*.txt traces found')
    return jobs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('uplink', nargs='?', type=Path, help='uplink trace or dataset directory')
    parser.add_argument('downlink', nargs='?', type=Path)
    parser.add_argument('delay', nargs='?', type=Path)
    args = parser.parse_args()
    created = []
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
            dest = ROOT / 'datasets' / 'lines' / 'converted' / name
            jobs = [ConversionJob(args.uplink, dest / 'up.txt', 'capacity'),
                    ConversionJob(args.downlink, dest / 'down.txt', 'capacity')]
            if args.delay is not None:
                jobs.extend([ConversionJob(args.delay, dest / 'up.delay.txt', 'delay'),
                             ConversionJob(args.delay, dest / 'down.delay.txt', 'delay')])
        targets = [job.target for job in jobs]
        if len(set(targets)) != len(targets):
            raise ValueError('multiple inputs map to the same output filename')
        for target in targets:
            if target.exists():
                raise ValueError(f'output already exists: {target}')
            legacy = target.parent / f'{target.parent.name}.{target.name}'
            if legacy.exists():
                raise ValueError(f'legacy output already exists: {legacy}')
        dest.mkdir(parents=True, exist_ok=True)
        # Convert and validate the entire batch before publishing any files.
        with tempfile.TemporaryDirectory(prefix='.trace-to-lines-', dir=dest) as temp:
            staged = []
            converted = {}
            durations = {}
            for index, job in enumerate(jobs):
                tmp = Path(temp) / str(index)
                if job.kind == 'copy':
                    shutil.copyfile(converted[job.source], tmp)
                else:
                    with tmp.open('w', encoding='utf-8') as output:
                        if job.kind == 'delay':
                            write_delay(job.source, output)
                        elif job.kind == 'constant-delay':
                            if not math.isfinite(LUMOS_ONE_WAY_DELAY_MS) or LUMOS_ONE_WAY_DELAY_MS < 0:
                                raise ValueError('LUMOS_ONE_WAY_DELAY_MS must be finite and nonnegative')
                            output.write(f'0,{LUMOS_ONE_WAY_DELAY_MS:g}\n# end_ms={durations[job.source]}\n')
                        else:
                            durations[job.source] = write_capacity(job.source, output)
                            converted[job.source] = tmp
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
