"""Shared parallel runner with live progress, ETA, and per-run metric summaries."""
import json
import os
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed


def fmt_dur(sec: float) -> str:
    s = max(0, int(round(sec)))
    h, rem = divmod(s, 3600)
    m, sec_i = divmod(rem, 60)
    return f"{h}:{m:02d}:{sec_i:02d}" if h else f"{m:02d}:{sec_i:02d}"


def _weighted_rtt(flows) -> float:
    tot = sum(f.get("throughput_mbps", 0.0) for f in flows)
    if tot > 0:
        return sum(f.get("rtt_mean_ms", 0.0) * f.get("throughput_mbps", 0.0) for f in flows) / tot
    if flows:
        return sum(f.get("rtt_mean_ms", 0.0) for f in flows) / len(flows)
    return 0.0


def fmt_cap_metrics(prefix: str) -> str:
    path = prefix + "-metrics.json"
    if not os.path.exists(path):
        return "no-metrics"
    try:
        with open(path) as f:
            m = json.load(f)
        rtt = _weighted_rtt(m.get("flows", []))
        return (
            f"util={m.get('utilization', 0.0) * 100:5.1f}% "
            f"thr={m.get('total_throughput_mbps', 0.0):6.2f}M "
            f"rtt={rtt:5.1f}ms jain={m.get('jain', 1.0):.2f}"
        )
    except Exception as exc:
        return f"metrics-err({exc})"


def fmt_lte_metrics(prefix: str) -> str:
    path = prefix + "-metrics.json"
    if not os.path.exists(path):
        return "no-metrics"
    try:
        with open(path) as f:
            m = json.load(f)
        rtt = _weighted_rtt(m.get("flows", []))
        return (
            f"thr={m.get('total_throughput_mbps', 0.0):6.2f}M "
            f"rtt={rtt:5.1f}ms jain={m.get('jain', 1.0):.2f} "
            f"ho={m.get('handovers', 0)}"
        )
    except Exception as exc:
        return f"metrics-err({exc})"


def run_cmd_job(cmd, cwd, prefix: str, timeout: int = 1200, metrics_fn=fmt_cap_metrics):
    t0 = time.monotonic()
    with open(prefix + "-stdout.log", "w") as log:
        try:
            proc = subprocess.run(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            dt = time.monotonic() - t0
            if proc.returncode != 0:
                return f"FAIL({proc.returncode})", "", dt
            return "OK", metrics_fn(prefix), dt
        except subprocess.TimeoutExpired:
            dt = time.monotonic() - t0
            return "TIMEOUT", "", dt


def run_parallel(title: str, jobs, njobs: int, worker_fn, label_fn):
    total = len(jobs)
    if total == 0:
        print(f"==> [{title}] 0 jobs (all done, skipped)", flush=True)
        return
    print(f"==> [{title}] {total} jobs ({njobs} parallel)", flush=True)
    t_start = time.monotonic()
    counts = {"OK": 0, "FAIL": 0, "TIMEOUT": 0}
    w = len(str(total))
    with ThreadPoolExecutor(max_workers=njobs) as ex:
        futs = {ex.submit(worker_fn, job): job for job in jobs}
        done = 0
        for fut in as_completed(futs):
            done += 1
            job = futs[fut]
            status, summary, job_dt = fut.result()
            if status == "OK":
                counts["OK"] += 1
            elif status == "TIMEOUT":
                counts["TIMEOUT"] += 1
            else:
                counts["FAIL"] += 1
            elapsed = time.monotonic() - t_start
            eta = elapsed * (total - done) / done if done else 0.0
            pct = 100.0 * done / total
            label = label_fn(job)
            extra = f" -> {summary}" if summary else ""
            print(
                f"[{done:{w}d}/{total} {pct:5.1f}% | el {fmt_dur(elapsed)} | eta {fmt_dur(eta)}] "
                f"{status:<7s} {label}{extra} ({job_dt:.1f}s)",
                flush=True,
            )
    total_el = time.monotonic() - t_start
    print(
        f"==> [{title}] done in {fmt_dur(total_el)}: "
        f"{counts['OK']} ok, {counts['FAIL']} fail, {counts['TIMEOUT']} timeout",
        flush=True,
    )
