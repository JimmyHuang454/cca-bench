# Special 场景（Copa-Auto v3 专项优势场景集）

本目录包含 10 个面向真实非 D/D/1 链路物理特征构造的专项测试场景，用于对比固定 $\delta=0.5$ 的原版 Copa、BBR、CUBIC 与自适应 Copa-Auto v3（`copa_auto_v3`）。

在 `datasets/lines/basic` 中，41 个场景均采用**分段恒定平滑速率 + 平稳基础延迟**（即 $j=0, k=0$ 的确定性 D/D/1 链路），按理论模型其最优参数恰好就是原版 Copa 的 $\delta=0.5$，因此 `copa_auto_v3` 在 `basic` 上会主动收敛到 $\delta=0.5$ 而与原版表现接近。本目录聚焦于真实无线、卫星、蜂窝与宽带接入网中普遍存在、但 `basic` 未覆盖的四类非平稳/随机特征（且各场景单流 BDP 均控制在 TCP 默认 128 KB 发送缓冲以内，确保各算法真实进入拥塞控制）：

| 场景 | 核心物理特征 | 对应参数机制 |
|---|---|---|
| `starlink_jitter_steady` | 低轨卫星稳态延迟抖动（34 Mbps, $j \approx 6\text{--}8\text{ ms}$） | 非排队抖动 $j > 0$（原版 Copa 被 $1/(\delta j)$ 饿死） |
| `starlink_reconfig_15s` | 低轨卫星 15 秒切星：容量阶跃（38→12→42→10M）+ 基延跃变 + TDMA 突发 | $R_0$ 跳变 + $j > 0$ + 骤降泄洪 |
| `wifi6_ampdu_burst` | Wi-Fi 6 CSMA/CA 随机争用 + A-MPDU TXOP 聚合突发（36 Mbps） | MAC 聚合突发（$j > 0$ 与 $k = c/\mu > 0$） |
| `wifi_shared_contention` | 拥挤 Wi-Fi 多用户抢占（44→10→48 Mbps 随机阻塞） | 变带宽突发争用（吞吐 vs 尾延迟权衡） |
| `5g_tdd_tdma_high_bw` | 5G NR TDD 帧调度（7 ms 静默 + 3 ms 突发，36 Mbps）+ HARQ 抖动 | 周期成帧抖动 $j > 0$ |
| `5g_fast_fading_stochastic` | 5G 两态马尔可夫快衰落（6–14 ↔ 40–56 Mbps，延迟恒定） | 纯 Kingman 随机色散 $k = c/\mu > 0, j = 0$ |
| `ack_aggregation_docsis` | 平稳 36 Mbps 数据链路 + 下行 14 ms 周期性 ACK 聚合 | 反向 ACK 聚合抖动（原版 Copa 经典盲区） |
| `cellular_handover_jitter_burst` | 5G↔4G 跨站切换（38→10→42 Mbps + 基延 +25 ms 跃变） | 跨站 $R_0/j$ 突变 + 突发适应 |
| `dynamic_jitter_regime_switch` | 平稳有线 42M → 抖动无线 42M → 平稳限速 12M | $\delta$ 双向自适应（$0.5 \to 0.03 \to 0.5$） |
| `mmwave_intermittent_burst` | 5G 毫米波 44 Mbps 帧突发 + 每 12 秒 600 ms 短遮挡跌至 6M | 高速突发 + 短暂遮挡快速泄洪 |

所有场景均为 60 秒，格式见 [FORMAT.md](../FORMAT.md)，由 [gen_special_lines.py](../../../scripts/data/gen_special_lines.py) 确定性生成（固定随机种子，可完全复现）。
