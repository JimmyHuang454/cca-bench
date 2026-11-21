# ns-3 拥塞控制评测框架（Copa-Auto & 多基线对比）

本目录提供基于 **ns-3** 的高速、确定性多数据集拥塞控制评测环境，直接 `#include` 复用 [../kernel/tcp_copa_auto/copa_auto_core.h](../kernel/tcp_copa_auto/copa_auto_core.h) 中的纯 64 位定点整数状态机，确保 **ns-3 仿真与 Linux 内核模块完全同源、零逻辑漂移**。

相比实时 Mahimahi 评测，本框架在保留真实 TCP（SACK + PRR + Pacing + RTO）与 5 ms 级蜂窝/卫星轨迹还原的前提下，运行速度提升 **10–30 倍**，且支持在 macOS / Linux 本地及 GitHub Actions 上免内核权限并发运行。

---

## 1. 仓库目录结构

```text
copa-delta-repro/
├── kernel/                 # 纯 Linux 内核 C 模块（tcp_copa_auto/, tcp_leocc/）
├── ns3/                    # ns-3 仿真引擎、overlay、论文遗留脚本（scripts/paper/）
│   ├── ns3-overlay/        # TcpCopaKernel, TcpMortiseCopa, TcpBbrV2/V3 及 3 个仿真引擎
│   └── ns-3/               # ns-3 源码与构建目录（gitignored）
├── datasets/               # 统一数据集目录
│   ├── lines/              # 统一场景 JSON：wired/, wifi/, 4g/, mahimahi/(gitignored)
│   ├── traces/             # Mahimahi/Lumos5G/NYC/LeoCC 原始轨迹缓存（gitignored）
│   └── workloads/          # Mortise .wk 应用负载（app/websearch/vl2/pageload.wk）
├── scripts/                # 分类脚本：bench/, data/, eval/, env/
└── results/                # 统一输出（gitignored）：cache/, ns3/<shard>/, kernel/, reports/
```

---

## 2. 环境初始化与编译

已全部封装进根目录 [../Makefile](../Makefile) 与 [Makefile](Makefile)，在**仓库根目录**或 `ns3/` 目录下均可直接执行：

### 2.1 首次初始化 ns-3（含 `lte` + `wifi` 物理层模块的 `optimized` 构建）

```bash
make setup-bench
```

等价于拉取 ns-3 基线 commit（`84a73f9c00abf5e5b8aa676e1775261fb717c3ac`）、将最新 [../kernel/tcp_copa_auto/copa_auto_core.h](../kernel/tcp_copa_auto/copa_auto_core.h) 同步到 `ns3-overlay/src/internet/model/` 并覆盖进 `ns3/ns-3`，以 `optimized` 模式编出全部 3 个仿真引擎：
- `ns3-dev-tcp-capacity-trace-optimized`：有线链路（`wired`）+ 精确 1 ms `mm-link` 门控（`4g`/`5g`/`leo`/`synth`）+ Bulk / `.wk` 负载
- `ns3-dev-tcp-lte-highspeed-handover-optimized`：3GPP 4G 物理层 + 14 条实景路线（`datasets/lines/4g/*.json`）+ Bulk / `.wk` 负载
- `ns3-dev-tcp-wifi-bench-optimized`：原生 802.11ax Wi-Fi MAC/PHY + CSMA/CA 竞争（`datasets/lines/wifi/*.json`）+ Bulk / `.wk` 负载

### 2.2 修改 `copa_auto_core.h` 后的快速测试与增量重编译

```bash
make test-core     # 跑 C 核心单元测试（kernel/tcp_copa_auto）
make build-bench   # 同步最新 C 核心与 overlay 并增量重编 3 个仿真引擎
```

---

## 3. 本地运行指南

### 3.1 通过 Makefile 一键跑分片并自动生成汇总报告（推荐）

`make bench-*` 会自动完成：**确认 `.wk` 负载 →（若为 4G/Wi-Fi PHY 自动探测并缓存信道容量）→ 并发运行 ns-3 评测 → 调用 `scripts/eval/eval_report.py` 在 `results/ns3/<shard>/` 下生成并打印 `comparison_summary.md`**。

