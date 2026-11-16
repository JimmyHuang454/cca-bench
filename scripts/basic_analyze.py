"""Summarize the basic scenario: whole-run and per-segment utilization, RTT, power, Jain.
usage: python3 basic_analyze.py RUN_DIR OUT_DIR"""
import csv, glob, json, math, os, statistics, sys

RUN, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
GROUPS, FLOWS = ["A", "B", "C", "D", "E"], [1, 2, 4]
R0 = {"D": [50, 100, 20, 150, 30, 80], "E": [50, 100, 20, 150, 30, 80]}  # base RTT per segment [ms]
MAIN = ["copa", "cubic", "bbr", "copa-auto", "copa+gcv", "copa+nosw", "copa-auto+nosw", "copa+gcv+nosw"]
FIXED = ["copa", "copa-d0.2", "copa-d0.1", "copa-d0.05", "copa-d0.03", "copa-d0.02", "copa-d0.01"]
FIXED_NOSW = ["copa+nosw"] + [c + "+nosw" for c in FIXED[1:]]
SEG, NSEG, WARM, BASE = 30.0, 6, 5.0, 50.0


def ci(xs):
    if len(xs) < 2:
        return 0.0
    t = {2: 12.71, 3: 4.30, 4: 3.18, 5: 2.78}.get(len(xs), 2.0)
    return t * statistics.stdev(xs) / math.sqrt(len(xs))


def per_run(pre, g):
    base = R0.get(g, [BASE] * NSEG)
    m = json.load(open(pre + "-metrics.json"))
    fl = m["flows"]
    tot = sum(x["throughput_mbps"] for x in fl)
    whole = dict(util=m["utilization"], rtt=sum(x["rtt_mean_ms"] * x["throughput_mbps"] for x in fl) / tot,
                 p95=sum(x["rtt_p95_ms"] * x["throughput_mbps"] for x in fl) / tot, jain=m.get("jain", 1.0))
    whole["power"] = whole["util"] / (whole["rtt"] / BASE)
    seg = [dict(cap=0.0, thr=0.0, rw=0.0, w=0.0, n=0) for _ in range(NSEG)]
    with open(pre + "-timeseries.csv") as f:
        rd = csv.DictReader(f)
        n = sum(1 for k in rd.fieldnames if k.startswith("tput"))
        for row in rd:
            t = float(row["time_s"])
            k = int(t // SEG)
            if t < WARM or k >= NSEG:
                continue
            s = seg[k]
            s["cap"] += float(row["capacity_mbps"]); s["n"] += 1
            for i in range(n):
                th, lr = float(row[f"tput{i}_mbps"]), float(row[f"lrtt{i}_ms"])
                s["thr"] += th
                if lr > 0 and th > 0:
                    s["rw"] += lr * th; s["w"] += th
    out = []
    for i, s in enumerate(seg):
        u = s["thr"] / s["cap"] if s["cap"] else 0.0
        r = s["rw"] / s["w"] if s["w"] else float("nan")
        out.append(dict(util=u, rtt=r, power=u / (r / base[i]) if r == r else 0.0))
    if g in R0:
        # The base RTT varies: a single normalization would favour the low-RTT, high-capacity
        # segments (power above 1), so the whole-run power is the mean over the segments.
        whole["power"] = sum(x["power"] for x in out) / NSEG
    return whole, out


res = {}
for g in GROUPS:
    for n in FLOWS:
        d = os.path.join(RUN, f"{g}-n{n}")
        ccs = sorted({os.path.basename(p).split("-", 1)[1][:-len("-metrics.json")]
                      for p in glob.glob(os.path.join(d, "r*-metrics.json"))})
        for cc in ccs:
            runs = [per_run(p[:-len("-metrics.json")], g) for p in sorted(glob.glob(os.path.join(d, f"r*-{cc}-metrics.json")))]
            whole = {k: statistics.mean(r[0][k] for r in runs) for k in runs[0][0]}
            whole.update({k + "_ci": ci([r[0][k] for r in runs]) for k in ("util", "rtt", "power")})
            whole["n"] = len(runs)
            seg = [{k: statistics.mean(r[1][i][k] for r in runs) for k in ("util", "rtt", "power")} for i in range(NSEG)]
            res[f"{g}|{n}|{cc}"] = dict(whole=whole, seg=seg)
json.dump(res, open(os.path.join(OUT, "basic.json"), "w"), indent=1)

lines = []
for g in GROUPS:
    for n in FLOWS:
        lines.append(f"\n## 组 {g}，{n} 条流\n\n| 算法 | n | 利用率 | 平均 RTT | p95 | 功率 | Jain |\n|---|---|---|---|---|---|---|")
        for cc in MAIN:
            v = res.get(f"{g}|{n}|{cc}")
            if v:
                w = v["whole"]
                lines.append(f"| {cc} | {w['n']} | {w['util']*100:.1f}% ± {w['util_ci']*100:.1f} | {w['rtt']:.1f} ± {w['rtt_ci']:.1f} | {w['p95']:.1f} | {w['power']:.3f} ± {w['power_ci']:.3f} | {w['jain']:.2f} |")
        # per segment: copa-auto vs best fixed delta
        if all(f"{g}|{n}|{c}" in res for c in FIXED + ["copa-auto"]):
            lines.append("\n| 时段 | 最优固定 δ | 其功率 | copa-auto 功率 | 比例 | copa-auto 利用率 / RTT |\n|---|---|---|---|---|---|")
            for i in range(NSEG):
                best = max(FIXED, key=lambda c: res[f"{g}|{n}|{c}"]["seg"][i]["power"])
                bp = res[f"{g}|{n}|{best}"]["seg"][i]["power"]
                a = res[f"{g}|{n}|copa-auto"]["seg"][i]
                lines.append(f"| {i*30}–{i*30+30} s | {best} | {bp:.3f} | {a['power']:.3f} | {a['power']/bp*100:.0f}% | {a['util']*100:.0f}% / {a['rtt']:.1f} ms |")
for g in GROUPS:
    for n in FLOWS:
        if all(f"{g}|{n}|{c}" in res for c in FIXED_NOSW + ["copa-auto+nosw"]):
            lines.append(f"\n### 组 {g}，{n} 条流：copa-auto+nosw 与 +nosw 下每段最优固定 δ\n\n| 时段 | 最优固定 δ | 其功率 | copa-auto+nosw 功率 | 比例 |\n|---|---|---|---|---|")
            for i in range(NSEG):
                best = max(FIXED_NOSW, key=lambda c: res[f"{g}|{n}|{c}"]["seg"][i]["power"])
                bp = res[f"{g}|{n}|{best}"]["seg"][i]["power"]
                a = res[f"{g}|{n}|copa-auto+nosw"]["seg"][i]
                lines.append(f"| {i*30}–{i*30+30} s | {best} | {bp:.3f} | {a['power']:.3f} | {a['power']/bp*100:.0f}% |")
open(os.path.join(OUT, "basic.md"), "w").write("# 基础场景结果\n" + "\n".join(lines) + "\n")
print("\n".join(lines))
