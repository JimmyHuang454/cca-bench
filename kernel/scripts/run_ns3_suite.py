#!/usr/bin/env python3
"""Run Copa-Auto and baseline TCP CCAs in parallel on ns-3 across Mahimahi/LeoCC traces."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

SCRIPTS_DIR = Path(__file__).resolve().parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

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
    "rct_slowdown",
    "mortise_qoe",
]


def parse_workload_map(raw: str) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for item in raw.split(","):
        item = item.strip()
        if not item or "=" not in item:
            continue
        k, v = item.split("=", 1)
        out[k.strip()] = Path(v.strip())
    return out


def expand_shards(shard: str) -> list[str]:
    lumos_5g = [f"lumos-5g-s{i}-of-16" for i in range(1, 17)]
    lumos_4g = [f"lumos-4g-s{i}-of-18" for i in range(1, 19)]
    base = [
        "lte", "nyc-stat", "nyc-walk-1", "nyc-walk-2", "nyc-walk-3",
        "nyc-drive-1", "nyc-drive-2", "nyc-subway", "synth", "leo",
    ]
    composite = {
        "full": base + lumos_5g + lumos_4g,
        "lumos-5g-all": lumos_5g,
        "lumos-4g-all": lumos_4g,
        "lumos-all": lumos_5g + lumos_4g,
        "datasets": ["nyc-subway", "lumos-5g", "leo"],
        "rct-datasets": ["lte", "nyc-rct", "lumos-rct", "leo"],
    }
    return composite.get(shard, [shard])


def default_queues_for_trace(trace_name: str, shard: str) -> list[str]:
    if shard in ("leo", "local-leo") or trace_name.startswith("leo-") or trace_name.startswith("local-leo-"):
        return ["2000000", "75000000"]
    if "5g" in trace_name.lower() or shard.startswith("lumos-5g") or shard == "lumos-rct":
        return ["2000000", "16000000"]
    return ["150000", "2000000"]


def run_one_job(job: dict[str, Any]) -> tuple[dict[str, Any] | None, str]:
    cmd = [
        str(job["bin"]),
        f"--cc={job['cc']}",
        f"--mode={'bulk' if job['workload'] == 'bulk' else 'app'}",
        f"--downTrace={job['down_trace']}",
        f"--baseRttMs={job['base_rtt_ms']:.3f}",
        f"--queueBytes={job['queue_bytes']}",
        f"--durS={job['dur_s']:.1f}",
        f"--warmupS={job['warmup_s']:.1f}",
        f"--binMs={job['bin_ms']:.3f}",
        f"--clockGranularityMs={job['clock_ms']:.3f}",
        f"--jitterUs={job['jitter_us']}",
    ]
    if job.get("up_trace"):
        cmd.append(f"--upTrace={job['up_trace']}")
    if job["delay_trace"]:
        cmd.append(f"--delayTrace={job['delay_trace']}")
    if job["workload_file"]:
        cmd.append(f"--workloadFile={job['workload_file']}")

    t0 = time.monotonic()
    res = subprocess.run(cmd, capture_output=True, text=True)
    dt = time.monotonic() - t0
    tag = f"{job['trace']} q={job['queue_bytes']} {job['workload']} {job['cc']} r{job['repeat']}"
    if res.returncode != 0:
        err = (res.stderr or res.stdout or "").strip().splitlines()[-3:]
        return None, f"[FAIL {dt:.1f}s] {tag}: {' | '.join(err)}"

    out_line = ""
    for line in reversed(res.stdout.splitlines()):
        s = line.strip()
        if s.startswith("{") and s.endswith("}"):
            out_line = s
            break
    if not out_line:
        return None, f"[FAIL {dt:.1f}s] {tag}: no JSON output"

    data = json.loads(out_line)
    row: dict[str, Any] = {
        "stack": "ns3",
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
            f"[OK {dt:.1f}s] {tag} | RCT={data.get('rct_mean_ms', 0):.1f}ms "
            f"SD={data.get('rct_slowdown', 0):.2f}x QoE={data.get('mortise_qoe', 0):.2f}"
        )
    return row, msg


def main() -> int:
    parser = argparse.ArgumentParser(description="Run ns-3 CC benchmark suite.")
    parser.add_argument("--bin", required=True, type=Path, help="Path to compiled tcp_copa_bench binary")
    parser.add_argument("--shard", default="smoke", help="Trace shard name (same as run_mahimahi_suite.py)")
    parser.add_argument("--ccs", default="copa_auto,copa_auto_nc,copa_auto_nc_j0,mortise_copa,bbr,cubic")
    parser.add_argument("--workloads", default="bulk,app")
    parser.add_argument("--workload-map", default="")
    parser.add_argument("--queues", default="", help="Optional comma-separated queue sizes in bytes")
    parser.add_argument("--bulk-secs", type=int, default=60)
    parser.add_argument("--warmup-s", type=float, default=5.0)
    parser.add_argument("--bin-ms", type=float, default=5.0, help="Downlink trace rate-bin clock in ms (default 5.0)")
    parser.add_argument("--clock-ms", type=float, default=1.0, help="TCP ClockGranularity in ms (default 1.0)")
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--parallel", type=int, default=0, help="0 = CPU count")
    parser.add_argument("--jitter-us", type=int, default=-1)
    parser.add_argument("--trace-dir", type=Path, default=Path("/tmp/ns3_traces"))
    parser.add_argument("--local-leo-dir", type=Path, default=None)
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args()

    if not args.bin.is_file():
        print(f"ERROR: ns-3 binary not found: {args.bin}", file=sys.stderr)
        return 1

    args.trace_dir.mkdir(parents=True, exist_ok=True)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    sub_shards = expand_shards(args.shard)
    ccs = [c.strip() for c in args.ccs.split(",") if c.strip()]
    workloads = [w.strip() for w in args.workloads.split(",") if w.strip()]
    wk_map = parse_workload_map(args.workload_map)
    override_queues = [q.strip() for q in args.queues.split(",") if q.strip()]

    trace_pairs: list[tuple[str, str]] = []
    seen_traces: set[str] = set()
    for sh in sub_shards:
        for tr in rms.resolve_shard_traces(sh, args.trace_dir, args.local_leo_dir):
            if tr not in seen_traces:
                seen_traces.add(tr)
                trace_pairs.append((sh, tr))

    print(
        f"ns-3 suite: shard={args.shard} ({len(sub_shards)} sub-shards) traces={len(trace_pairs)} "
        f"ccs={ccs} workloads={workloads} repeats={args.repeats} bin_ms={args.bin_ms} clock_ms={args.clock_ms}",
        flush=True,
    )

    jobs: list[dict[str, Any]] = []
    for sh, trace_name in trace_pairs:
        default_bulk_secs = 115 if sh in ("leo", "local-leo") and args.bulk_secs == 60 else args.bulk_secs
        base_replay = rms.prepare_replay(trace_name, args.trace_dir, default_bulk_secs)
        queues = override_queues or default_queues_for_trace(trace_name, sh)
        for rep in range(1, max(args.repeats, 1) + 1):
            replay = rms.shift_replay(base_replay, args.trace_dir, rep, args.repeats)
            for q in queues:
                for wk in workloads:
                    wk_file = ""
                    dur_s = float(default_bulk_secs)
                    if wk != "bulk":
                        p = wk_map.get(wk)
                        if p is not None and p.is_file():
                            wk_file = str(p)
                            dur_s = max(float(default_bulk_secs), rms.workload_span_secs(p) + 20.0)
                    for cc in ccs:
                        jobs.append({
                            "bin": str(args.bin),
                            "trace": trace_name,
                            "down_trace": str(replay.down),
                            "up_trace": str(replay.up) if replay.up != replay.down else "",
                            "delay_trace": str(replay.delay) if replay.delay is not None else "",
                            "base_rtt_ms": replay.base_rtt_ms,
                            "queue_bytes": q,
                            "workload": wk,
                            "workload_file": wk_file,
                            "dur_s": dur_s,
                            "warmup_s": args.warmup_s,
                            "bin_ms": args.bin_ms,
                            "clock_ms": args.clock_ms,
                            "jitter_us": args.jitter_us,
                            "cc": cc,
                            "repeat": rep,
                        })

    workers = args.parallel if args.parallel > 0 else max(os.cpu_count() or 2, 1)
    print(f"Dispatching {len(jobs)} ns-3 jobs across {workers} workers...", flush=True)

    rows: list[dict[str, Any]] = []
    failures = 0
    t_start = time.monotonic()
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as ex:
        for row, msg in ex.map(run_one_job, jobs):
            print(msg, flush=True)
            if row is not None:
                rows.append(row)
            else:
                failures += 1

    out_csv = args.out_dir / "shard_eval.csv"
    with out_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=EVAL_FIELDNAMES, extrasaction="ignore")
        writer.writeheader()
        for r in rows:
            writer.writerow(r)

    elapsed = time.monotonic() - t_start
    print(f"Completed {len(rows)}/{len(jobs)} jobs ({failures} failed) in {elapsed:.1f}s -> {out_csv}", flush=True)
    return 1 if (failures > 0 and not rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
