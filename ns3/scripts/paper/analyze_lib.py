import glob, json, math, os, statistics
STUDIES = [("hsr-4g", "高铁 4G（350 km/h）"), ("hwy-4g", "高速 4G（120 km/h）"),
           ("walk-line", "步行线路"), ("run-line", "跑步线路")]
DELTAS = [0.5, 0.3, 0.2, 0.15, 0.1, 0.07, 0.05, 0.03, 0.02, 0.01]
BASE_RTT = 50.0
def cc_of(d): return "copa" if d == 0.5 else f"copa-d{d:g}"
def _stems(sweep, study):
    for d in glob.glob(os.path.join(sweep, study, "raw", "*")):
        for f in glob.glob(os.path.join(d, "r*-metrics.json")):
            stem = os.path.basename(f)[:-len("-metrics.json")]
            run, _, cc = stem.partition("-")
            yield d, run, cc, f
def capacity(sweep, study):
    return [json.load(open(f))["total_throughput_mbps"] for _, _, cc, f in _stems(sweep, study) if cc == ""]
def load(sweep, study):
    probes = {(d, run): json.load(open(f))["total_throughput_mbps"] for d, run, cc, f in _stems(sweep, study) if cc == ""}
    out = {}
    for d, run, cc, f in _stems(sweep, study):
        if cc in ("", "rttprobe") or (d, run) not in probes: continue
        fl = json.load(open(f))["flows"][0]
        out.setdefault(cc, []).append({"run": run, "thr": fl["throughput_mbps"], "util": fl["throughput_mbps"] / probes[(d, run)],
                                       "rtt_mean": fl["rtt_mean_ms"], "rtt_p95": fl["rtt_p95_ms"]})
    return out
def mean_ci(xs):
    m = statistics.mean(xs)
    if len(xs) < 2: return m, float("nan")
    t = {2: 12.71, 3: 4.30, 4: 3.18, 5: 2.78}.get(len(xs), 2.0)
    return m, t * statistics.stdev(xs) / math.sqrt(len(xs))
def summarize(runs):
    s = {"n": len(runs)}
    for k in ("util", "thr", "rtt_mean", "rtt_p95"):
        s[k], s[k + "_ci"] = mean_ci([r[k] for r in runs])
    s["power"], s["power_ci"] = mean_ci([r["util"] / (r["rtt_mean"] / BASE_RTT) for r in runs])
    return s
