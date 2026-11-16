"""Replay LeoCC Starlink traces: 4 paths x 2 directions x sampled traces x 1/2/4 flows x algorithms.
Buffer and loss follow the LeoReplayer README, except that the uplink loss is the same for all
algorithms. "@x" sets the lower bound of the adaptive delta to x.
usage: python3 leocc_runner.py ZIP TRACE_DIR OUT_DIR JOBS [CCS comma-separated]"""
import itertools, os, random, sys, zipfile
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from progress import fmt_cap_metrics, run_cmd_job, run_parallel
ROOT = os.environ["NS3_ROOT"]
BIN = ROOT + "/build/scratch/ns3-dev-tcp-capacity-trace-default"
ZIP, TR, OUT, NJ = sys.argv[1], os.path.abspath(sys.argv[2]), os.path.abspath(sys.argv[3]), int(sys.argv[4])
PATHS, DIRS, FLOWS, NTRACE = "ABCD", ["downlink", "uplink"], [1, 2, 4], 3
FIXED = ["0.2", "0.1", "0.05", "0.02", "0.01", "0.005", "0.002"]
CCS = ["copa-auto+nosw", "copa-auto", "copa-auto+nosw@0.001", "copa-auto@0.001", "copa+nosw", "copa", "bbr", "cubic"] + \
      [f"copa-d{d}+nosw" for d in FIXED]
if len(sys.argv) > 5:
    CCS = sys.argv[5].split(",")
SET = {"downlink": dict(q=75000000, snd=67108864, loss=0.0001), "uplink": dict(q=750000, snd=4194304, loss=0.002)}
rng = random.Random(2025)
sel = {(p, d): rng.sample(range(1, 601), NTRACE) for p, d in itertools.product(PATHS, DIRS)}
z = zipfile.ZipFile(ZIP)
for (p, d), ids in sel.items():
    for i in ids:
        for f in (f"bw_{i}.txt", f"delay_{i}.txt"):
            m = f"Anonymous_{p}_{d}/{i}/{f}"
            if not os.path.exists(os.path.join(TR, m)):
                z.extract(m, TR)
jobs = []
for n, cc, ((p, d), ids) in itertools.product(FLOWS, CCS, sel.items()):
    for k, i in enumerate(ids):
        o = os.path.join(OUT, f"{p}-{d}-n{n}")
        os.makedirs(o, exist_ok=True)
        pre = os.path.join(o, f"t{i}-{cc}")
        if os.path.exists(pre + "-metrics.json"):
            continue
        base, _, floor = cc.partition("@")
        t, s = os.path.join(TR, f"Anonymous_{p}_{d}", str(i)), SET[d]
        cmd = [BIN, f"--ccs={','.join([base] * n)}", "--profile=mahimahi", f"--downTrace={t}/bw_{i}.txt",
               f"--upTrace={t}/bw_{i}.txt", f"--delayTrace={t}/delay_{i}.txt", f"--randomLoss={s['loss']}",
               "--bottleneckDelay=0", "--accessDelay=0", "--stopTime=120", "--warmup=5", f"--queueBytes={s['q']}",
               f"--sndBuf={s['snd']}", f"--run={k + 1}"]
        if floor:
            cmd.append(f"--ns3::TcpCopa::AdaptiveDeltaMin={floor}")
        jobs.append(cmd + [f"--prefix={pre}"])
def go(cmd):
    pre = cmd[-1].split("=", 1)[1]
    timeout = int(os.environ.get("RUN_TIMEOUT", "1800"))
    return run_cmd_job(cmd, ROOT, pre, timeout=timeout, metrics_fn=fmt_cap_metrics)


def label_of(cmd):
    pre = cmd[-1].split("=", 1)[1]
    return f"{os.path.basename(os.path.dirname(pre))}/{os.path.basename(pre)}"


run_parallel("leo", jobs, NJ, go, label_of)
