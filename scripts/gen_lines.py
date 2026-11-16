"""Write the city and countryside lines of scratch/lines. usage: python3 gen_lines.py OUT_DIR
The track length is checked against the timeline."""
import json, math, sys
OUT = sys.argv[1]
CITY = dict(car_loss_db=9, edge_rsrp_dbm=-100, cell=dict(side="left", offset_m=60, height_m=25, tx_dbm=46, range_mode="soft", antenna=dict(type="omni")), tunnel_cell_spacing_m=0, tunnel_loss_db=30, tunnel_ramp_m=10)
RURAL = dict(car_loss_db=9, edge_rsrp_dbm=-105, cell=dict(side="left", offset_m=150, height_m=40, tx_dbm=46, range_mode="soft", antenna=dict(type="omni")), tunnel_cell_spacing_m=0, tunnel_loss_db=40, tunnel_ramp_m=30)
def dist(tl):
    d = 0.0
    for k, v in tl.items():
        a, b = map(float, k.split("-")); s = v["speed_kmh"]
        m = sum(s) / 2 if isinstance(s, list) else s
        d += m / 3.6 * (b - a)
    return d
def line(name, comment, defaults, points, timeline, cells=None, grid=None, inter=None, **over):
    d = json.loads(json.dumps(defaults)); d.update(over)
    L = sum(math.dist(points[i], points[i + 1]) for i in range(len(points) - 1))
    assert L >= dist(timeline), (name, L, dist(timeline))
    o = dict(_comment=comment, tech="4g", defaults=d, track=dict(points=points), timeline=timeline)
    if grid: o["cell_grid"] = grid
    if cells: o["cells"] = cells
    if inter: o["interferers"] = inter
    with open(f"{OUT}/{name}.json", "w") as f:
        json.dump(o, f, ensure_ascii=False, indent=2)
    print(name, "length", round(L), "distance", round(dist(timeline)), "duration", max(float(k.split('-')[1]) for k in timeline))
line("city-drive", "城市驾车：主干道走走停停，红灯、高楼遮挡、晚高峰路口人多。宏站间距约 450 m。", CITY,
     [[0,0,0],[900,0,0],[900,700,0],[1800,700,0]],
     {"0-10":dict(speed_kmh=[0,40]),"10-40":dict(speed_kmh=40),"40-50":dict(speed_kmh=[40,0]),
      "50-80":dict(speed_kmh=0,crowd_ues=20,crowd_mbps=2,_comment="红灯 30 s"),"80-90":dict(speed_kmh=[0,50]),
      "90-120":dict(speed_kmh=50,car_loss_db=21,_comment="高楼之间，多 12 dB"),"120-130":dict(speed_kmh=[50,15]),
      "130-150":dict(speed_kmh=15,crowd_ues=15,crowd_mbps=2,_comment="拥堵"),"150-180":dict(speed_kmh=[15,45])},
     grid=[dict(from_m=100,to_m=1700,spacing_m=450,side="alternate",id_prefix="M",range_m=300)],
     inter=[dict(id="J1",track_m=1100,side="right",offset_m=150,height_m=25,range_m=250,power_dbm=-90)])
line("city-bus", "城市公交：车内人多，频繁进出站。站距约 400 m，车速低。", CITY,
     [[0,0,0],[1400,0,0]],
     {"0-8":dict(speed_kmh=[0,30]),"8-40":dict(speed_kmh=30),"40-48":dict(speed_kmh=[30,0]),
      "48-68":dict(speed_kmh=0,crowd_ues=15,crowd_mbps=2,_comment="到站上下客"),"68-76":dict(speed_kmh=[0,30]),
      "76-108":dict(speed_kmh=30,crowd_ues=10,crowd_mbps=2),"108-116":dict(speed_kmh=[30,0]),
      "116-136":dict(speed_kmh=0,crowd_ues=15,crowd_mbps=2),"136-144":dict(speed_kmh=[0,30]),"144-180":dict(speed_kmh=30,crowd_ues=10,crowd_mbps=2)},
     grid=[dict(from_m=100,to_m=1300,spacing_m=400,side="alternate",id_prefix="M",range_m=350)], car_loss_db=10)
line("city-subway", "城市地铁：全程隧道，隧道内分段覆盖，到站车厢人很多。", CITY,
     [[0,0,-15],[2600,0,-15]],
     {"0-15":dict(speed_kmh=[0,70],tunnel=True),"15-55":dict(speed_kmh=70,tunnel=True),"55-70":dict(speed_kmh=[70,0],tunnel=True),
      "70-95":dict(speed_kmh=0,tunnel=True,crowd_ues=30,crowd_mbps=2,_comment="到站"),"95-110":dict(speed_kmh=[0,70],tunnel=True),
      "110-150":dict(speed_kmh=70,tunnel=True),"150-165":dict(speed_kmh=[70,0],tunnel=True),"165-180":dict(speed_kmh=0,tunnel=True,crowd_ues=30,crowd_mbps=2)},
     cells=[dict(id="M1",track_m=100,side="left",offset_m=80,height_m=40)], car_loss_db=15, tunnel_cell_spacing_m=300)
