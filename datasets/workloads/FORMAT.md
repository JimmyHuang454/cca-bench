# 应用层负载格式规范（`datasets/workloads/*.wk`）

`datasets/workloads/` 存放统一驱动 **ns-3**（`tcp-trace-bench --workloadFile=...`）与 **Linux 内核**（`run_kernel_lines.py`）的 `.wk` 纯文本应用层请求负载文件，用于评测拥塞控制算法在交互式、突发、重尾及多路复用业务下的请求完成时间（RCT）与 FCT Slowdown。

---

## 一、文件命名与语法规则

- **文件命名**：位于 `datasets/workloads/<负载名>.wk`；`<负载名>` 仅允许字母、数字、下划线 `_` 与连字符 `-`（`bulk` 为内置保留名，无需 `.wk` 文件）。
- **编码与注释**：UTF-8 纯文本；空行以及以 `#` 开头的注释行会被加载器忽略。
- **标准数据行格式**：与 `datasets/lines/` 保持一致，采用**英文逗号分隔**的两列绝对时间表：

```text
<t_ms>,<size>
```

### 1. 字段定义与严格校验规则

对应 Python 解析器（`scripts/bench/lines_common.py::parse_workload`）与 ns-3 加载器（`workload.h::LoadWorkloadFile`、`mortise-workload.h::LoadMortiseWorkload`、`tcp-capacity-trace.cc::LoadWorkloadFile`）：

| 字段 | 类型与单位 | 允许范围与约束 | 说明 |
|---|---|---|---|
| `t_ms` | 非负有限整数或浮点数（ms） | `t_0 >= 0`，且**单调非递减**（`t_i >= t_{i-1}`） | 请求相对工作负载起点的**绝对到达时刻**；允许 `t_i == t_{i-1}` 以表达同一毫秒内多对象并发突发（保留各自独立的 RCT 边界，而非合并成一个对象） |
| `size` | 字节数或带单位的数据量（见下表） | 换算后 `1 <= size_bytes <= 100,000,000`（约 95.37 MiB 上限） | 本条请求/数据块的应用层负载字节数；数值与单位间**不能有空格** |

**`<size>` 支持的单位写法**（**不区分大小写**，按二进制 1024 换算，小数按四舍五入取整为字节）：

| 写法 | 换算关系 | 数值要求 | 示例 |
|---|---|---|---|
| 无后缀 / `b` | `1 B` | 无后缀须为正整数（仅 `0–9`）；带 `b` 允许正有限数 | `4096`、`4096b` |
| `k` / `kb` | `1024 B`（1 KiB） | 正有限整数或小数 | `16kb`、`16k`、`1.5kb`（1536 B） |
| `m` / `mb` | `1024² B`（1 MiB） | 正有限整数或小数 | `2mb`、`2m`、`0.5mb`（524288 B） |

- 文件至少必须包含 **1 条**有效记录，否则报 `empty workload`。
- 任一记录时间戳倒退（`t_i < t_{i-1}`）、列数不为 2 或字节数超界（`<= 0` 或 `> 100,000,000`）均会直接报错拒绝。

### 2. 示例

```text
# 典型交互式网页加载：t=0 发 24 KiB HTML，30 ms 同时突发 CSS(64 KiB) + 封面图(0.5 MiB)，2530 ms 进入下一页
0,24kb
30,64kb
30,0.5mb
2530,18kb
2550,55296
```

### 3. 向后兼容旧版 Mortise 间隔格式

为兼容上游 Mortise 原始数据及旧测试夹具，所有 Python / ns-3 加载器均**自动按逗号检测格式**：

- 包含 `,`：按标准 CSV 格式 `<t_ms>,<size>` 解析；
- 不包含 `,`：按旧版 Mortise 空白分隔格式 `<gap_ms> <size>` 解析（其中 `gap_ms` 为相对上一条请求的时间间隔，`<size>` 同样支持单位后缀）；
- **同一文件内禁止混用两种格式**。

运行 Linux 内核上游 Rust `mortise` 客户端时，`run_kernel_lines.py` 与 `run_mahimahi_suite.py` 会自动调用 `lines_common.write_mortise_workload()` 将标准 `.wk` 无损转为临时 `<gap_ms>\t<size_bytes>` 文件再传入客户端，用户无需手动转换。

---

## 二、执行模型与到达/完成语义

在 ns-3（`workload.h::MortiseAppSender` / `MortiseAppReceiver`）中，`.wk` 文件按以下模型执行：

1. **单 TCP 长连接与触发时刻**：
   - 发送端在仿真时间 $t_0 = 0.001\text{ s}$ 与接收端建立一条持久 TCP 连接。
   - 第 $i$ 条请求的计划到达时刻为：
     $$t_i = t_0 + \frac{t_{\text{ms}, i}}{1000}\quad (\text{s})$$
   - 到达时刻 $t_i$ 触发时，发送端将 `size_i` 字节追加到待发送队列（`m_pending += size_i`），并在 Socket 发送缓冲区有空余时每次最多写入 `64 KiB`（受 TCP 流量控制与拥塞控制约束）。
   - 当队列全发完且下一条请求尚未到达时，连接自然进入 **应用受限（`app_limited`）/ 空闲态**，用于真实检验算法的空闲重启与突发探测行为。
