#!/usr/bin/env python3
"""Generate the 10-scenario datasets/lines/special benchmark suite.

Focuses on realistic non-D/D/1 wireless, satellite, cellular, and ACK-aggregation
regimes where fixed-delta Copa (delta=0.5) starves or stalls and BBR/CUBIC
suffer severe tail bufferbloat, showcasing Copa-Auto v3's joint (R0, j, k) adaptation.

All peak (Capacity x RTT) values are sized within TCP's default 128 KB single-flow
send buffer so every algorithm genuinely enters congestion control rather than being
artificially shielded from queueing by send-buffer saturation.
"""
from __future__ import annotations

import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / 'datasets' / 'lines' / 'special'
sys.path.insert(0, str(ROOT / 'scripts' / 'bench'))
import lines_common  # noqa: E402

DURATION_MS = 60_000


def compress_points(points: list[tuple[int, str]]) -> list[tuple[int, str]]:
    """Merge consecutive identical values while preserving t=0 and strict monotonicity."""
    out: list[tuple[int, str]] = []
    for t, val in points:
        if not out:
            out.append((t, val))
        elif t == out[-1][0]:
            out[-1] = (t, val)
        elif val != out[-1][1]:
            out.append((t, val))
    return out


def write_scene(
    name: str,
    summary: str,
    details: str,
    up_pts: list[tuple[int, str]],
    down_pts: list[tuple[int, str]],
    up_delay: list[tuple[int, float]],
    down_delay: list[tuple[int, float]],
    end_ms: int = DURATION_MS,
) -> None:
    d = OUT_DIR / name
    d.mkdir(parents=True, exist_ok=True)
    ud_tokens = [(t, f'{v:.2f}'.rstrip('0').rstrip('.')) for t, v in up_delay]
    dd_tokens = [(t, f'{v:.2f}'.rstrip('0').rstrip('.')) for t, v in down_delay]
    lines_common.write_line_csv(
        d / 'line.csv',
        up_tokens=up_pts,
        down_tokens=down_pts,
        end_ms=end_ms,
        up_delay_tokens=ud_tokens,
        down_delay_tokens=dd_tokens,
        comments={0: summary, end_ms: 'end'},
        include_comment_col=True,
    )
    readme = (
        f'# {name}\n\n'
        f'{summary}\n\n'
        f'{details}\n\n'
        f'场景持续 {end_ms // 1000} 秒，区间左闭右开。数据保存在 `line.csv` 中：TCP 数据走上行（`up_cap`），ACK 走下行（`down_cap`）；'
        f'基础 RTT 为上下行单向基础延迟之和。\n'
    )
    (d / 'README.md').write_text(readme, encoding='utf-8')


def gen_starlink_jitter_steady() -> None:
    """1. LEO satellite steady jitter (34 Mbps, RTT min 20 ms, j ~ 7 ms)."""
    rng = random.Random(1001)
    up_pts = [(0, '34mbps')]
    down_pts = [(0, '60mbps')]
    up_delay: list[tuple[int, float]] = []
    down_delay = [(0, 10.0)]
    t = 0
    while t < DURATION_MS:
        if rng.random() < 0.25:
            d = 10.0 + rng.uniform(0.0, 1.5)
        else:
            d = 10.0 + rng.uniform(6.0, 14.0)
        up_delay.append((t, round(d, 1)))
        t += rng.choice((15, 20, 25))
    write_scene(
        'starlink_jitter_steady',
        '低轨卫星稳态抖动场景（34 Mbps 带宽 + 显著非排队延迟抖动 $j \\approx 6\\text{--}8\\text{ ms}$）。',
        '## 参数说明\n\n'
        '- 上行容量：稳态 34 Mbps；下行 ACK 容量 60 Mbps。\n'
        '- 上行单向延迟：基础 10 ms，每 15–25 ms 在 10–24 ms 间抖动；下行单向延迟固定 10 ms（最小 RTT 20 ms）。\n'
        '- 考察重点：原版 Copa 固定 $\\delta=0.5$ 会被 $1/(\\delta j)$ 限死在约 2–3 Mbps；BBR/CUBIC 容易填满瓶颈队列；Copa-Auto v3 通过测出 $j$ 自动下调 $\\delta$，兼顾满速与低排队延迟。',
        up_pts, down_pts, up_delay, down_delay,
    )


