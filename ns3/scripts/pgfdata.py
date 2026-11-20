"""Export the data of the simulation figures as pgfplots tables (figures/data/*.dat).
usage: python3 pgfdata.py DATA_DIR OUT_DIR COMPARE_JSON (written by compare.py)"""
import glob, json, os, runpy, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze_lib as al  # noqa: E402

DATA, OUT, CMP = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(OUT, exist_ok=True)
tmp = tempfile.mkdtemp()
sys.argv = ["validate.py", DATA, tmp]
V = runpy.run_path(os.path.join(HERE, "validate.py"))


def w(name, header, rows):
    with open(os.path.join(OUT, name), "w") as f:
        f.write(" ".join(header) + "\n")
        for r in rows:
            f.write(" ".join(f"{x:.6g}" if isinstance(x, float) else str(x) for x in r) + "\n")


# power vs delta, per scenario
for study, _ in al.STUDIES:
    data = al.load(DATA, study)
    rows = []
    for d in al.DELTAS:
        s = al.summarize(data[al.cc_of(d)])
        rows.append([d, s["power"], 0.0 if s["power_ci"] != s["power_ci"] else s["power_ci"],
                     s["util"] * 100, s["rtt_mean"], 0.0 if s["rtt_mean_ci"] != s["rtt_mean_ci"] else s["rtt_mean_ci"]])
    w(f"power-{study}.dat", ["delta", "power", "ci", "util", "rtt", "rttci"], rows)

# trade-off plane, highway: other algorithms
data = al.load(DATA, "hwy-4g")
rows = []
for cc in ("cubic", "bbr", "bbrv2l", "bbrv3l"):
    s = al.summarize(data[cc])
    rows.append([cc, s["rtt_mean"], s["rtt_mean_ci"], s["util"] * 100, s["util_ci"] * 100])
w("tradeoff-hwy-others.dat", ["cc", "rtt", "rttci", "util", "utilci"], rows)

# model fit, highway (model B)
probes = al.capacity(DATA, "hwy-4g")
mu = sum(probes) / len(probes) * 1e6 / V["PKT_BITS"]
meas = {}
for d in al.DELTAS:
    s = al.summarize(data[al.cc_of(d)])
    meas[d] = (s["util"], s["rtt_mean"] / 1000)
pb = V["fit"](mu, [(d, meas[d][0], meas[d][1]) for d in V["TRAIN"]])
w("modelfit-hwy-train.dat", ["delta", "util", "rtt"], [[d, meas[d][0] * 100, meas[d][1] * 1e3] for d in V["TRAIN"]])
w("modelfit-hwy-test.dat", ["delta", "util", "rtt"], [[d, meas[d][0] * 100, meas[d][1] * 1e3] for d in V["TEST"]])
fine = [10 ** (-2.1 + 0.02 * k) for k in range(90)]
w("modelfit-hwy-model.dat", ["delta", "util", "rtt"],
  [[d, p[0] * 100, p[1] * 1e3] for d, p in zip(fine, V["predict"](pb, mu, fine))])
print("hwy model B delta*:", V["power_opt"](pb, mu)[0])

# online estimates, first highway run (every 4th half-period)
path = sorted(glob.glob(os.path.join(DATA, "hwy-4g", "raw", "*", "r1-copa-auto-copaest.data")))[0]
r = [list(map(float, ln.split())) for ln in open(path) if not ln.startswith("#")]
w("adaptive-trace-hwy.dat", ["t", "j", "dqs", "rho", "delta"], [[x[0], x[1], x[5], x[3], x[6]] for x in r[::4]])

# comparison with other algorithms
cmp = json.load(open(CMP))
CCS = ["copa", "cubic", "bbr", "copa-auto"]
for part, groups in (("single", ["hsr-4g", "hwy-4g", "walk-line", "run-line"]), ("multi", ["n2", "n4", "n2bg"])):
    rows = []
    for i, g in enumerate(groups):
        row = [i]
        for cc in CCS:
            v = cmp[part][f"{g}|{cc}"]
            row += [v["util"] * 100, v["rtt"], v["power"]]
        rows.append(row)
    w(f"compare-{part}.dat", ["x"] + [f"{k}{c.replace('-', '')}" for c in CCS for k in ("u", "r", "p")], rows)
