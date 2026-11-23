#!/usr/bin/env python3
"""Shared test harness and independent reference formulas for tcp-trace-bench."""
from __future__ import annotations

import bisect
import csv
import json
import math
import subprocess
import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BINARY = ROOT / 'ns3/ns-3/build/scratch/tcp-trace-bench/ns3-dev-tcp-trace-bench-optimized'
if str(ROOT / 'scripts/bench') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts/bench'))
import lines_common  # noqa: E402


@dataclass(frozen=True)
class LineScenario:
    """Path to a generated unified CSV lines scenario."""

    line: Path


@dataclass(frozen=True)
class FullBenchOutput:
    """Structured outputs from an --evalJson=0 tcp-trace-bench run."""

    series: list[dict[str, float]]
    rtts: list[tuple[float, float]]
    qdelays: list[tuple[float, float]]
    metrics: dict


# ---------------------------------------------------------------------------
# Independent Python reference implementations of tcp-trace-bench formulas
# ---------------------------------------------------------------------------


def parse_line_points(text: str, *, is_capacity: bool) -> list[tuple[int, float]]:
    """Parse lines-format text into (ms, value) points (pkts/ms for capacity, ms for delay)."""
    points: list[tuple[int, float]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        ms_s, val_s = line.split(',', 1)
        ms = int(ms_s.strip())
        val_s = val_s.strip()
        lower = val_s.lower()
        if is_capacity:
            if val_s == '0':
                val = 0.0
            elif lower.endswith('gbps'):
                val = float(val_s[:-4]) * 1000.0 / 12.0
            elif lower.endswith('mbps'):
                val = float(val_s[:-4]) / 12.0
            elif lower.endswith('kbps'):
                val = float(val_s[:-4]) / 12000.0
            elif lower.endswith('bps'):
                val = float(val_s[:-3]) / 12000000.0
            else:
                raise ValueError(f'unsupported capacity unit: {val_s!r}')
        else:
            val = float(val_s)
        points.append((ms, val))
    return points


def line_value(points: list[tuple[int, float]], ms: int) -> float:
    """Step-function lookup matching line-model.cc::LineValue."""
    times = [p[0] for p in points]
    idx = bisect.bisect_right(times, ms)
    return 0.0 if idx == 0 else points[idx - 1][1]


def periodic_line_value(points: list[tuple[int, float]], ms: int, period_ms: int = 0) -> float:
    """Periodic step-function lookup matching line-model.cc::PeriodicLineValue."""
    t = (ms % period_ms) if period_ms > 0 else ms
    return line_value(points, t)


def line_integral_raw(points: list[tuple[int, float]], end_ms: float, period_ms: int = 0) -> float:
    """Exact piecewise-constant integral matching line-model.cc::LineIntegral."""
    if end_ms <= 0.0:
        return 0.0

    def integrate(limit_ms: float) -> float:
        total = 0.0
        for i, (start_ms, val) in enumerate(points):
            if start_ms >= limit_ms:
                break
            next_ms = points[i + 1][0] if i + 1 < len(points) else limit_ms
            total += (min(next_ms, limit_ms) - start_ms) * val
        return total

    if period_ms <= 0:
        return integrate(end_ms)
    cycles = math.floor(end_ms / period_ms)
    return cycles * integrate(float(period_ms)) + integrate(end_ms - cycles * period_ms)


def line_integral_mbps(points: list[tuple[int, float]], end_ms: float, period_ms: int = 0) -> float:
    """Exact average capacity in Mbps matching line-model.cc::LineIntegral * 12 / endMs."""
    if end_ms <= 0.0:
        return 0.0
    return line_integral_raw(points, end_ms, period_ms) * 12.0 / end_ms


def line_time_average(points: list[tuple[int, float]], end_ms: float, period_ms: int = 0) -> float:
    """Exact time-weighted average of a delay line over [0, end_ms)."""
    if end_ms <= 0.0:
        return 0.0
    return line_integral_raw(points, end_ms, period_ms) / end_ms


def nearest_rank_percentile(values: list[float], p: float) -> float:
    """Nearest-rank percentile matching tcp-trace-bench.cc::Percentile."""
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = int(math.ceil(p / 100.0 * len(ordered)))
    idx = min(max(idx, 1), len(ordered)) - 1
    return ordered[idx]


def recompute_power_from_qdelays(
    qdelays: list[tuple[float, float]],
    *,
    up_delay_pts: list[tuple[int, float]],
    down_delay_pts: list[tuple[int, float]],
    duration_ms: float,
    period_ms: int,
    capacity_mbps: float,
    throughput_mbps: float,
    tail_percentile: float = 95.0,
) -> dict[str, float]:
    """Independently recompute time-weighted base RTT, per-packet qdelay stats, and power metrics."""
    eff_base = (
        line_time_average(up_delay_pts, duration_ms, period_ms)
        + line_time_average(down_delay_pts, duration_ms, period_ms)
    )
    qd_vals = [q for _, q in qdelays]
    qd_avg = (sum(qd_vals) / len(qd_vals)) if qd_vals else 0.0
    qd_p95 = nearest_rank_percentile(qd_vals, 95.0)
    qd_tail = nearest_rank_percentile(qd_vals, tail_percentile)
    util = (throughput_mbps / capacity_mbps) if capacity_mbps > 1e-6 else 0.0
    est_rtt_ms = eff_base + qd_avg
    est_rtt_p95_ms = eff_base + qd_p95
    est_rtt_tail_ms = eff_base + qd_tail
    util_power = (util / (est_rtt_ms / 1000.0)) if est_rtt_ms > 1e-6 else 0.0
    norm_power = util_power * (eff_base / 1000.0)
    norm_power_p95 = (util * eff_base / est_rtt_p95_ms) if est_rtt_p95_ms > 1e-6 else 0.0
    norm_power_tail = (util * eff_base / est_rtt_tail_ms) if est_rtt_tail_ms > 1e-6 else 0.0
    tput_power = (throughput_mbps / (est_rtt_ms / 1000.0)) if est_rtt_ms > 1e-6 else 0.0
    return {
        'base_rtt_ms': eff_base,
        'qdelay_avg_ms': qd_avg,
        'qdelay_p95_ms': qd_p95,
        'qdelay_tail_ms': qd_tail,
        'est_rtt_ms': est_rtt_ms,
        'utilization': util,
        'util_power': util_power,
        'norm_power': norm_power,
        'norm_power_p95': norm_power_p95,
        'norm_power_tail': norm_power_tail,
        'tput_power': tput_power,
    }


def recompute_rtt_percentiles(rtts: list[tuple[float, float]]) -> dict[str, float]:
    """Independently recompute raw ACK RTT mean and percentiles for -metrics.json cross-check."""
    raw_rtts = [r for _, r in rtts]
    return {
        'rtt_mean_ms': (sum(raw_rtts) / len(raw_rtts)) if raw_rtts else 0.0,
        'rtt_p50_ms': nearest_rank_percentile(raw_rtts, 50.0),
        'rtt_p95_ms': nearest_rank_percentile(raw_rtts, 95.0),
        'rtt_p99_ms': nearest_rank_percentile(raw_rtts, 99.0),
    }


def compute_single_request_fct(
    *,
    duration_s: float,
    arr_sec: float,
    size_bytes: int,
    request_base_rtt_ms: float,
    capacity_mbps: float,
) -> dict[str, float]:
    """Independently compute single-request FCT, slowdown, QoE, and throughput."""
    rct_ms = max((duration_s - arr_sec) * 1000.0, 0.1)
    tx_ms = (size_bytes * 8.0 / (capacity_mbps * 1000.0)) if capacity_mbps > 1e-6 else 0.0
    ideal_ms = max(request_base_rtt_ms, 1.0) + tx_ms
    sd = max(rct_ms / max(ideal_ms, 1e-3), 1.0)
    rct_s = max(rct_ms / 1000.0, 1e-6)
    thr_mbps = (size_bytes * 8.0) / (rct_s * 1e6)
    mortise_qoe = (thr_mbps * 0.5) / max(rct_ms, 1.0) * 100.0
    throughput_mbps = (size_bytes * 8.0) / max(duration_s, 1e-3) / 1e6
    return {
        'rct_ms': rct_ms,
        'ideal_ms': ideal_ms,
        'fct_slowdown': sd,
        'mortise_qoe': mortise_qoe,
        'throughput_mbps': throughput_mbps,
    }


# ---------------------------------------------------------------------------
# Base TestCase with managed temporary directories & binary runners
# ---------------------------------------------------------------------------


class TraceBenchCase(unittest.TestCase):
    """Base test case providing clean temp workspaces and tcp-trace-bench runners."""

    @classmethod
    def setUpClass(cls) -> None:
        if not BINARY.is_file():
            raise unittest.SkipTest(f'build tcp-trace-bench first: {BINARY}')

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_dir = Path(self._tmp.name)
        self._case_seq = 0

    def subdir(self, name: str) -> Path:
        self._case_seq += 1
        d = self.tmp_dir / f'{self._case_seq:02d}_{name}'
        d.mkdir(parents=True, exist_ok=True)
        return d

    def make_lines(
        self,
        *,
        up: str,
        down: str,
        up_delay: str | None = None,
        down_delay: str | None = None,
        name: str = 'scene',
    ) -> LineScenario:
        def parse_tokens(text: str, *, is_delay: bool) -> tuple[int, list[tuple[int, str]]]:
            pts: list[tuple[int, str]] = []
            end_ms: int | None = None
            for raw in text.splitlines():
                s = raw.strip()
                if not s:
                    continue
                if s.startswith('#'):
                    if is_delay and s.startswith('# end_ms='):
                        end_ms = int(s[len('# end_ms='):])
                    continue
                t_s, v_s = s.split(',', 1)
                pts.append((int(t_s.strip()), v_s.strip()))
            if not is_delay:
                end_ms = pts[-1][0]
                pts = pts[:-1]
            assert end_ms is not None and end_ms > 0
            return end_ms, pts

        up_end, up_toks = parse_tokens(up, is_delay=False)
        dn_end, dn_toks = parse_tokens(down, is_delay=False)
        ends = [up_end, dn_end]
        ud_toks = dd_toks = None
        if up_delay is not None:
            ud_end, ud_toks = parse_tokens(up_delay, is_delay=True)
            ends.append(ud_end)
        if down_delay is not None:
            dd_end, dd_toks = parse_tokens(down_delay, is_delay=True)
            ends.append(dd_end)
        d = self.subdir(name)
        p = d / 'line.csv'
        lines_common.write_line_csv(
            p,
            up_tokens=up_toks,
            down_tokens=dn_toks,
            end_ms=min(ends),
            up_delay_tokens=ud_toks,
            down_delay_tokens=dd_toks,
        )
        return LineScenario(line=p)

    def make_line_csv(self, content: str, name: str = 'scene') -> LineScenario:
        d = self.subdir(name)
        p = d / 'line.csv'
        p.write_text(content, encoding='utf-8')
        return LineScenario(line=p)

    def make_workload(self, content: str, name: str = 'work.csv') -> Path:
        d = self.subdir('wk')
        p = d / name
        p.write_text(content, encoding='utf-8')
        return p

    def _build_cmd(
        self,
        scenario: LineScenario,
        *,
        stop_time: float,
        cc: str,
        eval_json: bool,
        prefix: Path | None = None,
        bin_ms: float = 100.0,
        default_delay_ms: float = 10.0,
        queue_bytes: int | None = None,
        workload_file: Path | None = None,
        line_period_ms: int | None = None,
        tail_percentile: float | None = None,
        rtt_sample_log: bool = False,
        segment_size: int = 1448,
    ) -> list[str]:
        cmd = [
            str(BINARY),
            '--profile=lines',
            f'--lineFile={scenario.line}',
            f'--stopTime={stop_time}',
            f'--ccs={cc}',
            f'--defaultDelayMs={default_delay_ms}',
            f'--segmentSize={segment_size}',
            f'--evalJson={1 if eval_json else 0}',
        ]
        if queue_bytes is not None:
            cmd.append(f'--queueBytes={queue_bytes}')
        if workload_file is not None:
            cmd.append(f'--workloadFile={workload_file}')
        if line_period_ms is not None:
            cmd.append(f'--linePeriodMs={line_period_ms}')
        if tail_percentile is not None:
            cmd.append(f'--tailPercentile={tail_percentile:g}')
        if not eval_json:
            assert prefix is not None
            cmd.append(f'--prefix={prefix}')
            cmd.append(f'--binMs={bin_ms}')
            if rtt_sample_log:
                cmd.append('--rttSampleLog=1')
        return cmd

    def run_json(
        self,
        scenario: LineScenario,
        *,
        stop_time: float,
        cc: str = 'cubic',
        default_delay_ms: float = 10.0,
        queue_bytes: int | None = None,
        workload_file: Path | None = None,
        line_period_ms: int | None = None,
        tail_percentile: float | None = None,
    ) -> dict:
        cmd = self._build_cmd(
            scenario,
            stop_time=stop_time,
            cc=cc,
            eval_json=True,
            default_delay_ms=default_delay_ms,
            queue_bytes=queue_bytes,
            workload_file=workload_file,
            line_period_ms=line_period_ms,
            tail_percentile=tail_percentile,
        )
        res = subprocess.run(cmd, text=True, capture_output=True, check=True)
        line = next(l for l in reversed(res.stdout.splitlines()) if l.startswith('{'))
        return json.loads(line)

    def run_full(
        self,
        scenario: LineScenario,
        *,
        stop_time: float,
        cc: str = 'cubic',
        bin_ms: float = 100.0,
        default_delay_ms: float = 10.0,
        queue_bytes: int | None = None,
        workload_file: Path | None = None,
        line_period_ms: int | None = None,
        tail_percentile: float | None = None,
        rtt_sample_log: bool = False,
        name: str = 'run',
    ) -> FullBenchOutput:
        prefix = self.subdir(name) / 'out'
        cmd = self._build_cmd(
            scenario,
            stop_time=stop_time,
            cc=cc,
            eval_json=False,
            prefix=prefix,
            bin_ms=bin_ms,
            default_delay_ms=default_delay_ms,
            queue_bytes=queue_bytes,
            workload_file=workload_file,
            line_period_ms=line_period_ms,
            tail_percentile=tail_percentile,
            rtt_sample_log=rtt_sample_log,
        )
        subprocess.run(cmd, text=True, capture_output=True, check=True)

        series: list[dict[str, float]] = []
        with Path(f'{prefix}-timeseries.csv').open(newline='', encoding='utf-8') as f:
            for row in csv.DictReader(f):
                series.append({k: float(v) for k, v in row.items()})

        rtts: list[tuple[float, float]] = []
        rtt_path = Path(f'{prefix}-rtt-samples.csv')
        if rtt_sample_log and rtt_path.is_file():
            with rtt_path.open(newline='', encoding='utf-8') as f:
                for row in csv.DictReader(f):
                    rtts.append((float(row['time_s']), float(row['rtt_ms'])))

        qdelays: list[tuple[float, float]] = []
        qd_path = Path(f'{prefix}-qdelay-samples.csv')
        if rtt_sample_log and qd_path.is_file():
            with qd_path.open(newline='', encoding='utf-8') as f:
                for row in csv.DictReader(f):
                    qdelays.append((float(row['time_s']), float(row['qdelay_ms'])))

        metrics = json.loads(Path(f'{prefix}-metrics.json').read_text(encoding='utf-8'))
        return FullBenchOutput(series=series, rtts=rtts, qdelays=qdelays, metrics=metrics)

    def run_pair(
        self,
        scenario: LineScenario,
        *,
        stop_time: float,
        cc: str = 'cubic',
        bin_ms: float = 100.0,
        default_delay_ms: float = 10.0,
        queue_bytes: int | None = None,
        workload_file: Path | None = None,
        line_period_ms: int | None = None,
        tail_percentile: float | None = None,
        name: str = 'pair',
    ) -> tuple[dict, FullBenchOutput]:
        """Run identical parameters in both --evalJson=1 and --evalJson=0 --rttSampleLog=1 modes."""
        js = self.run_json(
            scenario,
            stop_time=stop_time,
            cc=cc,
            default_delay_ms=default_delay_ms,
            queue_bytes=queue_bytes,
            workload_file=workload_file,
            line_period_ms=line_period_ms,
            tail_percentile=tail_percentile,
        )
        full = self.run_full(
            scenario,
            stop_time=stop_time,
            cc=cc,
            bin_ms=bin_ms,
            default_delay_ms=default_delay_ms,
            queue_bytes=queue_bytes,
            workload_file=workload_file,
            line_period_ms=line_period_ms,
            tail_percentile=tail_percentile,
            rtt_sample_log=True,
            name=name,
        )
        return js, full
