"""Validate the wireless Copa model against the delta sweep.

Model (x = fraction of the usable capacity mu_e = r*mu):
  1/delta = x*mu_e*j + c*x^2/(1-x)        rho = r*x
  D       = R0' + c*x/(mu_e*(1-x))
Two checks per scenario:
  A  two-point model (r = 1) calibrated on delta = 0.5 and 0.1 only, as in the note
  B  three-parameter model (j, c, r; R0' in closed form) fitted on half of the deltas
     {0.5, 0.2, 0.1, 0.05, 0.02} and tested on the other half {0.3, 0.15, 0.07, 0.03, 0.01}
Prints a table and writes validation.csv / validation.md and figures to OUT.
usage: python3 validate.py SWEEP OUT
"""
import csv
import math
import os
import sys



sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_lib as al  # noqa: E402

SWEEP, OUT = sys.argv[1], sys.argv[2]
PKT_BITS = 1500 * 8
TRAIN = [0.5, 0.2, 0.1, 0.05, 0.02]
TEST = [0.3, 0.15, 0.07, 0.03, 0.01]
ACCENT = "#166e4e"


def x_of(delta, mu_e, j, c):
    lo, hi = 1e-7, 1 - 1e-9
    for _ in range(60):
        m = (lo + hi) / 2
        if m * mu_e * j + c * m * m / (1 - m) < 1 / delta:
            lo = m
        else:
            hi = m
    return (lo + hi) / 2


def predict(params, mu, deltas):
    j, c, r, r0 = params
    mu_e = r * mu
    out = []
    for d in deltas:
        x = x_of(d, mu_e, j, c)
        out.append((r * x, r0 + c * x / (mu_e * (1 - x))))
    return out


def fit(mu, pts, fix_r=None, sr=0.05, sd=0.003):
    """pts: [(delta, rho, D_seconds)]. Grid search over j, c, r; R0' closed form."""
    def cost_of(j, c, r):
        mu_e = r * mu
        xs = [x_of(d, mu_e, j, c) for d, _, _ in pts]
        q = [c * x / (mu_e * (1 - x)) for x in xs]
        r0 = sum(D - qq for (_, _, D), qq in zip(pts, q)) / len(pts)
        e = sum(((r * x - rho) / sr) ** 2 + ((r0 + qq - D) / sd) ** 2
                for (_, rho, D), x, qq in zip(pts, xs, q))
        return e, r0
    best = None
    js = [10 ** (-4.5 + 0.1 * k) for k in range(26)]       # 0.03 ms .. 10 ms
    cs = [10 ** (-0.5 + 0.08 * k) for k in range(36)]      # 0.3 .. 230
    rs = [fix_r] if fix_r else [0.6 + 0.02 * k for k in range(21)]
    for j in js:
        for c in cs:
            for r in rs:
                e, r0 = cost_of(j, c, r)
                if best is None or e < best[0]:
                    best = (e, (j, c, r, r0))
    # local refinement
    e0, (j, c, r, r0) = best
    step = [0.1, 0.08, 0.02]
    for _ in range(6):
        step = [s / 2 for s in step]
        for dj in (-1, 0, 1):
            for dc in (-1, 0, 1):
                for dr in ((-1, 0, 1) if not fix_r else (0,)):
                    jj, cc, rr = j * 10 ** (dj * step[0]), c * 10 ** (dc * step[1]), min(1.0, r + dr * step[2])
                    e, rr0 = cost_of(jj, cc, rr)
                    if e < best[0]:
                        best = (e, (jj, cc, rr, rr0))
        e0, (j, c, r, r0) = best
    return best[1]


def power_opt(params, mu):
    j, c, r, r0 = params
    mu_e = r * mu
    A = mu_e * r0 / c
    x = math.sqrt(A) / (1 + math.sqrt(A))
    delta = 1 / (x * mu_e * j + c * x * x / (1 - x))
    return delta, r * x, r0 + c * x / (mu_e * (1 - x))