```bash
# 查看所有目标和默认参数
make help

# 统一场景与链路评测（支持在仓库根目录或 ns3/ 下执行）
make bench-basic        # basic A-E 动态场景，默认 bulk + app
make bench-wired        # 有线链路（datasets/lines/wired/*.json：home, wan, dc, step）
make bench-wifi         # 原生 802.11ax Wi-Fi 物理层（datasets/lines/wifi/*.json：home, wall, cafe）
make bench-lines-quick  # 3GPP 4G 物理层快速子集（city-street, train-tunnel, highway-congestion）
make bench-lines        # 全部 14 条 3GPP 4G 物理层路线（datasets/lines/4g/*.json）
make bench-synth        # 合成轨迹（2 条，几秒出结果）
make bench-4g           # 4 条真实 Mahimahi 4G 驾车轨迹（兼容别名 bench-lte）
make bench-5g           # Lumos5G 5G 轨迹
make bench-leo          # LeoCC Starlink 卫星切换轨迹（自动 115 s + 10 ms 时变延迟）
make bench-datasets     # 6 大类数据集核心代表
make bench-full         # 全量 44 个分片（329 条轨迹）

# 将 Mahimahi 轨迹转换为统一 lines JSON（输出到 datasets/lines/mahimahi/）
make convert-mahimahi SHARD=4g,5g,leo,synth

# 汇总全部 ns-3 + Linux kernel 结果到 results/reports/all_summary.md
make report
```

**常用参数覆写示例**（支持所有变量任意组合）：

```bash
# 自定义分片、算法、负载集、并发数
make bench \
  SHARD=wired,wifi,4g \
  CCS=copa_auto_nc,copa_auto_nc_j0,mortise_copa,bbr,bbr3,cubic \
  WORKLOADS=bulk,app,websearch \
  REPEATS=1

# 手动固定 jitter_us（如 3 ms，跳过 MEASURE_J 探测）
make bench-4g JITTER_US=3000

# 调节快/慢时钟粒度（默认 CLOCK_MS=1.0, COPA_CLOCK_US=1）
make bench-wired CLOCK_MS=0.1 COPA_CLOCK_US=1000

# 仅重新汇总已有结果目录
make summary OUT_DIR=results/ns3/wired

# 清理评测输出
make clean-bench   # 清理 results/ns3
make clean-all     # 清理 results/{ns3,kernel,reports,cache}
```

> `SHARD` 支持单个分片、逗号拼接（如 `wired,wifi,lines-quick,4g,leo`）及复合别名：`full`、`datasets`、`rct-datasets`、`lumos-5g-all`、`lumos-4g-all`、`lumos-all`。

### 3.2 单次调试运行 `tcp-capacity-trace`

```bash
ns3/ns-3/build/scratch/ns3-dev-tcp-capacity-trace-optimized \
  --cc=copa_auto_nc \
  --downTrace=datasets/traces/mahimahi/ATT-LTE-driving.down \
  --upTrace=datasets/traces/mahimahi/ATT-LTE-driving.up \
  --baseRttMs=20 \
  --queueBytes=2000000 \
  --stopTime=60 \
  --warmup=5 \
  --clockGranularityMs=1.0 \
  --copaClockUs=1 \
  --jitterUs=-1 \
  --evalJson=1
```

### 3.3 时钟粒度与时间参数（默认值一览）