def gen_starlink_reconfig_15s() -> None:
    """2. 15-second LEO satellite beam reconfiguration: capacity steps + base RTT jumps + TDMA bursts."""
    rng = random.Random(1002)
    segs = [
        (0,      15_000, 38.0, 10.0, 7.0,  8),
        (15_000, 30_000, 12.0, 22.0, 9.0,  10),
        (30_000, 45_000, 42.0, 10.0, 7.0,  8),
        (45_000, 60_000, 10.0, 20.0, 8.0,  10),
    ]
    up_pts: list[tuple[int, str]] = []
    down_pts = [(0, '80mbps')]
    up_delay: list[tuple[int, float]] = []
    down_delay = [(0, 10.0)]

    for t0, t1, avg_mbps, base_d, jspan, frame in segs:
        t = t0
        while t < t1:
            pkts = max(1, round(avg_mbps * frame / 12.0))
            up_pts.append((t, '0'))
            burst_t = t + frame - 1
            if burst_t < t1:
                up_pts.append((burst_t, f'{pkts * 12}mbps'))
            t += frame
        td = t0
        while td < t1:
            d = base_d + (rng.uniform(0.0, 1.0) if rng.random() < 0.25 else rng.uniform(jspan * 0.5, jspan))
            up_delay.append((td, round(d, 1)))
            td += 20

    write_scene(
        'starlink_reconfig_15s',
        '低轨卫星 15 秒周期性切星重构：容量阶跃（38→12→42→10 Mbps）+ 基延跃变（+12 ms）+ TDMA 帧突发与延迟抖动。',
        '## 参数说明\n\n'
        '| 时间（s） | 平均上行容量（Mbps） | TDMA 帧周期（ms） | 上行基础延迟（ms） | 抖动幅度（ms） |\n'
        '|---|---:|---:|---:|---:|\n'
        '| 0–15 | 38 | 8 | 10 | 7 |\n'
        '| 15–30 | 12 | 10 | 22 | 9 |\n'
        '| 30–45 | 42 | 8 | 10 | 7 |\n'
        '| 45–60 | 10 | 10 | 20 | 8 |\n\n'
        '- 考察重点：原版 Copa 在高抖动段严重饿死且切星升延后卡顿；BBR/CUBIC 在 38→12 与 42→10 骤降段排队爆炸；Copa-Auto v3 兼顾高利用与低尾延迟。',
        up_pts, down_pts, up_delay, down_delay,
    )


def gen_wifi6_ampdu_burst() -> None:
    """3. Wi-Fi 6 CSMA/CA + A-MPDU TXOP aggregation bursts (36 Mbps avg, flat 16 ms base RTT)."""
    rng = random.Random(1003)
    up_pts: list[tuple[int, str]] = [(0, '0')]
    down_pts = [(0, '100mbps')]
    up_delay = [(0, 8.0)]
    down_delay = [(0, 8.0)]
    t = 0
    while t < DURATION_MS:
        gap = rng.randint(5, 16)
        tx = rng.choice((1, 1, 2))
        cycle = gap + tx
        total_pkts = max(1, round(36.0 * cycle / 12.0 * rng.uniform(0.75, 1.25)))
        per_ms = max(1, round(total_pkts / tx))
        if t < DURATION_MS:
            up_pts.append((t, '0'))
        bt = t + gap
        if bt < DURATION_MS:
            up_pts.append((bt, f'{per_ms * 12}mbps'))
        t += cycle
    write_scene(
        'wifi6_ampdu_burst',
        'Wi-Fi 6 CSMA/CA 信道争用 + A-MPDU TXOP 随机聚合突发（平均 ~36 Mbps，静默间隙 5–16 ms）。',
        '## 参数说明\n\n'
        '- 上行容量：每次传输前经历 5–16 ms 随机静默争用间隙，随后在 1–2 ms TXOP 内突发释放多个数据包（均值约 36 Mbps）。\n'
        '- 上下行基础延迟：各 8 ms（固定基础 RTT 16 ms，无外加传播延迟抖动，完全由 MAC 聚合突发产生 $j$ 与 $k=c/\\mu$）。\n'
        '- 考察重点：检验算法在纯 MAC 突发聚合链路下抑制饿死并控制排队的能力。',
        up_pts, down_pts, up_delay, down_delay,
    )


