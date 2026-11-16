"""Summarize the Mahimahi replay: per trace and flow count, mean and 95% CI over the start offsets.
usage: python3 mahimahi_analyze.py RUN_DIR OUT_DIR"""
import glob, json, math, os, statistics, sys

RUN, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
TRACES = ["Verizon-LTE-driving", "TMobile-LTE-driving", "ATT-LTE-driving", "ATT-LTE-driving-2016"]
FLOWS = [1, 2, 4]
MAIN = ["cubic", "bbr", "copa", "copa+nosw", "copa-auto", "copa-auto+nosw"]
FIXED = ["copa+nosw"] + [f"copa-d{d}+nosw" for d in ("0.2", "0.1", "0.05", "0.03", "0.02", "0.01")]
NAME = {"cubic": "CUBIC", "bbr": "BBRv1", "copa": "Copa", "copa+nosw": "Copa-NC", "copa-auto": "copa-auto",
        "copa-auto+nosw": "copa-auto-NC"}
BASE = 50.0
T95 = {2: 12.71, 3: 4.30, 4: 3.18, 5: 2.78}


def ci(xs):
    return T95.get(len(xs), 2.0) * statistics.stdev(xs) / math.sqrt(len(xs)) if len(xs) > 1 else 0.0


def one(path):
    m = json.load(open(path))
    fl = m["flows"]
    tot = sum(x["throughput_mbps"] for x in fl)
    if tot <= 0:
        return None
    r = sum(x["rtt_mean_ms"] * x["throughput_mbps"] for x in fl) / tot
    return dict(util=m["utilization"], rtt=r, p95=sum(x["rtt_p95_ms"] * x["throughput_mbps"] for x in fl) / tot,
                jain=m.get("jain", 1.0), power=m["utilization"] / (r / BASE), cap=m["avg_capacity_mbps"])


res, lines = {}, ["# 真实 trace 回放详细结果"]
for t in TRACES:
    for n in FLOWS:
        d = os.path.join(RUN, f"{t}-n{n}")
        for cc in MAIN + FIXED:
            runs = [x for x in (one(p) for p in sorted(glob.glob(os.path.join(d, f"r*-{cc}-metrics.json")))) if x]
            if runs:
                v = {k: statistics.mean(r[k] for r in runs) for k in runs[0]}
                v.update({k + "_ci": ci([r[k] for r in runs]) for k in ("power", "util", "rtt")})
                v.update(n=len(runs), powers=[r["power"] for r in runs])
                res[f"{t}|{n}|{cc}"] = v
        best = max(FIXED, key=lambda c: res.get(f"{t}|{n}|{c}", {}).get("power", -1))
        res[f"{t}|{n}|best"] = dict(cc=best, **res[f"{t}|{n}|{best}"])
        lines.append(f"\n## {t}，{n} 条流（平均容量 {res[f'{t}|{n}|copa']['cap']:.1f} Mbit/s）\n\n"
                     "| 算法 | 次数 | 功率 | 利用率 | 平均 RTT (ms) | p95 (ms) | Jain |\n|---|---|---|---|---|---|---|")
        for cc in MAIN + ["best"]:
            v = res[f"{t}|{n}|{cc}"]
            lab = NAME.get(cc) or ("最优 " + ("Copa-NC" if v["cc"] == "copa+nosw" else f"Copa-NC({v['cc'][6:-5]})"))
            lines.append(f"| {lab} | {v['n']} | {v['power']:.2f} ± {v['power_ci']:.2f} | {v['util']*100:.0f}% | "
                         f"{v['rtt']:.0f} | {v['p95']:.0f} | {v['jain']:.2f} |")
json.dump(res, open(os.path.join(OUT, "summary.json"), "w"), indent=1)
open(os.path.join(OUT, "detail.md"), "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
