"""Summarize the LeoCC replay per direction, path and flow count; power uses each trace's own base RTT.
usage: python3 leocc_analyze.py RUN_DIR TRACE_DIR OUT_DIR"""
import glob, json, math, os, re, statistics, sys

RUN, TR, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(OUT, exist_ok=True)
PATHS, DIRS, FLOWS, WARM, STOP = "ABCD", ["downlink", "uplink"], [1, 2, 4], 5.0, 120.0
MAIN = ["cubic", "bbr", "copa", "copa+nosw", "copa-auto", "copa-auto+nosw"]
FIXED = ["copa+nosw"] + [f"copa-d{d}+nosw" for d in ("0.2", "0.1", "0.05", "0.02", "0.01", "0.005", "0.002")]
NAME = {"cubic": "CUBIC", "bbr": "BBRv1", "copa": "Copa", "copa+nosw": "Copa-NC", "copa-auto": "copa-auto",
        "copa-auto+nosw": "copa-auto-NC"}
T95 = {2: 12.71, 3: 4.30, 4: 3.18, 5: 2.78, 6: 2.57, 7: 2.45, 8: 2.36, 9: 2.31, 10: 2.26, 11: 2.23, 12: 2.20}
base_cache = {}


def ci(xs):
    return T95.get(len(xs), 2.0) * statistics.stdev(xs) / math.sqrt(len(xs)) if len(xs) > 1 else 0.0


def base_rtt(p, d, t):
    k = (p, d, t)
    if k not in base_cache:
        ds = [int(x) for x in open(os.path.join(TR, f"Anonymous_{p}_{d}", t, f"delay_{t}.txt")).read().split()]
        w = ds[int(WARM * 100):int(STOP * 100)]
        base_cache[k] = 2 * statistics.mean(w)
    return base_cache[k]


def one(path, r0):
    m = json.load(open(path))
    fl = m["flows"]
    tot = sum(x["throughput_mbps"] for x in fl)
    if tot <= 0:
        return None
    r = sum(x["rtt_mean_ms"] * x["throughput_mbps"] for x in fl) / tot
    return dict(util=m["utilization"], rtt=r, p95=sum(x["rtt_p95_ms"] * x["throughput_mbps"] for x in fl) / tot,
                jain=m.get("jain", 1.0), power=m["utilization"] / (r / r0), cap=m["avg_capacity_mbps"], r0=r0)


runs = {}
for d in DIRS:
    for n in FLOWS:
        for p in PATHS:
            for path in glob.glob(os.path.join(RUN, f"{p}-{d}-n{n}", "t*-metrics.json")):
                m = re.match(r"t(\d+)-(.*)-metrics.json", os.path.basename(path))
                v = one(path, base_rtt(p, d, m.group(1)))
                if v:
                    runs.setdefault((d, n, m.group(2)), {})[(p, m.group(1))] = v
res, lines = {}, ["# LeoCC trace 回放详细结果"]


def agg(d, n, cc):
    rr = runs.get((d, n, cc), {})
    if not rr:
        return None
    xs = list(rr.values())
    v = {k: statistics.mean(x[k] for x in xs) for k in xs[0]}
    v.update(power_ci=ci([x["power"] for x in xs]), n=len(xs))
    v["path_power"] = {p: statistics.mean(x["power"] for k, x in rr.items() if k[0] == p)
                       for p in PATHS if any(k[0] == p for k in rr)}
    v["path_n"] = {p: sum(k[0] == p for k in rr) for p in PATHS}
    return v


for d in DIRS:
    for n in FLOWS:
        for cc in MAIN + FIXED:
            v = agg(d, n, cc)
            if v:
                res[f"{d}|{n}|{cc}"] = v
        comp = [c for c in FIXED if res.get(f"{d}|{n}|{c}", {}).get("n") == 12]
        best = max(comp, key=lambda c: res[f"{d}|{n}|{c}"]["power"])
        res[f"{d}|{n}|best"] = dict(cc=best, **res[f"{d}|{n}|{best}"])
        lines.append(f"\n## {'下行' if d == 'downlink' else '上行'}，{n} 条流（平均容量 {res[f'{d}|{n}|copa']['cap']:.0f} Mbit/s）\n\n"
                     "| 算法 | 次数 | 功率 | 利用率 | 平均 RTT (ms) | p95 (ms) | Jain | A | B | C | D |\n|---|---|---|---|---|---|---|---|---|---|---|")
        for cc in MAIN + ["best"]:
            v = res.get(f"{d}|{n}|{cc}")
            if not v:
                continue
            lab = NAME.get(cc) or ("最优 " + ("Copa-NC" if v["cc"] == "copa+nosw" else f"Copa-NC({v['cc'][6:-5]})"))
            pp = " | ".join(f"{v['path_power'][p]:.2f}" if p in v["path_power"] else "–" for p in PATHS)
            lines.append(f"| {lab} | {v['n']} | {v['power']:.2f} ± {v['power_ci']:.2f} | {v['util']*100:.0f}% | "
                         f"{v['rtt']:.0f} | {v['p95']:.0f} | {v['jain']:.2f} | {pp} |")
json.dump(res, open(os.path.join(OUT, "summary.json"), "w"), indent=1)
open(os.path.join(OUT, "detail.md"), "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
