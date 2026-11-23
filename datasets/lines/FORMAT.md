# Lines 链路轨迹格式规范（`datasets/lines/`）

`datasets/lines/` 存放供 **ns-3**（`tcp-trace-bench --profile=lines --lineFile=...`）与 **Linux 内核**（`run_kernel_lines.py`）统一读取的分段常数全双工链路轨迹。每个场景以单个标准 **RFC 4180 CSV** 文件描述；**列名本身不带单位**，具体单位直接写在单元格数值中（缺省使用默认单位）。

---

## 一、目录组织与文件命名

解析器（`scripts/bench/lines_common.py::discover`）支持两种单文件 CSV 组织形态（不再支持旧版 `.txt` 文件）：

### 1. 平铺单文件形式（推荐，适合批量实测轨迹）

无独立场景说明文档时，每个场景直接保存为一个 `.csv` 文件，文件名（不含 `.csv`）即场景名：

```text
datasets/lines/<数据集>/<场景名>.csv
# 例：
# datasets/lines/lumos/5g_trace_114_walking.csv
# datasets/lines/leocc/leo-bw.csv
# datasets/lines/cellular-traces-2018/nyc2018-4g-subway-no-cross.csv
```

### 2. 目录单文件形式（适合附带独立 `README.md` 的合成场景）

每个场景放在独立子目录中，轨迹固定命名为 `line.csv`，目录名即场景名：

```text
datasets/lines/<数据集>/<场景名>/
  line.csv          # 必填：统一 CSV 链路轨迹
  README.md         # 可选：场景物理含义与参数说明
```

> `discover()` 也支持直接传入单个 `.csv` 文件路径（如 `LINES_DIR = ROOT / 'datasets/lines/lumos/5g_trace_114_walking.csv'`）。

### 3. 方向与默认延迟说明

- **流量方向**：在 `tcp-trace-bench` 中，**TCP 数据包固定走上行（`up_cap` / `up_delay`）**，**TCP ACK 固定走下行（`down_cap` / `down_delay`）**；链路基础 RTT 为上下行单向基础延迟之和。
- **默认延迟（无需写多余的常量延迟列）**：延迟列全部可选。当某方向未提供延迟列或首行 `t = 0` 对应延迟为空时，自动使用评测脚本配置的默认单向基础延迟（默认 `10 ms`，总基础 RTT = `2 × 默认值`）：
  - ns-3 评测：在 `scripts/bench/run_trace_bench.py` 顶部修改 `DEFAULT_ONE_WAY_DELAY_MS`（自动透传 `--defaultDelayMs`）；
  - Linux 内核评测：通过 `run_kernel_lines.py --default-delay-ms <ms>`（或环境变量 `DEFAULT_ONE_WAY_DELAY_MS`）覆盖。
- **场景名唯一性**：同一数据集目录下递归扫描得到的场景名不允许重复。

---

## 二、标准 CSV 列定义与优先级回落规则

### 1. 编码与基本语法

- **编码**：UTF-8 纯文本（兼容 Excel 导出的 UTF-8 BOM）。
- **标准 CSV（RFC 4180）**：使用英文逗号 `,` 分隔；`comment` 等字段若包含英文逗号、双引号或首尾空格，用双引号 `"..."` 包裹（内部双引号写为 `""`）。空行与表外以 `#` 开头的整行注释会被忽略。
- **表头行**：第一条有效非注释行**必须是表头**。列名不区分大小写，**列出现顺序任意**（既可以所有场景采用完全一致的固定 8 列模板，也可以只写实际用到的子集列）；不允许未知列名或重复列名。

### 2. 列定义表（8 个标准列，列名均不含单位）

| 列名 | 必填/可选 | 默认单位 | 含义说明 |
|---|---|---|---|
| `time` | **必填** | `ms`（整数毫秒） | 严格单调递增的非负时间点（`t_0 = 0`，`t_i > t_{i-1}`），末行标定场景结束时刻 `end_ms` |
| `cap` | 容量列（见下方规则） | `1500 B` 发包数/ms | 上下行**通用**每 1 ms 发送能力（当同行的 `up_cap` 或 `down_cap` 未填写时作为回落值） |
| `up_cap` | 容量列（见下方规则） | `1500 B` 发包数/ms | **上行**（TCP 数据方向）每 1 ms 发送能力；非空时优先覆盖同行 `cap` |
| `down_cap` | 容量列（见下方规则） | `1500 B` 发包数/ms | **下行**（TCP ACK 方向）每 1 ms 发送能力；非空时优先覆盖同行 `cap` |
| `delay` | 可选 | `ms`（毫秒） | 上下行**通用**单向基础传播延迟（当同行的 `up_delay` 或 `down_delay` 未填写时作为回落值） |
| `up_delay` | 可选 | `ms`（毫秒） | **上行**单向基础传播延迟；非空时优先覆盖同行 `delay` |
| `down_delay` | 可选 | `ms`（毫秒） | **下行**单向基础传播延迟；非空时优先覆盖同行 `delay` |
| `comment` | 可选 | — | **行级注释列**：自由文本，用于标注阶段名称、切换/断流事件或结束标记；仿真器完全忽略该列内容，任意行均可留空 |

