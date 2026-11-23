# starlink_jitter_steady

低轨卫星稳态抖动场景（34 Mbps 带宽 + 显著非排队延迟抖动 $j \approx 6\text{--}8\text{ ms}$）。

## 参数说明

- 上行容量：稳态 34 Mbps；下行 ACK 容量 60 Mbps。
- 上行单向延迟：基础 10 ms，每 15–25 ms 在 10–24 ms 间抖动；下行单向延迟固定 10 ms（最小 RTT 20 ms）。
- 考察重点：原版 Copa 固定 $\delta=0.5$ 会被 $1/(\delta j)$ 限死在约 2–3 Mbps；BBR/CUBIC 容易填满瓶颈队列；Copa-Auto v3 通过测出 $j$ 自动下调 $\delta$，兼顾满速与低排队延迟。

场景持续 60 秒，区间左闭右开。TCP 数据走上行（`up.txt`），ACK 走下行（`down.txt`）；基础 RTT 为上下行单向基础延迟之和。容量和延迟具体时间点以本目录中的 lines 文件为准。
