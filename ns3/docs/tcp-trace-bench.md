# TCP Trace Bench

在 `scripts/bench/run_trace_bench.py` 顶部设置 `LINES_DIR`、`WORKLOADS`、`CCS`、`REPEATS` 和 `MAX_WORKERS`。先运行 `make build-bench`，然后执行 `make bench` 或 `python3 scripts/bench/run_trace_bench.py`。新入口只读取 lines TXT；旧数据集与物理层测试保留在 `make -C ns3 legacy-bench` 及原有专用目标。

`LINES_DIR` 可设为 `datasets/lines/lumos` 运行整个数据集，或设为 `datasets/lines/lumos/5g_trace_100_walking` 只运行该场景。结果分别写到 `results/trace-bench/lumos` 和 `results/trace-bench/lumos/5g_trace_100_walking`。

`MAX_WORKERS` 默认使用操作系统报告的逻辑 CPU 数，限制同时运行的独立 ns-3 进程数；设为 1 可串行运行。实际并发数不会超过待运行任务数。并发只影响实际执行时间，不改变仿真时间、输入或随机种子。每个任务由主进程汇总并保存断点；中断时停止正在运行的子进程，之后可续跑。

终端显示实验进度和每个结束任务的记录，包括场景、算法、负载、重复编号、实际运行耗时及主要指标；失败任务显示原因。最终概览仅统计所有重复均成功且指标有效的场景：Norm Power 按场景取算术平均，FCT Slowdown 按场景取几何平均；每项附完整场景数。详细结果和排名见报告。

每个场景目录必须包含 `up.txt` 与 `down.txt`，可选 `up.delay.txt` 和 `down.delay.txt`；也兼容旧的带场景名前缀的文件。格式见 `datasets/lines/TRACE-FORMAT.md`。两方向初始时刻为 0，容量文件必须以结束时刻的 0 结尾；延迟文件必须以 `# end_ms=<时间>` 标记有效时长。Bulk 运行时长取所有已提供文件的最短时长，不循环。Workload 使用该最短有效时长作为整个场景的共同循环周期；上下行容量和延迟同时回到起点，超过共同周期的数据不参与回放。缺失延迟使用配置里的单向延迟，默认 10 ms。

TCP 数据走上行，ACK 走下行。容量可以用每毫秒 1500 字节发包机会数，或带 `m` 后缀的 Mbps 表示；每毫秒的容量集中在该毫秒边界。延迟在包进入方向时采样，保持包的到达顺序，基础 RTT 的报告值为 ACK RTT 样本对应的双向基础延迟平均值。现有物理层并未被该回放模式模拟。

`bulk` 持续提交数据；其他 workload 每行包含相对前一事件的间隔 ms 与数据块字节数。应用写满 TCP socket 后等待可写回调；TCP 对端按字节范围记录数据块完成时间。所有 workload 使用一个长期 TCP 连接，不提供业务响应。数据块从仿真 1 ms 起安排，FCT 包含等待 TCP 连接建立和应用排队的时间。bulk 自仿真开始计时，无预热。Workload 在所有数据块被接收时结束，报告中的完成时间是从仿真开始到最后一块到达的时长；最后一块产生后最多等待配置的 `WORKLOAD_DRAIN_TIMEOUT_S`，超时则报告未完成。

输出固定在 `results/trace-bench/<数据集>/`：`runs.csv` 为逐次运行结果，`report.md` 按场景、算法和负载汇总，`metadata.json` 保存文件摘要及内部配置，`runs/` 保存每次运行的日志和断点。每次完成一个组合就更新报告和断点；部分运行失败或数据块未全部完成时保留报告并返回非零退出码。FCT slowdown 的理想值使用每个数据块产生时的基础 RTT 和全程平均上行容量，是动态链路下的近似。

报告按场景给出 bulk Norm Power 排名，并按场景和 workload 给出 FCT Slowdown 排名。综合排名按场景将所有选定 workload 一起计算：每种 workload 的重复运行先取算术平均，再对各 workload 等权取几何均值；综合分数为 `sqrt(Norm Power / Slowdown 几何均值)`。所有数值使用未按表格精度四舍五入的重复运行平均值计算。bulk 和全部 workload 的重复都必须成功且指标有效才能参与综合排名，基准为零时不计算差距。

`run_trace_bench.py` 中的 `RESUME = True` 默认启用断点续跑：重启时跳过输入、程序和参数一致的成功运行，失败或未完成的运行重新执行。设置为 `False` 可重新运行全部组合并覆盖对应结果。更改 lines、workload、二进制或运行参数后，对应旧断点不再复用；不同数据集同名目录也不会复用旧断点。调度和报告修改不会让旧结果失效；修改 Python 中的仿真语义时需要更新 `SIMULATION_SIGNATURE`。历史时间戳目录不会自动删除。

链路等待缓冲使用程序内的固定配置以便重复；runner 将 TCP 数据段大小设置为 1448 字节，以适配 1500 字节 MTU、IPv4 和 TCP 时间戳；报告及元数据记录该设置。TCP 的缓冲、重传超时和恢复等使用 ns-3 默认配置。Copa 的项目内实现会为自身开启算法所需的 pacing，BBR 和 ns-3 的 Copa 也由算法自身控制 pacing。ns-3 TCP 是模拟实现，不等于运行 Linux 内核 TCP。

## 源码结构

`ns3/ns3-overlay/scratch/tcp-trace-bench/` 下，`tcp-trace-bench.cc` 负责组装仿真和统计，`line-model.*` 负责时间序列读取，`link-gates.h` 实现链路门控，`workload.h` 实现应用读写，`cc-config.*` 负责算法配置。
