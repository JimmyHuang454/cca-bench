#!/usr/bin/env python3
"""Unified ns-3 CC benchmark runner for Wired, 802.11ax Wi-Fi, 3GPP 4G Lines, and Mahimahi/5G/LEO traces."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import math
import random
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
BENCH_DIR = Path(__file__).resolve().parent
if str(BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(BENCH_DIR))

import run_mahimahi_suite as rms  # noqa: E402

EVAL_FIELDNAMES = [
    "stack",
    "workload",
    "trace",
    "queue_bytes",
    "cc",
    "repeat",
    "duration_s",
    "capacity_mbps",
    "throughput_mbps",
    "utilization",
    "qdelay_avg_ms",
    "qdelay_p95_ms",
    "base_rtt_ms",
    "est_rtt_ms",
    "util_power",
    "norm_power",
    "tput_power",
    "drop_rate",
    "requests",
    "rct_mean_ms",
    "rct_p50_ms",
    "rct_p95_ms",
    "retrans_pkts",
    "retrans_ratio",
    "fct_slowdown",
    "mortise_qoe",
]

LINES_4G_QUICK = ["walk", "city-drive", "city-subway", "hsr"]
LINES_4G_CITY = ["city-drive", "city-bus", "city-subway", "city-elevated", "city-mall", "city-stadium"]
LINES_4G_RURAL = ["rural-road", "rural-mountain", "rural-village", "rural-edge"]
LINES_4G_FAST = ["highway", "hsr"]
LINES_4G_ALL = ["walk", "run"] + LINES_4G_CITY + LINES_4G_RURAL + LINES_4G_FAST


def default_workload_map() -> dict[str, Path]:
    wk_dir = REPO_ROOT / "datasets" / "workloads"
    return {
        "app": wk_dir / "app.wk",
        "websearch": wk_dir / "websearch.wk",
        "vl2": wk_dir / "vl2.wk",
        "pageload": wk_dir / "pageload.wk",
    }


def parse_workload_map(raw: str) -> dict[str, Path]:
    out = default_workload_map()
    for item in raw.split(","):
        item = item.strip()
        if not item or "=" not in item:
            continue
        k, v = item.split("=", 1)
        out[k.strip()] = Path(v.strip())
    return out


def parse_queue_bytes(tok: str) -> str:
    s = tok.strip().upper()
    if not s:
        return ""
    if s in ("RLC", "WIFI"):
        return s
    if s.isdigit():
        return s
    for suffix, mult in (("GB", 1_000_000_000), ("G", 1_000_000_000),
                         ("MB", 1_000_000), ("M", 1_000_000),
                         ("KB", 1_000), ("K", 1_000), ("B", 1)):
        if s.endswith(suffix):
            num = s[: -len(suffix)].strip()
            return str(int(round(float(num) * mult)))
    raise ValueError(f"Invalid queue size {tok!r}; expected e.g. 150KB, 2MB, or bytes")


def format_queue_size(raw: str | int) -> str:
    s = str(raw).strip()
    if not s or s.upper() in ("RLC", "WIFI"):
        return s.upper() if s else "-"
    try:
        n = int(float(s))
    except (TypeError, ValueError):
        return s
    if n >= 1_000_000:
        return f"{n / 1_000_000.0:g}MB"
    if n >= 1_000:
        return f"{n / 1_000.0:g}KB"
    return f"{n}B"


def format_duration(secs: float) -> str:
    if secs < 0 or secs != secs:
        return "--"
    s = int(round(secs))
    if s < 60:
        return f"{s}s"
    m, s = divmod(s, 60)
    if m < 60:
        return f"{m}m{s:02d}s"
    h, m = divmod(m, 60)
    return f"{h}h{m:02d}m"


def expand_shards(shard: str) -> list[str]:
    lumos_5g = [f"lumos-5g-s{i}-of-16" for i in range(1, 17)]
    lumos_4g = [f"lumos-4g-s{i}-of-18" for i in range(1, 19)]
    base = [
        "lte", "nyc-stat", "nyc-walk-1", "nyc-walk-2", "nyc-walk-3",
        "nyc-drive-1", "nyc-drive-2", "nyc-subway", "synth", "leo",
    ]
    composite = {
        "4g": ["lte"],
        "5g": ["lumos-5g"],
        "full": base + lumos_5g + lumos_4g,
        "lumos-5g-all": lumos_5g,
        "lumos-4g-all": lumos_4g,
        "lumos-all": lumos_5g + lumos_4g,
        "datasets": ["nyc-subway", "lumos-5g", "leo"],
        "rct-datasets": ["lte", "nyc-rct", "lumos-rct", "leo"],
    }
    out: list[str] = []
    seen: set[str] = set()
    for part in shard.split(","):
        part = part.strip()
        if not part:
            continue
        for item in composite.get(part, [part]):
            if item not in seen:
                seen.add(item)
                out.append(item)
    return out


def default_queues_for_trace(trace_name: str, shard: str) -> list[str]:
    if shard in ("leo", "local-leo") or trace_name.startswith("leo-") or trace_name.startswith("local-leo-"):
        return ["2000000", "75000000"]
    if "5g" in trace_name.lower() or shard.startswith("lumos-5g") or shard == "lumos-rct":
        return ["2000000", "16000000"]
    return ["150000", "2000000"]


def load_completed_keys(csv_path: Path) -> set[tuple[str, str, str, str, str]]:
    keys: set[tuple[str, str, str, str, str]] = set()
    if not csv_path.is_file():
        return keys
    with csv_path.open("r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            k = (
                row.get("trace", ""),
                row.get("queue_bytes", ""),
                row.get("workload", ""),
                row.get("cc", ""),
                row.get("repeat", ""),
            )
            if all(k):
                keys.add(k)
    return keys


def load_cap_cache(path: Path) -> dict[str, dict[str, float]]:
    if path.is_file():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            return {}
    return {}


def save_cap_cache(path: Path, cache: dict[str, dict[str, float]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cache, indent=2) + "\n", encoding="utf-8")


def parse_json_stdout(stdout: str) -> dict[str, Any] | None:
    for line in reversed(stdout.splitlines()):
        s = line.strip()
        if s.startswith("{") and s.endswith("}"):
            return json.loads(s)
    return None


def resolve_4g_line_file(json_path: Path, cache_dir: Path) -> Path:
    """Export a 4G line JSON into the resolved .line format consumed by line3d.h (cached)."""
    line_dir = cache_dir / "lines"
    line_dir.mkdir(parents=True, exist_ok=True)
    out_line = line_dir / f"{json_path.stem}.line"
    if out_line.is_file() and out_line.stat().st_mtime >= json_path.stat().st_mtime:
        return out_line
    preview_py = REPO_ROOT / "ns3" / "ns3-overlay" / "scratch" / "preview-line.py"
    subprocess.run(
        [sys.executable, str(preview_py), "--no-plot", "--export", str(out_line), str(json_path)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    return out_line


def probe_phy_capacity(kind: str, bin_path: Path, spec_path: Path, spec: dict[str, Any]) -> dict[str, float]:
    if kind == "4g":
        cmd = [
            str(bin_path),
            f"--scenario={spec_path}",
            "--capacityProbe=true",
            "--evalJson=1",
            "--RngRun=1",
        ]
    else:
        cmd = [
            str(bin_path),
            f"--distanceM={spec.get('distance_m', 5.0)}",
            f"--wallLossDb={spec.get('wall_loss_db', 0.0)}",
            f"--channelWidthMhz={spec.get('channel_width_mhz', 80)}",
            f"--bgStas={spec.get('bg_stas', 0)}",
            f"--bgMbpsEach={spec.get('bg_mbps_each', 0.0)}",
            f"--serverDelayMs={spec.get('server_delay_ms', 10.0)}",
            f"--durS={min(float(spec.get('duration_s', 40.0)), 20.0):.1f}",
            "--warmupS=2.0",
            "--capacityProbe=true",
            "--seed=1",
            "--run=1",
        ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        err = (res.stderr or res.stdout or "").strip().splitlines()[-3:]
        raise RuntimeError(f"Capacity probe failed for {spec_path.name}: {' | '.join(err)}")
    data = parse_json_stdout(res.stdout)
    if not data:
        raise RuntimeError(f"Capacity probe produced no JSON for {spec_path.name}")
    return {
        "capacity_mbps": max(float(data.get("throughput_mbps", 10.0)), 0.1),
        "base_rtt_ms": max(float(data.get("base_rtt_ms", 20.0)), 1.0),
    }


def write_basic_schedule(spec: dict[str, Any], seed: int, output: Path) -> None:
    """Render a JSON-defined basic line into the ns-3 capacity schedule."""
    tick = float(spec["tick_s"])
    sigma = float(spec["sigma"])
    access_delay = float(spec.get("access_delay_ms", 5))
    if tick <= 0 or sigma < 0:
        raise ValueError("basic line tick_s must be positive and sigma nonnegative")
    rng = random.Random(seed)
    t = 0.0
    with output.open("w", encoding="utf-8") as f:
        f.write(f"# {spec['name']} seed={seed}\n")
        for seg in spec["segments"]:
            duration = float(seg["duration_s"])
            count = round(duration / tick)
            rate = float(seg["bandwidth_mbps"])
            loss = float(seg["loss_rate"])
            delay = float(seg["base_rtt_ms"]) / 2 - access_delay
            if count <= 0 or abs(count * tick - duration) > 1e-6 or rate <= 0 or not 0 <= loss <= 1 or delay < 0:
                raise ValueError(f"invalid basic line segment: {seg}")
            for i in range(count):
                capacity = rate * math.exp(sigma * rng.gauss(0, 1) - sigma ** 2 / 2)
                f.write(f"{t + i * tick:.3f} {capacity:.4f} {loss:g} {delay:g}\n")
            t += duration
    if abs(t - float(spec["duration_s"])) > 1e-6:
        raise ValueError(f"{spec['name']}: segment durations do not match duration_s")


def run_one_job(job: dict[str, Any]) -> tuple[dict[str, Any] | None, str]:
    kind = job["kind"]
    if kind == "trace":
        cmd = [
            str(job["bin"]),
            f"--ccs={job['cc']}",
            "--profile=mahimahi",
            f"--downTrace={job['down_trace']}",
            f"--baseRttMs={job['base_rtt_ms']:.3f}",
            f"--queueBytes={job['queue_bytes']}",
            f"--stopTime={job['dur_s']:.1f}",
            f"--warmup={job['warmup_s']:.1f}",
            f"--clockGranularityMs={job['clock_ms']:.3f}",
            f"--copaClockUs={job['copa_clock_us']}",
            f"--jitterUs={job['jitter_us']}",
            "--evalJson=1",
        ]
        if job.get("up_trace"):
            cmd.append(f"--upTrace={job['up_trace']}")
        if job.get("delay_trace"):
            cmd.append(f"--delayTrace={job['delay_trace']}")
        if job.get("workload_file"):
            cmd.append(f"--workloadFile={job['workload_file']}")
    elif kind == "wired" or kind == "basic":
        spec = job["spec"]
        cmd = [
            str(job["bin"]),
            f"--ccs={job['cc']}",
            f"--profile={spec.get('profile', 'outage')}",
            f"--capacity={spec.get('bandwidth_mbps', 100.0)}",
            f"--outageDuration={spec.get('outage_duration_s', 0.0)}",
            f"--stepAt={spec.get('step_at_s', 20.0)}",
            f"--stepDuration={spec.get('step_duration_s', 15.0)}",
            f"--stepFactor={spec.get('step_factor', 0.25)}",
            f"--randomLoss={spec.get('loss_rate', 0.0)}",
            f"--aqm={spec.get('aqm', 'none')}",
            f"--baseRttMs={spec.get('base_rtt_ms', 20.0)}",
            f"--queueBytes={job['queue_bytes']}",
            f"--stopTime={job['dur_s']:.1f}",
            f"--warmup={job['warmup_s']:.1f}",
            f"--clockGranularityMs={job['clock_ms']:.3f}",
            f"--copaClockUs={job['copa_clock_us']}",
            f"--jitterUs={job['jitter_us']}",
            f"--run={job['repeat']}",
            "--evalJson=1",
        ]
        if kind == "basic":
            cmd.extend(["--profile=file", f"--traceFile={job['schedule']}"])
        if job.get("workload_file"):
            cmd.append(f"--workloadFile={job['workload_file']}")
    elif kind == "4g_line":
        cmd = [
            str(job["bin"]),
            f"--cc={job['cc']}",
            f"--scenario={job['scenario_file']}",
            f"--capacityMbps={job['capacity_mbps']:.4f}",
            f"--baseRttMs={job['base_rtt_ms']:.3f}",
            f"--clockGranularityMs={job['clock_ms']:.3f}",
            f"--copaClockUs={job['copa_clock_us']}",
            f"--jitterUs={job['jitter_us']}",
            f"--RngRun={job['repeat']}",
            "--evalJson=1",
        ]
        if job.get("workload_file"):
            cmd.append(f"--workloadFile={job['workload_file']}")
    elif kind == "wifi":
        spec = job["spec"]
        cmd = [
            str(job["bin"]),
            f"--cc={job['cc']}",
            f"--distanceM={spec.get('distance_m', 5.0)}",
            f"--wallLossDb={spec.get('wall_loss_db', 0.0)}",
            f"--channelWidthMhz={spec.get('channel_width_mhz', 80)}",
            f"--bgStas={spec.get('bg_stas', 0)}",
            f"--bgMbpsEach={spec.get('bg_mbps_each', 0.0)}",
            f"--serverDelayMs={spec.get('server_delay_ms', 10.0)}",
            f"--durS={job['dur_s']:.1f}",
            f"--warmupS={job['warmup_s']:.1f}",
            f"--capacityMbps={job['capacity_mbps']:.4f}",
            f"--baseRttMs={job['base_rtt_ms']:.3f}",
            f"--clockGranularityMs={job['clock_ms']:.3f}",
            f"--copaClockUs={job['copa_clock_us']}",
            f"--jitterUs={job['jitter_us']}",
            f"--run={job['repeat']}",
        ]
        if job.get("workload_file"):
            cmd.append(f"--workloadFile={job['workload_file']}")
    else:
        return None, f"[FAIL] unknown job kind {kind}"

    if job["cc"] in ("copa_auto_v2", "copa-auto-v2"):
        cmd.append(f"--ns3::TcpCopaAutoV2::MeasurementRtts={job['v2_measurement_rtts']}")

    t0 = time.monotonic()
    res = subprocess.run(cmd, capture_output=True, text=True)
    dt = time.monotonic() - t0
    tag = f"{job['trace']} q={format_queue_size(job['queue_bytes'])} {job['workload']} {job['cc']} r{job['repeat']}"
    if res.returncode != 0:
        err = (res.stderr or res.stdout or "").strip().splitlines()[-3:]
        return None, f"[FAIL {dt:.1f}s] {tag}: {' | '.join(err)}"

    data = parse_json_stdout(res.stdout)
    if not data:
        return None, f"[FAIL {dt:.1f}s] {tag}: no JSON output"

    if job["kind"] == "basic" and job.get("expected_requests") and data.get("requests", 0) != job["expected_requests"]:
        return None, f"[FAIL {dt:.1f}s] {tag}: completed {data.get('requests', 0)}/{job['expected_requests']} requests"

    row: dict[str, Any] = {
        "stack": job["stack"],
        "workload": rms.normalize_workload_label(job["workload"]),
        "trace": job["trace"],
        "queue_bytes": str(job["queue_bytes"]),
        "cc": job["cc"],
        "repeat": str(job["repeat"]),
    }
    row.update(data)
    if job["workload"] == "bulk":
        msg = (
            f"[OK {dt:.1f}s] {tag} | Util={data.get('utilization', 0):.3f} "
            f"QD={data.get('qdelay_avg_ms', 0):.1f}ms NPow={data.get('norm_power', 0):.3f}"
        )
    else:
        msg = (
            f"[OK {dt:.1f}s] {tag} | FCT={data.get('rct_mean_ms', 0):.1f}ms "
            f"FCT-SD={data.get('fct_slowdown', 0):.2f}x QoE={data.get('mortise_qoe', 0):.2f}"
        )
    return row, msg


def main() -> int:
    parser = argparse.ArgumentParser(description="Unified ns-3 CC benchmark runner.")
    parser.add_argument("--bin", required=True, type=Path, help="Path to tcp-capacity-trace binary")
    parser.add_argument("--lte-bin", type=Path, default=None, help="Path to tcp-lte-highspeed-handover binary")
    parser.add_argument("--wifi-bin", type=Path, default=None, help="Path to tcp-wifi-bench binary")
    parser.add_argument("--shard", default="synth", help="Shard(s): wired, wifi, lines, lines-quick, 4g, 5g, leo, datasets, full")
    parser.add_argument("--ccs", default="copa_auto,copa_auto_v2,copa_auto_nc,copa_auto_nc_j0,mortise_copa,bbr,bbr3,cubic")
    parser.add_argument("--workloads", default="bulk,websearch")
    parser.add_argument("--workload-map", default="")
    parser.add_argument("--basic-groups", default="ABCDE")
    parser.add_argument("--basic-sigmas", default="0.1,0.4")
    parser.add_argument("--queues", default="", help="Optional comma-separated queue sizes (e.g. 150KB,2MB)")
    parser.add_argument("--bulk-secs", type=int, default=60)
    parser.add_argument("--warmup-s", type=float, default=5.0)
    parser.add_argument("--bin-ms", type=float, default=5.0, help="Deprecated (mm-link gate is exact 1 ms)")
    parser.add_argument("--clock-ms", type=float, default=1.0, help="TCP ClockGranularity in ms (default 1.0)")
    parser.add_argument("--copa-clock-us", type=int, default=1, help="Copa clock quantization in us (default 1)")
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--parallel", type=int, default=0, help="0 = CPU count")
    parser.add_argument("--jitter-us", type=int, default=-1)
    parser.add_argument("--v2-measurement-rtts", type=int, choices=range(1, 257), default=16,
                        metavar="N", help="Copa-Auto v2 measurement period in SRTTs (1..256)")
    parser.add_argument("--trace-dir", type=Path, default=REPO_ROOT / "datasets" / "traces")
    parser.add_argument("--lines-dir", type=Path, default=REPO_ROOT / "datasets" / "lines")
    parser.add_argument("--cap-cache", type=Path, default=REPO_ROOT / "results" / "cache" / "phy_cap_cache.json")
    parser.add_argument("--local-leo-dir", type=Path, default=None)
    parser.add_argument("--resume", action="store_true", help="Skip jobs already present in out_dir/shard_eval.csv")
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args()

    if not args.bin.is_file():
        print(f"ERROR: tcp-capacity-trace binary not found: {args.bin}", file=sys.stderr)
        return 1

    lte_bin = args.lte_bin or (args.bin.parent / "ns3-dev-tcp-lte-highspeed-handover-optimized")
    wifi_bin = args.wifi_bin or (args.bin.parent / "ns3-dev-tcp-wifi-bench-optimized")

    args.trace_dir.mkdir(parents=True, exist_ok=True)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    sub_shards = expand_shards(args.shard)
    ccs = [c.strip() for c in args.ccs.split(",") if c.strip()]
    workloads = [w.strip() for w in args.workloads.split(",") if w.strip()]
    wk_map = parse_workload_map(args.workload_map)
    override_queues = [parse_queue_bytes(q) for q in args.queues.split(",") if q.strip()]
    repeats = max(args.repeats, 1)

    out_csv = args.out_dir / "shard_eval.csv"
    completed_keys = load_completed_keys(out_csv) if args.resume else set()
    cap_cache = load_cap_cache(args.cap_cache)

    for wk in workloads:
        if wk != "bulk" and (wk not in wk_map or not wk_map[wk].is_file()):
            parser.error(f"workload file not found for {wk}: {wk_map.get(wk)}")

    # Separate line-based shards from Mahimahi trace shards
    scenario_entries: list[dict[str, Any]] = []
    seen_names: set[str] = set()

    for sh in sub_shards:
        if sh == "basic":
            groups = set(args.basic_groups.upper())
            sigmas = {float(x) for x in args.basic_sigmas.split(",") if x.strip()}
            if not groups or not sigmas:
                parser.error("basic groups and sigmas must not be empty")
            for p in sorted((args.lines_dir / "basic").glob("*.json")):
                spec = json.loads(p.read_text(encoding="utf-8"))
                if str(spec["group"]).upper() not in groups or float(spec["sigma"]) not in sigmas:
                    continue
                name = spec.get("name", p.stem)
                if name in seen_names:
                    raise ValueError(f"duplicate scenario name: {name}")
                seen_names.add(name)
                qs = override_queues or [parse_queue_bytes(q) for q in spec.get("queues", ["2MB"])]
                scenario_entries.append({"kind": "basic", "stack": "ns3", "name": name, "spec": spec, "queues": qs})
            if not any(e["kind"] == "basic" for e in scenario_entries):
                parser.error(f"no basic JSON scenarios match in {args.lines_dir / 'basic'}")
        elif sh == "wired":
            for p in sorted((args.lines_dir / "wired").glob("*.json")):
                spec = json.loads(p.read_text(encoding="utf-8"))
                name = spec.get("name", p.stem)
                if name in seen_names:
                    continue
                seen_names.add(name)
                qs = override_queues or [parse_queue_bytes(q) for q in spec.get("queues", ["150KB", "2MB"])]
                scenario_entries.append({"kind": "wired", "stack": "ns3-wired", "name": name, "spec": spec, "queues": qs})
        elif sh == "wifi":
            if not wifi_bin.is_file():
                print(f"ERROR: tcp-wifi-bench binary not found: {wifi_bin} (run `make setup-bench`)", file=sys.stderr)
                return 1
            for p in sorted((args.lines_dir / "wifi").glob("*.json")):
                spec = json.loads(p.read_text(encoding="utf-8"))
                name = spec.get("name", p.stem)
                if name in seen_names:
                    continue
                seen_names.add(name)
                scenario_entries.append({"kind": "wifi", "stack": "ns3-wifi", "name": name, "path": p, "spec": spec, "queues": ["WIFI"]})
        elif sh in ("lines", "lines-quick", "lines-city", "lines-rural", "lines-fast"):
            if not lte_bin.is_file():
                print(f"ERROR: tcp-lte-highspeed-handover binary not found: {lte_bin} (run `make setup-bench`)", file=sys.stderr)
                return 1
            pick = {
                "lines": LINES_4G_ALL,
                "lines-quick": LINES_4G_QUICK,
                "lines-city": LINES_4G_CITY,
                "lines-rural": LINES_4G_RURAL,
                "lines-fast": LINES_4G_FAST,
            }[sh]
            for stem in pick:
                p = args.lines_dir / "4g" / f"{stem}.json"
                if not p.is_file():
                    continue
                name = f"line-{stem}"
                if name in seen_names:
                    continue
                seen_names.add(name)
                resolved_line = resolve_4g_line_file(p, args.cap_cache.parent)
                scenario_entries.append({"kind": "4g_line", "stack": "ns3-4g-phy", "name": name, "path": resolved_line, "spec": json.loads(p.read_text(encoding="utf-8")), "queues": ["RLC"]})
        else:
            for tr in rms.resolve_shard_traces(sh, args.trace_dir, args.local_leo_dir):
                if tr in seen_names:
                    continue
                seen_names.add(tr)
                qs = override_queues or default_queues_for_trace(tr, sh)
                scenario_entries.append({"kind": "trace", "stack": "ns3", "name": tr, "shard": sh, "queues": qs})

    # Pre-probe any uncached 4G/Wi-Fi PHY scenarios in parallel
    need_probe = [e for e in scenario_entries if e["kind"] in ("4g_line", "wifi") and e["name"] not in cap_cache]
    workers = args.parallel if args.parallel > 0 else max(os.cpu_count() or 2, 1)
    if need_probe:
        print(f"Probing UDP channel capacity for {len(need_probe)} PHY scenario(s) (cached in {args.cap_cache})...", flush=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(workers, len(need_probe))) as pex:
            fmap = {
                pex.submit(
                    probe_phy_capacity,
                    "4g" if e["kind"] == "4g_line" else "wifi",
                    lte_bin if e["kind"] == "4g_line" else wifi_bin,
                    e["path"],
                    e["spec"],
                ): e
                for e in need_probe
            }
            for fut in concurrent.futures.as_completed(fmap):
                e = fmap[fut]
                info = fut.result()
                cap_cache[e["name"]] = info
                save_cap_cache(args.cap_cache, cap_cache)
                print(
                    f"  [probe] {e['name']}: cap={info['capacity_mbps']:.2f} Mbps, base_rtt={info['base_rtt_ms']:.1f} ms",
                    flush=True,
                )

    total_jobs = 0
    matched_resumed = 0
    for e in scenario_entries:
        total_jobs += len(e["queues"]) * len(workloads) * len(ccs) * repeats
        if completed_keys:
            for rep in range(1, repeats + 1):
                for q in e["queues"]:
                    for wk in workloads:
                        wk_norm = rms.normalize_workload_label(wk)
                        for cc in ccs:
                            if (e["name"], str(q), wk_norm, cc, str(rep)) in completed_keys:
                                matched_resumed += 1

    print(
        f"ns-3 suite: shard={args.shard} ({len(sub_shards)} sub-shards) scenarios={len(scenario_entries)} "
        f"ccs={ccs} workloads={workloads} repeats={repeats} clock_ms={args.clock_ms} copa_clock_us={args.copa_clock_us} v2_measurement_rtts={args.v2_measurement_rtts}",
        flush=True,
    )
    resume_note = f" ({matched_resumed} cached via --resume)" if matched_resumed else ""
    print(
        f"Dispatching {total_jobs} ns-3 jobs{resume_note} across {workers} workers -> {out_csv}",
        flush=True,
    )

    def iter_jobs():
        for idx, e in enumerate(scenario_entries, 1):
            kind = e["kind"]
            name = e["name"]
            queues = e["queues"]

            if completed_keys:
                all_done = all(
                    (name, str(q), rms.normalize_workload_label(wk), cc, str(rep)) in completed_keys
                    for rep in range(1, repeats + 1)
                    for q in queues
                    for wk in workloads
                    for cc in ccs
                )
                if all_done:
                    continue

            if kind == "trace":
                sh = e["shard"]
                default_bulk_secs = 115 if sh in ("leo", "local-leo") and args.bulk_secs == 60 else args.bulk_secs
                if idx == 1 or idx % 20 == 0 or len(scenario_entries) <= 10:
                    print(f"[prep {idx}/{len(scenario_entries)}] {name} ({sh})...", flush=True)
                base_replay = rms.prepare_replay(name, args.trace_dir, default_bulk_secs)
                for rep in range(1, repeats + 1):
                    replay = rms.shift_replay(base_replay, args.trace_dir, rep, repeats)
                    for q in queues:
                        for wk in workloads:
                            wk_norm = rms.normalize_workload_label(wk)
                            wk_file = ""
                            dur_s = float(default_bulk_secs)
                            if wk != "bulk":
                                p = wk_map.get(wk)
                                if p is not None and p.is_file():
                                    wk_file = str(p)
                                    dur_s = max(float(default_bulk_secs), rms.workload_span_secs(p) + 20.0)
                            for cc in ccs:
                                if (name, str(q), wk_norm, cc, str(rep)) in completed_keys:
                                    continue
                                yield {
                                    "kind": "trace",
                                    "stack": e["stack"],
                                    "bin": str(args.bin),
                                    "trace": name,
                                    "down_trace": str(replay.down),
                                    "up_trace": str(replay.up) if replay.up != replay.down else "",
                                    "delay_trace": str(replay.delay) if replay.delay is not None else "",
                                    "base_rtt_ms": replay.base_rtt_ms,
                                    "queue_bytes": q,
                                    "workload": wk,
                                    "workload_file": wk_file,
                                    "dur_s": dur_s,
                                    "warmup_s": args.warmup_s,
                                    "clock_ms": args.clock_ms,
                                    "copa_clock_us": args.copa_clock_us,
                                    "v2_measurement_rtts": args.v2_measurement_rtts,
                                    "jitter_us": args.jitter_us,
                                    "cc": cc,
                                    "repeat": rep,
                                }
            elif kind in ("wired", "basic"):
                spec = e["spec"]
                base_dur = float(spec.get("duration_s", args.bulk_secs))
                for rep in range(1, repeats + 1):
                    schedule = ""
                    if kind == "basic":
                        schedule_path = args.out_dir / "schedules" / f"{name}-r{rep}.txt"
                        schedule_path.parent.mkdir(parents=True, exist_ok=True)
                        write_basic_schedule(spec, rep, schedule_path)
                        schedule = str(schedule_path.resolve())
                    for q in queues:
                        for wk in workloads:
                            wk_norm = rms.normalize_workload_label(wk)
                            wk_file = ""
                            dur_s = base_dur
                            if wk != "bulk":
                                p = wk_map.get(wk)
                                if p is not None and p.is_file():
                                    wk_file = str(p)
                                    dur_s = max(base_dur, rms.workload_span_secs(p) + 20.0)
                            for cc in ccs:
                                if (name, str(q), wk_norm, cc, str(rep)) in completed_keys:
                                    continue
                                yield {
                                    "kind": kind,
                                    "schedule": schedule,
                                    "expected_requests": sum(1 for line in Path(wk_file).read_text().splitlines() if line.strip() and not line.lstrip().startswith("#")) if kind == "basic" and wk_file else 0,
                                    "stack": e["stack"],
                                    "bin": str(args.bin),
                                    "trace": name,
                                    "spec": spec,
                                    "queue_bytes": q,
                                    "workload": wk,
                                    "workload_file": wk_file,
                                    "dur_s": dur_s,
                                    "warmup_s": args.warmup_s,
                                    "clock_ms": args.clock_ms,
                                    "copa_clock_us": args.copa_clock_us,
                                    "v2_measurement_rtts": args.v2_measurement_rtts,
                                    "jitter_us": args.jitter_us,
                                    "cc": cc,
                                    "repeat": rep,
                                }
            else:
                info = cap_cache.get(name, {"capacity_mbps": 20.0, "base_rtt_ms": 25.0})
                spec = e["spec"]
                base_dur = float(spec.get("duration_s", 40.0))
                for rep in range(1, repeats + 1):
                    for q in queues:
                        for wk in workloads:
                            wk_norm = rms.normalize_workload_label(wk)
                            wk_file = ""
                            dur_s = base_dur
                            if wk != "bulk":
                                p = wk_map.get(wk)
                                if p is not None and p.is_file():
                                    wk_file = str(p)
                                    dur_s = max(base_dur, rms.workload_span_secs(p) + 20.0)
                            for cc in ccs:
                                if (name, str(q), wk_norm, cc, str(rep)) in completed_keys:
                                    continue
                                yield {
                                    "kind": kind,
                                    "stack": e["stack"],
                                    "bin": str(lte_bin if kind == "4g_line" else wifi_bin),
                                    "trace": name,
                                    "scenario_file": str(e["path"]),
                                    "spec": spec,
                                    "capacity_mbps": info["capacity_mbps"],
                                    "base_rtt_ms": info["base_rtt_ms"],
                                    "queue_bytes": q,
                                    "workload": wk,
                                    "workload_file": wk_file,
                                    "dur_s": dur_s,
                                    "warmup_s": args.warmup_s,
                                    "clock_ms": args.clock_ms,
                                    "copa_clock_us": args.copa_clock_us,
                                    "v2_measurement_rtts": args.v2_measurement_rtts,
                                    "jitter_us": args.jitter_us,
                                    "cc": cc,
                                    "repeat": rep,
                                }

    write_header = not (args.resume and out_csv.is_file() and out_csv.stat().st_size > 0)
    csv_mode = "a" if (args.resume and not write_header) else "w"

    ok_new = 0
    failures = 0
    skipped = matched_resumed
    t_start = time.monotonic()
    max_in_flight = max(workers * 3, 16)
    job_iter = iter_jobs()
    exhausted = False
    pending: set[concurrent.futures.Future] = set()

    with out_csv.open(csv_mode, newline="", encoding="utf-8") as csv_f:
        writer = csv.DictWriter(csv_f, fieldnames=EVAL_FIELDNAMES, extrasaction="ignore")
        if write_header:
            writer.writeheader()
            csv_f.flush()

        with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as ex:
            while not exhausted or pending:
                while not exhausted and len(pending) < max_in_flight:
                    try:
                        job = next(job_iter)
                    except StopIteration:
                        exhausted = True
                        break
                    pending.add(ex.submit(run_one_job, job))

                if not pending:
                    break

                done_set, pending = concurrent.futures.wait(
                    pending, timeout=15.0, return_when=concurrent.futures.FIRST_COMPLETED
                )
                if not done_set:
                    done_total = skipped + ok_new + failures
                    elapsed = time.monotonic() - t_start
                    print(
                        f"  [running {done_total}/{total_jobs} | {format_duration(elapsed)} elapsed | "
                        f"{len(pending)} active]",
                        flush=True,
                    )
                    continue

                for fut in done_set:
                    row, msg = fut.result()
                    if row is not None:
                        writer.writerow(row)
                        csv_f.flush()
                        ok_new += 1
                    else:
                        failures += 1

                    done_total = skipped + ok_new + failures
                    pct = (100.0 * done_total / total_jobs) if total_jobs > 0 else 100.0
                    elapsed = time.monotonic() - t_start
                    ran_now = ok_new + failures
                    rem = max(total_jobs - done_total, 0)
                    eta = (elapsed / ran_now * rem) if ran_now > 0 else -1.0
                    prefix = (
                        f"[{done_total:>{len(str(total_jobs))}}/{total_jobs} {pct:5.1f}% | "
                        f"{format_duration(elapsed)} | ETA {format_duration(eta)}]"
                    )
                    print(f"{prefix} {msg}", flush=True)

    elapsed = time.monotonic() - t_start
    total_ok = skipped + ok_new
    print(
        f"Completed {total_ok}/{total_jobs} jobs ({ok_new} new, {skipped} resumed, {failures} failed) "
        f"in {format_duration(elapsed)} -> {out_csv}",
        flush=True,
    )
    return 1 if failures > 0 and ("basic" in sub_shards or total_ok == 0) else 0


if __name__ == "__main__":
    raise SystemExit(main())