| 参数（`Makefile` / `run_ns3_suite.py` / ns-3 CLI） | 默认值 | 含义与作用 |
| :--- | :--- | :--- |
| ns-3 内部事件调度精度（固定） | **`1 ns`** | `ns3::Time::NS`，ns-3 默认离散事件时间分辨率 |
| Mahimahi 链路发包门控（固定） | **`1 ms`（精确 `mm-link`）** | `MahimahiGate` 逐毫秒精确放行 1500B 包，与真实 Linux `mm-link` 1:1 等价 |
| `CLOCK_MS` / `--clock-ms` / `--clockGranularityMs` | **`1.0 ms`** | `ns3::TcpSocketBase::ClockGranularity`，TCP 协议栈 RTO/RTT 定时器时钟（对标 Linux `HZ=1000`，可调 `0.1` 快时钟 / `4.0` 慢时钟） |
| `COPA_CLOCK_US` / `--copa-clock-us` / `--copaClockUs` | **`1 µs`** | 喂给 `TcpCopaKernel` / `TcpMortiseCopa` 的时间戳与 RTT 量化粒度（`1` = 微秒高精度，`1000` / `4000` = 模拟 1–4 ms 慢时钟） |
| `V2_MEASUREMENT_RTTS` / `--v2-measurement-rtts` | **`16`** | `copa_auto_v2` 的测量周期长度（单位：SRTT）；更改该值时请使用不同的 `OUT_DIR`，避免 `RESUME=1` 复用旧结果 |
| `--bulk-secs` / `--stopTime` | **`60 s`**（`leo` 为 **`115 s`**） | Bulk 流仿真总时长 |
| `--warmup-s` / `--warmup` | **`5.0 s`** | Bulk 指标统计前跳过的预热时长 |

---

## 4. 在 GitHub Actions 上运行（`ns-3 CCAs Bench`）

Workflow 位于 [.github/workflows/ns3-bench.yml](../.github/workflows/ns3-bench.yml)：
1. 打开 GitHub 仓库 **Actions** → 选择 **ns-3 CCAs Bench** → 点击 **Run workflow**；
2. 首次运行会自动拉取并缓存 `ns-3` 6 模块 `optimized` 构建，后续缓存命中后编译仅需几秒；
3. 运行完成后在 **Summary** 查看完整 Markdown 报表，或在 **Artifacts** 下载 `ns3-comparison-summary`。

| Input 参数 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `mode` | `quick` | 评测模式（`quick` / `rct` / `rct-datasets` / `smoke` / `lte` / `nyc-subway` / `lumos-5g` / `lumos-5g-all` / `lumos-4g-all` / `lumos-all` / `leo` / `datasets` / `full`） |
| `theta` | `0.98` | `copa_auto_theta<X>` 的吞吐权重（支持逗号分隔多值，如 `0.9,0.98`） |
| `jitter_us` | `-1` | `-1` = 自动探测 $j$；`>= 0` = 手动固定微秒值（如 `0` / `3000`，禁用主动 `MEASURE_J` 探测） |
| `repeats` | `1` | 每个配置的时间偏移重复次数 |
| `parallel` | 空（自动 CPU 核数） | 单 runner 最大并发 ns-3 进程数 |
| `queues` | 空（按数据集默认） | 覆写队列大小，如 `150KB,2MB` 或 `2MB` |
| `rct_workloads` | `app` | 要跑的请求负载，逗号分隔（`app,websearch,vl2,pageload`） |
| `ns3_ccs` | 空（按 `mode` 默认） | 覆写算法列表（逗号分隔，见第 5 节） |

---

## 5. 可选拥塞控制算法（19 种）

### 5.1 Copa 系列（共用 `kernel/tcp_copa_auto/copa_auto_core.h`）

| 算法名 | 特性说明 |
| :--- | :--- |
| `copa_auto` | 自适应 $\delta$（$\theta=0.50$），开启竞争模式切换（`mode_switching=true`），支持 `--jitterUs` |
| `copa_auto_nc` | 自适应 $\delta$（$\theta=0.50$），**关闭竞争模式切换**（默认模式消融对照），支持 `--jitterUs` |
| `copa_auto_nc_j0` | `copa_auto_nc` 且固定 `jitter_us=0`（完全关闭 $j$ 探测，启用速率域 $\rho_{\text{bw}}$ 兜底） |
| `copa_auto_nc_j3` | `copa_auto_nc` 且固定 `jitter_us=3000`（3 ms 手动固定抖动底噪，无 $j$ 探测） |
| `copa_auto_theta<X>` | 自定义 $\theta \in [0.01, 0.995]$，开启竞争模式（如 `copa_auto_theta0.9`、`copa_auto_theta0.98`） |
| `copa_auto_nc_theta<X>` | 自定义 $\theta \in [0.01, 0.995]$，关闭竞争模式 |
| `copa_orig` | 原版固定 $\delta=0.50$ Copa（开启竞争模式） |
| `mortise_copa` | **1:1 移植自官方 `BobAnkh/Mortise` `mortise_copa.bpf.c`**（[mortise_copa_ns3.h](mortise_copa_ns3.h)，$\delta=0.04$、`srtt/2` 更新、TCPCoop/Loss 切换） |