rows, md = [], ["# 模型验证\n"]
for study, name in al.STUDIES:
    data = al.load(SWEEP, study)
    probes = al.capacity(SWEEP, study)
    if not probes or "copa-d0.01" not in data:
        continue
    mu = sum(probes) / len(probes) * 1e6 / PKT_BITS
    meas = {}
    for d in al.DELTAS:
        s = al.summarize(data[al.cc_of(d)])
        meas[d] = (s["util"], s["rtt_mean"] / 1000, s["power"])
    # A: two-point model, r = 1
    pa = fit(mu, [(d, meas[d][0], meas[d][1]) for d in (0.5, 0.1)], fix_r=1.0)
    # B: three-parameter fit on TRAIN
    pb = fit(mu, [(d, meas[d][0], meas[d][1]) for d in TRAIN])
    preds_a = dict(zip(al.DELTAS, predict(pa, mu, al.DELTAS)))
    preds_b = dict(zip(al.DELTAS, predict(pb, mu, al.DELTAS)))
    d_opt, rho_opt, D_opt = power_opt(pb, mu)
    sim_best = max(al.DELTAS, key=lambda d: meas[d][2])
    err = lambda p, ds, k: sum(abs(p[d][k] - meas[d][k]) for d in ds) / len(ds)
    md.append(f"\n## {name}\n\nμ = {mu:.0f} 包/秒\n\n"
              f"- 模型 A（两点标定，δ=0.5、0.1）：j = {pa[0]*1e3:.2f} ms，c = {pa[1]:.1f}\n"
              f"- 模型 B（三参数，在 δ = {', '.join(f'{d:g}' for d in TRAIN)} 上拟合）："
              f"j = {pb[0]*1e3:.2f} ms，c = {pb[1]:.1f}，可用容量比例 r = {pb[2]:.2f}，R0' = {pb[3]*1e3:.1f} ms\n"
              f"- 模型 B 在测试集 δ = {', '.join(f'{d:g}' for d in TEST)} 上：利用率平均绝对误差 "
              f"{err(preds_b, TEST, 0)*100:.1f} 个百分点，平均 RTT 平均绝对误差 {err(preds_b, TEST, 1)*1e3:.1f} ms\n"
              f"- 模型 A 在其余 8 个 δ 上：利用率平均绝对误差 "
              f"{err(preds_a, [d for d in al.DELTAS if d not in (0.5, 0.1)], 0)*100:.1f} 个百分点\n"
              f"- 功率最优：模型 B 预测 δ* = {d_opt:.3f}（利用率 {rho_opt*100:.0f}%，平均 RTT {D_opt*1e3:.0f} ms）；"
              f"仿真中功率最大的是 δ = {sim_best:g}\n\n"
              "| δ | 仿真利用率 | 模型 B | 仿真平均 RTT | 模型 B | 集合 |\n|---|---|---|---|---|---|")
    for d in al.DELTAS:
        md.append(f"| {d:g} | {meas[d][0]*100:.0f}% | {preds_b[d][0]*100:.0f}% | {meas[d][1]*1e3:.1f} | "
                  f"{preds_b[d][1]*1e3:.1f} | {'拟合' if d in TRAIN else '测试'} |")
        rows.append([study, d, meas[d][0], preds_b[d][0], preds_a[d][0], meas[d][1] * 1e3,
                     preds_b[d][1] * 1e3, meas[d][2], "train" if d in TRAIN else "test"])
    rows.append([study, "opt", d_opt, rho_opt, "", D_opt * 1e3, "", "", sim_best])

with open(os.path.join(OUT, "validation.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["study", "delta", "util_sim", "util_modelB", "util_modelA", "rtt_sim_ms", "rtt_modelB_ms",
                "power_sim", "set"])
    w.writerows(rows)
open(os.path.join(OUT, "validation.md"), "w").write("\n".join(md) + "\n")
print("\n".join(md))
