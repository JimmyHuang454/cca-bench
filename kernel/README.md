# Linux Kernel TCP Bench（Mahimahi + `datasets/lines`）

基于 Mahimahi（增强版 LeoReplayer）与 Mortise 的 Linux 内核 TCP 拥塞控制自动化对比框架，直接复用仓库内 [datasets/lines](../datasets/lines/TRACE-FORMAT.md) 数据集与 [datasets/workloads](../datasets/workloads/FORMAT.md) 负载文件，与 ns-3 `tcp-trace-bench` 保持一致的数据源、方向语义、队列缓冲与排名口径。

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

## 在 GitHub Actions 上运行 Linux 内核测试

工作流 [Mahimahi Linux Kernel Bench](../.github/workflows/mahimahi-kernel.yml) 包含 4 个阶段：`plan → build → kernel-eval (matrix) → summarize`。
- `build` 阶段一次性编译并缓存增强版 Mahimahi 与 Mortise（Rust `client`/`server`/`manager` + Linux 6.10+ 4-arg ABI 补丁后的 `mortise_copa.bpf.o`），打包分发给所有并行 runner；
- `kernel-eval` 阶段按场景预估耗时自动均衡分片（LPT 装箱），编译并加载 `tcp_copa_auto.ko`（若包含 `leocc` 则自动编译加载 `tcp_leocc.ko`），自动 `modprobe` 主线算法（`bbr`、`vegas`、`westwood`、`illinois`、`veno`、`yeah`、`hybla`、`cdg`）；
- `summarize` 阶段汇总全部 shard 的 `shard_runs.csv`，输出与 ns-3 `tcp-trace-bench` 同构的 `runs.csv`、`metadata.json`、`report.md`（同步展示在 GitHub Actions Summary 页面，并上传 `mahimahi-kernel-lines-summary` artifact）。

### 工作流输入参数

| 输入 | 默认值 | 说明 |
|---|---|---|
| `dataset` | `basic` | 数据集：`basic`（27 场景）、`leocc`（1 场景）、`cellular-traces-2018`（2 场景）、`lumos`（26 场景）或 `all` |
| `scenarios` | 空 | 可选逗号分隔场景名过滤，空值表示运行整个 `dataset`（如 `elevator,bandwidth_drop_severe` 或 `5g_trace_100_walking`） |
| `workloads` | `bulk,app` | 逗号分隔负载，支持 `bulk`、`app`、`websearch`、`pageload`、`vl2`（直接读取 `datasets/workloads/*.wk`） |
| `kernel_ccs` | `copa_auto_v2,copa_auto_nc,mortise_copa,bbr,cubic` | 逗号分隔 Linux TCP 拥塞算法名 |
| `queues` | `500KB` | Mahimahi 瓶颈链路队列（每方向，默认 `500KB` 对齐 ns-3 `tcp-trace-bench`；端主机保持 Linux 默认参数；支持多队列如 `150KB,500KB,2MB` 或 `infinite`） |
| `measurement_rtts` | `16` | `copa_auto_v2` 的测量周期（1～256 SRTT，加载 `tcp_copa_auto.ko` 时设置） |
| `repeats` | `1` | 每个配置的重复次数 |
| `max_bulk_s` | `0` | Bulk 最大运行秒数（`0` 表示按 `lines` 全长运行；如对 800s 的 `lumos` 可填 `120` 截断） |
| `workload_timeout_s` | `600` | 单个 workload 的最大实际运行秒数（`slow_uplink` 1 Mbps 跑完 `app.wk` 约需 500s），超时或未完成全部请求按 `timeout` 计 0 分 |
| `max_shards` | `8` | 最大并发 GitHub Actions runner 数（每台 runner 固定 `parallel=1` 以避免 Mahimahi 调度抖动） |

### 方向映射与延迟回放语义

