# 5g_tdd_tdma_high_bw

5G NR TDD 帧结构链路（10 ms 帧周期内 7 ms 静默 + 3 ms 上行突发，均速 36 Mbps + HARQ 抖动）。

## 参数说明

- 上行容量：每 10 ms 帧周期中前 7 ms 为下行/保护静默，后 3 ms 以 10 pkts/ms 突发传输（平均 36 Mbps）。
- 延迟：基础单向延迟各 10 ms（基础 RTT 20 ms），上行叠加 35% 概率的 3–7 ms HARQ 调度抖动。
- 考察重点：典型 5G 周期性成帧抖动，原版 Copa 吞吐坍塌至数 Mbps，Copa-Auto v3 高效跑满带宽且控制排队。

场景持续 60 秒，区间左闭右开。数据保存在 `line.csv` 中：TCP 数据走上行（`up_cap`），ACK 走下行（`down_cap`）；基础 RTT 为上下行单向基础延迟之和。
