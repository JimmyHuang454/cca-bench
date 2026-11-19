# copa-auto-bench

Linux 内核 TCP 拥塞控制模块 **`tcp_copa_auto.ko`** 与基于 Mahimahi / LeoReplayer 的多数据集自动化对比评测框架。

## 目录结构

```text
copa-auto-bench/
├── kernel/
│   ├── tcp_copa_auto/           # Copa-Auto 内核 TCP 模块（copa_auto / _nc / _t90 / _t98 / _t / copa_orig）
│   └── tcp_leocc/               # LeoCC 内核 TCP 模块（SpaceNetLab/LeoCC 移植至 Linux 6.x 主线）
├── scripts/                     # Mahimahi/LeoReplayer 安装、workload 生成、并发执行与结果汇总
└── .github/workflows/
    └── mahimahi-compare.yml     # GitHub Actions 自动化对比评测流水线
```

## 本地验证内核核心单测

```bash
make -C kernel/tcp_copa_auto test
```

在 Linux 上编译并加载内核模块：

```bash
make -C kernel/tcp_copa_auto
sudo insmod kernel/tcp_copa_auto/tcp_copa_auto.ko
sysctl net.ipv4.tcp_available_congestion_control
```

---

## GitHub Actions 评测使用指南

### 如何运行

1. 打开仓库 **Actions** → 选择 **Mahimahi Comparison (copa-auto vs Mortise)** → 点击 **Run workflow**。
2. 默认只跑 **纯 Linux 内核 TCP 同栈对比**（`run_kernel: true`, `run_quinn: false`），消除 QUIC vs TCP 协议栈开销差异。
3. 运行结束后在 **Summary** 查看 Markdown 汇总表格，或在 **Artifacts** 下载 `comparison-summary`（含 CSV/MD 与 RCT 明细）。

### 可配置选项（Inputs）

| 选项 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `mode` | `quick` | 评测模式，见下表 |
| `theta` | `0.9` | `copa_auto` 吞吐-延迟权衡参数 $\theta \in [0.01, 0.99]$；`rct-datasets` 支持逗号分隔多个值（如 `0.9,0.98`） |
| `repeats` | `1` | 每个配置的时间偏移重复次数（>1 时汇总输出中位数与 min–max 区间） |
| `parallel` | 空（按 CPU 核数自动分配） | 单机并发数（高带宽 5G/Leo 默认 `0.75×CPU` [2..16]，常规默认 `1.5×CPU` [2..24]） |
| `rct_workloads` | `app,websearch,vl2,pageload` | `rct-datasets` 模式下要跑的请求负载 |
| `kernel_ccs` | 空（按 `mode` 默认） | 覆写 Linux 内核 TCP 栈算法列表（逗号分隔，支持 `{theta}` 占位符） |
| `quinn_ccs` | 空（按 `mode` 默认） | 覆写 QUIC (`quinn-perf`) 算法列表（仅 `run_quinn=true` 时生效） |
| `run_kernel` | `true` | 是否运行 Linux 内核 TCP 栈（`kernel-eval`） |
| `run_quinn` | `false` | 是否同时拉取 `quinn_repo` 编译并运行 QUIC 栈（`quinn-eval`） |
| `quinn_repo` / `quinn_ref` | `spongebob888/quinn-jls` / `jls-main` | `run_quinn=true` 时拉取的 QUIC 仓库与分支 |
| `send_fairness` / `stream_receive_window` | `false` / `8M` | 仅 QUIC 生效的传输参数 |

### 评测模式（`mode`）

