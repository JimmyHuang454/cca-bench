# Copa 的 δ 选择：仿真复现

本仓库用来复现论文仿真验证一节的全部实验，包括 Copa 的 [ns-3](https://github.com/nsnam/ns-3-dev-git) 实现、在线调节算法 copa-auto、场景程序、运行和分析脚本。

算法名称：

| 名称 | δ | 竞争模式 |
|---|---|---|
| Copa | 固定为 0.5（原版） | 开启 |
| Copa(δ) | 固定为给定值 | 开启 |
| Copa-NC | 固定为 0.5 | 关闭 |
| Copa-NC(δ) | 固定为给定值 | 关闭 |
| copa-auto | 在线调节 | 开启 |
| copa-auto-NC | 在线调节 | 关闭 |

脚本里对应 `copa`、`copa-d0.1`、`copa+nosw`、`copa-d0.1+nosw`、`copa-auto`、`copa-auto+nosw`。

## 目录

| 路径 | 内容 |
|---|---|
| `ns3-overlay/` | 覆盖到 ns-3 上的文件，见下文 |
| `scripts/` | 运行和分析脚本 |
| `config/hwy.args` | 高速 4G 多流场景的参数 |
| `docs/mobile-scenarios.md` | 移动场景和 JSON 线路格式的说明 |
| `Makefile` | 各步骤的入口 |
| `setup_ns3.sh` | 获取 ns-3、覆盖文件、编译 |
| `fetch_traces.sh` | 下载公开 trace |
| `smoke_test.sh` | 快速检查编译结果 |
| `run_all.sh` | 运行全部实验并生成作图数据 |

ns-3 基于提交 `84a73f9c00abf5e5b8aa676e1775261fb717c3ac`（ns-3.48 之后的开发版）。

`ns3-overlay/` 中的主要文件：

| 文件 | 作用 |
|---|---|
| `src/internet/model/tcp-copa.{h,cc}` | Copa 和 copa-auto。`AdaptiveDelta` 打开在线调节，`ModeSwitching` 控制竞争模式 |
| `src/internet/test/tcp-copa-test.cc` | Copa 单元测试 |
| `src/internet/model/tcp-bbr-v2.*`、`tcp-bbr-v3.*` | BBRv2、v3，论文的对比未使用，但场景程序需要编译它们 |
| `src/internet/model/tcp-socket*`、`tcp-tx-buffer.*` | 丢包统计等 TCP 改动 |
| `src/lte/model/*` | LTE 切换和调度相关的修正，移动场景使用 |
| `scratch/tcp-capacity-trace.cc` | 单瓶颈场景，容量按时间表变化，也可回放 Mahimahi 和 LeoCC 的 trace |
| `scratch/tcp-lte-highspeed-handover.cc` | LTE 移动场景，含切换 |
| `scratch/run-sweep.py` | 移动场景的参数和批量运行 |
| `scratch/fading-traces/`、`scratch/lines/` | 衰落 trace 和步行、跑步线路 |

## 环境

- 编译 ns-3 所需的 C++ 工具链、CMake、Git。
- Python 3（只用标准库）。`scratch/run-sweep.py` 有 matplotlib 时会额外画图，没有时跳过。
- 全部结果约需几十 GB 磁盘；Starlink 数据集压缩包 732 MB，解压全部约 60 GB，脚本只解压抽中的 24 条。

## 步骤

```bash
make setup
make smoke
make all JOBS=9
```

`make help` 列出全部目标。`make setup` 默认把 ns-3 放在 `./ns-3`，可用 `NS3_ROOT=...` 改位置，之后的命令要用同一个值。

也可以分开运行，已完成的运行会跳过，中断后重新执行同一条命令即可：

```bash
make basic JOBS=9
make lte JOBS=9
make leo JOBS=9
make mobile JOBS=9
make lines JOBS=9
make figures
```

`make lte` 和 `make leo` 会在需要时下载 trace，也可以先执行 `make fetch`。`make figures` 依赖前四步都已完成，作图数据写到 `results/figure-data/`。

## 实验场景

下面的利用率都以瓶颈容量为基准，功率 = 利用率 ÷ (平均 RTT / 基础 RTT)。

### 基础场景（`make basic`）

单瓶颈、没有切换，程序是 `tcp-capacity-trace`。全程 180 s，分 6 段，每段 30 s：

| 时段 (s) | 0–30 | 30–60 | 60–90 | 90–120 | 120–150 | 150–180 |
|---|---|---|---|---|---|---|
| 容量 (Mbit/s) | 40 | 20 | 60 | 10 | 40 | 30 |
| 丢包率 | 0 | 1% | 0.5% | 5% | 2% | 10% |
| 基础 RTT (ms) | 50 | 100 | 20 | 150 | 30 | 80 |

| 组 | 变化的量 |
|---|---|
| A | 容量 |
| B | 丢包率 |
| C | 容量和丢包率 |
| D | 基础 RTT |
| E | 三者都变 |

不变化的量取 40 Mbit/s、丢包率 0、基础 RTT 50 ms。段内容量每 20 ms 按对数正态分布波动，σ = 0.1 接近固定容量，σ = 0.4 模拟无线链路的快速波动。缓冲 2 MB，发送缓冲 4 MB，1、2、4 条流，每个配置 5 次运行。

### LTE 移动场景（`make mobile`、`make lines`）

固定场景和多流由 `make mobile` 运行。城市和乡村共 14 条 JSON 线路由 `make lines` 运行，见下一节“用例清单”。线路格式是本项目原创的，说明见 [`docs/mobile-scenarios.md`](docs/mobile-scenarios.md)。

### LTE trace 回放（`make lte`）

回放 Mahimahi 的 4 条车载 LTE trace：Verizon、T-Mobile、ATT、ATT-2016，上下行都回放，平均下行容量约 5–13 Mbit/s。规则与 Mahimahi 相同：每个发送机会发 1500 字节，包可跨多个机会，未用的机会作废，trace 播完后循环。

基础 RTT 固定 50 ms，无额外丢包，缓冲 2 MB，运行 180 s，前 5 s 不计。每条 trace 取 5 个均匀分布的起始位置，1、2、4 条流。

### Starlink trace 回放（`make leo`）

回放 LeoCC 的 Starlink trace，基础 RTT 随时间变化。4 条路径的下行平均单向时延约 12、27、54、77 ms，每条路径的每个方向用固定种子抽 3 条，共 24 条，运行 120 s，前 5 s 不计，1、2、4 条流。

| 方向 | 平均容量 | 缓冲 | 随机丢包率 | 发送缓冲 |
|---|---|---|---|---|
| 下行 | 约 375 Mbit/s | 75 MB | 0.01% | 64 MB |
| 上行 | 约 24 Mbit/s | 750 kB | 0.2% | 4 MB |

与 LeoCC 的回放工具 LeoReplayer 有两处不同：这里保持包序，因为 ns-3 的 TCP 没有 RACK；上行丢包率对所有算法都取 0.2%。下行 BBRv1 的超时运行改用 20 MB 发送缓冲重跑，两条 trace 核对的差异为：利用率不超过 2.5 个百分点，平均 RTT 不超过 4%。

### 算法

各场景比较 Copa、Copa-NC、copa-auto、copa-auto-NC、CUBIC、BBRv1，并扫描固定 δ 作为事后参照。基础场景还比较官方实现 genericCC 的速度规则（后缀 `+gcv`）。

## 用例清单

全部是 ns-3 LTE 仿真，含切换和链路失败。线路用 JSON 描述，文件在 `ns3-overlay/scratch/lines/`，无线参数在 `scratch/run-sweep.py`。线路参数按典型环境设定，不是实测轨迹。

### 城市（3GPP UMa 路损）

| 用例 | 名称 | 速度 | 主要考验 | 衰落 |
|---|---|---|---|---|
| 步行 | `walk-line` | 约 5 km/h | 楼间弱信号、地下通道、人群、电梯 | ETU 5 km/h |
| 跑步 | `run-line` | 10–12 km/h | 稀疏站点、高架下通道、同频干扰、电梯 | ETU 12 km/h |
| 驾车 | `city-drive` | 0–50 km/h | 走走停停、红灯、高楼遮挡、固定干扰源 | EVA 60 km/h |
| 公交 | `city-bus` | 0–30 km/h | 频繁进出站、周围负载变化 | EVA 60 km/h |
| 地铁 | `city-subway` | 0–70 km/h | 全程隧道分段覆盖、到站人群 | EVA 60 km/h |
| 高架 | `city-elevated` | 80 km/h | 站点密、切换多、突发同频干扰 | EVA 120 km/h |
| 商场 | `city-mall` | 0–4 km/h | 室内分布小区、人群、电梯 | ETU 3 km/h |
| 体育场 | `city-stadium` | 0–2 km/h | 几乎不动，容量变化全来自周围用户 | EPA 3 km/h |

### 乡村（3GPP RMa 路损）

| 用例 | 名称 | 速度 | 主要考验 | 衰落 |
|---|---|---|---|---|
| 公路 | `rural-road` | 30–80 km/h | 站距约 3 km，小区边缘信号弱，穿过村庄 | EVA 60 km/h |
| 山区 | `rural-mountain` | 40–50 km/h | 弯道爬坡、山体遮挡、无覆盖隧道 | EVA 60 km/h |
| 村庄步行 | `rural-village` | 0–4 km/h | 只有一个远处的基站，偶尔进屋 | ETU 5 km/h |
| 小区边缘 | `rural-edge` | 0–3 km/h | 基本不动，信号弱，容易链路失败 | EPA 3 km/h |
| 高速公路 | `hwy-line` | 0–120 km/h | 匝道、稀疏站点、隧道、拥堵停车 | EVA 120 km/h |
| 高铁 | `hsr-line` | 0–350 km/h | 进出站、干扰、覆盖空洞、隧道、弯道 | Rician 300 km/h |

### 固定场景（命令行参数）

| 用例 | 名称 | 速度 | 主要考验 |
|---|---|---|---|
| 高铁 4G | `hsr-4g` | 350 km/h | 匀速直线、切换、2 km 隧道 |
| 高速 4G | `hwy-4g` | 120 km/h | 匀速直线、切换；多流和背景流量也在这里测 |

### 单流结果

功率 = 利用率 ÷ (平均 RTT / 50 ms)，3 次运行的平均。

| 类别 | 线路 | 容量 (Mbit/s) | Copa | Copa-NC | copa-auto | copa-auto-NC | CUBIC | BBRv1 | Copa(0.05) | Copa(0.02) | 切换 | 链路失败 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 城市 | 步行 (`walk-line`) | 23.1 | 0.22 | 0.25 | 0.53 | 0.49 | 0.23 | 0.35 | 0.46 | 0.50 | 6.0 | 1.0 |
| 城市 | 跑步 (`run-line`) | 22.9 | 0.24 | 0.22 | 0.58 | 0.56 | 0.18 | 0.38 | 0.48 | 0.52 | 10.0 | 2.3 |
| 城市 | 驾车 (`city-drive`) | 14.0 | 0.27 | 0.21 | 0.39 | 0.60 | 0.21 | 0.22 | 0.56 | 0.29 | 4.7 | 1.0 |
| 城市 | 公交 (`city-bus`) | 14.9 | 0.23 | 0.31 | 0.45 | 0.63 | 0.18 | 0.32 | 0.47 | 0.47 | 3.3 | 0.0 |
| 城市 | 地铁 (`city-subway`) | 42.9 | 0.25 | 0.28 | 0.57 | 0.67 | 0.16 | 0.40 | 0.72 | 0.67 | 10.3 | 5.0 |
| 城市 | 高架 (`city-elevated`) | 28.0 | 0.41 | 0.42 | 0.69 | 0.67 | 0.23 | 0.34 | 0.80 | 0.74 | 11.3 | 0.0 |
| 城市 | 商场 (`city-mall`) | 11.4 | 0.14 | 0.33 | 0.23 | 0.51 | 0.07 | 0.38 | 0.33 | 0.15 | 0.0 | 0.0 |
| 城市 | 体育场 (`city-stadium`) | 12.1 | 0.20 | 0.15 | 0.43 | 0.45 | 0.15 | 0.30 | 0.48 | 0.35 | 1.3 | 0.0 |
| 乡村 | 公路 (`rural-road`) | 27.8 | 0.30 | 0.27 | 0.60 | 0.58 | 0.26 | 0.40 | 0.66 | 0.60 | 11.0 | 5.0 |
| 乡村 | 山区 (`rural-mountain`) | 52.1 | 0.29 | 0.30 | 0.66 | 0.65 | 0.20 | 0.44 | 0.69 | 0.65 | 18.7 | 0.7 |
| 乡村 | 村庄步行 (`rural-village`) | 16.6 | 0.34 | 0.31 | 0.48 | 0.40 | 0.16 | 0.29 | 0.49 | 0.38 | 0.0 | 2.0 |
| 乡村 | 小区边缘 (`rural-edge`) | 5.4 | 0.21 | 0.17 | 0.25 | 0.20 | 0.06 | 0.11 | 0.22 | 0.30 | 0.0 | 6.7 |
| 乡村 | 高速公路 (`hwy-line`) | 69.8 | 0.28 | 0.27 | 0.73 | 0.73 | 0.26 | 0.49 | 0.74 | 0.71 | 9.0 | 0.0 |
| 乡村 | 高铁 (`hsr-line`) | 50.4 | 0.33 | 0.27 | 0.63 | 0.60 | 0.20 | 0.43 | 0.63 | 0.64 | 18.7 | 3.0 |

### 多流结果

驾车、地铁、乡村公路、高铁跑了 2、4 条流，每个配置 3 次，其余线路没有多流结果。功率 ± 95% 置信区间。

| 流数 | 线路 | Copa | Copa-NC | copa-auto | copa-auto-NC | CUBIC | BBRv1 | Jain（copa-auto） | Jain（copa-auto-NC） |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 驾车 | 0.32 ± 0.37 | 0.43 ± 0.09 | 0.52 ± 0.44 | 0.54 ± 0.32 | 0.13 ± 0.12 | 0.28 ± 0.24 | 0.99 | 0.86 |
| 2 | 地铁 | 0.46 ± 0.18 | 0.43 ± 0.15 | 0.66 ± 0.26 | 0.69 ± 0.14 | 0.24 ± 0.04 | 0.42 ± 0.16 | 0.97 | 0.96 |
| 2 | 乡村公路 | 0.41 ± 0.27 | 0.45 ± 0.05 | 0.64 ± 0.17 | 0.67 ± 0.10 | 0.30 ± 0.05 | 0.38 ± 0.09 | 0.97 | 0.96 |
| 2 | 高铁 | 0.41 ± 0.09 | 0.40 ± 0.06 | 0.64 ± 0.09 | 0.65 ± 0.07 | 0.23 ± 0.04 | 0.40 ± 0.05 | 0.95 | 0.81 |
| 4 | 驾车 | 0.37 ± 0.30 | 0.58 ± 0.23 | 0.29 ± 0.23 | 0.63 ± 0.41 | 0.11 ± 0.10 | 0.22 ± 0.17 | 0.97 | 0.78 |
| 4 | 地铁 | 0.53 ± 0.13 | 0.52 ± 0.14 | 0.66 ± 0.04 | 0.69 ± 0.05 | 0.24 ± 0.13 | 0.43 ± 0.01 | 0.97 | 0.78 |
| 4 | 乡村公路 | 0.53 ± 0.11 | 0.52 ± 0.13 | 0.58 ± 0.06 | 0.59 ± 0.14 | 0.31 ± 0.02 | 0.35 ± 0.06 | 0.97 | 0.85 |
| 4 | 高铁 | 0.48 ± 0.05 | 0.48 ± 0.07 | 0.61 ± 0.04 | 0.64 ± 0.14 | 0.29 ± 0.05 | 0.40 ± 0.02 | 0.93 | 0.67 |

运行某一个用例：

```bash
cd ns-3
python3 scratch/run-sweep.py city-drive --ccs copa-auto,bbr --runs 3
```

## 实验与论文的对应

| 命令 | 论文章节 | 表、图 | 结果位置 |
|---|---|---|---|
| `make basic` | 基础场景 | `tab:basic-s01`、`tab:basic-s04`、`fig:basic-power`、`fig:basic-trace` | `results/basic-s*-summary/` |
| `make mobile` | 车载、行人、自适应 δ、算法对比 | `tab:adaptive`、`tab:adaptive-multi`、`tab:cmp-single`、`tab:cmp-multi` 及相应的图 | `results/mobile-summary.json`、`results/compare/` |
| `make lines` | 城市和乡村线路 | `tab:lines`、`tab:lines-multi` | `results/lines-summary.json`、`results/lines-multi-summary.json` |
| `make lte` | LTE trace 回放 | `tab:trace-power`、`fig:trace-power` | `results/lte-summary/` |
| `make leo` | Starlink trace 回放 | `tab:leo-power`、`fig:leo-power` | `results/leo-summary/` |

## copa-auto 的默认参数

半周期 8 个 RTT，其中前 3 个不计入，探测周期 20 s、时长 2 s，g = 0.25，a = 10，δ 范围 [0.001, 0.5]。这些是 `tcp-copa.cc` 的属性默认值，可以用 `--ns3::TcpCopa::AdaptiveFairIncrease=30` 这样的参数修改。

## 已知限制

- **运行时间**：10 核机器上 9 个并行时，LTE trace 720 次约十几分钟，Starlink 1080 次约 7 小时。
- **下行 BBRv1**：64 MB 发送缓冲下很慢，超过 30 分钟的运行由脚本改用 20 MB 重跑，路径 D 仍有运行可能超时，分析时不计入。
- **上行小 δ**：上行 Copa-NC(δ) 在 δ = 0.002、0.005 时可能超时，它们不参与上行最优值的选取。
- **未包含**：论文中比较步长 g 的表（`tab:gain`）是早期版本测得的，本仓库不复现。
- **行人场景**：`make mobile` 中步行、跑步只有 1 次运行，`make lines` 中有 3 次，结果仍不稳定。
- **城市和乡村线路**：每个配置 3 次，多流只测了 4 条线路；线路是按典型环境设定的，不是实测轨迹。

## 数据来源和许可

- **ns-3 及本仓库的代码**：GPL-2.0-only，见 `LICENSE`。
- **Mahimahi trace**：<https://github.com/ravinet/mahimahi>，由脚本下载，不随本仓库分发。
- **LeoCC trace**：<https://github.com/SpaceNetLab/LeoCC>，由脚本下载，不随本仓库分发。

使用这些 trace 请遵守原项目的许可并引用原论文。
