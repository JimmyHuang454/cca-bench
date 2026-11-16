"""Run the basic scenario (no handover): groups A/B/C x 1/2/4 flows x algorithms x runs.
usage: python3 basic_runner.py OUT_DIR JOBS [CCS comma-separated] [SIGMA] [GROUPS]"""
import itertools, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from progress import fmt_cap_metrics, run_cmd_job, run_parallel
ROOT = os.environ["NS3_ROOT"]
BIN = ROOT + "/build/scratch/ns3-dev-tcp-capacity-trace-default"
OUT, NJ = sys.argv[1], int(sys.argv[2])
SIGMA = sys.argv[4] if len(sys.argv) > 4 else "0.1"
GROUPS, FLOWS, RUNS = list(sys.argv[5]) if len(sys.argv) > 5 else ["A", "B", "C"], [4, 2, 1], [1, 2, 3, 4, 5]
CCS = ["copa-auto", "copa", "cubic", "bbr", "copa-d0.2", "copa-d0.1", "copa-d0.05", "copa-d0.03", "copa-d0.02", "copa-d0.01"]
if len(sys.argv) > 3 and sys.argv[3]:
    CCS = sys.argv[3].split(",")
os.makedirs(os.path.join(OUT, "sched"), exist_ok=True)
for g, r in itertools.product(GROUPS, RUNS):
    f = os.path.join(OUT, "sched", f"{g}{r}.txt")
    if not os.path.exists(f):
        subprocess.run(["python3", os.path.join(HERE, "gen_schedule.py"), g, str(r), f, SIGMA], check=True)
jobs = []
for n, g, cc, r in itertools.product(FLOWS, GROUPS, CCS, RUNS):
    d = os.path.join(OUT, f"{g}-n{n}")
    os.makedirs(d, exist_ok=True)
    pre = os.path.join(d, f"r{r}-{cc}")
    if os.path.exists(pre + "-metrics.json"):
        continue
    jobs.append([BIN, f"--ccs={','.join([cc] * n)}", "--profile=file",
                 f"--traceFile={os.path.join(OUT, 'sched', f'{g}{r}.txt')}", "--stopTime=180", "--warmup=5",
                 "--queueBytes=2000000", "--sndBuf=4194304", f"--run={r}", f"--prefix={pre}"])
def go(cmd):
    pre = cmd[-1].split("=", 1)[1]
    return run_cmd_job(cmd, ROOT, pre, timeout=1200, metrics_fn=fmt_cap_metrics)


def label_of(cmd):
    pre = cmd[-1].split("=", 1)[1]
    return f"{os.path.basename(os.path.dirname(pre))}/{os.path.basename(pre)}"


run_parallel(f"basic sigma={SIGMA}", jobs, NJ, go, label_of)
