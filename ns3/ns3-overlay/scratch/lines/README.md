# 线路配置（line）使用说明

这个目录存放线路的配置文件。每个 `.json` 文件描述一次运行（高铁列车、高速公路上的汽车，或城市里走路、跑步的人），包括：

- 轨道走向；
- 列车每个时间段的车速；
- 基站的位置和辐射范围；
- 隧道；
- 外部干扰。

| 文件 | 内容 |
|---|---|
| `README.md` | 本文：使用方法、字段速查、常见写法 |
| `FORMAT.md` | 完整的字段说明：每个字段的含义、默认值、校验规则 |
| `LAYOUT.md` | 高铁沿线一般怎么布站，仿真里有哪些可以控制的参数 |
| `hsr.json` | 高铁线路，全程 210 s、11.4 km，含加速、干扰、覆盖空洞、隧道、弯道、停站（研究 `hsr-line`） |
| `highway.json` | 高速公路线路，全程 180 s、4.4 km，含匝道加速、稀疏站点、隧道、拥堵停车、城郊加密站点（研究 `hwy-line`） |
| `walk.json` | 城市步行 5 km/h，全程 300 s、315 m，含楼间弱信号、路口人群、地下通道、商业街密集人群、写字楼大堂、电梯（研究 `walk-line`，说明见 [../docs/pedestrian.md](../docs/pedestrian.md)） |
| `run.json` | 城市跑步 10–12 km/h，全程 320 s、664 m，含稀疏站点的公园、高架下通道、CBD 背街遮挡 + 干扰、路口人群、人行街慢跑、电梯（研究 `run-line`） |

布局图 `<名字>-preview.png`、`<名字>-cells.png` 由 `preview-line.py` 画在本目录；跑完仿真后 `plot-line3d.py` 也会在结果目录里放一份。

> **当前状态**：4G 线路可以仿真（`tcp-lte-highspeed-handover --scenario=...`）。5G 线路还不能用 JSON 描述（`tech: "5g"` 会报错），5G 用 `hsr-5g` / `hwy-5g` 两个参数化场景，见 [../docs/real.md](../docs/real.md)。

## 一、快速上手

```bash
cd ns-3-dev-git

# 1. 复制示例，改成自己的线路
cp scratch/lines/hsr.json scratch/lines/myline.json      # 汽车线路可以从 highway.json 改

# 2. 检查并画出布局图（不需要编译或运行 ns-3）
python3 scratch/preview-line.py scratch/lines/myline.json
```

运行环境：Python 3 和 matplotlib。

第 2 步会做三件事：

1. **校验文件**。有错误时会指出具体位置，例如 `cells[0]: unknown field(s) ['ofset_m']`。
2. **在终端打印两张表**：
   - 每个时间段的车速、对应的里程、干扰、是否在隧道；
   - 每个基站的三维坐标、覆盖范围、所在里程，以及在轨道哪一侧、离轨道多少米。
3. **在 json 文件所在目录生成两张图**：
   - `myline-preview.png`：俯视图（真实坐标）、沿线展开图（看清基站在轨道左右多远）、车速曲线；
   - `myline-cells.png`：每个基站一张真实比例的近景，画出它到轨道的垂直偏移。

用 `--out 目录` 可以把图输出到别处。

## 二、文件结构一览

```json
{
  "tech": "4g",
  "defaults":   { 默认值 },
  "track":      { "points": [[x, y, z], ...] },
  "timeline":   { "起-止": { 这段时间的车速、干扰、隧道 }, ... },
  "cells":      [ 逐个列出的基站 ],
  "cell_grid":  [ 沿线批量布置的基站 ],
  "interferers":[ 固定干扰源 ]
}
```

- **两类信息**：
  - **按时间写**：车速、突发干扰、隧道。写在 `timeline` 里，键是 `"起-止"` 秒。
  - **按位置写**：轨道、基站、固定干扰源。单位是米。
- **单位后缀**：单位写在字段名里，`_s` 秒，`_m` 米，`_kmh` km/h，`_deg` 度，`_db` dB，`_dbm` dBm，`_dbi` dBi。
- **注释**：以 `_` 开头的字段（如 `"_comment"`）会被忽略，可以写注释。
- **拼写检查**：其他不认识的字段一律报错，防止拼错后被悄悄忽略。

## 三、字段速查

各字段的完整含义和默认值见 `FORMAT.md`。

### 顶层

| 字段 | 必填 | 说明 |
|---|---|---|
| `tech` | 否 | `"4g"`（默认）或 `"5g"` |
| `defaults` | 否 | 默认值 |
| `track` | 否 | 轨道折线；不写时为沿 x 轴的直线 |
| `timeline` | 是 | 时间段 |
| `cells` / `cell_grid` | 至少一个基站 | 基站 |
| `interferers` | 否 | 固定干扰源 |

### `defaults`

