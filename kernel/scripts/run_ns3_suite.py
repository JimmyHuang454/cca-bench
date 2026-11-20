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
    "fct_slowdown",
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


def load_completed_keys(csv_path: Path) -> tuple[set[tuple[str, str, str, str, str]], list[dict[str, Any]]]:
    keys: set[tuple[str, str, str, str, str]] = set()
    existing_rows: list[dict[str, Any]] = []
    if not csv_path.is_file():
        return keys, existing_rows
    with csv_path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            k = (
                row.get("trace", ""),
                row.get("queue_bytes", ""),
                row.get("workload", ""),
                row.get("cc", ""),
                row.get("repeat", ""),
            )
            if all(k):
                keys.add(k)
                existing_rows.append(row)
    return keys, existing_rows


def parse_queue_bytes(tok: str) -> str:
    """Parse '150KB', '2MB', '2M', or raw byte integer string into canonical byte string."""
    s = tok.strip().upper()
    if not s:
        return ""
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
    """Format byte count as human-readable KB / MB (e.g. 150000 -> 150KB, 2000000 -> 2MB)."""
    try:
        n = int(float(str(raw).strip()))
    except (TypeError, ValueError):
        return str(raw)
    if n >= 1_000_000:
        return f"{n / 1_000_000.0:g}MB"
    if n >= 1_000:
        return f"{n / 1_000.0:g}KB"
    return f"{n}B"


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
    tag = f"{job['trace']} q={format_queue_size(job['queue_bytes'])} {job['workload']} {job['cc']} r{job['repeat']}"
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
            f"[OK {dt:.1f}s] {tag} | FCT={data.get('rct_mean_ms', 0):.1f}ms "
            f"FCT-SD={data.get('fct_slowdown', 0):.2f}x QoE={data.get('mortise_qoe', 0):.2f}"
        )
    return row, msg


def main() -> int:
    parser = argparse.ArgumentParser(description="Run ns-3 CC benchmark suite.")
    parser.add_argument("--bin", required=True, type=Path, help="Path to compiled tcp_ccas_bench binary")
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
    parser.add_argument("--resume", action="store_true", help="Skip jobs already present in out_dir/shard_eval.csv")
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
    override_queues = [parse_queue_bytes(q) for q in args.queues.split(",") if q.strip()]

    trace_pairs: list[tuple[str, str]] = []
    seen_traces: set[str] = set()
    for sh in sub_shards:
        for tr in rms.resolve_shard_traces(sh, args.trace_dir, args.local_leo_dir):
            if tr not in seen_traces:
                seen_traces.add(tr)
                trace_pairs.append((sh, tr))

    out_csv = args.out_dir / "shard_eval.csv"
    completed_keys: set[tuple[str, str, str, str, str]] = set()
    existing_count = 0
    if args.resume and out_csv.is_file():
        completed_keys, existing_rows = load_completed_keys(out_csv)
        existing_count = len(existing_rows)

    repeats = max(args.repeats, 1)
    total_jobs = 0
    matched_resumed = 0
    for sh, trace_name in trace_pairs:
        queues = override_queues or default_queues_for_trace(trace_name, sh)
        total_jobs += len(queues) * len(workloads) * len(ccs) * repeats
        if completed_keys:
            for rep in range(1, repeats + 1):
                for q in queues:
                    for wk in workloads:
                        wk_norm = rms.normalize_workload_label(wk)
                        for cc in ccs:
                            if (trace_name, str(q), wk_norm, cc, str(rep)) in completed_keys:
                                matched_resumed += 1

    workers = args.parallel if args.parallel > 0 else max(os.cpu_count() or 2, 1)
    print(
        f"ns-3 suite: shard={args.shard} ({len(sub_shards)} sub-shards) traces={len(trace_pairs)} "
        f"ccs={ccs} workloads={workloads} repeats={repeats} bin_ms={args.bin_ms} clock_ms={args.clock_ms}",
        flush=True,
    )
    resume_note = f" ({matched_resumed} cached via --resume)" if matched_resumed else ""
    print(
        f"Dispatching {total_jobs} ns-3 jobs{resume_note} across {workers} workers "
        f"(streaming trace prep + incremental CSV -> {out_csv})...",
        flush=True,
    )

    def iter_jobs():
        for idx, (sh, trace_name) in enumerate(trace_pairs, 1):
            queues = override_queues or default_queues_for_trace(trace_name, sh)
            # Check if every job for this trace is already completed under --resume
            if completed_keys:
                all_done = True
                for rep in range(1, repeats + 1):
                    for q in queues:
                        for wk in workloads:
                            wk_norm = rms.normalize_workload_label(wk)
                            for cc in ccs:
                                if (trace_name, str(q), wk_norm, cc, str(rep)) not in completed_keys:
                                    all_done = False
                                    break
                            if not all_done:
                                break
                        if not all_done:
                            break
                    if not all_done:
                        break
                if all_done:
                    continue

            default_bulk_secs = 115 if sh in ("leo", "local-leo") and args.bulk_secs == 60 else args.bulk_secs
            t_prep = time.monotonic()
            if idx == 1 or idx % 20 == 0 or len(trace_pairs) <= 10:
                print(f"[prep {idx}/{len(trace_pairs)}] {trace_name} ({sh})...", flush=True)
            base_replay = rms.prepare_replay(trace_name, args.trace_dir, default_bulk_secs)
            dt_prep = time.monotonic() - t_prep
            if dt_prep >= 2.0:
                print(f"[prep {idx}/{len(trace_pairs)}] {trace_name} ready in {dt_prep:.1f}s", flush=True)

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
                            if (trace_name, str(q), wk_norm, cc, str(rep)) in completed_keys:
                                continue
                            yield {
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
                    pending, return_when=concurrent.futures.FIRST_COMPLETED
                )
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
    return 1 if (failures > 0 and total_ok == 0) else 0


if __name__ == "__main__":
    raise SystemExit(main())