line("city-elevated", "城市高架：80 km/h，站点密，同频干扰强，切换频繁。", CITY,
     [[0,0,12],[2000,0,12],[3400,800,12],[4400,800,12]],
     {"0-10":dict(speed_kmh=[0,80]),"10-90":dict(speed_kmh=80),"90-100":dict(speed_kmh=80,interference_dbm=-90),
      "100-140":dict(speed_kmh=80,interference_dbm=-95),"140-180":dict(speed_kmh=80)},
     grid=[dict(from_m=150,to_m=4000,spacing_m=500,side="alternate",id_prefix="M",range_m=330)])
line("city-mall", "商场室内：慢走、停留，建筑穿透损耗大，人群密集，最后乘电梯。", CITY,
     [[0,0,0],[260,0,0],[260,0,20],[320,0,20]],
     {"0-30":dict(speed_kmh=3,car_loss_db=15,crowd_ues=10,crowd_mbps=1),"30-70":dict(speed_kmh=0,car_loss_db=15,crowd_ues=15,crowd_mbps=1,_comment="店内停留"),
      "70-120":dict(speed_kmh=4,car_loss_db=15,crowd_ues=20,crowd_mbps=1),"120-140":dict(speed_kmh=0,car_loss_db=20,crowd_ues=5,crowd_mbps=1),
      "140-150":dict(speed_kmh=7.2,car_loss_db=30,_comment="电梯"),"150-180":dict(speed_kmh=3,car_loss_db=15,crowd_ues=10,crowd_mbps=1)},
     cells=[dict(id="S1",track_m=60,side="left",offset_m=20,height_m=6,range_m=120,_comment="室内分布小区"),dict(id="S2",track_m=240,side="right",offset_m=20,height_m=6,range_m=120)], car_loss_db=15, edge_rsrp_dbm=-95)
line("city-stadium", "体育场：几乎不动，容量变化完全来自周围用户。散场前后负载变化大。", CITY,
     [[0,0,0],[120,0,0]],
     {"0-40":dict(speed_kmh=0,crowd_ues=5,crowd_mbps=1),"40-80":dict(speed_kmh=0,crowd_ues=20,crowd_mbps=1),
      "80-120":dict(speed_kmh=0,crowd_ues=20,crowd_mbps=2,_comment="中场休息，大家都在看视频"),"120-150":dict(speed_kmh=2,crowd_ues=20,crowd_mbps=1),"150-180":dict(speed_kmh=2,crowd_ues=5,crowd_mbps=1)},
     cells=[dict(id="M1",track_m=20,side="left",offset_m=150,height_m=30),dict(id="M2",track_m=100,side="right",offset_m=180,height_m=30)], car_loss_db=6)
line("rural-road", "乡村公路：60–80 km/h，站距约 3 km，小区边缘信号弱。", RURAL,
     [[0,0,0],[2500,0,0],[3500,500,0]],
     {"0-10":dict(speed_kmh=[0,70]),"10-80":dict(speed_kmh=70),"80-90":dict(speed_kmh=[70,30],_comment="穿过村庄"),
      "90-110":dict(speed_kmh=30,crowd_ues=5,crowd_mbps=2),"110-120":dict(speed_kmh=[30,80]),"120-180":dict(speed_kmh=80)},
     grid=[dict(from_m=100,to_m=3400,spacing_m=3000,side="alternate",id_prefix="R",range_m=1800)])
line("rural-mountain", "山区公路：弯道爬坡，山体遮挡造成覆盖空洞，有一段无覆盖隧道。", RURAL,
     [[0,0,0],[800,300,40],[1400,100,90],[2000,600,140],[2800,500,200]],
     {"0-10":dict(speed_kmh=[0,50]),"10-50":dict(speed_kmh=50),"50-80":dict(speed_kmh=45,car_loss_db=24,_comment="山体遮挡，多 15 dB"),
      "80-95":dict(speed_kmh=45,tunnel=True,_comment="短隧道，没有覆盖"),"95-130":dict(speed_kmh=40),"130-155":dict(speed_kmh=40,car_loss_db=22),"155-180":dict(speed_kmh=50)},
     cells=[dict(id="R1",track_m=100,side="right",offset_m=200,height_m=50),dict(id="R2",track_m=1400,side="left",offset_m=250,height_m=60),dict(id="R3",track_m=2500,side="right",offset_m=200,height_m=60)], edge_rsrp_dbm=-105)
line("rural-village", "村庄步行：站点远，信号弱，偶尔进屋。", RURAL,
     [[0,0,0],[250,0,0]],
     {"0-60":dict(speed_kmh=4),"60-90":dict(speed_kmh=0,car_loss_db=15,_comment="进屋"),"90-150":dict(speed_kmh=4,crowd_ues=3,crowd_mbps=2),"150-180":dict(speed_kmh=0,crowd_ues=3,crowd_mbps=2)},
     cells=[dict(id="R1",track_m=20,side="left",offset_m=800,height_m=40,range_m=1200)], car_loss_db=3, edge_rsrp_dbm=-100)
line("rural-edge", "农田边缘：基本不动，处于小区边缘，SINR 低。", RURAL,
     [[0,0,0],[60,0,0]],
     {"0-90":dict(speed_kmh=0),"90-100":dict(speed_kmh=3),"100-180":dict(speed_kmh=0,interference_dbm=-105)},
     cells=[dict(id="R1",track_m=10,side="left",offset_m=1500,height_m=40,range_m=1600)], car_loss_db=3, edge_rsrp_dbm=-104)
