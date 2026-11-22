# Workload File Format (`.wk`)

All application-layer request workloads in `datasets/workloads/` use the unified **Mortise `.wk`** plain-text format so the exact same workload files drive both **ns-3** and **Linux Kernel** evaluations.

## Syntax
- Lines starting with `#` are metadata comments and ignored by loaders.
- Each non-empty data line has two whitespace-separated columns:
  ```text
  <gap_ms>	<size_bytes>
  ```
  - `gap_ms`: inter-arrival gap in milliseconds since the previous request (float or int, `>= 0`).
  - `size_bytes`: payload size in bytes for this request (positive integer).

## Built-in Workloads
| File | Pattern | Requests | Span | Total Size |
| :--- | :--- | ---: | ---: | ---: |
| `app.wk` | Mortise (NSDI'26) mixed object benchmark | 92 | 149.3 s | 58.30 MB |
| `websearch.wk` | DCTCP heavy-tailed search queries + large responses | 80 | 95.9 s | 29.10 MB |
| `vl2.wk` | VL2 bimodal extreme heavy tail (tiny RPCs + large transfers) | 80 | 89.9 s | 10.91 MB |
| `pageload.wk` | Bursty HTTP/3 webpage resource clusters + think time | 80 | 48.7 s | 19.91 MB |
| `http2_mux_rr.wk` | HTTP/2 16 KB Round-Robin pre-interleaved multi-stream page loads | 1326 | 35.3 s | 20.77 MB |
| `http2_mux_srpt.wk` | HTTP/2 16 KB SRPT (shortest-first) pre-interleaved multi-stream page loads | 1326 | 35.3 s | 20.77 MB |
| *(built-in `bulk`)* | Continuous saturated backlogged flow (no `.wk` file needed) | — | 60 s (115 s LEO) | Saturated |

Regenerate synthetic `.wk` files anytime with:
```bash
make workloads
```
