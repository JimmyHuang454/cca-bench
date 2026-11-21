# Trace 转换格式

在 `scripts/data/trace_to_lines.py` 顶部设置 `DATASET_DIR`，然后直接运行：

```sh
python3 scripts/data/trace_to_lines.py
```

默认转换 `datasets/traces/lumos/`，输出到 `datasets/lines/lumos/`。要转换单组文件，将 `DATASET_DIR` 设为 `None`，并设置 `UPLINK_TRACE`、`DOWNLINK_TRACE` 和可选的 `DELAY_TRACE`；输出到 `datasets/lines/converted/`。

也可以通过命令行临时指定一组 Mahimahi 上下行发包机会，以及可选的 LeoCC 延迟文件：

```sh
python3 scripts/data/trace_to_lines.py input.up input.down [delay.txt]
```

输出位于 `datasets/lines/converted/<场景名>/`，以输入的下行文件名为场景名，分别生成 `up.txt`、`down.txt`，提供延迟时再生成 `up.delay.txt` 和 `down.delay.txt`。

也可转换整个数据集：

```sh
python3 scripts/data/trace_to_lines.py datasets/traces/cellular-traces-2018
```

输出到 `datasets/lines/<输入目录名>/`。目录模式配对同名 `.up`、`.down`，可选配对同名 `.delay`。LeoCC 的 `bw_<名称>.txt` 和 `delay_<名称>.txt` 配对，带宽用于上下行；项目示例 `leo-bw.txt` 和 `leo-delay.txt` 也支持。Lumos 的 `5g_trace_*.down` / `4g_trace_*.down` 同时生成相同的上行和下行时间序列；对称上行是实验假设，不是实测上行。Lumos 若没有延迟输入，默认生成上下行各 20 ms 的单向延迟文件（基础 RTT 40 ms），可在脚本中调整 `LUMOS_ONE_WAY_DELAY_MS`。Lumos 的 `.mbps` 文件不参与本转换。缺少必须的配对文件时报错。目标文件已存在时报错，不覆盖原始文件或已有结果。

每个场景的文件放在独立的同名目录中，例如：

```text
datasets/lines/lumos/5g_trace_100_walking/
  up.txt
  down.txt
  up.delay.txt
  down.delay.txt
```

单组文件转换的输出目录同样是 `datasets/lines/converted/<场景名>/`。bench 的 `LINES_DIR` 可以指定数据集目录或单个场景目录；也兼容旧的带场景名前缀的文件。

Lumos 的 `.down` 表示下载方向；目前 `tcp-trace-bench` 的数据固定走 `.up`，ACK 走 `.down`。当前上下行使用相同时间序列，因此数据可经过由下行轨迹生成的容量变化；这不能模拟独立的真实上行或下载方向特有的非对称行为。

## 容量

每行 `时间点,发包机会数`，时间单位 ms，机会数是从该时间点起每 1 ms 可发送的 1500 字节机会数。连续相同的值合并，区间左闭右开。

容量值也可带小写 `m` 后缀，表示 Mbps，可以使用小数；同一个文件可以混用两种写法。例如：

```text
0,10
1000,20
3000,40m
4000,50.11m
5000,60
6000,0
```

其中 `1000,20` 表示每毫秒 20 个 1500 字节发包机会，`3000,40m` 表示 40 Mbps，`4000,50.11m` 表示 50.11 Mbps。容量每毫秒换算为字节额度，小数部分跨毫秒累计以保留精度；未使用的整数额度不会留到下一个毫秒。该后缀仅用于容量文件，延迟文件仍以 ms 表示。

```text
0,1
1,0
5,5
7,0
```

例如 `[5, 7)` 的每毫秒有 5 次机会。最后一行是结束时间及 0；转换器将原始最后一个时间戳 `T` 所在的 `[T, T+1)` 区间完整保留，因此结束时间为 `T+1`。结束后容量保持 0，不循环。上下行结束时间独立。

## 延迟

可选延迟输入遵循 LeoCC 格式：每行一个单向延迟值（ms），每个样本持续 10 ms。同一输入分别生成内容相同的上下行延迟文件。输出的每行 `时间点,延迟`，时间点和延迟单位均为 ms，连续相同的值合并：

```text
0,20
20,25
30,18
# end_ms=40
```

`# end_ms=40` 标记采样结束时间，并非一个延迟值；40 ms 之后没有观测值，不自动循环或推断后续延迟。没有延迟输入时不生成延迟文件。

该格式保留每毫秒机会数，不保留单个机会的发送顺序；不保证等同原始 Mahimahi 循环回放。`tcp-trace-bench` 使用每毫秒门控读取该格式。
