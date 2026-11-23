#!/usr/bin/env python3
"""Convert datasets/lines scenarios into Mahimahi (mm-link + mm-delay) replay files.

Direction mapping (critical for aligning datasets/lines with Mortise over Mahimahi):
  - In datasets/lines & ns-3 tcp-trace-bench:
      up.txt / up.delay.txt     = TCP data direction (sender -> receiver)
      down.txt / down.delay.txt = TCP ACK direction  (receiver -> sender)
  - In Mahimahi + Mortise (server outside Mahimahi -> client inside Mahimahi):
      Mahimahi downlink (mm-link arg 2, --downlink-log) = server -> client (TCP data)
      Mahimahi uplink   (mm-link arg 1)                 = client -> server (TCP ACK)
  Therefore:
      lines up.*   -> Mahimahi downlink.*
      lines down.* -> Mahimahi uplink.*
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path

_BENCH_DIR = str(Path(__file__).resolve().parents[1] / 'bench')
if _BENCH_DIR not in sys.path:
    sys.path.insert(0, _BENCH_DIR)

from lines_common import (  # noqa: E402
    DEFAULT_ONE_WAY_DELAY_MS,
    discover,
    integrate_piecewise,
    load_scenario_lines,
)

DELAY_INTERVAL_MS = 10


@dataclass(frozen=True)
class MahimahiScenario:
    name: str
    period_ms: int
    duration_s: float
    bulk_duration_s: float
    mm_uplink_trace: Path      # client -> server (ACKs, from lines/down.txt)
    mm_downlink_trace: Path    # server -> client (TCP data, from lines/up.txt)
    mm_uplink_delay: Path      # client -> server delay (from lines/down.delay.txt)
    mm_downlink_delay: Path    # server -> client delay (from lines/up.delay.txt)
    delay_interval_ms: int
    base_rtt_ms_bulk: float
    base_rtt_ms_period: float
    data_capacity_mbps_bulk: float
    data_capacity_mbps_period: float
    has_up_delay_file: bool
    has_down_delay_file: bool


def expand_capacity_to_mahimahi(
    points: list[tuple[int, float]],
    period_ms: int,
    output_path: Path,
) -> int:
    """Expand piecewise-constant capacity [(t_ms, pkts_per_ms), ...] into Mahimahi delivery ms timestamps.

    Carries fractional 1500B packet opportunity credit across milliseconds, matching
    ns-3 tcp-trace-bench link-gates.h. Returns total packet opportunities written.
    """
    if period_ms <= 0:
        raise ValueError(f'period_ms must be > 0, got {period_ms}')

    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = output_path.with_suffix(output_path.suffix + '.part')

    credit = 0.0
    written = 0
    buf: list[str] = []
    flush_threshold = 65536

    with tmp_path.open('w', encoding='utf-8') as out:
        for idx, (t_start, rate) in enumerate(points):
            if t_start >= period_ms:
                break
            t_next = points[idx + 1][0] if idx + 1 < len(points) else period_ms
            seg_end = min(t_next, period_ms)
            if seg_end <= t_start:
                continue
            if rate <= 0.0:
                continue
            int_rate = int(rate)
            is_exact_int = abs(rate - int_rate) <= 1e-12
            if is_exact_int and abs(credit) <= 1e-12:
                for ms in range(t_start, seg_end):
                    buf.append(f'{ms}\n' * int_rate)
                    written += int_rate
                    if len(buf) >= flush_threshold:
                        out.write(''.join(buf))
                        buf.clear()
            else:
                for ms in range(t_start, seg_end):
                    credit += rate
                    count = int(credit + 1e-12)
                    if count > 0:
                        credit -= count
                        if credit < 0.0:
                            credit = 0.0
                        buf.append(f'{ms}\n' * count)
                        written += count
                        if len(buf) >= flush_threshold:
                            out.write(''.join(buf))
                            buf.clear()
        if written == 0:
            # mm-link rejects empty trace files; place one sentinel opportunity far past the run.
            buf.append(f'{period_ms + 3_600_000}\n')
        if buf:
            out.write(''.join(buf))
            buf.clear()

    tmp_path.replace(output_path)
    return written


def expand_delay_to_mahimahi(
    points: list[tuple[int, float]],
    period_ms: int,
    output_path: Path,
    interval_ms: int = DELAY_INTERVAL_MS,
) -> int:
    """Expand piecewise-constant one-way delay [(t_ms, delay_ms), ...] into 10ms LeoReplayer format."""
    if period_ms <= 0 or interval_ms <= 0:
        raise ValueError('period_ms and interval_ms must be > 0')

    # Sample active segments within [0, period_ms)
    active_values: list[int] = []
    n_samples = max(1, math.ceil(period_ms / interval_ms))
    seg_idx = 0
    n_pts = len(points)
    for k in range(n_samples):
        t_ms = k * interval_ms
        while seg_idx + 1 < n_pts and points[seg_idx + 1][0] <= t_ms:
            seg_idx += 1
        val = max(0, int(round(points[seg_idx][1])))
        active_values.append(val)

    # Constant delay compresses to a single line (DelayQueue indexes modulo vector size).
    if len(set(active_values)) == 1:
        active_values = [active_values[0]]

    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = output_path.with_suffix(output_path.suffix + '.part')
    tmp_path.write_text('\n'.join(str(v) for v in active_values) + '\n', encoding='utf-8')
    tmp_path.replace(output_path)
    return len(active_values)


def convert_scenario(
    scenario: dict,
    out_dir: Path,
    *,
    default_one_way_delay_ms: float = DEFAULT_ONE_WAY_DELAY_MS,
    max_bulk_s: float = 0.0,
    delay_interval_ms: int = DELAY_INTERVAL_MS,
) -> MahimahiScenario:
    """Convert a discovered lines scenario dict into Mahimahi replay files under out_dir."""
    loaded = load_scenario_lines(scenario, default_one_way_delay_ms=default_one_way_delay_ms)
    period_ms = loaded['end_ms']
    up_pts = loaded['up']
    down_pts = loaded['down']
    up_delay_pts = loaded['up_delay']
    down_delay_pts = loaded['down_delay']
    has_up_delay = loaded['has_up_delay']
    has_down_delay = loaded['has_down_delay']

    duration_s = period_ms / 1000.0
    bulk_duration_s = min(duration_s, max_bulk_s) if max_bulk_s and max_bulk_s > 0 else duration_s
    bulk_ms = max(1, int(round(bulk_duration_s * 1000.0)))

    scene_dir = out_dir / scenario['name']
    scene_dir.mkdir(parents=True, exist_ok=True)

    # Explicit direction mapping:
    #   lines up.*   (TCP data) -> Mahimahi downlink (server -> client)
    #   lines down.* (TCP ACK)  -> Mahimahi uplink   (client -> server)
    mm_downlink_trace = scene_dir / 'mm_downlink.trace'
    mm_uplink_trace = scene_dir / 'mm_uplink.trace'
    mm_downlink_delay = scene_dir / 'mm_downlink.delay'
    mm_uplink_delay = scene_dir / 'mm_uplink.delay'

    expand_capacity_to_mahimahi(up_pts, period_ms, mm_downlink_trace)
    expand_capacity_to_mahimahi(down_pts, period_ms, mm_uplink_trace)
    expand_delay_to_mahimahi(up_delay_pts, period_ms, mm_downlink_delay, interval_ms=delay_interval_ms)
    expand_delay_to_mahimahi(down_delay_pts, period_ms, mm_uplink_delay, interval_ms=delay_interval_ms)

    base_rtt_ms_bulk = (
        integrate_piecewise(up_delay_pts, bulk_ms, period_ms)
        + integrate_piecewise(down_delay_pts, bulk_ms, period_ms)
    ) / bulk_ms
    base_rtt_ms_period = (
        integrate_piecewise(up_delay_pts, period_ms, period_ms)
        + integrate_piecewise(down_delay_pts, period_ms, period_ms)
    ) / period_ms

    # 1 packet opportunity (1500 B) per ms = 12 Mbps
    data_cap_bulk = integrate_piecewise(up_pts, bulk_ms, period_ms) * 12.0 / bulk_ms
    data_cap_period = integrate_piecewise(up_pts, period_ms, period_ms) * 12.0 / period_ms

    return MahimahiScenario(
        name=scenario['name'],
        period_ms=period_ms,
        duration_s=duration_s,
        bulk_duration_s=bulk_duration_s,
        mm_uplink_trace=mm_uplink_trace,
        mm_downlink_trace=mm_downlink_trace,
        mm_uplink_delay=mm_uplink_delay,
        mm_downlink_delay=mm_downlink_delay,
        delay_interval_ms=delay_interval_ms,
        base_rtt_ms_bulk=base_rtt_ms_bulk,
        base_rtt_ms_period=base_rtt_ms_period,
        data_capacity_mbps_bulk=data_cap_bulk,
        data_capacity_mbps_period=data_cap_period,
        has_up_delay_file=has_up_delay,
        has_down_delay_file=has_down_delay,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input_dir', type=Path, help='lines dataset directory or single scenario directory')
    parser.add_argument('output_dir', type=Path, help='output directory for converted Mahimahi traces')
    parser.add_argument('--default-delay-ms', type=float, default=DEFAULT_ONE_WAY_DELAY_MS)
    parser.add_argument('--max-bulk-s', type=float, default=0.0)
    args = parser.parse_args()

    scenarios = discover(args.input_dir)
    manifest = []
    for s in scenarios:
        conv = convert_scenario(
            s,
            args.output_dir,
            default_one_way_delay_ms=args.default_delay_ms,
            max_bulk_s=args.max_bulk_s,
        )
        manifest.append({
            'name': conv.name,
            'period_ms': conv.period_ms,
            'duration_s': conv.duration_s,
            'bulk_duration_s': conv.bulk_duration_s,
            'mm_uplink_trace': str(conv.mm_uplink_trace),
            'mm_downlink_trace': str(conv.mm_downlink_trace),
            'mm_uplink_delay': str(conv.mm_uplink_delay),
            'mm_downlink_delay': str(conv.mm_downlink_delay),
            'base_rtt_ms_bulk': conv.base_rtt_ms_bulk,
            'base_rtt_ms_period': conv.base_rtt_ms_period,
            'data_capacity_mbps_bulk': conv.data_capacity_mbps_bulk,
            'data_capacity_mbps_period': conv.data_capacity_mbps_period,
        })
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
