#!/usr/bin/env python3
"""
Experiments with independent replications for TCP congestion control on mobile
users in fast vehicles (high-speed trains and cars on highways).

Two kinds of studies (see --list):

  real scenarios      ns-3 LTE (4G) and 5G-LENA (5G NR) with handovers:
                        hsr-4g, hsr-5g      high-speed railway, 350 km/h
                        hwy-4g, hwy-5g      highway car, 120 km/h
                        hsr-line, hwy-line  3-D lines from scratch/lines/*.json
                                            (time-varying speed, tunnels,
                                            interference, real cell positions)
  controlled          tcp-capacity-trace, a bottleneck whose capacity follows a
                      known schedule (ground truth), to explain the mechanisms:
                        ctrl-outage*, ctrl-handover*, ctrl-hogap*, ctrl-model-*

Every run of a real scenario is paired with a UDP capacity probe on the same
channel realisation; utilisation = TCP throughput / probe throughput.

Usage (from the ns-3 root, after building the scratch programs):
  python3 scratch/run-sweep.py --list
  python3 scratch/run-sweep.py hsr-4g hwy-4g --runs 5 --jobs 2
  python3 scratch/run-sweep.py hwy-line --ccs cubic,bbr
  python3 scratch/run-sweep.py hsr-line --lines myline      # own scratch/lines/myline.json
  python3 scratch/run-sweep.py hsr-4g --analyze-only         # re-aggregate

Outputs (scratch/results/sweep/<study>/):
  raw/...                       per-run trace files and metrics.json
  runs.csv                      one row per (value, cc, run)
  summary.csv                   mean / std / ci95 per (value, cc, metric)
  summary.md                    markdown table (mean +- ci95), ready for README
  fig-<study>.{pdf,svg,png}     metrics vs the swept parameter, with 95% CI
                                (vector + 600 dpi, style in paper_style.py;
                                --panels adds one figure per metric)

Runs whose metrics.json already exists are skipped, so an interrupted sweep
can be resumed by running the same command again.
"""

import argparse
import csv
import glob
import json
import math
import os
import statistics
import subprocess
import threading
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "scratch", "results", "sweep")
CCS = ["cubic", "bbr", "bbrv2l", "bbrv3l", "copa"]
# Copa with other deltas than the default 0.5 ("copa" is delta = 0.5).
COPA_DELTAS = ["copa-d0.1", "copa-d0.03"]


def COPA_DELTA_OF(cc):  # pylint: disable=invalid-name
    """Delta of a Copa variant name ("copa" = 0.5), empty string otherwise."""
    if cc == "copa":
        return "0.5"
    return cc[6:] if cc.startswith("copa-d") else ""


# Algorithms of the fairness studies (used when --ccs is not given).
CCS_FAIR = CCS + COPA_DELTAS

# Realistic high-speed-rail baseline: rural macro cells along a viaduct
# (3GPP RMa, LOS dominated), 300 km/h Rician fast fading, 10 MHz carrier,
# RLC UM with a deep eNB buffer, Linux-like TCP timers and a 20 ms core.
# x2Delay stays 0: with a non-zero X2 delay, forwarded X2-U data can reach the
# target eNB after the UE context is gone and the ns-3 LTE module aborts
# ("X2-U data received but no X2uTeidInfo found").
BASELINE = {
    "speed": 300,
    "enbSpacing": 1000,
    "numberOfEnbs": 6,
    "pathloss": "rma-los",
    "trackOffset": 50,
    "fading": "rician300",
    "bandwidth": 50,
    "rlcMode": "um",
    "rlcBuffer": 1048576,
    "scheduler": "pf",
    "hoHysteresis": 3,
    "hoTtt": 256,
    "s1uDelay": 2,
    "serverDelay": 20,
    "minRto": 200,
    # Linux-like loss accounting in the rate samples, and Linux-like patience
    # before giving up a connection (see TcpSocketBase::FreshLossAccounting).
    "freshLoss": "true",
    "dataRetries": 100,
}

# study -> (scenario, swept parameter, values, extra fixed arguments)
STUDIES = {}

PROGRAMS = {
    "C": "tcp-lte-highspeed-handover",
    "N": "tcp-nr-highspeed-handover",
    "E": "tcp-capacity-trace",
}

# Realistic train line, common to the 4G and 5G scenarios: 350 km/h, cells
# 50 m beside the track, a 2 km tunnel (6.3-8.3 km) served by in-tunnel cells
# every 500 m (leaky-feeder sections), 24 dB car penetration loss, ~12 km run.
TRAIN_LINE = {
    "speed": 350,
    "trackOffset": 50,
    "carLoss": 24,
    "tunnelStart": 6300,
    "tunnelLength": 2000,
    "tunnelCellSpacing": 500,
    "simTime": 125,
    # Linux delayed-ACK timer (40 ms) instead of the ns-3 default (200 ms): with
    # a 200 ms minimum RTO, the ns-3 value makes a sender with a one-segment
    # window time out before every delayed ACK, a loop that never ends.
    "delAckTimeout": 40,
}

