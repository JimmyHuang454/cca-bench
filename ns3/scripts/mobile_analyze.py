"""Summary of the mobile scenarios: old data (Copa, CUBIC, BBRv1, fixed delta) and the rerun.
usage: python3 mobile_analyze.py DATA_DIR RERUN_DIR OUT_JSON"""
import glob, json, math, os, statistics, sys
DATA, NEW, OUT = sys.argv[1:4]
BASE = 50.0
SINGLE = [("hsr-4g", "350", "高铁 4G"), ("hwy-4g", "120", "高速 4G"), ("walk-line", "walk", "步行"), ("run-line", "run", "跑步")]
MULTI = [("n2", "2 条流"), ("n4", "4 条流"), ("n2bg", "2 条流 + 背景"), ("n1bg", "1 条流 + 背景")]
FIXED = ["copa", "copa-d0.3", "copa-d0.2", "copa-d0.15", "copa-d0.1", "copa-d0.07", "copa-d0.05", "copa-d0.03", "copa-d0.02", "copa-d0.01"]
def ci(x):
    if len(x) < 2: return 0.0
    t = {2: 12.71, 3: 4.30, 4: 3.18, 5: 2.78}.get(len(x), 2.0)
    return t * statistics.stdev(x) / math.sqrt(len(x))
def one(f, cap):
    m = json.load(open(f)); fl = m["flows"]; tot = sum(x["throughput_mbps"] for x in fl)
    r = sum(x["rtt_mean_ms"] * x["throughput_mbps"] for x in fl) / tot
    p = sum(x["rtt_p95_ms"] * x["throughput_mbps"] for x in fl) / tot
    return dict(util=tot / cap, rtt=r, p95=p, power=tot / cap / (r / BASE), jain=m.get("jain", 1.0)), m.get("params")
def agg(files, capdir):
    vals = []; params = set()
    for f in files:
        run = os.path.basename(f).split("-")[0]
        cap = json.load(open(f"{capdir}/{run}-metrics.json"))["total_throughput_mbps"]
        v, p = one(f, cap); vals.append(v); params.add(p)
    if not vals: return None
    out = {k: statistics.mean(v[k] for v in vals) for k in vals[0]}
    out["n"] = len(vals); out["power_ci"] = ci([v["power"] for v in vals]); out["util_ci"] = ci([v["util"] for v in vals]); out["rtt_ci"] = ci([v["rtt"] for v in vals])
    out["params"] = sorted(x for x in params if x)
    return out
res = {"single": {}, "multi": {}}
for st, val, name in SINGLE:
    d = f"{DATA}/{st}/raw/{val}"
    for cc in ["cubic", "bbr", "copa-auto"] + FIXED:
        res["single"][f"{st}|{cc}|old"] = agg(sorted(glob.glob(f"{d}/r*-{cc}-metrics.json")), d)
    for var, cc in [("sw", "copa-auto"), ("nosw", "copa-auto"), ("nosw", "copa")]:
        res["single"][f"{st}|{cc}|{var}"] = agg(sorted(glob.glob(f"{NEW}/{var}/{st}/raw/{val}/r*-{cc}-metrics.json")), d)
cap = f"{DATA}/hwy-4g/raw/120"
for sc, name in MULTI:
    d = f"{DATA}/multiflow/{sc}"
    for cc in ["cubic", "bbr", "copa-auto", "copa", "copa-d0.1", "copa-d0.05", "copa-d0.03", "copa-d0.02", "copa-d0.01"]:
        res["multi"][f"{sc}|{cc}|old"] = agg(sorted(glob.glob(f"{d}/r*-{cc}-metrics.json")), cap)
    for var, cc in [("sw", "copa-auto"), ("nosw", "copa-auto"), ("nosw", "copa")]:
        res["multi"][f"{sc}|{cc}|{var}"] = agg(sorted(glob.glob(f"{NEW}/{var}/multiflow/{sc}/r*-{cc}-metrics.json")), cap)
json.dump(res, open(OUT, "w"), indent=1, ensure_ascii=False)
def show(v): return "-" if not v else f"{v['power']:.3f}±{v['power_ci']:.3f} u{v['util']*100:.0f} r{v['rtt']:.0f} p{v['p95']:.0f} J{v['jain']:.2f}"
for st, val, name in SINGLE:
    print("==", name)
    for k in ["cubic|old", "bbr|old", "copa|old", "copa|nosw", "copa-auto|old", "copa-auto|sw", "copa-auto|nosw"]:
        print(f"  {k:16s}", show(res["single"][f"{st}|{k}"]))
    b = max(FIXED, key=lambda c: res["single"][f"{st}|{c}|old"]["power"] if res["single"][f"{st}|{c}|old"] else -1)
    print("  best fixed", b, show(res["single"][f"{st}|{b}|old"]))
for sc, name in MULTI:
    print("==", name)
    for k in ["cubic|old", "bbr|old", "copa|old", "copa|nosw", "copa-auto|old", "copa-auto|sw", "copa-auto|nosw"]:
        print(f"  {k:16s}", show(res["multi"][f"{sc}|{k}"]))
    fx = ["copa", "copa-d0.1", "copa-d0.05", "copa-d0.03", "copa-d0.02", "copa-d0.01"]
    b = max(fx, key=lambda c: res["multi"][f"{sc}|{c}|old"]["power"])
    print("  best fixed", b, show(res["multi"][f"{sc}|{b}|old"]))
pa = {k: v["params"] for part in res.values() for k, v in part.items() if v}
for st, _, _ in SINGLE:
    print(st, "params match:", len({tuple(p) for k, p in pa.items() if k.startswith(st + "|")}) == 1)
print("multi params match:", len({tuple(p) for k, p in res["multi"].items() if False}) == 0, len({p for k, v in res["multi"].items() if v for p in v["params"]}))
