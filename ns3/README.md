# ns-3 拥塞控制评测框架（Copa-Auto & 多基线对比）

本目录提供基于 **ns-3** 的高速、确定性多数据集拥塞控制评测环境，直接 `#include` 复用 [../kernel/tcp_copa_auto/copa_auto_core.h](../kernel/tcp_copa_auto/copa_auto_core.h) 中的纯 64 位定点整数状态机，确保 **ns-3 仿真与 Linux 内核模块完全同源、零逻辑漂移**。

相比实时 Mahimahi 评测，本框架在保留真实 TCP（SACK + PRR + Pacing + RTO）与 5 ms 级蜂窝/卫星轨迹还原的前提下，运行速度提升 **10–30 倍**，且支持在 macOS / Linux 本地及 GitHub Actions 上免内核权限并发运行。

---

## 1. 目录结构

```text
ns3/
├── tcp_ccas_bench.cc       # 统一 ns-3 评测入口（Bulk + Mortise .wk + Mahimahi/LeoCC 轨迹）
├── ns-3/                   # ns-3 源码与构建目录（gitignored，由 setup_ns3.sh 或下方命令初始化）
├── ns3-overlay/            # ns-3 定制扩展（含 TcpBbrV2、TcpBbrV3、tcp-capacity-trace 等）
├── setup_ns3.sh            # 原版 ns-3 全量初始化脚本
├── Makefile / run_all.sh   # 原版论文专项实验入口
├── config/ docs/ scripts/  # 原版实验配置与分析脚本
└── traces/ results/ smoke/ # 本地轨迹缓存与输出目录（gitignored）
```

> 配套调度与汇总脚本位于 [../kernel/scripts/](../kernel/scripts/)：`run_ns3_suite.py`、`ci_runner.py`、`gen_workloads.py`、`mahimahi_eval.py`。

---

## 2. 环境初始化与编译

已全部封装进根目录 [../Makefile](../Makefile) 与 [Makefile](Makefile)，在**仓库根目录**或 `ns3/` 目录下均可直接执行：

### 2.1 首次初始化 ns-3（精简 6 模块 `optimized` 构建，推荐）

```bash
make setup-bench
```

等价于拉取 ns-3 基线 commit（`84a73f9c00abf5e5b8aa676e1775261fb717c3ac`）、覆盖 `ns3-overlay/`、同步 `copa_auto_core.h` / `copa_auto_math.h` / `tcp_ccas_bench.cc`，并以 6 模块 `optimized` 编译（速度比 debug 快 3–5 倍）。编译产物路径：`ns3/ns-3/build/scratch/ns3-dev-tcp_ccas_bench-optimized`。

### 2.2 修改 `copa_auto_core.h` 后的快速测试与增量重编译（~1–2 秒）

```bash
make test-core     # 跑 C 核心单元测试（kernel/tcp_copa_auto）
make build-bench   # 同步头文件并增量重编 tcp_ccas_bench
```

---

## 3. 本地运行指南

### 3.1 通过 Makefile 一键跑分片并自动生成汇总报告（推荐）

`make bench` 会自动完成：**生成 `.wk` 负载 → 并发运行 ns-3 评测 → 调用 `mahimahi_eval.py` 输出并打印 `comparison_summary.md`**。

```bash
# 查看所有目标和默认参数
make help

# 快捷目标（支持在仓库根目录或 ns3/ 下执行）
make bench-synth       # 合成轨迹（2 条，几秒出结果）
make bench-lte         # 4 条真实 Mahimahi LTE-driving 轨迹
make bench-leo         # LeoCC Starlink 卫星切换轨迹（自动 115 s + 10ms 时变延迟）
make bench-datasets    # 6 大类数据集核心代表
make bench-full        # 全量 44 个分片（329 条轨迹）
```

**常用参数覆写示例**（支持所有变量任意组合）：

```bash
# 自定义分片、算法、负载集、并发数
make bench \
  SHARD=lte,leo \
  CCS=copa_auto_nc,copa_auto_nc_j0,mortise_copa,bbr,bbr3,cubic \
  WORKLOADS=bulk,app,websearch \
  REPEATS=1

# 手动固定 jitter_us（如 3 ms，跳过 MEASURE_J 探测）
make bench-lte JITTER_US=3000

# 调节仿真时钟与带宽槽粒度（默认 CLOCK_MS=1.0, BIN_MS=5.0）
make bench-synth CLOCK_MS=1.0 BIN_MS=5.0

# 仅重新汇总已有结果目录
make summary OUT_DIR=ns3/results/bench/lte

# 清理评测输出
make clean-bench
```