### 5.2 ns-3 原生基线算法

| 算法名 | ns-3 实现类 | 定位与场景 |
| :--- | :--- | :--- |
| `bbr` | `ns3::TcpBbr` | Google BBRv1 |
| `bbr2` | `ns3::TcpBbrV2` | Google BBRv2（来自 `ns3-overlay`） |
| `bbr3` | `ns3::TcpBbrV3` | Google BBRv3（来自 `ns3-overlay`） |
| `cubic` | `ns3::TcpCubic` | Linux 默认丢包型基线 |
| `vegas` | `ns3::TcpVegas` | 经典延迟型基线 |
| `veno` | `ns3::TcpVeno` | 无线混合丢包+排队估计 |
| `yeah` | `ns3::TcpYeah` | YeAH-TCP 快慢双模混合 |
| `illinois` | `ns3::TcpIllinois` | 延迟调制 AIMD |
| `westwood` | `ns3::TcpWestwoodPlus` | 基于带宽估计（无线丢包） |
| `hybla` | `ns3::TcpHybla` | 高 RTT 补偿（卫星 / LEO） |
| `reno` | `ns3::TcpLinuxReno` | 经典 NewReno |

---

## 6. 两大类评测场景与数据集总览

本框架包含**两类独立但互补的场景体系**，均共用最新 `TcpCopaKernel`（`copa_auto_core.h`）、1:1 官方 `TcpMortiseCopa` 与全部 ns-3 基线算法：

### 6.1 移植自 Mahimahi / 公开轨迹数据集（`tcp-capacity-trace`，精确 1 ms `mm-link` 门控）

通过 `make bench-*`（[run_ns3_suite.py](../kernel/scripts/run_ns3_suite.py)）驱动，全部按真实 Mahimahi 逐毫秒发包机会（1500B/slot）重放，支持 Bulk 与 4 种 `.wk` 应用负载：

| 数据集（6 大类，329 条轨迹） | 场景特征 | 对应 `--shard` / `make` 目标 | 默认浅 / 深队列 | 默认 Bulk 时长 |
| :--- | :--- | :--- | :--- | :--- |
| **Mahimahi Cellular 4G** | 4G 驾车、高抖动（4 条：`ATT-2016`/`ATT`/`TMobile`/`Verizon`） | `4g`（兼容 `lte`，`make bench-4g`） | `150 KB` / `2 MB` | `60 s` |
| **NYC 4G（NYU）** | 城市定点 5 条 / 步行 12 条 / 驾车 6 条（共 23 条） | `nyc-stat`、`nyc-walk-1..3`、`nyc-drive-1..2` | `150 KB` / `2 MB` | `60 s` |
| **NYC 4G Subway（2018）** | 地铁场景无/有交叉流（2 条） | `nyc-subway`（`nyc-rct` 精选 3 条） | `150 KB` / `2 MB` | `60 s` |
| **Lumos5G 5G（SIGCOMM'21）** | 商业 5G 毫米波步行/驾车、超高带宽（全集 121 条） | `lumos-5g`（2 条）、`lumos-rct`（1 条）、`lumos-5g-s1-of-16..s16-of-16` | `2 MB` / `16 MB` | `60 s` |
| **Lumos5G 4G（SIGCOMM'21）** | 商业 4G 轨迹（全集 175 条） | `lumos-4g-s1-of-18..s18-of-18` | `150 KB` / `2 MB` | `60 s` |
| **LeoCC Starlink** | LEO 卫星 15 s 级切换、保序 10 ms 时变延迟 + 带宽（2 条官方示例；支持 `--shard local-leo` 扩展） | `leo`（`make bench-leo`）/ `local-leo` | `2 MB` / `75 MB` | `115 s` |
| **合成轨迹（Synth）** | 稳态 48 Mbps 与阶跃带宽（2 条：`synth-48mbps`、`synth-step`） | `synth`（`make bench-synth`） | `150 KB` / `2 MB` | `60 s` |

