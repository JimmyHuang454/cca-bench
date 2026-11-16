"""Summarize the multi-flow runs on the lines: total utilization, weighted mean RTT, power, Jain.
usage: python3 lines_multi_analyze.py RUN_DIR OUT_JSON
RUN_DIR holds sw-n2, nc-n2, sw-n4, nc-n4."""
import glob, json, math, os, statistics, sys
RUN, OUT = sys.argv[1:3]
LINES = ["city-drive", "city-subway", "rural-road", "hsr-line"]
T95 = {2: 12.71, 3: 4.30}
def ci(x):
    return T95.get(len(x), 2.0) * statistics.stdev(x) / math.sqrt(len(x)) if len(x) > 1 else 0.0
res = {}
for n in (2, 4):
    for l in LINES:
        for lab, v, cc in (("Copa", "sw", "copa"), ("Copa-NC", "nc", "copa"), ("copa-auto", "sw", "copa-auto"),
                           ("copa-auto-NC", "nc", "copa-auto"), ("CUBIC", "sw", "cubic"), ("BBRv1", "sw", "bbr")):
            d = glob.glob(os.path.join(RUN, f"{v}-n{n}", l, "raw", "*"))[0]
            rows = []
            for f in sorted(glob.glob(f"{d}/r*-{cc}-metrics.json")):
                r = os.path.basename(f).split("-")[0]
                cap = json.load(open(f"{d}/{r}-metrics.json"))["total_throughput_mbps"]
                m = json.load(open(f)); fl = m["flows"]; assert len(fl) == n
                tot = sum(x["throughput_mbps"] for x in fl)
                rtt = sum(x["rtt_mean_ms"] * x["throughput_mbps"] for x in fl) / tot
                rows.append(dict(util=tot / cap, rtt=rtt, power=tot / cap / (rtt / 50.0), jain=m.get("jain", 1.0)))
            out = {k: statistics.mean(x[k] for x in rows) for k in rows[0]}
            out.update(power_ci=ci([x["power"] for x in rows]), n=len(rows))
            res[f"{l}|{n}|{lab}"] = out
json.dump(res, open(OUT, "w"), indent=1)