# 4G LTE (ns-3 lte module): band 3 (1.8 GHz FDD, 20 MHz), masts every 1.2 km,
# the train starts next to the first mast; other settings = BASELINE.
TRAIN_4G = dict(TRAIN_LINE, earfcn=1575, bandwidth=100, enbSpacing=1200, enbStart=0,
                numberOfEnbs=11)

# 5G NR SA (5G-LENA nr module, with scratch/patches/5g-lena-handover-fixes.patch):
# n78 3.5 GHz TDD DDDSU, 40 MHz, 30 kHz SCS, 3GPP RMa channel with shadowing
# and fast fading, masts every 800 m, X2 handover one TTT after the cell border.
BASELINE_N = dict(
    TRAIN_LINE,
    frequency=3.5e9,
    nrBandwidth=40e6,
    numerology=1,
    gnbSpacing=800,
    numberOfGnbs=16,
    gnbTxPower=46,
    gnbHeight=35,
    fastFading="true",
    shadowing="true",
    rlcBuffer=1048576,
    hoTtt=256,
    s1uDelay=2,
    serverDelay=20,
    minRto=200,
    freshLoss="true",
    dataRetries=100,
)

# Highway car (4G and 5G): 120 km/h, masts 80 m beside the road, no tunnel,
# 9 dB vehicle penetration loss (3GPP TR 38.901, car with ordinary glass),
# EVA 120 km/h fast fading (4G), about 6 km in 180 s. Masts every 1 km (4G,
# rural/suburban mix) and 500 m (5G n78, shorter coverage at 3.5 GHz).
HWY_LINE = dict(
    TRAIN_LINE,
    speed=120,
    trackOffset=80,
    carLoss=9,
    tunnelLength=0,
    simTime=185,
)
HWY_FADING = "scratch/fading-traces/fading_trace_EVA_120kmph.fad"
HWY_4G = dict(HWY_LINE, earfcn=1575, bandwidth=100, enbSpacing=1000, enbStart=0,
              numberOfEnbs=7, fading=HWY_FADING)
HWY_5G = dict(BASELINE_N, **HWY_LINE, gnbSpacing=500, numberOfGnbs=13)

# Settings of the 3-D lines (scratch/lines/<name>.json, see scratch/lines/README.md):
# the json sets track, speed profile, cells, antennas, tunnels and interference;
# radio and TCP settings are those of hsr-4g (hwy-line: EVA 120 km/h fading).
LINE_4G = {"earfcn": 1575, "bandwidth": 100, "delAckTimeout": 40}

STUDIES.update({
    "hsr-4g": ("C", "speed", [350], TRAIN_4G),
    "hsr-5g": ("N", "speed", [350], {}),
    "hwy-4g": ("C", "speed", [120], HWY_4G),
    "hwy-5g": ("N", "speed", [120], HWY_5G),
    # --lines name1,name2 replaces the line list of the two line studies.
    "hsr-line": ("C", "scenario", ["hsr"], LINE_4G),
    "hwy-line": ("C", "scenario", ["highway"], dict(LINE_4G, fading=HWY_FADING)),
    # Pedestrians in a city (walking 5 km/h, running 10-12 km/h): 3GPP UMa
    # macro cells with LOS/NLOS and shadowing, ETU fading at walking/running
    # speed; underpass, weak-signal streets, elevators and dense crowds are in
    # the line files (scratch/lines/walk.json, run.json).
    "walk-line": ("C", "scenario", ["walk"], dict(LINE_4G, pathloss="uma", fading="etu5")),
    "run-line": ("C", "scenario", ["run"], dict(LINE_4G, pathloss="uma", fading="etu12")),
    # City and countryside lines (scratch/lines/*.json, see docs/mobile-scenarios.md).
    "city-drive": ("C", "scenario", ["city-drive"], dict(LINE_4G, pathloss="uma", fading="eva60")),
    "city-bus": ("C", "scenario", ["city-bus"], dict(LINE_4G, pathloss="uma", fading="eva60")),
    "city-subway": ("C", "scenario", ["city-subway"], dict(LINE_4G, pathloss="uma", fading="eva60")),
    "city-elevated": ("C", "scenario", ["city-elevated"], dict(LINE_4G, pathloss="uma", fading=HWY_FADING)),
    "city-mall": ("C", "scenario", ["city-mall"], dict(LINE_4G, pathloss="uma", fading="etu3")),
    "city-stadium": ("C", "scenario", ["city-stadium"], dict(LINE_4G, pathloss="uma", fading="epa3")),
    "rural-road": ("C", "scenario", ["rural-road"], dict(LINE_4G, pathloss="rma", fading="eva60")),
    "rural-mountain": ("C", "scenario", ["rural-mountain"], dict(LINE_4G, pathloss="rma", fading="eva60")),
    "rural-village": ("C", "scenario", ["rural-village"], dict(LINE_4G, pathloss="rma", fading="etu5")),
    "rural-edge": ("C", "scenario", ["rural-edge"], dict(LINE_4G, pathloss="rma", fading="epa3")),
})