> GitHub Actions 的 `mode=full` 与本地的 `make bench-full`（`--shard full`）均会自动编排全部 **44 个分片（329 条轨迹）** 并行执行。

---

### 6.2 基于 `lines` JSON 格式定义的 3GPP 4G 物理层路线与合成场景

定义文件位于 [ns3-overlay/scratch/lines/](ns3-overlay/scratch/lines/)（格式规范见 [FORMAT.md](ns3-overlay/scratch/lines/FORMAT.md)），由 `tcp-lte-highspeed-handover`（完整 3GPP 4G 无线物理层 + 基站切换 + 多用户干扰 + 建筑物遮挡）和 `tcp-capacity-trace` 驱动：

| `make` 目标 | 底层仿真器 | 场景来源与内容 |
| :--- | :--- | :--- |
| **`make lines`** | `tcp-lte-highspeed-handover` | **14 条 `lines` JSON 真实生活路线**（单流 + 2/4 流多流竞争）：<br>• 步行/运动：`walk-line`（`walk.json`）、`run-line`（`run.json`）<br>• 城市交通：`city-drive`（`city-drive.json`）、`city-bus`（`city-bus.json`）、`city-subway`（`city-subway.json`）、`city-elevated`（`city-elevated.json`）<br>• 城市密集区：`city-mall`（`city-mall.json`）、`city-stadium`（`city-stadium.json`）<br>• 乡村/偏远：`rural-road`（`rural-road.json`）、`rural-mountain`（`rural-mountain.json`）、`rural-village`（`rural-village.json`）、`rural-edge`（`rural-edge.json`）<br>• 城际高速：`hwy-line`（`highway.json`）、`hsr-line`（`hsr.json`，300 km/h 高铁） |
| **`make mobile`** | `tcp-lte-highspeed-handover` | 3GPP 4G 高速公路/高铁切换专项 + 2/4 流与背景流公平性竞争（`config/hwy.args`） |
| **`make bench-basic`** | `tcp-capacity-trace` | A–E 带宽、丢包与基础 RTT 动态场景，运行 bulk 和 app workload |
| **`make 4g-paper`**（别名 `make lte`） / **`make leo`** | `tcp-capacity-trace` | 论文原版 1/2/4 流 180 s Mahimahi 4G 与 LeoCC `4.8K.zip` 大规模重放 |

---

## 7. 可选流量负载（5 种 Workloads）

| Workload | 流量特征 | 规模 | 来源 |
| :--- | :--- | :--- | :--- |
| **`bulk`** | 持续饱和流（跳过前 `5.0 s` warmup），测稳态吞吐、排队与 Power | 60 s（`leo` 115 s） | ns-3 `BulkSendApplication` |
| **`app`** | 混合中小对象请求，Mortise 论文原版 RCT 基准 | 92 请求，跨度 149.3 s，58.30 MB | `BobAnkh/Mortise` (`app.wk`) |
| **`websearch`** | DCTCP 重尾分布（大量小查询 + 少数大对象） | 80 请求，跨度 95.9 s，29.10 MB | `gen_workloads.py` (`websearch.wk`) |
| **`vl2`** | VL2 极端双峰重尾（极多超小 RPC + 极少数超大传输） | 80 请求，跨度 89.9 s，10.91 MB | `gen_workloads.py` (`vl2.wk`) |
| **`pageload`** | 突发式 HTTP/3 网页资源簇（成批到达 + 思考间隔） | 80 请求，跨度 48.7 s，19.91 MB | `gen_workloads.py` (`pageload.wk`) |

