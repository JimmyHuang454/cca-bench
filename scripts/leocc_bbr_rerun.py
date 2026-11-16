"""Rerun the downlink BBRv1 replays that were too slow with a 64 MB send buffer, with a 20 MB one.
20 MB is above BBRv1's 2 x BDP on the longest path (about 15.4 MB). It caps the start-up overshoot,
which is what makes these runs crawl. Runs that already finished are repeated as a check.
usage: python3 leocc_bbr_rerun.py TRACE_DIR FIRST_RUN_DIR OUT_DIR JOBS TIMEOUT_S"""
import glob, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from progress import fmt_cap_metrics, run_cmd_job, run_parallel
ROOT = os.environ["NS3_ROOT"]
BIN = ROOT + "/build/scratch/ns3-dev-tcp-capacity-trace-default"
TR, FIRST, OUT = (os.path.abspath(x) for x in sys.argv[1:4])
NJ, LIMIT = int(sys.argv[4]), int(sys.argv[5])
CHECK = [("A", 1, "490"), ("C", 1, "411")]
jobs = []
for p, n, i in CHECK + [(p, n, i) for n in (1, 2, 4) for p in "ABCD"
                        for i in sorted({re.match(r"t(\d+)-", os.path.basename(f)).group(1)
                                         for f in glob.glob(f"{FIRST}/{p}-downlink-n1/t*-copa-metrics.json")})
                        if not os.path.exists(f"{FIRST}/{p}-downlink-n{n}/t{i}-bbr-metrics.json")]:
    o = f"{OUT}/{p}-downlink-n{n}"
    os.makedirs(o, exist_ok=True)
    t = f"{TR}/Anonymous_{p}_downlink/{i}"
    jobs.append([BIN, "--ccs=" + ",".join(["bbr"] * n), "--profile=mahimahi", f"--downTrace={t}/bw_{i}.txt",
                 f"--upTrace={t}/bw_{i}.txt", f"--delayTrace={t}/delay_{i}.txt", "--randomLoss=0.0001",
                 "--bottleneckDelay=0", "--accessDelay=0", "--stopTime=120", "--warmup=5", "--queueBytes=75000000",
                 f"--sndBuf={20 * 2 ** 20}", f"--prefix={o}/t{i}-bbr"])
def go(cmd):
    pre = cmd[-1].split("=", 1)[1]
    return run_cmd_job(cmd, ROOT, pre, timeout=LIMIT, metrics_fn=fmt_cap_metrics)


def label_of(cmd):
    pre = cmd[-1].split("=", 1)[1]
    return pre[len(OUT) + 1:]


run_parallel("leo-bbr20", jobs, NJ, go, label_of)