| 字段 | 默认 | 说明 |
|---|---|---|
| `car_loss_db` | 0 | 车体穿透损耗 |
| `edge_rsrp_dbm` | -110 | `soft` 范围边缘的信号门限 |
| `cell` | | 所有基站的默认值，可写 `side`、`offset_m`、`height_m`、`tx_dbm`、`range_m`、`range_mode`、`antenna` |
| `tunnel_cell_spacing_m` | 500 | 隧道内小区间距 |
| `tunnel_loss_db` | 40 | 隧道内外之间的损耗 |

### `track`

| 字段 | 说明 |
|---|---|
| `points` | `[[x, y, z], ...]`，至少两个点，相邻点直线相连。总长度要不短于列车跑的距离 |

### `timeline` 每个时间段

| 字段 | 必填 | 说明 |
|---|---|---|
| `speed_kmh` | 是 | `350` 匀速；`[0, 350]` 线性加/减速；`0` 停车 |
| `interference_dbm` | 否 | 这段时间手机收到的干扰功率 |
| `tunnel` | 否 | `true` 表示这段时间在隧道里 |
| `car_loss_db` | 否 | 只在这段时间覆盖车体损耗 |

时间段要求：从 0 开始，首尾相接，不重叠。最后一段的结束时刻就是仿真时长。

### `cells` 每个基站

| 字段 | 默认 | 说明 |
|---|---|---|
| `id` | 序号 | 名称 |
| `track_m` | | 所在里程（与 `pos` 二选一） |
| `side` | `"left"` | 在前进方向的 `"left"` 或 `"right"` |
| `offset_m` | 50 | 离轨道中心线的水平距离 |
| `height_m` | 35 | 天线高出轨面的高度 |
| `pos` | | 绝对坐标 `[x, y, z]`（与 `track_m` 二选一） |
| `range_m` | | 辐射范围 |
| `range_mode` | `"soft"` | `"soft"`：范围处信号刚好等于门限，范围外继续衰减；`"hard"`：范围外信号切断 |
| `tx_dbm` | 46 | 发射功率（`soft` 且给了 `range_m` 时由范围反推） |
| `antenna` | 全向 | `{"type": "sector", "azimuth_deg", "tilt_deg", "beamwidth_deg", "gain_dbi"}`；方位角 0 指向 +x，逆时针为正 |

### `cell_grid` 每组

| 字段 | 默认 | 说明 |
|---|---|---|
| `from_m`、`to_m` | 0、全程 | 起止里程 |
| `spacing_m` | 必填 | 站间距 |
| `side` | `"left"` | `"left"`、`"right"`，或 `"alternate"`（左右交替，从左侧开始） |
| `id_prefix` | `"g"` | 名称前缀，生成 A1、A2…… |
| 其余 | | `offset_m`、`height_m`、`range_m`、`range_mode`、`tx_dbm`、`antenna` |

### `interferers` 每个干扰源

| 字段 | 默认 | 说明 |
|---|---|---|
| `track_m` + `side` + `offset_m` + `height_m` | 左侧、100 m、20 m | 相对轨道定位 |
| `pos` | | 或者绝对坐标 |
| `range_m` | 必填 | 作用半径 |
| `power_dbm` | 必填 | 列车在范围内时手机收到的干扰功率 |
| `id` | `J1`、`J2`…… | 名称 |

## 四、常见写法

**在线路 10 m 处放一个基站（右侧 25 m）**

```json
"cells": [ { "id": "S", "track_m": 10, "side": "right", "offset_m": 25 } ]
```

**之字形布站：每 1200 m 一个，左右交替，离轨道 40 m**

```json
"cell_grid": [ { "from_m": 0, "to_m": 12000, "spacing_m": 1200, "side": "alternate",
                 "offset_m": 40, "range_m": 800, "id_prefix": "A" } ]
```

**弯道后加密站距**：再加一组 `cell_grid`，用不同的 `from_m` 和 `spacing_m`。

**山坡上离线路较远的站**

```json
{ "id": "P", "pos": [9800, 2300, 60] }
```

**定向扇区，主瓣沿线路方向**

```json
{ "id": "C", "track_m": 3000, "side": "right", "offset_m": 80,
  "antenna": { "type": "sector", "azimuth_deg": 0, "tilt_deg": 4, "beamwidth_deg": 65, "gain_dbi": 15 } }
```

**制造覆盖空洞**：把某个站设成硬边界，并让它的范围和下一个站接不上。

```json
{ "id": "E", "track_m": 4800, "range_m": 300, "range_mode": "hard" }
```

**起步加速、巡航、进站停车**

```json
"timeline": {
  "0-40":    { "speed_kmh": [0, 350] },
  "40-100":  { "speed_kmh": 350 },
  "100-130": { "speed_kmh": [350, 0] },
  "130-150": { "speed_kmh": 0 }
}
```

**某段时间有突发干扰**

```json
"60-80": { "speed_kmh": 350, "interference_dbm": -85 }
```

**某段时间过隧道**

```json
"80-100": { "speed_kmh": 350, "tunnel": true }
```