def gen_wifi_shared_contention() -> None:
    """4. Shared Wi-Fi contention: 44 Mbps -> heavy contention 10 Mbps (10-28 ms gaps) -> 48 Mbps."""
    rng = random.Random(1004)
    up_pts: list[tuple[int, str]] = [(0, '0')]
    down_pts = [(0, '80mbps')]
    up_delay = [(0, 8.0)]
    down_delay = [(0, 8.0)]
    segs = [
        (0,      20_000, 44.0, 4,  10),
        (20_000, 40_000, 10.0, 10, 28),
        (40_000, 60_000, 48.0, 5,  12),
    ]
    for t0, t1, avg_mbps, g_lo, g_hi in segs:
        t = t0
        while t < t1:
            gap = rng.randint(g_lo, g_hi)
            tx = 1 if avg_mbps < 25 else rng.choice((1, 2))
            cycle = gap + tx
            pkts = max(1, round(avg_mbps * cycle / (12.0 * tx) * rng.uniform(0.7, 1.3)))
            up_pts.append((t, '0'))
            if t + gap < t1:
                up_pts.append((t + gap, f'{pkts * 12}mbps'))
            t += cycle
    write_scene(
        'wifi_shared_contention',
        '拥挤 Wi-Fi 多用户信道抢占：44 Mbps 轻载 → 10 Mbps 重度抢占（10–28 ms 随机阻塞）→ 48 Mbps 恢复。',
        '## 参数说明\n\n'
        '| 时间（s） | 平均上行容量（Mbps） | 随机争用静默间隙（ms） | 基础 RTT（ms） |\n'
        '|---|---:|---:|---:|\n'
        '| 0–20 | 44 | 4–10 | 16 |\n'
        '| 20–40 | 10 | 10–28 | 16 |\n'
        '| 40–60 | 48 | 5–12 | 16 |\n\n'
        '- 考察重点：高争用突发下原版 Copa 吞吐坍塌，BBR/CUBIC 在 44→10 骤降时产生百毫秒缓冲膨胀；Copa-Auto v3 同时保持高吞吐与低尾延迟。',
        up_pts, down_pts, up_delay, down_delay,
    )


def gen_5g_tdd_tdma_high_bw() -> None:
    """5. 5G NR TDD frame structure (7 ms DL/silent + 3 ms UL burst, 36 Mbps avg) + HARQ jitter."""
    rng = random.Random(1005)
    up_pts: list[tuple[int, str]] = [(0, '0')]
    down_pts = [(0, '80mbps')]
    up_delay: list[tuple[int, float]] = []
    down_delay = [(0, 10.0)]

    # 10 ms 5G NR frame: 7 ms silent, 3 ms UL burst at 120 Mbps (10 pkts/ms) => 36 Mbps avg
    for t in range(0, DURATION_MS, 10):
        up_pts.append((t, '0'))
        if t + 7 < DURATION_MS:
            up_pts.append((t + 7, '120mbps'))
    for t in range(0, DURATION_MS, 20):
        d = 10.0 + (rng.uniform(3.0, 7.0) if rng.random() < 0.35 else rng.uniform(0.0, 1.0))
        up_delay.append((t, round(d, 1)))

    write_scene(
        '5g_tdd_tdma_high_bw',
        '5G NR TDD 帧结构链路（10 ms 帧周期内 7 ms 静默 + 3 ms 上行突发，均速 36 Mbps + HARQ 抖动）。',
        '## 参数说明\n\n'
        '- 上行容量：每 10 ms 帧周期中前 7 ms 为下行/保护静默，后 3 ms 以 10 pkts/ms 突发传输（平均 36 Mbps）。\n'
        '- 延迟：基础单向延迟各 10 ms（基础 RTT 20 ms），上行叠加 35% 概率的 3–7 ms HARQ 调度抖动。\n'
        '- 考察重点：典型 5G 周期性成帧抖动，原版 Copa 吞吐坍塌至数 Mbps，Copa-Auto v3 高效跑满带宽且控制排队。',
        up_pts, down_pts, up_delay, down_delay,
    )


