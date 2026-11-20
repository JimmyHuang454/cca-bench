"""Summarize the city and countryside lines: utilization, mean RTT, power per algorithm.
Power = utilization / (mean RTT / 50 ms), as in the other mobile scenarios.
usage: python3 lines_analyze.py SWEEP_DIR NOSW_DIR OUT_JSON"""
import glob, json, math, os, statistics, sys
SW, NC, OUT = sys.argv[1:4]
LINES = ["walk-line", "run-line", "city-drive", "city-bus", "city-subway", "city-elevated", "city-mall", "city-stadium",
         "rural-road", "rural-mountain", "rural-village", "rural-edge", "hwy-line", "hsr-line"]
T95 = {2: 12.71, 3: 4.30, 4: 3.18, 5: 2.78}
def ci(x):
    return T95.get(len(x), 2.0) * statistics.stdev(x) / math.sqrt(len(x)) if len(x) > 1 else 0.0
def collect(root, line, cc):
    d = glob.glob(os.path.join(root, line, "raw", "*"))[0]
    rows = []
    for f in sorted(glob.glob(f"{d}/r*-{cc}-metrics.json")):
        r = os.path.basename(f).split("-")[0]
        cap = json.load(open(f"{d}/{r}-metrics.json"))["total_throughput_mbps"]
        m = json.load(open(f)); x = m["flows"][0]
        u = x["throughput_mbps"] / cap
        rows.append(dict(cap=cap, util=u, rtt=x["rtt_mean_ms"], p95=x["rtt_p95_ms"], power=u / (x["rtt_mean_ms"] / 50.0),
                         handovers=m.get("handovers", 0), rlf=m.get("rlf", 0)))
    v = {k: statistics.mean(r[k] for r in rows) for k in rows[0]}
    v.update(power_ci=ci([r["power"] for r in rows]), n=len(rows), powers=[r["power"] for r in rows])
    return v
res = {}
for l in LINES:
    for lab, root, cc in (("Copa", SW, "copa"), ("Copa-NC", NC, "copa"), ("copa-auto", SW, "copa-auto"), ("copa-auto-NC", NC, "copa-auto"),
                          ("CUBIC", SW, "cubic"), ("BBRv1", SW, "bbr"), ("Copa(0.05)", SW, "copa-d0.05"), ("Copa(0.02)", SW, "copa-d0.02")):
        res[f"{l}|{lab}"] = collect(root, l, cc)
json.dump(res, open(OUT, "w"), indent=1)
print("\n| 线路 | Copa-auto (util / rtt / power) | Copa (power) | CUBIC (power) | BBRv1 (power) | Copa-auto-NC (power) |")
print("|---|---|---|---|---|---|")
for l in LINES:
    a = res[f"{l}|copa-auto"]
    an = res[f"{l}|copa-auto-NC"]
    co = res[f"{l}|Copa"]
    cu = res[f"{l}|CUBIC"]
    bb = res[f"{l}|BBRv1"]
    print(f"| {l} | {a['util']*100:.1f}% / {a['rtt']:.1f}ms / {a['power']:.3f} | {co['power']:.3f} | {cu['power']:.3f} | {bb['power']:.3f} | {an['power']:.3f} |")