洞内小区会按 `tunnel_cell_spacing_m` 自动生成，不用自己写。

**线路旁固定的干扰源**（左侧 200 m，半径 500 m 内 -80 dBm）

```json
"interferers": [ { "track_m": 3000, "side": "left", "offset_m": 200, "range_m": 500, "power_dbm": -80 } ]
```

**干扰功率怎么选**：20 MHz 带宽、手机噪声系数 9 dB 时，热噪声约 -92 dBm。

| 干扰功率 | SINR 约下降 |
|---|---|
| -92 dBm | 3 dB |
| -82 dBm | 10 dB |
| -72 dBm | 20 dB |

## 五、常见报错

| 报错 | 原因 |
|---|---|
| `unknown field(s) [...]; allowed: [...]` | 字段名拼错了，后面列出了允许的字段 |
| `gap or overlap between '0-10' and '40-60'` | 时间段之间有空隙或重叠 |
| `timeline must start at 0 s` | 第一个时间段没有从 0 开始 |
| `give either pos [x, y, z] or track_m (not both)` | 基站或干扰源的位置写了两种，或一种都没写 |
| `track_m ... outside the track` | 里程超出了轨道长度 |
| `the train travels ... but the track is only ... long` | 车速积分出的距离比轨道长，需要把 `track.points` 延长 |
| `range_mode 'hard' needs range_m` | 硬边界必须给出范围 |
| `not valid JSON` | JSON 语法错误，常见原因是多了逗号，或用了单引号 |

## 六、运行仿真

```bash
# 先编译一次
./ns3 build tcp-lte-highspeed-handover

# 跑一条或几条线路（文件名不带 .json），每个算法重复 5 次
python3 scratch/run-sweep.py hsr-line --lines myline --ccs cubic,bbr --runs 5 --jobs 2
# 汽车线路用 hwy-line（快衰落换成 120 km/h 的 EVA）：
# python3 scratch/run-sweep.py hwy-line --lines myline --ccs cubic,bbr --runs 5 --jobs 2

# 分段统计表、随时间变化的图和布局图
python3 scratch/plot-line3d.py myline --ccs cubic,bbr
```

`run-sweep.py` 会先调用 `preview-line.py --export` 校验 JSON，并生成解析后的文件 `<名字>.resolved`。这个文件里是列车每 0.2 s 的位置、所有基站（包括洞内小区）的坐标和天线，以及干扰。之后的仿真都基于这个文件。每个随机种子还会额外跑一次饱和 UDP，作为容量参考。

**仿真里怎样用到 JSON 的各个字段**

| 字段 | 实现方式 |
|---|---|
| 列车运动 | 沿三维轨道按车速剖面移动（ns-3 `WaypointMobilityModel`） |
| `soft` 范围 | 按 3GPP RMa 视距平均路径损耗（不含阴影，含车体损耗）反推发射功率，使范围处的 RSRP 等于 `edge_rsrp_dbm`。实际发射功率写在输出的 `-line.data` 里 |
| `hard` 范围 | 超出范围后该基站信号额外衰减 300 dB，相当于没有信号 |
| 扇区天线 | 3GPP 风格方向图：水平和垂直方向各为抛物线，带下倾角，前后比 20 dB；全向天线增益为 0 dBi |
| 隧道 | 洞内小区沿轨道布置（离轨道 3 m、高 6 m）；洞内外之间的链路叠加 `tunnel_loss_db`，在洞口 100 m 内逐渐增大到该值 |
| 干扰 | 每 100 ms 根据时间和列车位置更新手机的干扰功率 |
| 车体损耗 | 按时间段切换 |
| 其余无线和 TCP 参数 | 1.8 GHz/20 MHz、RMa 视距、快衰落（`hsr-line`：莱斯 300 km/h；`hwy-line`：EVA 120 km/h）、A3 切换（迟滞 3 dB、TTT 256 ms）、RLC UM 1 MB、服务器单向时延 20 ms、最小 RTO 200 ms、延迟 ACK 40 ms |

**输出**（在 `results/sweep/hsr-line/` 或 `results/sweep/hwy-line/` 下）

| 文件 | 内容 |
|---|---|
| `summary.md` | 整条线路的汇总：吞吐、容量利用率、RTT、重传、切换损失 |
| `line-<名字>-segments.md` | 按 `timeline` 时间段分段：容量、各算法吞吐和利用率、切换和 RLF 次数 |
| `fig-line-<名字>.png` | 第 1 次运行随时间的变化，共三层：车速（含隧道和干扰时段）；SINR 和服务小区；各算法吞吐与容量，并标出切换和 RLF |
| `<名字>-preview.png`、`<名字>-cells.png` | 线路布局图（俯视、沿线展开、车速；每个基站的近景） |
| `raw/<名字>/r*-trajectory.data` | 每 0.1 s 一行：列车坐标、车速、干扰、车体损耗、是否在隧道、服务小区、SINR |

## 七、结果

见 [../docs/real.md](../docs/real.md) 中的“三维线路”一节。
