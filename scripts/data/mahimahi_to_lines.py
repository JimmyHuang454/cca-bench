#!/usr/bin/env python3
"""Convert Mahimahi / Lumos5G / NYC / LeoCC traces into unified lines/*.json specs."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
BENCH_DIR = REPO_ROOT / "scripts" / "bench"
if str(BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(BENCH_DIR))

import run_mahimahi_suite as rms  # noqa: E402


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


def infer_tech_and_queues(trace_name: str, shard: str) -> tuple[str, list[str], float]:
    if shard in ("leo", "local-leo") or trace_name.startswith("leo-") or trace_name.startswith("local-leo-"):
        return "leo", ["2MB", "75MB"], 115.0
    if "5g" in trace_name.lower() or shard.startswith("lumos-5g") or shard == "lumos-rct":
        return "5g", ["2MB", "16MB"], 60.0
    return "trace", ["150KB", "2MB"], 60.0


def summarize_mahimahi_file(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    times: list[int] = []
    with path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            s = line.strip()
            if s.isdigit():
                times.append(int(s))
    if not times or times[-1] <= 0:
        return {}
    period_ms = times[-1]
    mean_mbps = len(times) * 1500.0 * 8.0 / (period_ms * 1000.0)
    # 100ms window peak Mbps
    peak_cnt = 0
    cur_bin = -1
    cur_cnt = 0
    for t in times:
        b = t // 100
        if b != cur_bin:
            if cur_cnt > peak_cnt:
                peak_cnt = cur_cnt
            cur_bin = b
            cur_cnt = 1
        else:
            cur_cnt += 1
    if cur_cnt > peak_cnt:
        peak_cnt = cur_cnt
    peak_100ms_mbps = peak_cnt * 1500.0 * 8.0 / 100000.0
    return {
        "opportunities": len(times),
        "period_s": round(period_ms / 1000.0, 2),
        "mean_mbps": round(mean_mbps, 2),
        "peak_100ms_mbps": round(peak_100ms_mbps, 2),
    }


def rel_or_abs(p: Path, base: Path) -> str:
    try:
        return str(p.resolve().relative_to(base.resolve()))
    except ValueError:
        return str(p.resolve())


def convert_shard(shard: str, trace_dir: Path, out_dir: Path, local_leo_dir: Path | None) -> int:
    sub_shards = expand_shards(shard)
    trace_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    pairs: list[tuple[str, str]] = []
    seen: set[str] = set()
    for sh in sub_shards:
        for tr in rms.resolve_shard_traces(sh, trace_dir, local_leo_dir):
            if tr not in seen:
                seen.add(tr)
                pairs.append((sh, tr))

    index: list[dict[str, Any]] = []
    for idx, (sh, trace_name) in enumerate(pairs, 1):
        tech, queues, dur_s = infer_tech_and_queues(trace_name, sh)
        replay = rms.prepare_replay(trace_name, trace_dir, int(dur_s))
        stats = summarize_mahimahi_file(replay.down)
        spec: dict[str, Any] = {
            "_comment": f"Auto-converted from Mahimahi trace '{trace_name}' (shard={sh})",
            "name": trace_name,
            "tech": tech,
            "shard": sh,
            "down_trace": rel_or_abs(replay.down, REPO_ROOT),
            "up_trace": rel_or_abs(replay.up, REPO_ROOT) if replay.up != replay.down else "",
            "delay_trace": rel_or_abs(replay.delay, REPO_ROOT) if replay.delay is not None else "",
            "base_rtt_ms": round(float(replay.base_rtt_ms), 2),
            "queues": queues,
            "duration_s": dur_s,
            "stats": stats,
        }
        dst = out_dir / f"{trace_name}.json"
        dst.write_text(json.dumps(spec, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        index.append({"name": trace_name, "tech": tech, "shard": sh, "file": dst.name, "stats": stats})
        if idx == 1 or idx % 20 == 0 or idx == len(pairs):
            print(f"[convert {idx}/{len(pairs)}] {dst.name} (tech={tech}, mean={stats.get('mean_mbps', 0)} Mbps)", flush=True)

    (out_dir / "_index.json").write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Converted {len(pairs)} Mahimahi traces -> {out_dir}", flush=True)
    return 0


def convert_local_dir(in_dir: Path, out_dir: Path, base_rtt_ms: float, queues: list[str], dur_s: float) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    downs = sorted(in_dir.glob("*.down"))
    if not downs:
        print(f"No *.down files found in {in_dir}", file=sys.stderr)
        return 1
    for idx, down in enumerate(downs, 1):
        name = down.stem
        up = down.with_suffix(".up")
        delay = down.with_suffix(".delay")
        tech = "5g" if "5g" in name.lower() else ("leo" if "leo" in name.lower() else "trace")
        stats = summarize_mahimahi_file(down)
        spec = {
            "_comment": f"Auto-converted from local Mahimahi file '{down.name}'",
            "name": name,
            "tech": tech,
            "shard": "local",
            "down_trace": rel_or_abs(down, REPO_ROOT),
            "up_trace": rel_or_abs(up, REPO_ROOT) if up.is_file() else "",
            "delay_trace": rel_or_abs(delay, REPO_ROOT) if delay.is_file() else "",
            "base_rtt_ms": base_rtt_ms,
            "queues": queues,
            "duration_s": dur_s,
            "stats": stats,
        }
        dst = out_dir / f"{name}.json"
        dst.write_text(json.dumps(spec, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"[convert {idx}/{len(downs)}] {dst.name} (mean={stats.get('mean_mbps', 0)} Mbps)", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert Mahimahi traces to unified lines/*.json specs.")
    parser.add_argument("--shard", default="4g,datasets,synth", help="Shard(s) to convert (e.g. 4g,5g,leo,full)")
    parser.add_argument("--in-dir", type=Path, default=None, help="Optional local directory of *.down traces")
    parser.add_argument("--trace-dir", type=Path, default=REPO_ROOT / "datasets" / "traces")
    parser.add_argument("--out-dir", type=Path, default=REPO_ROOT / "datasets" / "lines" / "mahimahi")
    parser.add_argument("--local-leo-dir", type=Path, default=None)
    parser.add_argument("--base-rtt-ms", type=float, default=20.0)
    parser.add_argument("--queues", default="150KB,2MB")
    parser.add_argument("--dur-s", type=float, default=60.0)
    args = parser.parse_args()

    if args.in_dir is not None:
        qs = [q.strip() for q in args.queues.split(",") if q.strip()]
        return convert_local_dir(args.in_dir, args.out_dir, args.base_rtt_ms, qs, args.dur_s)
    return convert_shard(args.shard, args.trace_dir, args.out_dir, args.local_leo_dir)


if __name__ == "__main__":
    raise SystemExit(main())
