# Trace 数据集目录

`datasets/traces/` 是本地缓存，不纳入 Git。数据按来源保存：

- `mahimahi/`：Mahimahi LTE 驾车轨迹及上行原始文件。
- `lumos/`：Lumos 4G/5G 原始采样、转换后的下行轨迹及共用 ACK 轨迹；下载的上游仓库也放在此目录。
- `cellular-traces-nyc/`：Cellular-Traces-NYC 数据集。
- `cellular-traces-2018/`：Cellular-Traces-2018 地铁上下行轨迹。
- `leocc/`：LeoCC 示例带宽和延迟轨迹。
- `synthetic/`：合成轨迹。
- `shifted/`：bench 运行时生成的重复实验偏移缓存，不是原始数据集。

`--trace-dir` 仍指向这些目录的父目录；脚本按数据集自动选择子目录。通过 `--local-leo-dir` 指定的外部 LeoCC 数据保持在原位置。