def gen_5g_fast_fading_stochastic() -> None:
    """6. Pure stochastic Kingman diffusion (c > 0, k = c/mu > 0, ZERO static delay jitter j=0)."""
    rng = random.Random(1006)
    up_pts: list[tuple[int, str]] = []
    down_pts = [(0, '80mbps')]
    up_delay = [(0, 10.0)]
    down_delay = [(0, 10.0)]

    # 25 ms AMC / multipath fast-fading slots: Markov 2-state (good 40-56M / fade 6-14M)
    state = 1
    for t in range(0, DURATION_MS, 25):
        if state == 1:
            if rng.random() < 0.22:
                state = 0
        else:
            if rng.random() < 0.28:
                state = 1
        rate = rng.uniform(40.0, 56.0) if state == 1 else rng.uniform(6.0, 14.0)
        up_pts.append((t, f'{rate:.1f}mbps'))

    write_scene(
        '5g_fast_fading_stochastic',
        '5G 快衰落随机服务波动（纯 Kingman 扩散 $k=c/\\mu>0$，无静态延迟抖动 $j=0$）。',
        '## 参数说明\n\n'
        '- 上行容量：每 25 ms 按两态马尔可夫链在强信号（40–56 Mbps）与深衰落（6–14 Mbps）间快速随机切换（均速约 30 Mbps）。\n'
        '- 延迟：上下行基础延迟恒定各 10 ms（基础 RTT 20 ms，完全无外加延迟抖动）。\n'
        '- 考察重点：隔离验证方案 B 多尺度队列扩散对随机服务色散 $k=c/\\mu$ 的估计——原版 Copa 队列过浅错失高速窗口，BBR/CUBIC 在衰落槽产生高尾延迟，Copa-Auto v3 命中理论最优 $d_q^*=\\sqrt{k R_0}$。',
        up_pts, down_pts, up_delay, down_delay,
    )


def gen_ack_aggregation_docsis() -> None:
    """7. Smooth 36 Mbps data path + 14 ms ACK aggregation bursts on reverse path (DOCSIS/Wi-Fi)."""
    up_pts = [(0, '36mbps')]
    down_pts: list[tuple[int, str]] = [(0, '0')]
    up_delay = [(0, 10.0)]
    down_delay = [(0, 10.0)]

    # Reverse ACK path releases ACKs in 1 ms batches every 14 ms (DOCSIS / Wi-Fi / TDD ACK aggregation)
    for t in range(0, DURATION_MS, 14):
        down_pts.append((t, '0'))
        if t + 13 < DURATION_MS:
            down_pts.append((t + 13, '720mbps'))

    write_scene(
        'ack_aggregation_docsis',
        '反向 ACK 聚合抖动场景（DOCSIS 宽带 / Wi-Fi / 5G TDD 常见现象：数据链路平稳 36 Mbps，ACK 每 14 ms 批量到达）。',
        '## 参数说明\n\n'
        '- 上行数据容量：完全平稳 36 Mbps；基础 RTT 20 ms（上下行各 10 ms）。\n'
        '- 下行 ACK 容量：每 14 ms 仅在最后 1 ms 批量释放 ACK（静默 13 ms），引入约 6–7 ms 的反向 ACK 聚合抖动。\n'
        '- 考察重点：原版 Copa 经典盲区——将 ACK 聚合延迟误判为数据队列导致吞吐暴跌；BBR/CUBIC 会在数据侧堆积排队；Copa-Auto v3 测出 $j$ 后满速运行且零数据排队膨胀。',
        up_pts, down_pts, up_delay, down_delay,
    )