- **显式方向映射**：
  - `datasets/lines` 中 `up.txt` + `up.delay.txt` 为 TCP 数据方向、`down.txt` + `down.delay.txt` 为 TCP ACK 方向；
  - Mortise 架构中 `server` 位于 Mahimahi 外侧、`client` 位于 Mahimahi 内侧，数据由 `server → client`（Mahimahi **downlink**）发送、ACK 由 `client → server`（Mahimahi **uplink**）返回；
  - 转换脚本 [lines_to_mahimahi.py](../scripts/data/lines_to_mahimahi.py) 自动将 `lines/up.*` 映射到 Mahimahi `downlink`、`lines/down.*` 映射到 Mahimahi `uplink`，确保非对称带宽、单向断连和非对称延迟场景与 ns-3 语义完全一致。
- **动态与非对称延迟**：
  - [install_mahimahi.sh](../scripts/env/install_mahimahi.sh) 为 LeoReplayer `mm-delay` 补丁增加了 `--downlink-delay <file>` 双方向独立动态延迟支持，并将 `DelayQueue` 改为单调时间戳保序 FIFO 队列（延迟骤降或同毫秒突发不会造成乱序），且不再额外注入随机丢包。
- **队列与预热**：
  - Linux 端主机保持默认 TCP 缓冲与 qdisc；`mm-link` 瓶颈队列默认每方向 `500KB`、`bulk` 预热 `0s`，与 ns-3 `tcp-trace-bench` 保持同口径。

### 可选 Linux 内核 TCP 拥塞控制算法

| 算法名 | 来源 | 说明 |
|---|---|---|
| `copa_auto_v2` | `tcp_copa_auto.ko`（本仓库） | v2 单周期估计，周期由 `measurement_rtts` 控制 |
| `copa_auto` | `tcp_copa_auto.ko`（本仓库） | 自适应 $\delta$（$\theta=0.50$），开启竞争模式切换 |
| `copa_auto_theta<X>` | `tcp_copa_auto.ko`（本仓库） | 自定义 $\theta \in [0.01, 0.995]$，如 `copa_auto_theta0.9`、`copa_auto_theta0.98` |
| `copa_auto_nc` / `copa_auto_nc_j0` | `tcp_copa_auto.ko`（本仓库） | 自适应 $\delta$，关闭竞争模式切换 |
| `copa` / `copa_orig` | `tcp_copa_auto.ko`（本仓库） | 原版固定 $\delta=0.50$ Copa（`copa` 自动映射到内核 `copa_orig`，与 ns-3 `copa` 一致） |
| `mortise_copa` | eBPF（[BobAnkh/Mortise](https://github.com/BobAnkh/Mortise)） | Mortise 官方 eBPF Copa（开启竞争模式 `mode_switch=true` + 在线调参） |
| `mortise_copa_nc` | eBPF（[BobAnkh/Mortise](https://github.com/BobAnkh/Mortise)） | Mortise eBPF Copa（关闭竞争模式 `mode_switch=false`，保留在线 $\delta$ 调参） |
| `leocc` | `tcp_leocc.ko`（本仓库） | LEO 低轨卫星专用算法（选择时自动编译加载） |
| `bbr` / `cubic` / `vegas` / `cdg` / `veno` / `yeah` / `illinois` / `westwood` / `hybla` / `reno` | Linux 主线内置 | 自动按需 `modprobe` 加载 |

### 常用 Actions 填写示例

- **快速冒烟验证**：`dataset=basic`，`scenarios=elevator,bandwidth_drop_severe`，`workloads=bulk,app`，`max_shards=2`
- **完整 `basic` 27 场景对比**：`dataset=basic`，`scenarios=`（留空），`workloads=bulk,websearch,pageload,app`，`max_shards=12`
- **Starlink `leocc` 专项**：`dataset=leocc`，`kernel_ccs=copa_auto_v2,copa_auto_nc,mortise_copa,leocc,bbr,cubic,hybla`，`workloads=bulk,websearch,pageload,app`
- **Lumos 5G 抽样快速对比**：`dataset=lumos`，`scenarios=5g_trace_100_walking,5g_trace_103_driving`，`max_bulk_s=120`，`queues=500KB,2MB`