| `mode` | 数据集分片（Shard） | 负载 | 队列（Bytes） | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| `quick` | `smoke`（`ATT-LTE-driving-2016`） | `bulk`, `app` | `150000` | 快速验证（约 2–4 分钟） |
| `rct` | `smoke`（`ATT-LTE-driving-2016`） | `app` | `2000000` | LTE `app.wk` RCT 专项对比 |
| `rct-datasets` | `lte`, `nyc-rct`, `lumos-rct`, `leo` | `rct_workloads`（默认 4 种） | LTE/NYC/Leo `2M`，Lumos5G `16M` | 学界多数据集 × 多负载 RCT 全套评测 |
| `smoke` | `smoke` | `bulk`, `app` | `150000, 2000000` | 单 trace 双队列完整默认算法集 |
| `datasets` | `nyc-subway`, `lumos-5g`, `leo` | `bulk` | `150000` / `2M` | 多场景 Bulk 吞吐与延迟对比 |
| `lumos-5g-all` | `lumos-5g-s1-of-12` .. `s12-of-12`（12 分片并行） | `bulk`, `app` | `2M` / `16M` | **Lumos5G 5G 全集（121 条）** |
| `lumos-4g-all` | `lumos-4g-s1-of-18` .. `s18-of-18`（18 分片并行） | `bulk`, `app` | `2M` | **Lumos5G 4G 全集（175 条）** |
| `lumos-all` | 以上 30 分片并行 | `bulk`, `app` | 5G `2M`/`16M`，4G `2M` | **Lumos5G 5G + 4G 全集（296 条）** |
| `nyc-subway` / `lumos-5g` / `leo` | 对应单个分片 | `bulk`（或 `bulk, app`） | 按分片默认 | 单独跑某一类真实轨迹 |
| `full` | 全部 10 个常规分片 | `bulk`, `app` | `150000, 2000000` | 常规全量评测 |

### 数据集（Datasets）