# Scenario E (capacity-trace bottleneck): known ground-truth capacity, 20 Mbit/s
# nominal (5..40 Mbit/s for the random profiles), 50 ms base RTT, 500 KB FIFO
# (about 4 BDP, a deep per-user cellular buffer) and Linux-like 200 ms minRTO.
BASELINE_E = {
    "capacity": 20,
    "capMin": 5,
    "capMax": 40,
    "bottleneckDelay": 20,
    "accessDelay": 5,
    "queueBytes": 500000,
    "minRto": 200,
    "stopTime": 60,
    "warmup": 5,
    "eventJitter": 2,
}

# Controlled studies (scenario E).
STUDIES.update({
    # Outage (tunnel, coverage hole): capacity drops to 0 for a given time.
    "ctrl-outage": ("E", "outageDuration", [0.2, 0.5, 1, 2, 3, 5, 10], {"profile": "outage"}),
    # Handovers: short interruptions at random intervals, new random capacity after each.
    "ctrl-handover": ("E", "hoInterval", [1, 2, 5, 10], {"profile": "handover"}),
    "ctrl-hogap": ("E", "hoGap", [30, 80, 200, 500], {"profile": "handover"}),
    # The same with the data held by the base station instead of dropped
    # (handover with data forwarding, RLC AM re-establishment).
    "ctrl-outage-hold": ("E", "outageDuration", [0.2, 0.5, 1, 2, 3, 5, 10],
                         {"profile": "outage", "disruption": "hold"}),
    "ctrl-handover-hold": ("E", "hoInterval", [1, 2, 5, 10],
                           {"profile": "handover", "disruption": "hold"}),
    "ctrl-hogap-hold": ("E", "hoGap", [30, 80, 200, 500],
                        {"profile": "handover", "disruption": "hold"}),
    # Validation of the analytical model: constant capacity across handovers,
    # and handovers whose interruption (200 ms) is longer than the queue.
    "ctrl-model-interval": ("E", "hoInterval", [1, 2, 5, 10],
                            {"profile": "handover", "capMin": 20, "capMax": 20}),
    "ctrl-model-gap": ("E", "hoGap", [80, 200, 500, 1000],
                       {"profile": "handover", "capMin": 20, "capMax": 20}),
    "ctrl-model-interval-rto": ("E", "hoInterval", [1, 2, 5, 10],
                                {"profile": "handover", "capMin": 20, "capMax": 20,
                                 "hoGap": 200}),
    "ctrl-handover-rto": ("E", "hoInterval", [1, 2, 5, 10],
                          {"profile": "handover", "hoGap": 200}),
})

# Fairness (Jain index) between flows sharing one bottleneck, for every
# algorithm and Copa delta (CCS_FAIR): flows of the same algorithm
# (numFlows), and the algorithm under test against CUBIC, BBRv1 or Copa with
# delta = 0.5 (competitor). Constant 20 Mbit/s, or the handover profile
# (new cell every 5 s on average, 80 ms interruption, 5..40 Mbit/s). The
# second flow starts 2 s after the first.
FAIR_CONST = {"profile": "file", "traceFile": "scratch/capacity-traces/constant-20.txt",
              "flowStagger": 2}
FAIR_HO = {"profile": "handover", "flowStagger": 2}
STUDIES.update({
    "fair-intra": ("E", "numFlows", [2, 4], FAIR_CONST),
    "fair-inter": ("E", "competitor", ["cubic", "bbr", "copa"], FAIR_CONST),
    "fair-intra-ho": ("E", "numFlows", [2, 4], FAIR_HO),
    "fair-inter-ho": ("E", "competitor", ["cubic", "bbr", "copa"], FAIR_HO),
    # Two flows to the same phone on the highway 4G scenario.
    "fair-hwy-4g": ("C", "numFlows", [2], HWY_4G),
    "fair-hwy-4g-cubic": ("C", "competitor", ["cubic"], HWY_4G),
})
FAIR_STUDIES = tuple(k for k in STUDIES if k.startswith("fair-"))

# Metrics reported per scenario: (key, label, better)
METRICS = {
    "C": [
        ("throughput_mbps", "Throughput (Mbps)", "high"),
        ("utilization", "Capacity utilisation", "high"),
        ("rtt_p50_ms", "RTT p50 (ms)", "low"),
        ("rtt_p95_ms", "RTT p95 (ms)", "low"),
        ("rtt_p99_ms", "RTT p99 (ms)", "low"),
        ("retx", "Retransmissions", "low"),
        ("ho_deficit_mb", "Handover deficit (MB/HO)", "low"),
        ("ho_recovery_s", "Handover 90% recovery (s)", "low"),
        ("jain", "Jain fairness", "high"),
        ("share", "Share of flow 0", "high"),
    ],
    "E": [
        ("throughput_mbps", "Throughput (Mbps)", "high"),
        ("utilization", "Utilisation", "high"),
        ("recovery_s", "Recovery to 90% (s)", "low"),
        ("first_data_s", "First data after outage (s)", "low"),
        ("ramp_s", "Ramp-up after first data (s)", "low"),
        ("rtt_p50_ms", "RTT p50 (ms)", "low"),
        ("rtt_p95_ms", "RTT p95 (ms)", "low"),
        ("rtt_p99_ms", "RTT p99 (ms)", "low"),
        ("retx", "Retransmissions", "low"),
        ("stepdown_rtt_peak_ms", "RTT peak after step-down (ms)", "low"),
        ("jain", "Jain fairness", "high"),
        ("share", "Share of flow 0", "high"),
    ],
}

