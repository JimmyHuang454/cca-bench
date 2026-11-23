#!/usr/bin/env python3
"""Unified Mahimahi downlink-log + RCT CSV analyzer for ns-3 and Linux kernel (Mortise)."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any


def mortise_retrans_penalty(r: float) -> float:
    """Piecewise retransmission penalty P(r) from Mortise (NSDI '26, Eq. 6)."""
    if r < 0.05:
        return 0.0
    if r < 0.1:
        return 4.0 * r - 0.2
    if r < 0.4:
        return r + 0.1
    return 0.5


def validate_tail_percentile(p: float) -> float:
    val = float(p)
    if not math.isfinite(val) or val <= 0.0 or val > 100.0:
        raise ValueError(f"tail_percentile must be in (0, 100], got {p!r}")
    return val


def percentile(sorted_vals: list[float], p: float) -> float:
    if not sorted_vals:
        return math.nan
    k = (len(sorted_vals) - 1) * p
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return sorted_vals[int(k)]
    return sorted_vals[f] * (c - k) + sorted_vals[c] * (k - f)


def parse_downlink_log(
    path: Path,
    min_rtt_ms: float = 20.0,
    warmup_s: float = 0.0,
    max_s: float | None = None,
    tail_percentile: float = 95.0,
) -> dict[str, Any]:
    """Parse Mahimahi mm-link --downlink-log over [warmup_s, max_s] relative to first timestamp."""
    tail_p = validate_tail_percentile(tail_percentile)
    first_ts: float | None = None
    last_ts: float | None = None
    win_start_ts = 0.0
    win_end_ts = math.inf
    has_window = warmup_s > 0.0 or max_s is not None

    win_delivered = 0
    win_opportunity = 0
    win_dropped = 0
    win_act_start: float | None = None
    win_act_end: float | None = None
    qdelays_win: list[float] = []

    all_delivered = 0
    all_opportunity = 0
    all_dropped = 0
    all_act_start: float | None = None
    all_act_end: float | None = None
    qdelays_outside: list[float] = []

    with path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            parts = line.split()
            nparts = len(parts)
            if nparts < 3:
                continue
            try:
                ts = float(parts[0])
            except ValueError:
                continue

            if first_ts is None:
                first_ts = ts
                win_start_ts = first_ts + warmup_s * 1000.0
                win_end_ts = (first_ts + max_s * 1000.0) if max_s is not None else math.inf
            last_ts = ts

            kind = parts[1]
            if kind == "#":
                nbytes = int(parts[2])
                if has_window:
                    if all_act_start is None:
                        all_act_start = ts
                    all_act_end = ts
                    all_opportunity += nbytes
                if win_start_ts <= ts <= win_end_ts:
                    if win_act_start is None:
                        win_act_start = ts
                    win_act_end = ts
                    win_opportunity += nbytes
            elif kind == "-":
                nbytes = int(parts[2])
                has_qd = nparts >= 4
                qdelay = float(parts[3]) if has_qd else math.nan
                if has_window:
                    if all_act_start is None:
                        all_act_start = ts
                    all_act_end = ts
                    all_delivered += nbytes
                if win_start_ts <= ts <= win_end_ts:
                    if win_act_start is None:
                        win_act_start = ts
                    win_act_end = ts
                    win_delivered += nbytes
                    if has_qd and not math.isnan(qdelay):
                        qdelays_win.append(qdelay)
                elif has_window and has_qd and not math.isnan(qdelay):
                    qdelays_outside.append(qdelay)
            elif kind == "d":
                nbytes = int(parts[3]) if nparts >= 4 else int(parts[2])
                if has_window:
                    if all_act_start is None:
                        all_act_start = ts
                    all_act_end = ts
                    all_dropped += nbytes
                if win_start_ts <= ts <= win_end_ts:
                    if win_act_start is None:
                        win_act_start = ts
                    win_act_end = ts
                    win_dropped += nbytes

    if first_ts is None or last_ts is None or last_ts <= first_ts:
        return {
            "duration_s": math.nan,
            "tail_percentile": tail_p,
            "delivered_bytes": 0,
            "capacity_mbps": math.nan,
            "throughput_mbps": math.nan,
            "utilization": math.nan,
            "qdelay_avg_ms": math.nan,
            "qdelay_p95_ms": math.nan,
            "qdelay_tail_ms": math.nan,
            "est_rtt_ms": math.nan,
            "util_power": math.nan,
            "norm_power": math.nan,
            "norm_power_p95": math.nan,
            "norm_power_tail": math.nan,
            "tput_power": math.nan,
            "drop_rate": math.nan,
        }

    total_span_s = (last_ts - first_ts) / 1000.0
    # If the run finished too early (e.g., short legacy trace), skip warmup filter so metrics stay valid.
    if has_window and total_span_s <= warmup_s + 2.0:
        delivered_bytes = all_delivered
        opportunity_bytes = all_opportunity
        dropped_bytes = all_dropped
        act_start = all_act_start
        act_end = all_act_end
        qdelays = qdelays_win + qdelays_outside
    else:
        delivered_bytes = win_delivered
        opportunity_bytes = win_opportunity
        dropped_bytes = win_dropped
        act_start = win_act_start
        act_end = win_act_end
        qdelays = qdelays_win

    duration_s = (act_end - act_start) / 1000.0 if (act_start is not None and act_end is not None and act_end > act_start) else math.nan
    tput_mbps = (delivered_bytes * 8.0 / 1e6 / duration_s) if duration_s and duration_s > 0 else math.nan
    cap_mbps = (opportunity_bytes * 8.0 / 1e6 / duration_s) if duration_s and duration_s > 0 else math.nan
    util = (delivered_bytes / opportunity_bytes) if opportunity_bytes > 0 else math.nan

    qdelays.sort()
    qdelay_avg = (sum(qdelays) / len(qdelays)) if qdelays else math.nan
    qdelay_p95 = percentile(qdelays, 0.95)
    qdelay_tail = percentile(qdelays, tail_p / 100.0)
    est_rtt_ms = (min_rtt_ms + qdelay_avg) if not math.isnan(qdelay_avg) else math.nan
    est_rtt_p95_ms = (min_rtt_ms + qdelay_p95) if not math.isnan(qdelay_p95) else math.nan
    est_rtt_tail_ms = (min_rtt_ms + qdelay_tail) if not math.isnan(qdelay_tail) else math.nan
    util_power = (util / (est_rtt_ms / 1000.0)) if (est_rtt_ms and est_rtt_ms > 0 and not math.isnan(util)) else math.nan
    norm_power = (util / (est_rtt_ms / min_rtt_ms)) if (est_rtt_ms and est_rtt_ms > 0 and not math.isnan(util)) else math.nan
    norm_power_p95 = (util / (est_rtt_p95_ms / min_rtt_ms)) if (est_rtt_p95_ms and est_rtt_p95_ms > 0 and not math.isnan(util)) else math.nan
    norm_power_tail = (util / (est_rtt_tail_ms / min_rtt_ms)) if (est_rtt_tail_ms and est_rtt_tail_ms > 0 and not math.isnan(util)) else math.nan
    tput_power = (tput_mbps / est_rtt_ms) if (est_rtt_ms and est_rtt_ms > 0 and not math.isnan(tput_mbps)) else math.nan

    total_offered = delivered_bytes + dropped_bytes
    drop_rate = (dropped_bytes / total_offered) if total_offered > 0 else 0.0

    return {
        "duration_s": duration_s,
        "tail_percentile": tail_p,
        "delivered_bytes": delivered_bytes,
        "capacity_mbps": cap_mbps,
        "throughput_mbps": tput_mbps,
        "utilization": util,
        "qdelay_avg_ms": qdelay_avg,
        "qdelay_p95_ms": qdelay_p95,
        "qdelay_tail_ms": qdelay_tail,
        "est_rtt_ms": est_rtt_ms,
        "util_power": util_power,
        "norm_power": norm_power,
        "norm_power_p95": norm_power_p95,
        "norm_power_tail": norm_power_tail,
        "tput_power": tput_power,
        "drop_rate": drop_rate,
    }


def parse_rct_csv(
    path: Path,
    delivered_bytes: int = 0,
    base_rtt_ms: float = 20.0,
    capacity_mbps: float = math.nan,
    tail_percentile: float = 95.0,
) -> dict[str, Any]:
    """Parse Mortise-compatible RCT CSV (`id,size,client_send,server_recv,client_recv,rct`)."""
    tail_p = validate_tail_percentile(tail_percentile)
    if not path.exists():
        return {}
    sizes: list[int] = []
    rcts_ms: list[float] = []
    retrans_pkts = 0

    with path.open("r", encoding="utf-8", errors="ignore") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header:
            return {}
        rows = list(reader)

    if not rows:
        return {}

    data_rows = rows
    if len(rows[-1]) >= 4 and rows[-1][0] == "0":
        try:
            retrans_pkts = int(float(rows[-1][3]))
        except ValueError:
            retrans_pkts = 0
        data_rows = rows[:-1]

    for row in data_rows:
        if len(row) < 6:
            continue
        try:
            sz = int(float(row[1]))
            rct = max(float(row[5]), 1.0)
        except ValueError:
            continue
        sizes.append(sz)
        rcts_ms.append(rct)

    if not rcts_ms:
        return {"retrans_pkts": retrans_pkts}

    sorted_rcts = sorted(rcts_ms)
    total_pkts = max(delivered_bytes / 1448.0, sum(sizes) / 1448.0, 1.0)
    retrans_ratio = retrans_pkts / total_pkts
    penalty = mortise_retrans_penalty(retrans_ratio)
    req_tput_mb_s = [(sz / 1e6) / (rct / 1000.0) for sz, rct in zip(sizes, rcts_ms)]
    qoe_vals = [v * (1.0 - penalty) for v in req_tput_mb_s]

    fct_slowdown = math.nan
    fct_slowdown_p95 = math.nan
    fct_slowdown_tail = math.nan
    if isinstance(capacity_mbps, (int, float)) and not math.isnan(capacity_mbps) and capacity_mbps > 0:
        cap_bytes_per_ms = capacity_mbps * 1e6 / 8.0 / 1000.0
        log_sum = 0.0
        slowdowns: list[float] = []
        for sz, rct in zip(sizes, rcts_ms):
            ideal_ms = base_rtt_ms + (sz / cap_bytes_per_ms)
            sd = max(rct / ideal_ms, 1.0)
            slowdowns.append(sd)
            log_sum += math.log(sd)
        fct_slowdown = math.exp(log_sum / len(rcts_ms))
        slowdowns.sort()
        fct_slowdown_p95 = percentile(slowdowns, 0.95)
        fct_slowdown_tail = percentile(slowdowns, tail_p / 100.0)

    return {
        "tail_percentile": tail_p,
        "requests": len(rcts_ms),
        "rct_mean_ms": sum(rcts_ms) / len(rcts_ms),
        "rct_p50_ms": percentile(sorted_rcts, 0.50),
        "rct_p95_ms": percentile(sorted_rcts, 0.95),
        "rct_tail_ms": percentile(sorted_rcts, tail_p / 100.0),
        "retrans_pkts": retrans_pkts,
        "retrans_ratio": retrans_ratio,
        "fct_slowdown": fct_slowdown,
        "fct_slowdown_p95": fct_slowdown_p95,
        "fct_slowdown_tail": fct_slowdown_tail,
        "mortise_qoe": sum(qoe_vals) / len(qoe_vals),
    }


def parse_meta_from_stem(stem: str) -> dict[str, str]:
    """Extract metadata from `<stack>__<workload>__<trace>__q<bytes>__<cc>[__r<k>]`."""
    parts = stem.split("__")
    if len(parts) >= 5:
        repeat = "1"
        tail = parts[4:]
        if len(tail) >= 2 and tail[-1].startswith("r") and tail[-1][1:].isdigit():
            repeat = tail[-1][1:]
            tail = tail[:-1]
        return {
            "stack": parts[0],
            "workload": parts[1],
            "trace": parts[2],
            "queue_bytes": parts[3].lstrip("q"),
            "cc": "__".join(tail),
            "repeat": repeat,
        }
    return {
        "stack": "unknown",
        "workload": "unknown",
        "trace": stem,
        "queue_bytes": "",
        "cc": "",
        "repeat": "1",
    }


def format_queue_size(raw: Any) -> str:
    """Format queue size in bytes as KB/MB (e.g. 150000 -> 150KB, 2000000 -> 2MB)."""
    s = str(raw).strip()
    if not s or s == "-":
        return "-"
    try:
        n = int(float(s))
    except ValueError:
        return s
    if n >= 1_000_000:
        return f"{n / 1_000_000.0:g}MB"
    if n >= 1_000:
        return f"{n / 1_000.0:g}KB"
    return f"{n}B"


def queue_sort_key(raw: Any) -> tuple[int, str]:
    s = str(raw).strip()
    try:
        return (int(float(s)), s)
    except ValueError:
        return (10**18, s)


def fmt_num(val: Any, digits: int = 2) -> str:
    if val is None or (isinstance(val, float) and math.isnan(val)):
        return "-"
    if isinstance(val, int):
        return str(val)
    return f"{val:.{digits}f}"


def trace_group(trace: str) -> str:
    if trace.startswith("wired-") or trace.startswith("wifi-") or trace.startswith("line-"):
        return trace
    if trace.startswith("leo-starlink-"):
        return "Starlink-" + trace.rsplit("-", 1)[-1]
    if trace.startswith("nyc2018-4g-subway-"):
        return trace
    if trace.startswith("5g_trace_"):
        return "Lumos5G-" + trace.rsplit("_", 1)[-1]
    if "LTE-driving" in trace:
        return "4G-driving"
    if trace.endswith("-home1") or trace.endswith("-home3") or trace.endswith("-home") or trace in {
        "trace-3457194-timessquare",
        "trace-3458374-timessquare",
    }:
        return "NYC-stationary"
    if trace.endswith("-bus") or trace.endswith("-taxi1") or trace.endswith("-taxi3"):
        return "NYC-driving"
    if trace.startswith("trace-"):
        return "NYC-walking"
    if trace.startswith("synth-"):
        return trace
    return trace or "other"


def mean_valid(rows: list[dict[str, Any]], key: str) -> float:
    vals = [r[key] for r in rows if key in r and isinstance(r[key], (int, float)) and not math.isnan(r[key])]
    return (sum(vals) / len(vals)) if vals else math.nan


CSV_INT_FIELDS = {"requests", "retrans_pkts"}
CSV_FLOAT_FIELDS = {
    "duration_s",
    "tail_percentile",
    "capacity_mbps",
    "throughput_mbps",
    "utilization",
    "qdelay_avg_ms",
    "qdelay_p95_ms",
    "qdelay_tail_ms",
    "base_rtt_ms",
    "est_rtt_ms",
    "util_power",
    "norm_power",
    "norm_power_p95",
    "norm_power_tail",
    "tput_power",
    "drop_rate",
    "rct_mean_ms",
    "rct_p50_ms",
    "rct_p95_ms",
    "rct_tail_ms",
    "retrans_ratio",
    "fct_slowdown",
    "fct_slowdown_p95",
    "fct_slowdown_tail",
    "rct_slowdown",
    "mortise_qoe",
}


def record_key(row: dict[str, Any]) -> tuple[str, str, str, str, str, str]:
    return (
        str(row.get("stack", "")),
        str(row.get("workload", "")),
        str(row.get("trace", "")),
        str(row.get("queue_bytes", "")),
        str(row.get("cc", "")),
        str(row.get("repeat", "1")),
    )


def load_shard_csv(path: Path) -> list[dict[str, Any]]:
    """Load pre-aggregated per-shard evaluation rows written by mahimahi_eval.py."""
    rows: list[dict[str, Any]] = []
    with path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for raw in reader:
            row: dict[str, Any] = {}
            for k, v in raw.items():
                if k is None:
                    continue
                if v is None or v == "":
                    continue
                if k in CSV_INT_FIELDS:
                    try:
                        row[k] = int(float(v))
                    except ValueError:
                        continue
                elif k in CSV_FLOAT_FIELDS:
                    try:
                        val_f = float(v)
                    except ValueError:
                        val_f = math.nan
                    if k == "rct_slowdown":
                        row.setdefault("fct_slowdown", val_f)
                    else:
                        row[k] = val_f
                else:
                    row[k] = v
            if row:
                rows.append(row)
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description="Unified benchmark evaluator for ns-3 and Linux kernel TCP.")
    parser.add_argument(
        "--input-dir",
        required=True,
        nargs="+",
        type=Path,
        help="One or more directories containing *.downlink.log, shard_eval.csv, and *.rct.csv",
    )
    parser.add_argument("--out-csv", required=True, type=Path, help="Output summary CSV")
    parser.add_argument("--out-md", required=True, type=Path, help="Output summary Markdown")
    parser.add_argument("--min-rtt-ms", type=float, default=20.0, help="Base propagation RTT in ms")
    parser.add_argument("--bulk-warmup-s", type=float, default=5.0, help="Warmup seconds skipped for bulk runs")
    parser.add_argument("--bulk-end-s", type=float, default=None, help="Optional end timestamp (s) for bulk window")
    parser.add_argument("--tail-percentile", type=float, default=95.0, help="Tail percentile in (0, 100] (e.g. 50, 90, 95, 99)")
    args = parser.parse_args()
    tail_p = validate_tail_percentile(args.tail_percentile)

    input_dirs = [d for d in args.input_dir if d.exists()]
    logs: list[Path] = []
    for d in input_dirs:
        logs.extend(d.rglob("*.downlink.log"))
    logs = sorted(set(logs))
    records: list[dict[str, Any]] = []
    seen_keys: set[tuple[str, str, str, str, str, str]] = set()

    for log_path in logs:
        stem = log_path.name[: -len(".downlink.log")]
        meta = parse_meta_from_stem(stem)
        is_bulk = meta["workload"] == "bulk"
        warmup = args.bulk_warmup_s if is_bulk else 0.0
        max_s = args.bulk_end_s if is_bulk else None

        rtt_file = log_path.with_name(f"{meta['trace']}.base-rtt-ms")
        base_rtt_ms = float(rtt_file.read_text()) if rtt_file.exists() else args.min_rtt_ms
        row: dict[str, Any] = {**meta, "base_rtt_ms": base_rtt_ms}
        row.update(
            parse_downlink_log(
                log_path, min_rtt_ms=base_rtt_ms, warmup_s=warmup, max_s=max_s, tail_percentile=tail_p
            )
        )

        rct_path = log_path.with_name(f"{stem}.rct.csv")
        row.update(
            parse_rct_csv(
                rct_path,
                delivered_bytes=row.get("delivered_bytes", 0),
                base_rtt_ms=base_rtt_ms,
                capacity_mbps=row.get("capacity_mbps", math.nan),
                tail_percentile=tail_p,
            )
        )

        seen_keys.add(record_key(row))
        records.append(row)

    out_csv_resolved = args.out_csv.resolve()
    shard_csvs: list[Path] = []
    for d in input_dirs:
        shard_csvs.extend(d.rglob("shard_eval.csv"))
    for shard_csv in sorted(set(shard_csvs)):
        if shard_csv.resolve() == out_csv_resolved:
            continue
        for row in load_shard_csv(shard_csv):
            rk = record_key(row)
            if rk in seen_keys:
                continue
            seen_keys.add(rk)
            records.append(row)

    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "stack",
        "workload",
        "trace",
        "queue_bytes",
        "cc",
        "repeat",
        "duration_s",
        "tail_percentile",
        "capacity_mbps",
        "throughput_mbps",
        "utilization",
        "qdelay_avg_ms",
        "qdelay_p95_ms",
        "qdelay_tail_ms",
        "base_rtt_ms",
        "est_rtt_ms",
        "util_power",
        "norm_power",
        "norm_power_p95",
        "norm_power_tail",
        "tput_power",
        "drop_rate",
        "requests",
        "rct_mean_ms",
        "rct_p50_ms",
        "rct_p95_ms",
        "rct_tail_ms",
        "retrans_pkts",
        "retrans_ratio",
        "fct_slowdown",
        "fct_slowdown_p95",
        "fct_slowdown_tail",
        "mortise_qoe",
    ]
    with args.out_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for r in records:
            writer.writerow(r)

    bulk_groups: dict[tuple[str, str, str, str], list[dict[str, Any]]] = {}
    app_groups: dict[tuple[str, str, str, str], list[dict[str, Any]]] = {}
    for r in records:
        gk = (
            trace_group(r.get("trace", "")),
            r.get("queue_bytes", "-"),
            r.get("stack", "-"),
            r.get("cc", "-"),
        )
        if r.get("workload") == "bulk":
            bulk_groups.setdefault(gk, []).append(r)
        else:
            app_groups.setdefault(gk, []).append(r)

    lines = [
        "# Copa-Auto Unified Benchmark Summary",
        "",
        f"Bulk metrics skip the first `{args.bulk_warmup_s:.1f}s` warmup window.",
        "- Note: `copa_auto_nc` / `copa-auto-nc-*` runs **Copa default mode only (`mode_switching=false`)**; `copa_auto` enables competitive mode (`mode_switching=true`).",
        "",
        "## 1. Bulk Saturated Flows (Kleinrock Power & Delay-Utilization Tradeoff)",
        "",
        "| Trace Group | Queue | Stack | CC | N | Cap (Mbps) | Thr (Mbps) | Util | QDelay Avg (ms) | QDelay P95 (ms) | Power (Util/s) | Norm Power | Drop |",
        "|---|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for gk in sorted(bulk_groups.keys(), key=lambda x: (x[0], queue_sort_key(x[1]), x[2], x[3])):
        grp, q, stk, cc = gk
        grows = bulk_groups[gk]
        lines.append(
            "| {grp} | {q} | {stk} | {cc} | {n} | {cap} | {thr} | {util} | {qd} | {qdp95} | {pwr} | {npwr} | {drop} |".format(
                grp=grp,
                q=format_queue_size(q),
                stk=stk,
                cc=cc,
                n=len(grows),
                cap=fmt_num(mean_valid(grows, "capacity_mbps")),
                thr=fmt_num(mean_valid(grows, "throughput_mbps")),
                util=fmt_num(mean_valid(grows, "utilization"), 3),
                qd=fmt_num(mean_valid(grows, "qdelay_avg_ms")),
                qdp95=fmt_num(mean_valid(grows, "qdelay_p95_ms")),
                pwr=fmt_num(mean_valid(grows, "util_power")),
                npwr=fmt_num(mean_valid(grows, "norm_power"), 3),
                drop=fmt_num(mean_valid(grows, "drop_rate"), 4),
            )
        )

    lines.extend([
        "",
        "## 2. File Download Workload (`app.csv`: FCT Slowdown & Mortise QoE)",
        "",
        "| Trace Group | Queue | Stack | CC | N | Thr (Mbps) | QDelay Avg (ms) | Drop | Mean RCT (ms) | P95 RCT (ms) | FCT Slowdown | Mortise QoE |",
        "|---|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for gk in sorted(app_groups.keys(), key=lambda x: (x[0], queue_sort_key(x[1]), x[2], x[3])):
        grp, q, stk, cc = gk
        grows = app_groups[gk]
        lines.append(
            "| {grp} | {q} | {stk} | {cc} | {n} | {thr} | {qd} | {drop} | {rct} | {rct95} | {sd} | {qoe} |".format(
                grp=grp,
                q=format_queue_size(q),
                stk=stk,
                cc=cc,
                n=len(grows),
                thr=fmt_num(mean_valid(grows, "throughput_mbps")),
                qd=fmt_num(mean_valid(grows, "qdelay_avg_ms")),
                drop=fmt_num(mean_valid(grows, "drop_rate"), 4),
                rct=fmt_num(mean_valid(grows, "rct_mean_ms")),
                rct95=fmt_num(mean_valid(grows, "rct_p95_ms")),
                sd=fmt_num(mean_valid(grows, "fct_slowdown"), 2),
                qoe=fmt_num(mean_valid(grows, "mortise_qoe")),
            )
        )

    # Overall Summary: equal weight per (Trace Group, Queue)
    # Composite Score = sqrt(Norm Power / FCT Slowdown) in (0, 1] (higher is better)
    cc_bulk: dict[tuple[str, str], list[float]] = {}
    cc_app: dict[tuple[str, str], list[float]] = {}
    for (_, _, stk, cc), grows in bulk_groups.items():
        v = mean_valid(grows, "norm_power")
        if not math.isnan(v):
            cc_bulk.setdefault((stk, cc), []).append(v)
    for (_, _, stk, cc), grows in app_groups.items():
        v = mean_valid(grows, "fct_slowdown")
        if not math.isnan(v) and v > 0:
            cc_app.setdefault((stk, cc), []).append(v)

    all_ccs = sorted(set(cc_bulk.keys()) | set(cc_app.keys()))
    if all_ccs:
        summary_rows: list[dict[str, Any]] = []
        for sk in all_ccs:
            stk, cc = sk
            bvals = cc_bulk.get(sk, [])
            avals = cc_app.get(sk, [])
            bp = (sum(bvals) / len(bvals)) if bvals else math.nan
            sd = math.exp(sum(math.log(v) for v in avals) / len(avals)) if avals else math.nan
            ov = math.sqrt(bp / sd) if (not math.isnan(bp) and not math.isnan(sd) and bp >= 0 and sd > 0) else math.nan
            summary_rows.append({
                "stk": stk,
                "cc": cc,
                "bn": len(bvals),
                "bp": bp,
                "an": len(avals),
                "sd": sd,
                "ov": ov,
            })

        best_bp = max((r["bp"] for r in summary_rows if not math.isnan(r["bp"])), default=math.nan)
        best_sd = min((r["sd"] for r in summary_rows if not math.isnan(r["sd"]) and r["sd"] > 0), default=math.nan)
        best_ov = max((r["ov"] for r in summary_rows if not math.isnan(r["ov"])), default=math.nan)

        def fmt_with_gap(val: float, best: float, digits: int, higher_better: bool, suffix: str = "") -> str:
            if math.isnan(val):
                return "-"
            base = f"{val:.{digits}f}{suffix}"
            if math.isnan(best) or best <= 0:
                return base
            pct = ((val - best) / best) * 100.0
            if abs(pct) < 0.05:
                return f"**{base}** (0.0%)"
            return f"{base} ({pct:+.1f}%)"

        summary_rows.sort(
            key=lambda r: (
                0 if not math.isnan(r["ov"]) else 1,
                -(r["ov"] if not math.isnan(r["ov"]) else -1.0),
                r["stk"],
                r["cc"],
            )
        )

        lines.extend([
            "",
            "## 3. Overall Summary (Normalized Power, FCT Slowdown & Composite Score)",
            "",
            "| Stack | CC | Bulk Groups | Norm Power (↑) | App Groups | FCT Slowdown (↓) | Composite Score (↑) |",
            "|---|---|---:|---:|---:|---:|---:|",
        ])
        for r in summary_rows:
            lines.append(
                "| {stk} | {cc} | {bn} | {bp} | {an} | {sd} | {ov} |".format(
                    stk=r["stk"],
                    cc=r["cc"],
                    bn=r["bn"],
                    bp=fmt_with_gap(r["bp"], best_bp, 3, True),
                    an=r["an"],
                    sd=fmt_with_gap(r["sd"], best_sd, 2, False, "x"),
                    ov=fmt_with_gap(r["ov"], best_ov, 3, True),
                )
            )

    # Section 4: Cross-Stack Consistency (ns-3 vs Linux Kernel)
    ns3_bulk: dict[tuple[str, str, str], float] = {}
    kern_bulk: dict[tuple[str, str, str], float] = {}
    for (grp, q, stk, cc), grows in bulk_groups.items():
        v = mean_valid(grows, "norm_power")
        if math.isnan(v):
            continue
        if stk.startswith("ns3"):
            ns3_bulk[(grp, q, cc)] = v
        elif stk == "kernel":
            kern_bulk[(grp, q, cc)] = v

    ns3_app: dict[tuple[str, str, str], float] = {}
    kern_app: dict[tuple[str, str, str], float] = {}
    for (grp, q, stk, cc), grows in app_groups.items():
        v = mean_valid(grows, "fct_slowdown")
        if math.isnan(v):
            continue
        if stk.startswith("ns3"):
            ns3_app[(grp, q, cc)] = v
        elif stk == "kernel":
            kern_app[(grp, q, cc)] = v

    overlap_keys = sorted(
        (set(ns3_bulk.keys()) & set(kern_bulk.keys())) | (set(ns3_app.keys()) & set(kern_app.keys())),
        key=lambda x: (x[0], queue_sort_key(x[1]), x[2]),
    )
    if overlap_keys:
        lines.extend([
            "",
            "## 4. Cross-Stack Consistency (ns-3 vs Linux Kernel)",
            "",
            "| Trace Group | Queue | CC | ns-3 NormPower | Kernel NormPower | Δ Power | ns-3 Slowdown | Kernel Slowdown | Δ Slowdown |",
            "|---|---:|---|---:|---:|---:|---:|---:|---:|",
        ])
        for grp, q, cc in overlap_keys:
            nb = ns3_bulk.get((grp, q, cc), math.nan)
            kb = kern_bulk.get((grp, q, cc), math.nan)
            db = ((nb - kb) / kb * 100.0) if (not math.isnan(nb) and not math.isnan(kb) and kb > 0) else math.nan
            na = ns3_app.get((grp, q, cc), math.nan)
            ka = kern_app.get((grp, q, cc), math.nan)
            da = ((na - ka) / ka * 100.0) if (not math.isnan(na) and not math.isnan(ka) and ka > 0) else math.nan
            lines.append(
                "| {grp} | {q} | {cc} | {nb} | {kb} | {db} | {na} | {ka} | {da} |".format(
                    grp=grp,
                    q=format_queue_size(q),
                    cc=cc,
                    nb=fmt_num(nb, 3),
                    kb=fmt_num(kb, 3),
                    db=(f"{db:+.1f}%" if not math.isnan(db) else "-"),
                    na=(f"{na:.2f}x" if not math.isnan(na) else "-"),
                    ka=(f"{ka:.2f}x" if not math.isnan(ka) else "-"),
                    da=(f"{da:+.1f}%" if not math.isnan(da) else "-"),
                )
            )

    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Analyzed {len(records)} runs -> {args.out_csv}, {args.out_md}")


if __name__ == "__main__":
    main()
