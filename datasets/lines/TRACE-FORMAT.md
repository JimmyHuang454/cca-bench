# Trace 转换格式

转换一组 Mahimahi 上下行发包机会，可选提供 LeoCC 延迟文件：

```sh
python3 scripts/data/trace_to_lines.py input.up input.down [delay.txt]
```

输出位于 `datasets/lines/converted/`，以输入的下行文件名为场景名，分别生成 `<场景>.up.txt`、`<场景>.down.txt`，提供延迟时再生成 `<场景>.up.delay.txt` 和 `<场景>.down.delay.txt`。

也可转换整个数据集：

```sh
python3 scripts/data/trace_to_lines.py datasets/traces/cellular-traces-2018
```

输出到 `datasets/lines/<输入目录名>/`。目录模式配对同名 `.up`、`.down`，可选配对同名 `.delay`。LeoCC 的 `bw_<名称>.txt` 和 `delay_<名称>.txt` 配对，带宽用于上下行；项目示例 `leo-bw.txt` 和 `leo-delay.txt` 也支持。缺少必须的配对文件时报错。目标文件已存在时报错，不覆盖原始文件或已有结果。

## 容量

每行 `时间点,发包机会数`，时间单位 ms，机会数是从该时间点起每 1 ms 可发送的 1500 字节机会数。连续相同的值合并，区间左闭右开。

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
