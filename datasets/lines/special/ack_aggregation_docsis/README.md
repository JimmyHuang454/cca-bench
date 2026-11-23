# ack_aggregation_docsis

反向 ACK 聚合抖动场景（DOCSIS 宽带 / Wi-Fi / 5G TDD 常见现象：数据链路平稳 36 Mbps，ACK 每 14 ms 批量到达）。

## 参数说明

- 上行数据容量：完全平稳 36 Mbps；基础 RTT 20 ms（上下行各 10 ms）。
- 下行 ACK 容量：每 14 ms 仅在最后 1 ms 批量释放 ACK（静默 13 ms），引入约 6–7 ms 的反向 ACK 聚合抖动。
- 考察重点：原版 Copa 经典盲区——将 ACK 聚合延迟误判为数据队列导致吞吐暴跌；BBR/CUBIC 会在数据侧堆积排队；Copa-Auto v3 测出 $j$ 后满速运行且零数据排队膨胀。

场景持续 60 秒，区间左闭右开。TCP 数据走上行（`up.txt`），ACK 走下行（`down.txt`）；基础 RTT 为上下行单向基础延迟之和。容量和延迟具体时间点以本目录中的 lines 文件为准。
