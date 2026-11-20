"""Replay Mahimahi LTE traces: traces x start offsets x 1/2/4 flows x algorithms.
The two directions of a trace were recorded together, so the uplink is cut at the end
of the downlink: both wrap at the same point.
usage: python3 mahimahi_runner.py TRACE_DIR OUT_DIR JOBS [CCS comma-separated]"""
import itertools, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from progress import fmt_cap_metrics, run_cmd_job, run_parallel
ROOT = os.environ["NS3_ROOT"]
BIN = ROOT + "/build/scratch/ns3-dev-tcp-capacity-trace-default"
TR, OUT, NJ = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), int(sys.argv[3])
TRACES = ["Verizon-LTE-driving", "TMobile-LTE-driving", "ATT-LTE-driving", "ATT-LTE-driving-2016"]
FLOWS, OFFSETS = [4, 2, 1], range(5)
FIXED = ["copa-d0.2", "copa-d0.1", "copa-d0.05", "copa-d0.03", "copa-d0.02", "copa-d0.01"]
CCS = ["copa-auto", "copa-auto+nosw", "copa", "copa+nosw", "cubic", "bbr"] + [c + "+nosw" for c in FIXED]
if len(sys.argv) > 4:
    CCS = sys.argv[4].split(",")
UP = os.path.join(OUT, "up")
os.makedirs(UP, exist_ok=True)
DUR = {}
for t in TRACES:
    DUR[t] = int(open(os.path.join(TR, t + ".down")).read().split()[-1])
    with open(os.path.join(TR, t + ".up")) as f, open(os.path.join(UP, t + ".up"), "w") as g:
        g.writelines(x for x in f if int(x) <= DUR[t])
jobs = []
for n, t, cc, r in itertools.product(FLOWS, TRACES, CCS, OFFSETS):
    d = os.path.join(OUT, f"{t}-n{n}")
    os.makedirs(d, exist_ok=True)
    pre = os.path.join(d, f"r{r}-{cc}")
    if os.path.exists(pre + "-metrics.json"):
        continue
    jobs.append([BIN, f"--ccs={','.join([cc] * n)}", "--profile=mahimahi",
                 f"--downTrace={os.path.join(TR, t + '.down')}", f"--upTrace={os.path.join(UP, t + '.up')}",
                 f"--traceOffset={r * DUR[t] / 1000 / 5}", "--stopTime=180", "--warmup=5",
                 "--queueBytes=2000000", "--sndBuf=4194304", f"--run={r + 1}", f"--prefix={pre}"])
def go(cmd):
    pre = cmd[-1].split("=", 1)[1]
    return run_cmd_job(cmd, ROOT, pre, timeout=1200, metrics_fn=fmt_cap_metrics)


def label_of(cmd):
    pre = cmd[-1].split("=", 1)[1]
    return f"{os.path.basename(os.path.dirname(pre))}/{os.path.basename(pre)}"


run_parallel("lte", jobs, NJ, go, label_of)
