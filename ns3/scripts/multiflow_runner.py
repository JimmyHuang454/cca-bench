"""Run the multi-flow / background experiments of copa-auto (hwy-4g, runs 1-3)."""
import itertools, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from progress import fmt_lte_metrics, run_cmd_job, run_parallel
ROOT = os.environ["NS3_ROOT"]
BIN = ROOT + "/build/scratch/ns3-dev-tcp-lte-highspeed-handover-default"
OUT = sys.argv[1]
ARGS = open(sys.argv[2]).read().split() + ["--simTime=185"]
SCEN = {"n1": (1, 0), "n2": (2, 0), "n4": (4, 0), "n2bg": (2, 10), "n1bg": (1, 10)}
CCS = sys.argv[5].split(",") if len(sys.argv) > 5 else ["copa-auto", "copa", "copa-d0.1", "copa-d0.05", "copa-d0.03", "copa-d0.02", "copa-d0.01"]
want = sys.argv[3].split(",")
jobs = []
for sc in want:
    n, bg = SCEN[sc]
    for cc, r in itertools.product(CCS, (1, 2, 3)):
        d = os.path.join(OUT, sc); os.makedirs(d, exist_ok=True)
        pre = os.path.join(d, f"r{r}")
        if os.path.exists(f"{pre}-{cc}-metrics.json"):
            continue
        jobs.append([BIN, f"--run={r}", f"--prefix={pre}", f"--cc={cc}", f"--numFlows={n}",
                     f"--bgMbps={bg}"] + ARGS)
def _prefix_of(cmd):
    return cmd[2].split("=")[1] + "-" + cmd[3].split("=")[1]


def go(cmd):
    return run_cmd_job(cmd, ROOT, _prefix_of(cmd), timeout=1200, metrics_fn=fmt_lte_metrics)


def label_of(cmd):
    p = _prefix_of(cmd)
    return f"{os.path.basename(os.path.dirname(p))}/{os.path.basename(p)}"


run_parallel("mobile-multiflow", jobs, int(sys.argv[4]) if len(sys.argv) > 4 else 6, go, label_of)
