# Basic 动态链路 JSON

每个 JSON 定义一个场景。`segments` 顺序执行，每段指定持续时间、平均带宽、丢包率和基础 RTT；各段时长之和必须等于 `duration_s`。`tick_s` 指定带宽更新间隔，`sigma` 指定带宽对数正态随机波动强度；每次重复使用独立种子，相同种子对所有算法共用同一条链路。

`base_rtt_ms` 包含两侧接入链路延迟：瓶颈单向延迟为 `base_rtt_ms / 2 - access_delay_ms`，不得小于 0。`queues` 与其他 bench 场景一致，可用 `QUEUES` 覆盖。

`BASIC_GROUPS` 和 `BASIC_SIGMAS` 是 JSON 筛选条件，不会自动生成新场景。添加场景时请使用唯一的 `name`，并设置可匹配的 `group` 与 `sigma`。修改已有场景后，使用新 `OUT_DIR` 或设置 `RESUME=0`，避免复用旧结果。