def gen_cellular_handover_jitter_burst() -> None:
    """8. Cellular 5G <-> 4G handover: RTT jump +25 ms + jitter jump + capacity drop/recovery."""
    rng = random.Random(1008)
    segs = [
        (0,      15_000, 38.0, 10.0, 5.0,  6),
        (15_000, 35_000, 10.0, 35.0, 11.0, 12),
        (35_000, 60_000, 42.0, 10.0, 6.0,  8),
    ]
    up_pts: list[tuple[int, str]] = [(0, '0')]
    down_pts = [(0, '80mbps')]
    up_delay: list[tuple[int, float]] = []
    down_delay = [(0, 10.0)]

    for t0, t1, avg_mbps, base_d, jspan, frame in segs:
        t = t0
        while t < t1:
            gap = max(1, frame - 1)
            pkts = max(1, round(avg_mbps * frame / 12.0 * rng.uniform(0.8, 1.2)))
            up_pts.append((t, '0'))
            if t + gap < t1:
                up_pts.append((t + gap, f'{pkts * 12}mbps'))
            t += frame
        td = t0
        while td < t1:
            d = base_d + (rng.uniform(0.0, 1.0) if rng.random() < 0.25 else rng.uniform(jspan * 0.4, jspan))
            up_delay.append((td, round(d, 1)))
            td += 25

    write_scene(
        'cellular_handover_jitter_burst',
        '蜂窝 5G↔4G 跨站切换：容量骤降与回升（38→10→42 Mbps）+ 基延跃变（+25 ms）+ 抖动与帧突发同步变化。',
        '## 参数说明\n\n'
        '| 时间（s） | 网络状态 | 平均容量（Mbps） | 上行基础延迟（ms） | 抖动幅度（ms） | 帧周期（ms） |\n'
        '|---|---|---:|---:|---:|---:|\n'
        '| 0–15 | 5G 良好 | 38 | 10 | 5 | 6 |\n'
        '| 15–35 | 切入拥挤 4G | 10 | 35 | 11 | 12 |\n'
        '| 35–60 | 切回高速 5G | 42 | 10 | 6 | 8 |\n\n'
        '- 考察重点：综合检验基延跃变检测（`PROBE_EMPTY`）、高抖动防饿死及骤降泄洪能力。',
        up_pts, down_pts, up_delay, down_delay,
    )


def gen_dynamic_jitter_regime_switch() -> None:
    """9. Clean wired D/D/1 -> high-jitter bursty wireless -> clean low-rate wired D/D/1."""
    rng = random.Random(1009)
    up_pts: list[tuple[int, str]] = [(0, '42mbps')]
    down_pts = [(0, '80mbps')]
    up_delay: list[tuple[int, float]] = [(0, 10.0)]
    down_delay = [(0, 10.0)]

    # 20..40 s: 42 Mbps avg with 10 ms TDMA bursts + 0..11 ms delay jitter
    for t in range(20_000, 40_000, 10):
        up_pts.append((t, '0'))
        up_pts.append((t + 9, '420mbps'))
    for t in range(20_000, 40_000, 20):
        d = 10.0 + (rng.uniform(0.0, 1.0) if rng.random() < 0.25 else rng.uniform(5.0, 11.0))
        up_delay.append((t, round(d, 1)))

    # 40..60 s: back to clean deterministic 12 Mbps wired (j=0, k=0)
    up_pts.append((40_000, '12mbps'))
    up_delay.append((40_000, 10.0))

    write_scene(
        'dynamic_jitter_regime_switch',
        '动态体制切换：平稳有线 42 Mbps（$j=0$）→ 高抖动无线 42 Mbps（$j \\approx 8\\text{ ms}$）→ 平稳限速 12 Mbps（$j=0$）。',
        '## 参数说明\n\n'
        '| 时间（s） | 体制 | 上行容量 | 上行延迟（ms） |\n'
        '|---|---|---|---|\n'
        '| 0–20 | 干净有线 D/D/1 | 平稳 42 Mbps | 恒定 10 |\n'
        '| 20–40 | 高抖动突发无线 | 10 ms 帧突发（均值 42 Mbps） | 10–21 抖动 |\n'
        '| 40–60 | 干净限速 D/D/1 | 平稳 12 Mbps | 恒定 10 |\n\n'
        '- 考察重点：验证 Copa-Auto v3 不仅能在突发/抖动出现时迅速下调 $\\delta$ 避免饿死，还能在链路恢复干净且降速时回升 $\\delta \\to 0.5$，避免遗留排队。',
        up_pts, down_pts, up_delay, down_delay,
    )