---

## 8. 评估指标体系

[mahimahi_eval.py](../kernel/scripts/mahimahi_eval.py) 生成的 `comparison_summary.md` 包含三张核心报表：

1. **Bulk — `Norm Power`（↑，无量纲 $\in [0, 1]$）**：
   $$\text{Norm Power} = \text{Util} \cdot \frac{R_0}{R_0 + \overline{d_q}}$$
   同时考核链路利用率与排队延迟控制。
2. **`app.wk` — `FCT Slowdown`（↓，$\ge 1.0\text{x}$）与 `Mortise QoE`（↑）**：
   $$\text{FCT Slowdown} = \exp\!\left(\frac{1}{N}\sum_{i=1}^{N}\ln\max\!\left(\frac{\text{FCT}_i}{R_0 + S_i/C},\; 1\right)\right)$$
   以物理最快完成时间为基准取几何平均慢化比，同时兼顾小文件响应、大文件吞吐与长尾方差；并保留原版 `Mortise QoE` 便于与论文直接对标。
3. **Overall Summary — `Composite Score`（↑，$\in (0, 1]$）**：
   $$\text{Composite Score} = \sqrt{\frac{\text{Norm Power}}{\text{FCT Slowdown}}}$$
   按 `(Trace Group, Queue)` 等权汇总并自动标出每列相对最优值的百分比差距（`Norm Power` / `Composite Score` 标 `-X.X%`，`FCT Slowdown` 标 `+X.X%`）。

### Experimental Copa-Auto v2

`copa_auto_v2` is included in the default `make bench` algorithms. Change the period with `make bench V2_MEASUREMENT_RTTS=8 OUT_DIR=...`. `copa-auto-v2` (or `copa_auto_v2`) selects the independent ns-3 `TcpCopaAutoV2` algorithm. It defaults to no competitive mode, no jitter probing, and a 16-SRTT measurement period. Each period independently finds its minimum RTT and maximum valid per-round delivery rate, derives utilization from the rate ratio, and fits k with a robust weighted absolute-error fit over the current period, downweighting observations near utilization 1. Rounds at utilization 1 are excluded from the fit. Period-local sample coverage, application limitation, and fitting error determine the delta-update gain (0.1–0.5). Periods with fewer than two usable k values do not update delta. Delta uses Q20 numerical representation; its representable positive range is approximately 0.000000954 to 4096, not the v1 algorithmic limits. The first valid RTT starts the first measurement period.

Example (after `make build-bench`):

```sh
ns-3/build/scratch/ns3-dev-tcp-capacity-trace-optimized \
  --ccs=copa-auto-v2 --profile=step --stopTime=20 --prefix=/tmp/copa-v2 \
  --debugMs=1000 --ns3::TcpCopaAutoV2::MeasurementRtts=16
```

Period diagnostics, including sample count, utilization spread, fit error, confidence and gain, are printed to stderr when `--debugMs` is enabled. `MeasurementRtts` accepts 1 through 256; short periods may have insufficient samples for fitting. The maximum delivery rate is an estimate, and the period minimum RTT can include a persistent queue.

`make bench-basic` 从 `datasets/lines/basic/*.json` 读取场景定义，默认运行 A–E、sigma 0.1 和 0.4，使用 2 MB 队列及单条流。可设置 `BASIC_GROUPS=AE BASIC_SIGMAS=0.4 REPEATS=3 WORKLOADS=bulk,app` 筛选已有 JSON；新增波动强度需添加对应 JSON。结果默认位于 `results/ns3/basic/`。运行前修改过算法时先执行 `make build-bench`，并用 `RESUME=0` 重新测评。动态 RTT 场景的排队延迟按 RTT 样本对应的基础 RTT 计算；FCT 基准采用请求开始时的计划基础 RTT 和全程平均容量，属于近似值。未完成全部请求的任务会报告失败。
