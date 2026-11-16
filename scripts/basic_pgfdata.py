"""pgfplots tables of the basic scenario: power per configuration and one time series.
usage: python3 basic_pgfdata.py BASIC_S01_JSON BASIC_S04_JSON RUNS_S04_DIR OUT_DIR"""
import csv, json, os, sys
S01, S04, RUNS, OUT = sys.argv[1:5]
CCS = {"copa": "copa", "copa+nosw": "nosw", "bbr": "bbr", "cubic": "cubic", "copa-auto+nosw": "auto"}
CFG = [(g, n) for g in "ABCDE" for n in (1, 2, 4)]
for name, path in (("s01", S01), ("s04", S04)):
    r = json.load(open(path))
    with open(os.path.join(OUT, f"basic-power-{name}.dat"), "w") as f:
        f.write("x " + " ".join(CCS.values()) + "\n")
        for i, (g, n) in enumerate(CFG):
            f.write(str(i) + " " + " ".join(f"{r[f'{g}|{n}|{c}']['whole']['power']:.4f}" for c in CCS) + "\n")
def binned(cc):
    rows = list(csv.DictReader(open(f"{RUNS}/C-n1/r1-{cc}-timeseries.csv")))
    out = {}
    for x in rows:
        t = int(float(x["time_s"]))
        d = out.setdefault(t, [0.0, 0.0, 0.0, 0.0, 0])
        lr = float(x["lrtt0_ms"])
        d[0] += float(x["capacity_mbps"]); d[1] += float(x["tput0_mbps"]); d[4] += 1
        if lr > 0:
            d[2] += lr; d[3] += 1
    return {t: (v[0] / v[4], v[1] / v[4], v[2] / v[3] if v[3] else float("nan")) for t, v in out.items()}
series = {c: binned(c) for c in ("copa", "bbr", "cubic", "copa-auto+nosw")}
delta = {}
for ln in open(f"{RUNS}/C-n1/r1-copa-auto+nosw-copaest.data"):
    if not ln.startswith("#"):
        p = ln.split()
        delta.setdefault(int(float(p[0])), []).append(float(p[6]))
with open(os.path.join(OUT, "basic-trace-c1.dat"), "w") as f:
    f.write("t cap tcopa tbbr tcubic tauto rcopa rbbr rcubic rauto dauto\n")
    for t in sorted(series["copa"]):
        if t >= 180 or t not in series["copa-auto+nosw"]:
            continue
        v = [series[c].get(t, (0, 0, float("nan"))) for c in ("copa", "bbr", "cubic", "copa-auto+nosw")]
        d = delta.get(t, [float("nan")])
        vals = [v[0][0]] + [x[1] for x in v] + [x[2] for x in v] + [sorted(d)[len(d) // 2]]
        f.write(f"{t} " + " ".join("nan" if a != a else f"{a:.3f}" for a in vals) + "\n")
