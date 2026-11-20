# 移动场景

移动场景是 ns-3 的 LTE 仿真，含切换，程序是 `scratch/tcp-lte-highspeed-handover.cc`，批量运行由 `scratch/run-sweep.py` 负责。每次运行都配一次饱和 UDP 探测，信道实现相同，利用率以探测到的容量为基准。

## 四个场景

| 场景 | 名称 | 描述方式 | 速度 | 运行次数 |
|---|---|---|---|---|
| 高铁 4G | `hsr-4g` | 命令行参数 | 350 km/h | 3 |
| 高速 4G | `hwy-4g` | 命令行参数 | 120 km/h | 3 |
| 步行 | `walk-line` | JSON 线路 `walk.json` | 约 5 km/h | 1 |
| 跑步 | `run-line` | JSON 线路 `run.json` | 10–12 km/h | 1 |

共同设置：LTE 频段 3（EARFCN 1575），20 MHz（100 RB），RLC UM，比例公平调度。

- **高铁 4G**：基站间距 1.2 km，共 11 个，离轨道 50 m，2 km 隧道，24 dB 车厢穿透损耗，仿真 125 s。
- **高速 4G**：基站间距 1 km，共 7 个，离道路 80 m，9 dB 车身穿透损耗，EVA 120 km/h 衰落，仿真 185 s。多流实验也在这个场景上运行，参数见 `config/hwy.args`。
- **步行和跑步**：3GPP UMa 城区宏站，ETU 5、12 km/h 衰落，其余由 JSON 线路决定。

## JSON 线路格式

用 JSON 文件描述一次移动过程，是本项目原创的做法。用它可以不改代码，只写一个文件，就得到时变的速度、干扰、遮挡和负载，以及真实的基站布局。

一个文件描述一次运行，包括两类内容：

- **按时间写**：速度、突发干扰、隧道、遮挡损耗、周围人群。
- **按位置写**：三维轨迹、基站和覆盖范围、固定干扰源。

程序沿三维折线对速度积分，得到终端每个时刻的位置，再计算它和各基站、干扰源之间的距离。

完整字段见 [`FORMAT.md`](../ns3-overlay/scratch/lines/FORMAT.md)，使用方法见 [`README.md`](../ns3-overlay/scratch/lines/README.md)，布站参考见 [`LAYOUT.md`](../ns3-overlay/scratch/lines/LAYOUT.md)。实现在 `scratch/line3d.h`。

### 最小示例

```json
{
  "tech": "4g",
  "defaults": { "car_loss_db": 3 },
  "track": { "points": [[0, 0, 0], [125, 0, 0], [125, 69, 0]] },
  "timeline": {
    "0-40":   { "speed_kmh": 5 },
    "40-70":  { "speed_kmh": 5, "car_loss_db": 18 },
    "70-90":  { "speed_kmh": [5, 0] },
    "90-120": { "speed_kmh": 0, "crowd_ues": 15, "crowd_mbps": 2 }
  },
  "cells": [
    { "id": "M1", "track_m": 10, "side": "left", "offset_m": 60, "height_m": 25, "range_m": 300 }
  ]
}
```

键名带单位（`_s`、`_m`、`_kmh`、`_db`、`_dbm`），以 `_` 开头的键是注释，其余未知字段会报错。时间段必须从 0 开始、首尾相接，最后一段的结束时刻就是仿真时长。

### `timeline` 的主要字段

| 字段 | 含义 |
|---|---|
| `speed_kmh` | 一个数表示匀速，`[起, 止]` 表示线性变速，`0` 表示停止 |
| `car_loss_db` | 这段时间的穿透或遮挡损耗，如楼间遮挡、电梯轿厢 |
| `tunnel` | 这段时间在隧道或地下通道里 |
| `interference_dbm` | 这段时间的突发干扰 |
| `crowd_ues`、`crowd_mbps` | 周围人群的用户数和每人的下行负载 |

### 步行线路

全程 300 s，约 315 m，5 个基站。

| 时段 (s) | 过程 | 主要设置 |
|---|---|---|
| 0–40 | 临街人行道 | 5 km/h |
| 40–70 | 楼间窄巷，宏站被挡 | 损耗 18 dB |
| 70–90 | 走到路口，减速 | 5 → 0 km/h |
| 90–120 | 等红灯 | 15 人，每人 2 Mbit/s |
| 120–125 | 起步 | 0 → 5 km/h |
| 125–175 | 地下通道，无室内覆盖 | `tunnel` |
| 175–235 | 商业步行街 | 损耗 7 dB，40 人，每人 3 Mbit/s |
| 235–270 | 进写字楼大堂，等电梯 | 损耗 15 dB，等候时 8 人 |
| 270–283 | 乘电梯上 6 楼 | 损耗 38 dB |
| 283–300 | 办公层走廊 | 损耗 12 dB |

### 跑步线路

全程 320 s，约 664 m，5 个基站。

| 时段 (s) | 过程 | 主要设置 |
|---|---|---|
| 0–70 | 起跑，滨江公园，站点稀疏 | 10 km/h |
| 70–85 | 高架桥下的通道 | `tunnel` |
| 85–125 | CBD 背街 | 12 km/h，损耗 16 dB，干扰 −95 dBm |
| 125–165 | 路口等红灯 | 20 人，每人 3 Mbit/s |
| 165–215 | 人行街慢跑 | 8 km/h，损耗 7 dB，40 人 |
| 215–260 | 恢复配速后放松 | 12 → 5 km/h |
| 260–290 | 进住宅楼，等电梯 | 损耗 15 dB |
| 290–305 | 乘电梯上 5 楼 | 损耗 38 dB |
| 305–320 | 5 楼走廊 | 损耗 10 dB |

### 其他线路

城市和乡村还有 10 条线路由 `scripts/gen_lines.py` 生成，全部用例见 [`README.md`](../README.md) 的“用例清单”。`hsr.json`（高铁）和 `highway.json`（高速）对应 `hsr-line`、`hwy-line`，有变速、弯道和隧道；`hsr-4g`、`hwy-4g` 是匀速直线的固定场景，不用这两个文件。

### 写自己的线路

先检查并预览，这一步不需要运行 ns-3，但需要 matplotlib：

```bash
cd ns-3
python3 scratch/preview-line.py scratch/lines/myline.json
```

然后运行：

```bash
python3 scratch/run-sweep.py hsr-line --lines myline --ccs copa-auto,bbr --runs 3
```

`hsr-line` 的无线和 TCP 设置取自 `hsr-4g`。

## 运行

```bash
make mobile JOBS=9
make lines JOBS=9
```

结果汇总在 `results/mobile-summary.json` 和 `results/lines-summary.json`。步行和跑步线路单次仿真约 40 分钟。

## 局限

- 步行和跑步在 `make mobile` 中只有 1 次运行，在 `make lines` 中有 3 次，结果仍不稳定。
- 线路是根据典型城市环境设定的，不是实测轨迹。