# Metrics that only make sense for some studies.
ONLY_IN = {
    "jain": FAIR_STUDIES,
    "share": FAIR_STUDIES,
    "stepdown_rtt_peak_ms": (),
    "first_data_s": ("ctrl-outage", "ctrl-outage-hold"),
    "ramp_s": ("ctrl-outage", "ctrl-outage-hold"),
    "recovery_s": tuple(k for k in STUDIES if k.startswith("ctrl-")),
}


def shown_metrics(study, metric_defs):
    """Metrics displayed for a study."""
    return [m for m in metric_defs if m[0] not in ONLY_IN or study in ONLY_IN[m[0]]]

# Two-sided 95% Student-t quantiles, index = degrees of freedom.
SET_OVERRIDES = set()  # keys given with --set, also applied to scenario E
PANELS = False  # --panels: also write one single-column figure per metric

T95 = [float("nan"), 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262,
       2.228, 2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
       2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]


def t95(dof):
    """Two-sided 95% t quantile."""
    if dof <= 0:
        return float("nan")
    return T95[dof] if dof < len(T95) else 1.96


def find_binary(program):
    """Locate the compiled scratch program."""
    patterns = [
        os.path.join(ROOT, "build", "scratch", f"ns3*-{program}-*"),
        os.path.join(ROOT, "build", "scratch", f"*{program}*"),
    ]
    for pattern in patterns:
        found = [p for p in glob.glob(pattern) if os.access(p, os.X_OK)]
        if found:
            return sorted(found)[0]
    sys.exit(f"binary for {program} not found, run ./ns3 build first")


def value_tag(value):
    """File-system friendly representation of a parameter value."""
    return str(value).replace("/", "_").replace(".", "p")


# ---------------------------------------------------------------------------
# Job execution
# ---------------------------------------------------------------------------


def make_jobs(study, ccs, runs, baseline):
    """Return the list of jobs of a study."""
    scenario, param, values, extra = STUDIES[study]
    jobs = []
    for value in values:
        if scenario == "E":
            args = dict(BASELINE_E)
        elif scenario == "N":
            args = dict(BASELINE_N)
        else:
            args = dict(baseline)
        if scenario == "E":
            args.update({k: v for k, v in baseline.items() if k in SET_OVERRIDES})
        args.update(extra)
        args[param] = value
        rundir = os.path.join(OUT, study, "raw", value_tag(value))
        for run in range(1, runs + 1):
            if scenario in ("C", "N"):
                # Capacity reference measured with the same channel realisation.
                # One saturating UDP flow, whatever the number of TCP flows.
                probe_args = {k: v for k, v in args.items() if k not in ("numFlows", "competitor")}
                jobs.append((scenario, study, value, "probe", run, probe_args, rundir))
                if scenario == "C":
                    # RTT of the unloaded path (UDP echo), the reference of
                    # the RTT plots.
                    jobs.append((scenario, study, value, "rttprobe", run, probe_args, rundir))
            for cc in ccs:
                if param == "competitor" and value == cc:
                    continue
                jobs.append((scenario, study, value, cc, run, args, rundir))
    return jobs


def job_stem(job):
    """Prefix of the output files of one job, and the metrics path."""
    scenario, _study, _value, cc, run, args, rundir = job
    prefix = os.path.join(rundir, f"r{run}")
    if scenario in ("C", "N"):
        base = prefix if cc == "probe" else f"{prefix}-{cc}"
    else:
        base = f"{prefix}-{cc}"
    return prefix, base


def flows_spec(cc, args):
    """--ccs value of a scenario E job: the algorithm under test first."""
    if "competitor" in args:
        other = cc if args["competitor"] == "self" else args["competitor"]
        return f"{cc},{other}"
    return ",".join([cc] * int(args.get("numFlows", 1)))


RESOLVE_LOCK = threading.Lock()


def resolve_line(name, rundir):
    """Validate scratch/lines/<name>.json and write its resolved form (once)."""
    src = name if name.endswith(".json") else os.path.join(ROOT, "scratch", "lines",
                                                            f"{name}.json")
    dst = os.path.join(rundir, os.path.splitext(os.path.basename(src))[0] + ".resolved")
    with RESOLVE_LOCK:
        if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
            os.makedirs(rundir, exist_ok=True)
            subprocess.run([sys.executable, os.path.join(ROOT, "scratch", "preview-line.py"),
                            src, "--export", dst, "--no-plot"], check=True,
                           stdout=subprocess.DEVNULL)
    return dst