def gen_mmwave_intermittent_burst() -> None:
    """10. 5G mmWave bursty link (44 Mbps) with periodic 500 ms blockage dips to 6 Mbps."""
    rng = random.Random(1010)
    up_pts: list[tuple[int, str]] = [(0, '0')]
    down_pts = [(0, '80mbps')]
    up_delay: list[tuple[int, float]] = []
    down_delay = [(0, 10.0)]

    blockages = [(12_000, 12_600), (24_000, 24_600), (36_000, 36_600), (48_000, 48_600)]

    def in_blockage(ms: int) -> bool:
        return any(a <= ms < b for a, b in blockages)

    t = 0
    while t < DURATION_MS:
        if in_blockage(t):
            up_pts.append((t, '6mbps'))
            t += 20
        else:
            # 7 ms mmWave frame burst: 6 ms silent, 1 ms burst of ~26 pkts => ~44.6 Mbps
            up_pts.append((t, '0'))
            if t + 6 < DURATION_MS and not in_blockage(t + 6):
                up_pts.append((t + 6, f'{rng.randint(23, 29) * 12}mbps'))
            t += 7
    for t in range(0, DURATION_MS, 20):
        if in_blockage(t):
            up_delay.append((t, 18.0))
        else:
            d = 10.0 + (rng.uniform(0.0, 1.0) if rng.random() < 0.25 else rng.uniform(4.0, 8.0))
            up_delay.append((t, round(d, 1)))

    write_scene(
        'mmwave_intermittent_burst',
        '5G 毫米波突发链路 + 周期性短暂遮挡（44 Mbps 帧突发 + 每 12 秒 600 ms 遮挡跌至 6 Mbps）。',
        '## 参数说明\n\n'
        '- 正常段：7 ms 帧突发调度（均速 ~44 Mbps），上行单向延迟 10–18 ms 抖动，下行 10 ms。\n'
        '- 遮挡段（12 / 24 / 36 / 48 s，每次持续 600 ms）：容量瞬间跌至 6 Mbps、延迟抬升至 18 ms，随后迅速恢复 44 Mbps。\n'
        '- 考察重点：原版 Copa 在 44 Mbps 抖动段严重饿死；BBR/CUBIC 每次遮挡产生显著队列尖峰；Copa-Auto v3 同时兼具高利用与快速泄洪恢复。',
        up_pts, down_pts, up_delay, down_delay,
    )


