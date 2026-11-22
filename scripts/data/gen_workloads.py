#!/usr/bin/env python3
"""Generate deterministic Mortise-format (<gap_ms>\\t<size_bytes>) academic RCT workloads."""

from __future__ import annotations

import argparse
from pathlib import Path

MASK64 = (1 << 64) - 1


def xorshift64(seed: int):
    x = seed & MASK64
    while True:
        x ^= (x << 13) & MASK64
        x ^= (x >> 7) & MASK64
        x ^= (x << 17) & MASK64
        yield (x >> 11) / float(1 << 53)


def interp_cdf(cdf: list[tuple[float, int]], u: float) -> int:
    if u <= cdf[0][0]:
        return cdf[0][1]
    for (p0, v0), (p1, v1) in zip(cdf, cdf[1:]):
        if u <= p1:
            frac = (u - p0) / max(p1 - p0, 1e-9)
            return int(round(v0 + frac * (v1 - v0)))
    return cdf[-1][1]


def dctcp_websearch() -> list[tuple[int, int]]:
    """DCTCP Web Search (Alizadeh et al., SIGCOMM '10) empirical CDF workload."""
    cdf = [
        (0.00, 6 * 1024),
        (0.15, 13 * 1024),
        (0.20, 19 * 1024),
        (0.30, 33 * 1024),
        (0.53, 53 * 1024),
        (0.60, 133 * 1024),
        (0.70, 380 * 1024),
        (0.80, 750 * 1024),
        (0.90, 1400 * 1024),
        (0.97, 2200 * 1024),
        (1.00, 3200 * 1024),
    ]
    rng = xorshift64(0xDCDC_2010)
    out: list[tuple[int, int]] = []
    for i in range(80):
        u_size = next(rng)
        u_gap = next(rng)
        gap_ms = 0 if (i == 0 or u_gap < 0.25) else int(300.0 + 2400.0 * u_gap)
        out.append((gap_ms, interp_cdf(cdf, u_size)))
    return out


def vl2_datamining() -> list[tuple[int, int]]:
    """VL2 Data Mining (Greenberg et al., SIGCOMM '09): 80% short RPC/query + 20% large."""
    cdf = [
        (0.00, 2 * 1024),
        (0.50, 12 * 1024),
        (0.80, 80 * 1024),
        (0.85, 250 * 1024),
        (0.90, 600 * 1024),
        (0.95, 1500 * 1024),
        (1.00, 3000 * 1024),
    ]
    rng = xorshift64(0x564C_3209)
    out: list[tuple[int, int]] = []
    for i in range(80):
        u_size = next(rng)
        u_gap = next(rng)
        gap_ms = 0 if (i == 0 or u_gap < 0.30) else int(200.0 + 2000.0 * u_gap)
        out.append((gap_ms, interp_cdf(cdf, u_size)))
    return out


def http3_pageload() -> list[tuple[int, int]]:
    """Interactive HTTP/3 Web PageLoad burst workload (20 page views x 4 objects)."""
    objs = [18 * 1024, 54 * 1024, 180 * 1024, 720 * 1024]
    out: list[tuple[int, int]] = []
    for p in range(20):
        for k, sz in enumerate(objs):
            if p == 0 and k == 0:
                gap_ms = 0
            elif k == 0:
                gap_ms = 2500
            else:
                gap_ms = 20
            out.append((gap_ms, sz))
    return out


def _http2_mux_pages() -> list[tuple[int, int, list[int]]]:
    """Deterministic 12-page multi-stream HTTP/2 object catalog: (think_ms, html_bytes, burst_streams)."""
    rng = xorshift64(0x4832_2015)

    def rand_bytes(lo_kb: int, hi_kb: int) -> int:
        return int(round((lo_kb + (hi_kb - lo_kb) * next(rng)) * 1024))

    pages: list[tuple[int, int, list[int]]] = []
    for p in range(12):
        think_ms = 0 if p == 0 else int(2500 + 1000 * next(rng))
        html_bytes = rand_bytes(18, 32)
        burst_streams = [
            rand_bytes(24, 80),     # critical CSS
            rand_bytes(600, 1600),  # large hero image / media chunk / JS bundle
            rand_bytes(24, 80),     # critical JS
            rand_bytes(90, 260),    # image / asset 1
            rand_bytes(90, 260),    # image / asset 2
            rand_bytes(90, 260),    # image / asset 3
            rand_bytes(4, 24),      # small XHR / API 1
            rand_bytes(4, 24),      # small XHR / API 2
            rand_bytes(4, 24),      # small XHR / UI icon 3
        ]
        pages.append((think_ms, html_bytes, burst_streams))
    return pages


def _interleave_http2_frames(sched: str, frame_bytes: int = 16384) -> list[tuple[int, int]]:
    """Pre-interleave multi-stream HTTP/2 page objects into <= 16 KB DATA frames (pure .wk)."""
    out: list[tuple[int, int]] = []
    for think_ms, html_bytes, burst_streams in _http2_mux_pages():
        # Wave 1: HTML document split into HTTP/2 DATA frames
        rem = html_bytes
        first = True
        while rem > 0:
            chunk = min(rem, frame_bytes)
            out.append((think_ms if first else 0, chunk))
            rem -= chunk
            first = False

        # Wave 2: 9 concurrent page resources triggered 30 ms after HTML, interleaved into 16 KB frames
        active = list(burst_streams)
        wave_first = True
        while active:
            if sched == "srpt":
                idx = min(range(len(active)), key=lambda i: active[i])
                rem_s = active.pop(idx)
                chunk = min(rem_s, frame_bytes)
                out.append((30 if wave_first else 0, chunk))
                wave_first = False
                if rem_s > chunk:
                    active.insert(idx, rem_s - chunk)
            else:
                rem_s = active.pop(0)
                chunk = min(rem_s, frame_bytes)
                out.append((30 if wave_first else 0, chunk))
                wave_first = False
                if rem_s > chunk:
                    active.append(rem_s - chunk)
    return out


def http2_mux_rr() -> list[tuple[int, int]]:
    """HTTP/2 multiplexed page loads with 16 KB Round-Robin frame interleaving."""
    return _interleave_http2_frames("rr")


def http2_mux_srpt() -> list[tuple[int, int]]:
    """HTTP/2 multiplexed page loads with 16 KB SRPT (shortest-remaining-first) frame scheduling."""
    return _interleave_http2_frames("srpt")


GENERATORS = {
    "websearch": dctcp_websearch,
    "vl2": vl2_datamining,
    "pageload": http3_pageload,
    "http2_mux_rr": http2_mux_rr,
    "http2_mux_srpt": http2_mux_srpt,
}


def write_workload(path: Path, items: list[tuple[int, int]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"{gap_ms}\t{size}" for gap_ms, size in items]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument(
        "--only",
        default="websearch,vl2,pageload,http2_mux_rr,http2_mux_srpt",
        help="Comma-separated workload names to generate",
    )
    args = parser.parse_args()
    names = [n.strip() for n in args.only.split(",") if n.strip()]
    for name in names:
        if name not in GENERATORS:
            raise SystemExit(f"Unknown workload {name!r}; expected one of {sorted(GENERATORS)}")
        items = GENERATORS[name]()
        dst = args.out_dir / f"{name}.wk"
        write_workload(dst, items)
        span_s = sum(g for g, _ in items) / 1000.0
        total_mb = sum(s for _, s in items) / 1e6
        print(f"Generated {dst}: {len(items)} requests, span={span_s:.1f}s, total={total_mb:.2f} MB")


if __name__ == "__main__":
    main()