def run_job(job, binaries):
    """Run one simulation unless its result already exists."""
    scenario, _study, _value, cc, run, args, rundir = job
    prefix, base = job_stem(job)
    if os.path.exists(base + "-metrics.json"):
        return "skip"
    os.makedirs(rundir, exist_ok=True)
    if scenario == "E":
        cmd = [binaries[scenario], f"--run={run}", f"--prefix={base}",
               f"--ccs={flows_spec(cc, args)}"]
        args = {k: v for k, v in args.items() if k not in ("competitor", "numFlows")}
    else:
        cmd = [binaries[scenario], f"--run={run}", f"--prefix={prefix}"]
    if cc == "probe":
        cmd.append("--capacityProbe=true")
    elif cc == "rttprobe":
        cmd.append("--rttProbe=true")
    elif scenario != "E":
        cmd.append(f"--cc={cc}")
    for key, value in args.items():
        if key == "scenario":
            value = resolve_line(value, rundir)
        if key == "line" and "/" not in str(value):
            value = os.path.join("scratch", "lines", f"{value}.line")
        cmd.append(f"--{key}={value}")
    with open(base + "-stdout.log", "w", encoding="utf-8") as log:
        result = subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=False)
    return "ok" if result.returncode == 0 else f"FAILED ({result.returncode}): {' '.join(cmd)}"


# ---------------------------------------------------------------------------
# Per-run metrics
# ---------------------------------------------------------------------------


def read_columns(path, ncols):
    """Read whitespace separated rows with at least ncols fields."""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            parts = line.split()
            if len(parts) >= ncols:
                rows.append(parts)
    return rows


def handover_metrics(base, dt=0.1):
    """Mean data deficit and 90% recovery time around each handover.

    Baseline = median total throughput over [t-5 s, t-1 s] before the
    handover start t. Deficit = bytes missing w.r.t. the baseline over
    [t-1 s, t+4 s]. Recovery = time after t until throughput is back to 90%
    of the baseline (after having dropped below it).
    """
    tput = [(float(r[0]), float(r[1])) for r in read_columns(base + "-throughput.data", 2)]
    starts = [float(r[0]) for r in read_columns(base + "-handover.data", 2) if r[1] == "start"]
    deficits, recoveries = [], []
    for t in starts:
        before = [m for s, m in tput if t - 5.0 <= s <= t - 1.0]
        if len(before) < 5:
            continue
        ref = statistics.median(before)
        if ref <= 0.1:
            continue
        window = [(s, m) for s, m in tput if t - 1.0 < s <= t + 4.0]
        deficits.append(sum(max(0.0, ref - m) * dt for _, m in window) / 8.0)
        dipped = False
        for s, m in window:
            if s < t:
                continue
            if m < 0.9 * ref:
                dipped = True
            elif dipped:
                recoveries.append(s - t)
                break
        else:
            if not dipped:
                recoveries.append(0.0)
    mean = lambda v: statistics.mean(v) if v else float("nan")  # noqa: E731
    return mean(deficits), mean(recoveries)


def metrics_c(job, capacity):
    """Metrics of a scenario C run."""
    _prefix, base = job_stem(job)
    with open(base + "-metrics.json", encoding="utf-8") as stream:
        data = json.load(stream)
    flow0 = data["flows"][0]
    deficit, recovery = handover_metrics(base)
    tput = data["total_throughput_mbps"] if job[3] != "probe" else flow0["throughput_mbps"]
    return {
        "throughput_mbps": flow0["throughput_mbps"],
        "total_throughput_mbps": tput,
        "utilization": tput / capacity if capacity else float("nan"),
        "rtt_p50_ms": flow0["rtt_p50_ms"],
        "rtt_p95_ms": flow0["rtt_p95_ms"],
        "rtt_p99_ms": flow0["rtt_p99_ms"],
        "mean_delay_ms": flow0["mean_delay_ms"],
        "lost": flow0["lost"],
        "retx": flow0["retx"],
        "handovers": data["handovers"],
        "ho_deficit_mb": deficit,
        "ho_recovery_s": recovery,
        "jain": data["jain"],
        "share": flow0["throughput_mbps"] / tput if tput else float("nan"),
    }