> `SHARD` 支持单个分片、逗号拼接（如 `lte,leo,nyc-subway`）及复合别名：`full`（全部 44 分片）、`datasets`（6 大类代表）、`rct-datasets`、`lumos-5g-all`（16 分片）、`lumos-4g-all`（18 分片）、`lumos-all`（34 分片）。

### 3.2 单次调试运行 `tcp_ccas_bench`

```bash
ns3/ns-3/build/scratch/ns3-dev-tcp_ccas_bench-optimized \
  --cc=copa_auto_nc \
  --mode=bulk \
  --downTrace=ns3/traces/mahimahi/ATT-LTE-driving.down \
  --upTrace=ns3/traces/mahimahi/ATT-LTE-driving.up \
  --baseRttMs=20 \
  --queueBytes=2000000 \
  --durS=60 \
  --warmupS=5 \
  --binMs=5.0 \
  --clockGranularityMs=1.0 \
  --jitterUs=-1
```

### 3.3 时钟粒度与时间参数（默认值一览）

| 参数（`run_ns3_suite.py` / `tcp_ccas_bench`） | 默认值 | 含义与作用 |
| :--- | :--- | :--- |
| ns-3 内部事件调度精度（固定） | **`1 ns`** | `ns3::Time::NS`，ns-3 默认离散事件时间分辨率 |
| `copa_auto_core.h` 时间戳精度（固定） | **`1 µs`** | `Simulator::Now().GetMicroSeconds()`，与 Linux 内核 `tcp_jiffies32` / `srtt_us` 对齐 |
| `--clock-ms` / `--clockGranularityMs` | **`1.0 ms`** | `ns3::TcpSocketBase::ClockGranularity`，TCP 协议栈 RTO/RTT 定时器时钟粒度（与 ns-3 默认 `1 ms` 一致，可调如 `0.1` / `1.0` / `4.0` ms） |
| `--bin-ms` / `--binMs` | **`5.0 ms`** | Mahimahi 轨迹带宽时间槽（rate bin）采样与更新周期（决定下行带宽轨迹重放粒度；上行固定 `10 ms`，LeoCC 时变延迟固定 `10 ms`） |
| `--bulk-secs` / `--durS` | **`60 s`**（`leo` 为 **`115 s`**） | Bulk 流仿真总时长 |
| `--warmup-s` / `--warmupS` | **`5.0 s`** | Bulk 指标统计前跳过的预热时长 |

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

## 6. 可选数据集与分片（6 大类，329 条轨迹）

| 数据集 | 场景特征 | 对应 `--shard` | 默认浅 / 深队列 | 默认 Bulk 时长 |
| :--- | :--- | :--- | :--- | :--- |
| **Mahimahi Cellular LTE** | 蜂窝驾车、高抖动（4 条：`ATT-2016`/`ATT`/`TMobile`/`Verizon`） | `lte`（`smoke` 仅第 1 条） | `150 KB` / `2 MB` | `60 s` |
| **NYC 4G（NYU）** | 城市定点 5 条 / 步行 12 条 / 驾车 6 条（共 23 条） | `nyc-stat`、`nyc-walk-1..3`、`nyc-drive-1..2` | `150 KB` / `2 MB` | `60 s` |
| **NYC 4G Subway（2018）** | 地铁场景无/有交叉流（2 条） | `nyc-subway`（`nyc-rct` 精选 3 条） | `150 KB` / `2 MB` | `60 s` |
| **Lumos5G 5G（SIGCOMM'21）** | 商业 5G 毫米波步行/驾车、超高带宽（全集 121 条） | `lumos-5g`（2 条）、`lumos-rct`（1 条）、`lumos-5g-s1-of-16..s16-of-16` | `2 MB` / `16 MB` | `60 s` |
| **Lumos5G 4G（SIGCOMM'21）** | 商业 4G 轨迹（全集 175 条） | `lumos-4g-s1-of-18..s18-of-18` | `150 KB` / `2 MB` | `60 s` |
| **LeoCC Starlink** | LEO 卫星 15 s 级切换、10 ms 时变延迟 + 带宽（2 条官方示例；支持 `--shard local-leo --local-leo-dir` 扩展） | `leo` / `local-leo` | `2 MB` / `75 MB` | `115 s` |
| **合成轨迹（Synth）** | 稳态 48 Mbps 与阶跃带宽（2 条：`synth-48mbps`、`synth-step`） | `synth` | `150 KB` / `2 MB` | `60 s` |

> GitHub Actions 的 `mode=full` 与本地的 `--shard full` 均会自动编排全部 **44 个分片（329 条轨迹）** 并行执行。

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
