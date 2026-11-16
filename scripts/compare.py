"""Copa (delta=0.5), CUBIC, BBRv1 and copa-auto: single-flow (4 mobile scenarios)
and multi-flow (hwy-4g) summary. usage: python3 compare.py DATA_DIR OUT_DIR"""
import glob, json, os, statistics, sys

DATA, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
CCS = [("copa", "Copa（δ=0.5）"), ("cubic", "CUBIC"), ("bbr", "BBRv1"), ("copa-auto", "copa-auto（本文）")]
SINGLE = [("hsr-4g", "350", "高铁 4G"), ("hwy-4g", "120", "高速 4G"), ("walk-line", None, "步行"), ("run-line", None, "跑步")]
MULTI = [("n2", "2 条流"), ("n4", "4 条流"), ("n2bg", "2 条流 + 背景")]
BASE = 50.0


def one(m, cap):
    fl = m["flows"]
    tot = sum(x["throughput_mbps"] for x in fl)
    rtt = sum(x["rtt_mean_ms"] * x["throughput_mbps"] for x in fl) / tot
    p95 = sum(x["rtt_p95_ms"] * x["throughput_mbps"] for x in fl) / tot
    u = tot / cap
    return dict(util=u, thr=tot, rtt=rtt, p95=p95, power=u / (rtt / BASE), jain=m.get("jain", 1.0))


def agg(vals):
    return {k: statistics.mean(v[k] for v in vals) for k in vals[0]} | {"n": len(vals)}


res = {"single": {}, "multi": {}}
for study, val, name in SINGLE:
    d = glob.glob(os.path.join(DATA, study, "raw", val or "*"))[0]
    for cc, _ in CCS:
        vals = []
        for f in sorted(glob.glob(f"{d}/r*-{cc}-metrics.json")):
            r = os.path.basename(f).split("-")[0]
            cap = json.load(open(f"{d}/{r}-metrics.json"))["total_throughput_mbps"]
            vals.append(one(json.load(open(f)), cap))
        if vals:
            res["single"][f"{study}|{cc}"] = agg(vals)
cap = {r: json.load(open(f"{DATA}/hwy-4g/raw/120/r{r}-metrics.json"))["total_throughput_mbps"] for r in (1, 2, 3)}
for sc, name in MULTI:
    for cc, _ in CCS:
        vals = []
        for r in (1, 2, 3):
            f = f"{DATA}/multiflow/{sc}/r{r}-{cc}-metrics.json"
            if os.path.exists(f):
                vals.append(one(json.load(open(f)), cap[r]))
        if vals:
            res["multi"][f"{sc}|{cc}"] = agg(vals)
json.dump(res, open(os.path.join(OUT, "compare.json"), "w"), indent=1)
for part, groups in (("single", [(s, n) for s, _, n in SINGLE]), ("multi", MULTI)):
    for g, name in groups:
        for cc, lab in CCS:
            v = res[part].get(f"{g}|{cc}")
            if v:
                print(f"{part:6s} {name:10s} {cc:9s} n={v['n']} util={v['util']*100:5.1f}% thr={v['thr']:5.1f} rtt={v['rtt']:6.1f} p95={v['p95']:6.1f} power={v['power']:.3f} jain={v['jain']:.2f}")