### 3. 通用列与方向列的逐行优先级回落规则

允许 `cap` 与 `up_cap`/`down_cap` 共存，也允许 `delay` 与 `up_delay`/`down_delay` 共存。对**每一行**，各通道按以下优先级取值（**方向专用列 > 通用列 > 沿用上一行前值**）：

| 目标通道 | 本行有效取值优先级 |
|---|---|
| **上行容量** | `up_cap`（非空） → `cap`（非空） → 沿用上一行上行容量 |
| **下行容量** | `down_cap`（非空） → `cap`（非空） → 沿用上一行下行容量 |
| **上行延迟** | `up_delay`（非空） → `delay`（非空） → 沿用上一行上行延迟（`t = 0` 仍空则全程用脚本默认延迟） |
| **下行延迟** | `down_delay`（非空） → `delay`（非空） → 沿用上一行下行延迟（`t = 0` 仍空则全程用脚本默认延迟） |

---

## 三、单元格数值与单位语法

**通用规则**：数值与单位后缀之间**不能有空格**（如 `10ms`、`1.5s`、`10.33kb` 合法；`10 ms`、`10 kb` 非法）。

### 1. 时间列（`time`）

| 写法 | 含义与换算 | 允许范围 | 示例 |
|---|---|---|---|
| 无后缀（默认） | 毫秒（`ms`） | 非负**整数**（仅 `0–9`） | `0`、`1`、`1000` |
| `ms`（不区分大小写） | 毫秒 | 非负有限数，换算结果须为精确整数 ms | `0ms`、`500ms`、`1000MS` |
| `s`（不区分大小写） | 秒（`× 1000 ms`） | 非负有限数，换算结果须为精确整数 ms（最多 3 位小数） | `0s`、`1s`、`1.5s`、`60s` |

> 换算结果必须落在 `0 <= t_ms <= 10^12` 且为整数毫秒（拒绝 `1.5`、`1.5ms`、`0.0005s` 等亚毫秒时间点）。

### 2. 容量列（`cap` / `up_cap` / `down_cap`）

| 写法 | 含义与换算 | 允许范围 | 示例 |
|---|---|---|---|
| 无后缀（默认） | 每 1 ms 可发送的 **1500 字节**发包机会数（`1 = 1500 B/ms = 12 Mbps`） | 非负**整数**（仅 `0–9`），`0 <= v <= 1,000,000` | `20`（240 Mbps） |
| `b`、`kb`、`mb`（不区分大小写） | 每 1 ms 可发送字节数（二进制 1024：`1kb = 1024 B`，`1mb = 1024² B`），换算为 `bytes_per_ms / 1500.0` 机会/ms | 非负有限**小数或整数**，换算后机会数 `<= 1,000,000` | `1500b`、`10.33kb`、`0.25mb` |
| 小写 `m` | 链路速率（Mbps），换算为 `Mbps / 12.0` 机会/ms | 非负有限**小数或整数**，`0 <= v <= 12,000,000` | `40m`、`50.11m` |

> 为避免歧义，拒绝无后缀小数（如 `2.5`、`2.0`）、`mbps`、单独 `k` 或大写 `M`。

### 3. 延迟列（`delay` / `up_delay` / `down_delay`）

| 写法 | 含义与换算 | 允许范围 | 示例 |
|---|---|---|---|
| 无后缀（默认） / `ms`（不区分大小写） | 毫秒（`ms`） | 非负有限浮点数或整数 | `10`、`25.5`、`10ms` |
| `us`（不区分大小写） | 微秒（`÷ 1000 ms`） | 非负有限浮点数或整数 | `500us`（0.5 ms） |
| `s`（不区分大小写） | 秒（`× 1000 ms`） | 非负有限浮点数或整数 | `0.1s`（100 ms）、`1s` |

---

## 四、严格校验规则

Python 解析器（`lines_common.py::parse_line_csv`）与 ns-3 加载器（`line-model.cc::ReadLineCsv`）执行完全一致的校验：

1. **列覆盖要求**：表头必须包含 `time`，且容量列必须能覆盖上下行（即包含 `cap`，或同时包含 `up_cap` 与 `down_cap`）。
2. **首行约束（`t = 0`）**：第一条数据行必须满足 `time == 0`，且经过优先级回落后，上/下行容量均不能为空。
3. **单调性与结束标记**：至少 2 条数据行，`time` 换算为 ms 后严格递增；**最后一行（`t = end_ms > 0`）经过回落后，上/下行容量都必须显式为 `0`**（写 `cap=0` 或 `up_cap=0, down_cap=0` 均可），延迟列推荐留空。
4. **延迟一致性**：若某方向在 `t = 0` 回落解析后为空（即启用默认延迟），则该方向在后续行中不得再出现非空延迟值。
5. **非空单元格全校验**：即使某行同时填了 `cap` 与 `up_cap`/`down_cap`，所有非空单元格都会做合法性校验。
6. **列数约束**：每行字段数不得超过表头列数（末尾省略的可选空列自动视为空字符串）。

---

## 五、完整示例