2. **接收端按序完成判定（RCT）**：
   - TCP 为字节流协议，接收端持续累加收到的字节数 `m_accum`；每当 `m_accum >= size_i` 时，扣减 `size_i` 并立即记录第 $i$ 条请求的完成时间（Request Completion Time）：
     $$\text{RCT}_i = \max\!\big((t_{\text{recv}, i} - t_i) \times 1000,\; 0.1\big)\text{ ms}$$
   - 若单次收到的大段数据连续跨越多个小请求边界，会在同一接收回调中依次结算对应请求的 RCT。
3. **停止与超时机制**：
   - 全部 $N$ 条请求均被接收端完整收到后，仿真**立即提前终止**；期间底层 `lines` 轨迹通过 `--linePeriodMs` 循环回放。
   - 若达到最大仿真时长上限（`run_trace_bench.py` 默认 `WORKLOAD_MAX_SIMULATION_S = 1800 s`）仍未完成全部请求，该次运行记为 **`timeout`**，并在综合排名中按 **0 分**计入。
4. **HTTP/2 多路复用帧近似**：
   - `http2_mux_rr.wk` 与 `http2_mux_srpt.wk` 通过生成器将每页的 1 个 HTML 对象 + 9 条并发资源流（30 ms 后触发）预先切分为 $\le 16\text{ KB}$ 的 HTTP/2 `DATA` 帧，按 Round-Robin 或 SRPT（最短剩余优先）交错排成纯 `.wk` 序列，从而在不改动传输层代码的前提下复现单 TCP 连接上的多流交错与队头阻塞效应。

---

## 三、核心评测指标定义

`tcp-trace-bench --evalJson=1` 对 `.wk` 负载输出以下核心指标：

- **完成时间分布**：`rct_mean_ms`、`rct_p50_ms`、`rct_p95_ms`、`rct_tail_ms`（按 `--tailPercentile` 计算，如 P98）。
- **FCT Slowdown（越小越好，下限 `1.0`）**：
  对第 $i$ 条请求，先按其触发时刻 $t_i$ 对应的基础 RTT $R_{0,i}$ 与全程平均链路容量 $C_{\text{avg}}$（Mbps）计算理想完成时间：
  $$\text{ideal}_i = \max(R_{0,i},\; 1\text{ ms}) + \frac{\text{size}_i \times 8}{C_{\text{avg}} \times 1000},\qquad \text{slowdown}_i = \max\!\left(\frac{\text{RCT}_i}{\text{ideal}_i},\; 1.0\right)$$
  - `fct_slowdown`：所有请求 $\text{slowdown}_i$ 的**几何均值**；
  - `fct_slowdown_p95` / `fct_slowdown_tail`：P95 与配置尾分位点（如 P98）上的 slowdown，也是 `run_trace_bench.py` 负载排名的主指标。
- **`mortise_qoe`**：逐请求计算 $(\text{thr}_i \times 0.5) / \max(\text{RCT}_i, 1.0) \times 100$ 后取算术均值（其中 $\text{thr}_i$ 为单请求有效吞吐 Mbps）。

---

## 四、内置负载清单

| 文件 | 业务模式与来源 | 请求/帧数 | 到达跨度 | 总数据量 |
|---|---|---:|---:|---:|
| `app.wk` | Mortise (NSDI'26) 混合对象基准（含 1–10 s 长空闲 + 2.1 MB 大突发） | 92 | 149.3 s | 58.30 MB |
| `websearch.wk` | DCTCP (SIGCOMM'10) 重尾网页搜索查询与大响应 | 80 | 95.9 s | 29.10 MB |
| `vl2.wk` | VL2 (SIGCOMM'09) 极端双峰重尾（80% 短 RPC + 20% 大传输） | 80 | 89.9 s | 10.91 MB |
| `pageload.wk` | 交互式 HTTP/3 网页加载（20 页 × 4 资源突发 + 2.5 s 思考时间） | 80 | 48.7 s | 19.91 MB |
| `http2_mux_rr.wk` | HTTP/2 多流网页加载（12 页，16 KB DATA 帧 Round-Robin 交错） | 1326 | 35.3 s | 20.77 MB |
| `http2_mux_srpt.wk` | HTTP/2 多流网页加载（12 页，16 KB DATA 帧 SRPT 最短优先交错） | 1326 | 35.3 s | 20.77 MB |
| *（内置 `bulk`）* | 持续饱和满载流（无需 `.wk` 文件；ns-3 用 `BulkSendApplication`） | — | 与场景等长 | 饱和持续发送 |

---

## 五、生成与校验

重新确定性生成所有合成 `.wk` 文件（由 `scripts/data/gen_workloads.py` 固定随机种子生成，不含上游实测 `app.wk`）：

```sh
make workloads
```

校验自定义 `.wk` 文件是否符合加载器规范：

```sh
python3 -c "from pathlib import Path; import sys; sys.path.insert(0, 'scripts/bench'); import lines_common; print(lines_common.workload(Path('datasets/workloads/app.wk')))"
```