def read_csv_rows(path):
    """Rows of a CSV file as dicts, empty when missing."""
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def metrics_e(job):
    """Metrics of a scenario E run (capacity-trace bottleneck).

    Recovery: after every outage-end / step-up / cell-change event, the time
    until the 250 ms moving average of the aggregate throughput reaches 90% of
    the (known) capacity. If it is not reached before the next event, the
    time to that event is used (censored), so slow algorithms are not hidden.
    """
    _prefix, base = job_stem(job)
    with open(base + "-metrics.json", encoding="utf-8") as stream:
        data = json.load(stream)
    series = read_csv_rows(base + "-timeseries.csv")
    events = read_csv_rows(base + "-events.csv")
    t = [float(r["time_s"]) for r in series]
    cap = [float(r["capacity_mbps"]) for r in series]
    tcols = [k for k in (series[0].keys() if series else []) if k.startswith("tput")]
    total = [sum(float(r[k]) for k in tcols) for r in series]
    rtt0 = [float(r.get("rtt0_ms", 0) or 0) for r in series]
    ev = [(float(e["time_s"]), e["event"]) for e in events]
    window = 5
    recoveries = []
    first = []
    for i, (te, name) in enumerate(ev):
        if name == "outage-end" and te >= 5.0:
            k0 = next((k for k in range(len(t)) if t[k] > te and total[k] > 0.1), None)
            if k0 is not None:
                first.append(t[k0] - te)
        if name not in ("outage-end", "step-up", "cell-change") or te < 5.0:
            continue
        t_next = min([x for x, _ in ev[i + 1:]] + [t[-1] if t else te])
        rec = None
        for k in range(len(t)):
            if t[k] <= te or t[k] > t_next:
                continue
            w = total[max(0, k - window + 1):k + 1]
            if len(w) == window and cap[k] > 0 and sum(w) / window >= 0.9 * cap[k]:
                rec = t[k] - te
                break
        recoveries.append(rec if rec is not None else t_next - te)
    peaks = []
    for te, name in ev:
        if name == "step-down":
            win = [r for x, r in zip(t, rtt0) if te <= x <= te + 5.0 and r > 0]
            if win:
                peaks.append(max(win))
    flow0 = data["flows"][0]
    mean = lambda v: statistics.mean(v) if v else float("nan")  # noqa: E731
    return {
        "throughput_mbps": flow0["throughput_mbps"],
        "total_throughput_mbps": data["total_throughput_mbps"],
        "utilization": data["utilization"],
        "recovery_s": mean(recoveries),
        "first_data_s": mean(first),
        "ramp_s": (mean(recoveries) - mean(first)) if first else float("nan"),
        "rtt_p50_ms": flow0["rtt_p50_ms"],
        "rtt_p95_ms": flow0["rtt_p95_ms"],
        "rtt_p99_ms": flow0["rtt_p99_ms"],
        "retx": flow0["retx"],
        "stepdown_rtt_peak_ms": mean(peaks),
        "jain": data["jain"],
        "share": (flow0["throughput_mbps"] / data["total_throughput_mbps"]
                  if data["total_throughput_mbps"] else float("nan")),
    }


def summarize(values):
    """Return (n, mean, std, ci95) ignoring NaNs."""
    vals = [v for v in values if v is not None and not math.isnan(v)]
    n = len(vals)
    if n == 0:
        return 0, float("nan"), float("nan"), float("nan")
    mean = statistics.mean(vals)
    std = statistics.stdev(vals) if n > 1 else 0.0
    ci = t95(n - 1) * std / math.sqrt(n) if n > 1 else float("nan")
    return n, mean, std, ci