### 示例 1：统一固定 8 列表头（同表混用 `cap`/`up_cap`、`delay`/`up_delay` 与时间/延迟单位）

```csv
time,cap,up_cap,down_cap,delay,up_delay,down_delay,comment
0,100m,,,10ms,,,对称 100 Mbps / 单向 10 ms（RTT 20 ms）
20s,,20m,,,40ms,,仅上行骤降至 20 Mbps 且上行延迟升至 40 ms（下行沿用 100m / 10ms）
40s,100m,,,10ms,,,"双方向同步恢复, RTT 回到 20 ms"
60s,0,,,,,,end
```

### 示例 2：只写 `delay` + `up_delay`（下行无专用列，自动回落 `delay`）

```csv
time,cap,delay,up_delay,comment
0,50m,10,30,上行 30 ms、下行回落用 10 ms（RTT 40 ms）
1s,50m,15,40,上行 40 ms、下行回落用 15 ms（RTT 55 ms）
2s,0,,,end
```

### 示例 3：对称 5G 实测轨迹（无延迟列，走 Python 默认延迟）

```csv
time,cap
0,30.76kb
12,32.23kb
13,30.76kb
800000,0
```

### 示例 4：非对称蜂窝轨迹（上下行变化时刻独立，空单元格沿用前值）

```csv
time,up_cap,down_cap
0,8.79kb,1.46kb
1,0,
6,,2.93kb
12,14.65kb,1.46kb
60000,0,0
```

---

## 六、仿真执行与门控语义

在 ns-3 `tcp-trace-bench`（`link-gates.h::MahimahiGate`、`line-model.cc`）中，CSV 轨迹按以下规则回放：

1. **场景周期与循环**：
   - `bulk` 模式运行到 `end_ms / 1000` 秒停止，不循环；`.wk` workload 模式通过 `--linePeriodMs=<end_ms>` 按 `t % end_ms` 循环回放，所有请求收完即提前结束。
2. **每毫秒字节额度与小数累积**：
   - 第 `k` 毫秒发放字节额度 `bytes = 1500.0 * value + fractionalBytes`（对 `b`/`kb`/`mb` 写法即为精确的每毫秒字节数），向下取整为本毫秒可用整数额度 `left = floor(bytes + 1e-9)`，小数余量 `bytes - left` 累积到下一毫秒以保证 `Xm` 及小数单位写法长期无漂移。
   - **未使用的整数额度不会留到下一毫秒**（避免静默后产生虚假突发）。
   - 队头数据包按实际字节大小（扣除 2 字节 PPP 头）消费额度，可跨多个毫秒逐步发送；最后一字节发完时递交接收端并记录该包排队延迟。
3. **单向保序延迟**：
   - 数据包进入门控前按当前毫秒对应的延迟值 `d` 安排入队时间，并用 `lastArrival = max(lastArrival, now + d)` 严格保序，避免基础延迟骤降时发生乱序超车。
4. **队列缓冲与丢包**：
   - 上下行门控队列容量分别受 `--queueBytes` 限制（`run_trace_bench.py` 默认 `auto`，按场景平均容量和基础 RTT 定标为 `clamp(max(1.5*BDP, 80ms), 500 KB, 16 MiB)`）；入队时若 `queued + pkt_size > queueBytes` 则尾丢弃并计入门控丢包率。

---

## 七、从原始 Trace 转换与格式校验

### 1. 批量/单组转换（`scripts/data/trace_to_lines.py`）

- 顶部配置 `DATASET_DIR` 后直接运行（默认将 `datasets/traces/lumos/` 转为 `datasets/lines/lumos/*.csv`）：
  ```sh
  python3 scripts/data/trace_to_lines.py
  ```
- 命令行转换整个目录（输出到 `datasets/lines/<目录名>/<场景名>.csv`）：
  ```sh
  python3 scripts/data/trace_to_lines.py datasets/traces/cellular-traces-2018
  ```
- 命令行转换单组文件（输出到 `datasets/lines/converted/<场景名>.csv`）：
  ```sh
  python3 scripts/data/trace_to_lines.py input.up input.down [delay.txt]
  ```
- **输出规范**：
  - 容量默认优先以小数 `kb` / `mb` 输出（`CAPACITY_DECIMALS = 2`，可通过 `--decimals <N>` 调整，如 `1.46kb`、`10.25kb`、`30.76kb`、`1.43mb`，零值写 `0`）。
  - 对称容量（Lumos、LeoCC）自动输出精简表头 `time,cap`（带延迟时为 `time,cap,delay`）；非对称上下行（Mahimahi `.up`/`.down`）输出 `time,up_cap,down_cap`。
  - **仅当存在真实 `.delay` / `delay_*.txt` 源文件时才输出 `delay` 列**；无延迟源的数据集（如 Lumos、cellular-traces-2018）不构造多余延迟列，直接由评测脚本的 `DEFAULT_ONE_WAY_DELAY_MS` 生效。

### 2. 校验自定义 CSV 场景

```sh
python3 -c "from pathlib import Path; import sys; sys.path.insert(0, 'scripts/bench'); import lines_common; print(len(lines_common.discover(Path('datasets/lines/special'))))"
```
