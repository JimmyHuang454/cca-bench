#!/usr/bin/env python3
"""Convert Mahimahi and LeoCC traces to compact, piecewise-constant text files."""
from __future__ import annotations

import argparse
import itertools
import math
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Iterator, TextIO

ROOT = Path(__file__).resolve().parents[2]


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


def write_capacity(path: Path, output: TextIO) -> None:
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


def write_delay(path: Path, output: TextIO) -> None:
    """Compress 10 ms LeoCC samples; the end marker is not a delay value."""
    previous = None
    count = 0
    for count, value in enumerate(numbers(path, integer=False), 1):
        if value != previous:
            output.write(f'{(count - 1) * 10},{value:g}\n')
            previous = value
    output.write(f'# end_ms={count * 10}\n')


def directory_jobs(directory: Path, dest: Path) -> list[tuple[Path, Path, bool]]:
    jobs = []
    for down in sorted(directory.glob('*.down')):
        up = down.with_suffix('.up')
        if not up.is_file():
            raise ValueError(f'missing uplink file: {up}')
        name = down.stem
        jobs.extend([(up, dest / f'{name}.up.txt', False),
                     (down, dest / f'{name}.down.txt', False)])
        delay = down.with_suffix('.delay')
        if delay.is_file():
            jobs.extend([(delay, dest / f'{name}.up.delay.txt', True),
                         (delay, dest / f'{name}.down.delay.txt', True)])
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
        jobs.extend([(bw, dest / f'{name}.up.txt', False),
                     (bw, dest / f'{name}.down.txt', False),
                     (delay, dest / f'{name}.up.delay.txt', True),
                     (delay, dest / f'{name}.down.delay.txt', True)])
    if not jobs:
        raise ValueError(f'{directory}: no .up/.down pairs or bw_*.txt traces found')
    return jobs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('uplink', type=Path, help='uplink trace or dataset directory')
    parser.add_argument('downlink', nargs='?', type=Path)
    parser.add_argument('delay', nargs='?', type=Path)
    args = parser.parse_args()
    created = []
    try:
        if args.uplink.is_dir():
            if args.downlink or args.delay:
                raise ValueError('directory mode accepts only one argument')
            dest = ROOT / 'datasets' / 'lines' / args.uplink.resolve().name
            jobs = directory_jobs(args.uplink, dest)
        else:
            if args.downlink is None:
                raise ValueError('downlink trace is required')
            dest = ROOT / 'datasets' / 'lines' / 'converted'
            name = args.downlink.stem
            jobs = [(args.uplink, dest / f'{name}.up.txt', False),
                    (args.downlink, dest / f'{name}.down.txt', False)]
            if args.delay is not None:
                jobs.extend([(args.delay, dest / f'{name}.up.delay.txt', True),
                             (args.delay, dest / f'{name}.down.delay.txt', True)])
        targets = [target for _, target, _ in jobs]
        if len(set(targets)) != len(targets):
            raise ValueError('multiple inputs map to the same output filename')
        for target in targets:
            if target.exists():
                raise ValueError(f'output already exists: {target}')
        dest.mkdir(parents=True, exist_ok=True)
        # Convert and validate the entire batch before publishing any files.
        with tempfile.TemporaryDirectory(prefix='.trace-to-lines-', dir=dest) as temp:
            staged = []
            for index, (source, target, is_delay) in enumerate(jobs):
                tmp = Path(temp) / str(index)
                with tmp.open('w', encoding='utf-8') as output:
                    (write_delay if is_delay else write_capacity)(source, output)
                staged.append((tmp, target))
            for tmp, target in staged:
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