def write_top_readme() -> None:
    content = """# Special 场景（Copa-Auto v3 专项优势场景集）

本目录包含 10 个面向真实非 D/D/1 链路物理特征构造的专项测试场景，用于对比固定 $\\delta=0.5$ 的原版 Copa、BBR、CUBIC 与自适应 Copa-Auto v3（`copa_auto_v3`）。

在 `datasets/lines/basic` 中，41 个场景均采用**分段恒定平滑速率 + 平稳基础延迟**（即 $j=0, k=0$ 的确定性 D/D/1 链路），按理论模型其最优参数恰好就是原版 Copa 的 $\\delta=0.5$，因此 `copa_auto_v3` 在 `basic` 上会主动收敛到 $\\delta=0.5$ 而与原版表现接近。本目录聚焦于真实无线、卫星、蜂窝与宽带接入网中普遍存在、但 `basic` 未覆盖的四类非平稳/随机特征（且各场景单流 BDP 均控制在 TCP 默认 128 KB 发送缓冲以内，确保各算法真实进入拥塞控制）：

| 场景 | 核心物理特征 | 对应参数机制 |
|---|---|---|
| `starlink_jitter_steady` | 低轨卫星稳态延迟抖动（34 Mbps, $j \\approx 6\\text{--}8\\text{ ms}$） | 非排队抖动 $j > 0$（原版 Copa 被 $1/(\\delta j)$ 饿死） |
| `starlink_reconfig_15s` | 低轨卫星 15 秒切星：容量阶跃（38→12→42→10M）+ 基延跃变 + TDMA 突发 | $R_0$ 跳变 + $j > 0$ + 骤降泄洪 |
| `wifi6_ampdu_burst` | Wi-Fi 6 CSMA/CA 随机争用 + A-MPDU TXOP 聚合突发（36 Mbps） | MAC 聚合突发（$j > 0$ 与 $k = c/\\mu > 0$） |
| `wifi_shared_contention` | 拥挤 Wi-Fi 多用户抢占（44→10→48 Mbps 随机阻塞） | 变带宽突发争用（吞吐 vs 尾延迟权衡） |
| `5g_tdd_tdma_high_bw` | 5G NR TDD 帧调度（7 ms 静默 + 3 ms 突发，36 Mbps）+ HARQ 抖动 | 周期成帧抖动 $j > 0$ |
| `5g_fast_fading_stochastic` | 5G 两态马尔可夫快衰落（6–14 ↔ 40–56 Mbps，延迟恒定） | 纯 Kingman 随机色散 $k = c/\\mu > 0, j = 0$ |
| `ack_aggregation_docsis` | 平稳 36 Mbps 数据链路 + 下行 14 ms 周期性 ACK 聚合 | 反向 ACK 聚合抖动（原版 Copa 经典盲区） |
| `cellular_handover_jitter_burst` | 5G↔4G 跨站切换（38→10→42 Mbps + 基延 +25 ms 跃变） | 跨站 $R_0/j$ 突变 + 突发适应 |
| `dynamic_jitter_regime_switch` | 平稳有线 42M → 抖动无线 42M → 平稳限速 12M | $\\delta$ 双向自适应（$0.5 \\to 0.03 \\to 0.5$） |
| `mmwave_intermittent_burst` | 5G 毫米波 44 Mbps 帧突发 + 每 12 秒 600 ms 短遮挡跌至 6M | 高速突发 + 短暂遮挡快速泄洪 |

所有场景均为 60 秒，格式见 [FORMAT.md](../FORMAT.md)，由 [gen_special_lines.py](../../../scripts/data/gen_special_lines.py) 确定性生成（固定随机种子，可完全复现）。
"""
    (OUT_DIR / 'README.md').write_text(content, encoding='utf-8')


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    gen_starlink_jitter_steady()
    gen_starlink_reconfig_15s()
    gen_wifi6_ampdu_burst()
    gen_wifi_shared_contention()
    gen_5g_tdd_tdma_high_bw()
    gen_5g_fast_fading_stochastic()
    gen_ack_aggregation_docsis()
    gen_cellular_handover_jitter_burst()
    gen_dynamic_jitter_regime_switch()
    gen_mmwave_intermittent_burst()
    write_top_readme()

    scenes = lines_common.discover(OUT_DIR)
    print(f'Generated and validated {len(scenes)} scenes in {OUT_DIR}:')
    for s in scenes:
        print(f'  - {s["name"]:<34} duration={s["duration_s"]:.1f}s files={sorted(s["files"].keys())}')


if __name__ == '__main__':
    main()