| 数据集 | 场景特征 | 包含的 Trace（对应 Shard） | 来源 |
| :--- | :--- | :--- | :--- |
| **Mahimahi Cellular LTE** | 蜂窝驾车、高抖动（15–40 ms）、带宽波动 | `ATT-LTE-driving-2016`、`ATT-LTE-driving`、`TMobile-LTE-driving`、`Verizon-LTE-driving`（`lte`；`smoke` 仅第 1 条） | [ravinet/mahimahi](https://github.com/ravinet/mahimahi/tree/master/traces) |
| **NYC 4G（NYU）** | 城市定点 / 步行 / 公交与出租车移动蜂窝 | 定点 5 条（`nyc-stat`）、步行 12 条（`nyc-walk-1/2/3`）、驾车 6 条（`nyc-drive-1/2`） | [Soheil-ab/Cellular-Traces-NYC](https://github.com/Soheil-ab/Cellular-Traces-NYC) |
| **NYC 4G Subway（2018）** | 地铁场景（无交叉流 / 有交叉流） | `nyc2018-4g-subway-no-cross`、`nyc2018-4g-subway-with-cross`（`nyc-subway`；`nyc-rct` 精选 3 条） | [Soheil-ab/Cellular-Traces-2018](https://github.com/Soheil-ab/Cellular-Traces-2018) |
| **Lumos5G（SIGCOMM'21）** | 商业 5G 毫米波 + 4G 步行/驾车、超高带宽、大 BDP | **5G 全集 121 条**（`lumos-5g-all`）+ **4G 全集 175 条**（`lumos-4g-all`）；`lumos-all` 跑全部 **296 条**；抽样 `lumos-5g` 2 条、`lumos-rct` 1 条 | [SIGCOMM21-5G/artifact](https://github.com/SIGCOMM21-5G/artifact) |
| **LeoCC Starlink** | LEO 低轨卫星 15 秒级切换、RTT 跳变 + 带宽切换 | GitHub 仓库自带官方示例全含：`leo-starlink-example-fixed`、`leo-starlink-example-dynamic`（`leo`，使用 `leoreplayer`）；作者另在清华云盘提供 8 类 × 600 条扩展集（可上传至本仓库 Releases 后接入） | [SpaceNetLab/LeoCC](https://github.com/SpaceNetLab/LeoCC) |
| **合成轨迹（Synth）** | 稳态与阶跃带宽基准（参考 Mortise Fig. 9） | `synth-48mbps`（48 Mbps）、`synth-step`（7.5 → 13 → 6 Mbps）（`synth`） | [`scripts/run_mahimahi_suite.py`](scripts/run_mahimahi_suite.py) |

### 流量负载（Workloads）

| Workload | 流量特征 | 规模 | 来源 |
| :--- | :--- | :--- | :--- |
| **`bulk`** | 持续饱和流（连续 2 MB 块），测稳态吞吐、RTT、排队延迟与 QoE | 默认 30–45s（LeoCC 115s） | 工作流自动生成 |
| **`app`** | 混合中小对象请求，Mortise 论文原版 RCT 基准 | 92 请求，跨度 149.3s，总计 58.30 MB | [BobAnkh/Mortise (`app.wk`)](https://github.com/BobAnkh/Mortise/blob/main/mortise/workload/app.wk) |
| **`websearch`** | 经典重尾分布（大量几 KB 小查询 + 少数几 MB 大对象） | 80 请求，跨度 95.9s，总计 29.10 MB | DCTCP ([SIGCOMM'10](https://dl.acm.org/doi/10.1145/1851182.1851192))，由 [`scripts/gen_workloads.py`](scripts/gen_workloads.py) 生成 |
| **`vl2`** | 极端双峰重尾（极多超小 RPC + 极少数超大传输） | 80 请求，跨度 89.9s，总计 10.91 MB | VL2 ([SIGCOMM'09](https://dl.acm.org/doi/10.1145/1592568.1592576))，由 [`scripts/gen_workloads.py`](scripts/gen_workloads.py) 生成 |
| **`pageload`** | 突发式网页资源簇（HTML/CSS/JS/图片成批到达 + 思考间隔） | 80 请求，跨度 48.7s，总计 19.91 MB | HTTP/3 网页加载模型，由 [`scripts/gen_workloads.py`](scripts/gen_workloads.py) 生成 |

### 可选拥塞控制算法（通过 `kernel_ccs` / `quinn_ccs` 自由挑选）

在 GitHub Actions 触发界面中，留空 `kernel_ccs` / `quinn_ccs` 会按 `mode` 跑默认算法集；填入逗号分隔的算法名即可**只跑你指定的算法组合**（支持 `{theta}` 占位符，会自动替换为 `theta` 输入框的首个值）。

#### 1. Linux 内核 TCP 栈（`kernel_ccs`，`run_kernel=true`）

| 算法名 | 分类 / 来源 | 说明 |
| :--- | :--- | :--- |
| `copa_auto` | `tcp_copa_auto.ko`（本仓库） | 自适应 $\delta$（默认 $\theta=0.50$），开启竞争模式切换 |
| `copa_auto_theta<X>` | `tcp_copa_auto.ko`（本仓库） | 自定义 $\theta \in [0.01, 0.995]$，如 `copa_auto_theta0.9`、`copa_auto_theta0.98` |
| `copa_auto_nc` | `tcp_copa_auto.ko`（本仓库） | 自适应 $\delta$（$\theta=0.50$），关闭竞争模式切换 |
| `copa_orig` | `tcp_copa_auto.ko`（本仓库） | 原版固定 $\delta=0.50$ Copa（现代 Linux 6.x 内核实现） |
| `mortise_copa` | eBPF（[BobAnkh/Mortise](https://github.com/BobAnkh/Mortise)） | 官方 Mortise eBPF Copa |
| `leocc` | `tcp_leocc.ko`（[SpaceNetLab/LeoCC](https://github.com/SpaceNetLab/LeoCC)） | LEO 低轨卫星专用算法（仿真版适配 Linux 6.x，`leo` 分片默认自动加入） |
| `bbr` | Linux 主线内置 | Google 官方 BBRv1 |
| `cubic` | Linux 主线内置 | Linux 默认丢包型基线 |
| `vegas` | Linux 主线内置 | 经典基于排队延迟算法 |
| `cdg` | Linux 主线内置 | CAIA Delay-Gradient（基于延迟梯度） |
| `veno` | Linux 主线内置 | 混合丢包+排队估计（无线链路优化） |
| `yeah` | Linux 主线内置 | YeAH-TCP（快慢双模混合算法） |
| `illinois` | Linux 主线内置 | 延迟调制 AIMD 混合算法 |
| `westwood` | Linux 主线内置 | 基于带宽估计（无线丢包场景） |
| `hybla` | Linux 主线内置 | 高 RTT 补偿（卫星 / LeoCC 场景） |
| `reno` | Linux 主线内置 | 经典 NewReno |

各 `mode` 留空 `kernel_ccs` 时的默认算法集：
- `quick` / `datasets`：`copa_auto,copa_auto_nc,copa_auto_theta<theta>,mortise_copa`（`leo` 分片自动附加 `leocc`）
- `rct`：`copa_auto,copa_auto_theta0.9,copa_auto_theta0.98,copa_orig,mortise_copa`
- `rct-datasets`：`copa_auto,copa_auto_theta<theta...>,copa_auto_nc,copa_orig,mortise_copa,bbr,cubic,vegas,cdg`（`leo` 分片自动附加 `leocc`）
- `smoke` / `full` / `lumos-*-all` 等：`copa_auto,copa_auto_nc,copa_auto_theta<theta>,copa_orig,mortise_copa,bbr,cubic`

#### 2. QUIC 用户态栈（`quinn_ccs`，需勾选 `run_quinn=true`）

- 可选算法：`copa-auto`、`copa-auto-theta<X>`、`copa-auto-nc`、`copa`、`copa-nc`、`bbr1`、`bbr2`、`bbr3`、`cubic`、`new-reno`

#### 3. 常用 Actions 填写示例

- **快速冒烟（纯内核 TCP）**：`mode=quick`，`theta=0.9`，`kernel_ccs` 留空，`run_kernel=true`，`run_quinn=false`
- **自定义内核全算法横向对比**：
  - `mode=rct-datasets`，`theta=0.9,0.98`，`repeats=3`，`rct_workloads=app,websearch,vl2,pageload`
  - `kernel_ccs=copa_auto,copa_auto_theta0.9,copa_auto_theta0.98,copa_auto_nc,copa_orig,mortise_copa,leocc,bbr,cubic,vegas,cdg,veno,yeah,hybla`
- **卫星场景专项对比**：
  - `mode=leo`，`kernel_ccs=copa_auto,copa_auto_theta0.98,leocc,mortise_copa,bbr,cubic,hybla`
- **补充看 QUIC 侧 BBRv1/v2/v3**：
  - 勾选 `run_quinn=true`，`quinn_ccs=copa-auto,copa-auto-theta0.98,bbr1,bbr2,bbr3,cubic`

---

## 本地运行外置 LeoCC Starlink 数据集

下载清华云盘 Starlink 轨迹集（A–H 目录，每条轨迹包含 `bw_*.txt` 与 `delay_*.txt`）后，在 Linux 机器上加载 `tcp_copa_auto.ko` 与 `tcp_leocc.ko`，然后直接通过 `--shard local-leo --local-leo-dir` 运行：

```bash
python3 scripts/run_mahimahi_suite.py \
  --stack kernel \
  --shard local-leo \
  --local-leo-dir /path/to/starlink_traces/A \
  --ccs copa_auto,copa_auto_theta0.98,leocc,mortise_copa,bbr,cubic \
  --queues 2000000 \
  --workloads bulk,app \
  --bulk-secs 115 --app-secs 0 --parallel 2 \
  --bin /path/to/mortise/target/release/client \
  --trace-dir /tmp/traces \
  --app-workload /tmp/workloads/app.wk \
  --bulk-workload /tmp/workloads/bulk.wk \
  --out-dir /tmp/local_leo_out
```
