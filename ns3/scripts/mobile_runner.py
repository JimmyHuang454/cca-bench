"""Rerun the Copa family (copa-auto, fixed delta, no mode switching) on the mobile scenarios.
Single flow: hsr-4g, hwy-4g (3 runs), walk-line, run-line (1 run), same settings as
scratch/run-sweep.py. Multi-flow: hwy-4g with n2, n4, n2bg, n1bg (3 runs).
usage: python3 mobile_runner.py OUT_DIR HWY_ARGS JOBS"""
import importlib.util, os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from progress import fmt_lte_metrics, run_cmd_job, run_parallel
OUT, HWY_ARGS, NJ = sys.argv[1], sys.argv[2], int(sys.argv[3])
ROOT = os.environ["NS3_ROOT"]
BIN = ROOT + "/build/scratch/ns3-dev-tcp-lte-highspeed-handover-default"
spec = importlib.util.spec_from_file_location("rs", ROOT + "/scratch/run-sweep.py")
rs = importlib.util.module_from_spec(spec)
argv, sys.argv = sys.argv, ["x"]
spec.loader.exec_module(rs)
sys.argv = argv
NOSW = "--ns3::TcpCopa::ModeSwitching=false"
FIXED = ["copa", "copa-d0.3", "copa-d0.2", "copa-d0.15", "copa-d0.1", "copa-d0.07", "copa-d0.05", "copa-d0.03", "copa-d0.02", "copa-d0.01"]
MFIXED = ["copa", "copa-d0.1", "copa-d0.05", "copa-d0.03", "copa-d0.02", "copa-d0.01"]
VARIANTS = [("sw", "copa-auto", []), ("nosw", "copa-auto", [NOSW]), ("nosw", "copa", [NOSW])] \
    + [("sw", c, []) for c in FIXED]
SINGLE = [("hsr-4g", 3), ("hwy-4g", 3), ("walk-line", 1), ("run-line", 1)]
MULTI = {"n2": (2, 0), "n4": (4, 0), "n2bg": (2, 10), "n1bg": (1, 10)}
jobs = []
for var, cc, extra in VARIANTS:
    for study, runs in SINGLE:
        rs.OUT = os.path.join(OUT, var)
        for job in rs.make_jobs(study, [cc], runs, dict(rs.BASELINE)):
            if job[3] != cc:
                continue
            _, base = rs.job_stem(job)
            if os.path.exists(base + "-metrics.json"):
                continue
            scenario, st, value, c, run, args, rundir = job
            args = dict(args)
            if var == "nosw":
                args["ns3::TcpCopa::ModeSwitching"] = "false"
            jobs.append(("single", (scenario, st, value, c, run, args, rundir), None))
    for sc, (n, bg) in MULTI.items():
        if var == "sw" and cc != "copa-auto" and cc not in MFIXED:
            continue
        for r in (1, 2, 3):
            d = os.path.join(OUT, var, "multiflow", sc)
            os.makedirs(d, exist_ok=True)
            pre = os.path.join(d, f"r{r}")
            if os.path.exists(f"{pre}-{cc}-metrics.json"):
                continue
            cmd = [BIN, f"--run={r}", f"--prefix={pre}", f"--cc={cc}", f"--numFlows={n}", f"--bgMbps={bg}"] \
                + open(HWY_ARGS).read().split() + ["--simTime=185"] + extra
            jobs.append(("multi", cmd, f"{pre}-{cc}-stdout.log"))
binaries = {"C": BIN}
def go(j):
    kind, a, log = j
    if kind == "single":
        t0 = time.monotonic()
        st = rs.run_job(a, binaries)
        dt = time.monotonic() - t0
        _, base = rs.job_stem(a)
        if st in ("ok", "skip"):
            return "OK", fmt_lte_metrics(base), dt
        return st, "", dt
    pre = log[:-len("-stdout.log")]
    return run_cmd_job(a, ROOT, pre, timeout=1200, metrics_fn=fmt_lte_metrics)


def label_of(j):
    kind, a, log = j
    if kind == "single":
        return f"{a[1]}/{a[3]}/r{a[4]}"
    pre = log[:-len("-stdout.log")]
    return f"{os.path.basename(os.path.dirname(pre))}/{os.path.basename(pre)}"


run_parallel("mobile-copa", jobs, NJ, go, label_of)
