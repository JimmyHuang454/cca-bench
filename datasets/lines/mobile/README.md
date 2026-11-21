# Mobile 场景

本目录包含 24 个人为构造的移动网络基准场景：8 个 4G、8 个 5G、6 个 Wi-Fi，以及 Wi-Fi 与 4G 相互切换的 2 个场景。每个子目录的 README 说明用途和完整的分段参数。这些轨迹并非实测数据，不模拟无线协议细节。

每个场景包含 `up.txt`、`down.txt`、`up.delay.txt` 和 `down.delay.txt`；格式见 [TRACE-FORMAT.md](../TRACE-FORMAT.md)。TCP 数据走上行，ACK 走下行。所有场景持续 120 秒，容量为零的时段表示对应方向没有发送机会。