def analyze(study, ccs, runs, baseline):
    """Collect per-run metrics, aggregate them and plot."""
    scenario, param, values, _extra = STUDIES[study]
    jobs = make_jobs(study, ccs, runs, baseline)
    rows = []
    capacity = {}
    if scenario in ("C", "N"):
        for job in jobs:
            if job[3] == "probe":
                _p, base = job_stem(job)
                if os.path.exists(base + "-metrics.json"):
                    with open(base + "-metrics.json", encoding="utf-8") as stream:
                        capacity[(job[2], job[4])] = json.load(stream)["flows"][0][
                            "throughput_mbps"
                        ]
    for job in jobs:
        if job[3] == "rttprobe":
            continue
        _p, base = job_stem(job)
        if not os.path.exists(base + "-metrics.json"):
            continue
        if scenario in ("C", "N"):
            m = metrics_c(job, capacity.get((job[2], job[4])))
        else:
            m = metrics_e(job)
        rows.append({"value": job[2], "cc": job[3], "run": job[4], **m})

    outdir = os.path.join(OUT, study)
    os.makedirs(outdir, exist_ok=True)
    if not rows:
        print(f"[{study}] no results yet")
        return
    keys = list(rows[0].keys())
    with open(os.path.join(outdir, "runs.csv"), "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)

    metric_defs = METRICS["C" if scenario == "N" else scenario]
    order = (["probe"] if scenario in ("C", "N") else []) + [c for c in ccs]
    summary = []
    for value in values:
        for cc in order:
            sel = [r for r in rows if r["value"] == value and r["cc"] == cc]
            if not sel:
                continue
            for key, _label, _better in metric_defs + (
                [("total_throughput_mbps", "", "")] if scenario in ("C", "E", "N") else []
            ):
                n, mean, std, ci = summarize([r[key] for r in sel])
                summary.append(
                    {"value": value, "cc": cc, "metric": key, "n": n,
                     "mean": mean, "std": std, "ci95": ci}
                )
    with open(os.path.join(outdir, "summary.csv"), "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=["value", "cc", "metric", "n", "mean", "std", "ci95"]
        )
        writer.writeheader()
        writer.writerows(summary)

    write_markdown(study, param, values, order, metric_defs, summary, outdir)
    plot(study, param, values, [c for c in order if c != "probe"], metric_defs, summary, outdir,
         panels=PANELS)
    print(f"[{study}] {len(rows)} runs aggregated -> {os.path.relpath(outdir, ROOT)}")


def fmt(mean, ci):
    """Format mean +- ci."""
    if math.isnan(mean):
        return "-"
    digits = 3 if abs(mean) < 1 else (2 if abs(mean) < 10 else 1)
    if math.isnan(ci):
        return f"{mean:.{digits}f}"
    return f"{mean:.{digits}f} ± {ci:.{digits}f}"


def write_markdown(study, param, values, order, metric_defs, summary, outdir):
    """Write a markdown table: one row per (value, cc)."""
    index = {(s["value"], s["cc"], s["metric"]): s for s in summary}
    shown = shown_metrics(study, metric_defs)
    lines = [
        f"| {param} | 算法 | n | " + " | ".join(label for _k, label, _b in shown) + " |",
        "|" + "---|" * (3 + len(shown)),
    ]
    for value in values:
        for cc in order:
            cells = []
            n = 0
            for key, _label, _better in shown:
                s = index.get((value, cc, key))
                if s is None or (cc == "probe" and key not in ("throughput_mbps", "utilization")):
                    cells.append("-")
                    continue
                n = max(n, s["n"])
                cells.append(fmt(s["mean"], s["ci95"]))
            if n == 0:
                continue
            name = "容量探测 (UDP)" if cc == "probe" else (
                f"{cc}（δ={COPA_DELTA_OF(cc)}）" if COPA_DELTA_OF(cc) else cc)
            lines.append(f"| {value} | {name} | {n} | " + " | ".join(cells) + " |")
    with open(os.path.join(outdir, "summary.md"), "w", encoding="utf-8") as stream:
        stream.write("\n".join(lines) + "\n")


PARAM_LABELS = {
    "line": "Railway line",
    "scenario": "Railway line (3-D)",
    "rlcBuffer": "RLC buffer",
    "rlcMode": "RLC mode",
    "speed": "Train speed (km/h)",
    "enbSpacing": "eNB spacing (m)",
    "fading": "Fast fading",
    "pathloss": "Pathloss model",
    "bandwidth": "Bandwidth (RBs)",
    "hoHysteresis": "A3 hysteresis (dB)",
    "hoTtt": "Time-to-trigger (ms)",
    "serverDelay": "Server one-way delay (ms)",
    "minRto": "Minimum RTO (ms)",
    "scheduler": "MAC scheduler",
    "competitor": "Competing flow",
    "numFlows": "Number of flows",
    "outageDuration": "Outage duration (s)",
    "eventJitter": "Event jitter (s)",
    "transport": "Transport",
    "stepFactor": "Capacity after step (fraction)",
    "hoInterval": "Mean handover interval (s)",
    "hoGap": "Handover interruption (ms)",
    "fluctSigma": "Capacity fluctuation $\\sigma$",
    "queueBytes": "Bottleneck buffer",
    "aqm": "Queue discipline",
    "randomLoss": "Random loss rate",
}


def value_label(param, value):
    """Human readable tick label of a swept value."""
    if param in ("rlcBuffer", "queueBytes"):
        v = int(float(value))
        if v % 1024 == 0:
            return f"{v / 1048576:g} MiB" if v >= 1048576 else f"{v / 1024:g} KiB"
        return f"{v / 1e6:g} MB" if v >= 1e6 else f"{v / 1e3:g} KB"
    if param == "competitor":
        if str(value) in ("self", "none"):
            return {"self": "same algorithm", "none": "none"}[str(value)]
        import paper_style as ps  # pylint: disable=import-outside-toplevel
        return ps.algo(str(value))[2]
    return str(value)


def plot(study, param, values, ccs, metric_defs, summary, outdir, panels=False):
    """Plot every metric vs the swept value, one solid line per algorithm.

    Writes fig-<study>.{pdf,svg,png} (all metrics, double-column width) and,
    with panels=True, one single-column figure per metric.
    """
    try:
        import paper_style as ps
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available, skipping plots")
        return
    ps.apply()
    index = {(s["value"], s["cc"], s["metric"]): s for s in summary}
    shown = shown_metrics(study, metric_defs)
    xs = list(range(len(values)))
    xlabel = PARAM_LABELS.get(param, param)
    ticks = [value_label(param, v) for v in values]

    def draw(ax, key, label, better):
        for i, cc in enumerate(ccs):
            mean, ci = [], []
            for value in values:
                s = index.get((value, cc, key))
                mean.append(s["mean"] if s else float("nan"))
                ci.append(0.0 if (s is None or math.isnan(s["ci95"])) else s["ci95"])
            color, marker, name = ps.algo(cc)
            offset = (i - (len(ccs) - 1) / 2) * 0.05
            ax.errorbar([x + offset for x in xs], mean, yerr=ci, color=color, marker=marker,
                        linestyle="-", linewidth=1.1, markersize=3.8, markeredgewidth=0,
                        capsize=1.8, elinewidth=0.7, capthick=0.7, label=name)
        ax.set_xticks(xs)
        ax.set_xticklabels(ticks)
        ax.set_xlim(-0.4, len(values) - 0.6)
        arrow = "\u2191" if better == "high" else "\u2193"
        ax.set_title(f"{label} {arrow}", loc="left")
        ax.set_xlabel(xlabel)
        if key in ("utilization", "jain"):
            ax.set_ylim(bottom=0)

    cols = 3
    nrows = math.ceil(len(shown) / cols)
    fig, axes = plt.subplots(nrows, cols, figsize=(ps.COL2, 1.95 * nrows + 0.35),
                             squeeze=False)
    for ax, (key, label, better) in zip(axes.flat, shown):
        draw(ax, key, label, better)
    for ax in list(axes.flat)[len(shown):]:
        ax.axis("off")
    handles, labels = axes.flat[0].get_legend_handles_labels()
    fig.tight_layout(h_pad=1.0, w_pad=1.2, rect=(0, 0, 1, 1 - 0.3 / (1.95 * nrows + 0.35)))
    fig.legend(handles, labels, loc="upper center", ncol=min(len(labels), 7),
               bbox_to_anchor=(0.5, 1.0))
    ps.save(fig, os.path.join(outdir, f"fig-{study}"))

    if panels:
        for key, label, better in shown:
            fig, ax = plt.subplots(figsize=(ps.COL1, 2.3))
            draw(ax, key, label, better)
            ax.set_title("")
            ax.set_ylabel(label)
            ps.legend_top(ax, ncol=3 if len(ccs) > 4 else len(ccs))
            ps.save(fig, os.path.join(outdir, f"fig-{study}-{key}"))


# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("studies", nargs="*", help="studies to run (see --list)")
    parser.add_argument("--list", action="store_true", help="list the available studies")
    parser.add_argument("--runs", type=int, default=5, help="replications per point")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--ccs", default="",
                        help="algorithms (default: " + ",".join(CCS) + "; fairness studies: "
                        + ",".join(CCS_FAIR) + ")")
    parser.add_argument("--set", action="append", default=[],
                        help="override a baseline parameter, e.g. --set bandwidth=25")
    parser.add_argument("--analyze-only", action="store_true")
    parser.add_argument("--lines", default="",
                        help="hsr-line / hwy-line: comma-separated names of scratch/lines/*.json")
    parser.add_argument("--panels", action="store_true",
                        help="also write one single-column figure per metric")
    args = parser.parse_args()

    if args.list or not args.studies:
        print("baseline (realistic):", " ".join(f"--{k}={v}" for k, v in BASELINE.items()))
        print("baseline E (capacity trace):",
              " ".join(f"--{k}={v}" for k, v in BASELINE_E.items()))
        for name, (scenario, param, values, extra) in STUDIES.items():
            more = " " + " ".join(f"--{k}={v}" for k, v in extra.items()) if extra else ""
            print(f"  {name:14s} scenario {scenario}  {param} in {values}{more}")
        return

    baseline = dict(BASELINE)
    for item in args.set:
        key, _, value = item.partition("=")
        baseline[key] = value
        SET_OVERRIDES.add(key)
    explicit = [c for c in args.ccs.split(",") if c]

    def ccs_of(study):
        """Algorithms of a study: --ccs, else the default list of its kind."""
        return explicit or (CCS_FAIR if study in FAIR_STUDIES else CCS)
    global OUT, PANELS  # pylint: disable=global-statement
    PANELS = args.panels

    if args.lines:
        for st in ("hsr-line", "hwy-line"):
            scen, par, _vals, extra = STUDIES[st]
            STUDIES[st] = (scen, par, [x for x in args.lines.split(",") if x], extra)

    for study in args.studies:
        if study not in STUDIES:
            sys.exit(f"unknown study {study}, see --list")

    if not args.analyze_only:
        needed = {STUDIES[s][0] for s in args.studies}
        binaries = {s: find_binary(p) for s, p in PROGRAMS.items() if s in needed}
        jobs = [j for s in args.studies for j in make_jobs(s, ccs_of(s), args.runs, baseline)]
        print(f"{len(jobs)} jobs, {args.jobs} in parallel")
        done = 0
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = {pool.submit(run_job, j, binaries): j for j in jobs}
            for fut in as_completed(futures):
                done += 1
                status = fut.result()
                job = futures[fut]
                if status.startswith("FAILED"):
                    print(f"  {status}")
                elif status == "ok":
                    print(f"  [{done}/{len(jobs)}] {job[1]} {job[2]} {job[3]} run {job[4]}",
                          flush=True)

    for study in args.studies:
        # Aggregate every algorithm that has results, not only this call's --ccs.
        analyze(study, CCS_FAIR + [c for c in explicit if c not in CCS_FAIR], args.runs, baseline)


if __name__ == "__main__":
    main()
