"""Copa on a jittery, bursty wireless link: fixed point and optimal delta.
  Copa fixed point:  1/delta = rho*mu*j + c*rho^2/(1-rho)
  Mean RTT:          D(rho) = R0 + j + c*rho/(mu*(1-rho))
  Objective:         U_w = log(rho) - w*log(D)   (w=1: Kleinrock power)
"""
import math
R0 = 0.050
def delta_of(rho, mu, j, c): return 1.0 / (rho * mu * j + c * rho * rho / (1 - rho))
def D(rho, mu, j, c): return R0 + j + c * rho / (mu * (1 - rho))
def rho_of(delta, mu, j, c):
    lo, hi = 1e-6, 1 - 1e-9
    for _ in range(200):
        m = (lo + hi) / 2
        if delta_of(m, mu, j, c) > delta: lo = m
        else: hi = m
    return (lo + hi) / 2
def fit(mu, p1, p2):
    (d1, r1), (d2, r2) = p1, p2
    a1, b1 = r1 * mu, r1 * r1 / (1 - r1)
    a2, b2 = r2 * mu, r2 * r2 / (1 - r2)
    det = a1 * b2 - a2 * b1
    j = (b2 / d1 - b1 / d2) / det
    c = (a1 / d2 - a2 / d1) / det
    return j, c
def best(mu, j, c, w):
    grid = [i / 10000 for i in range(1, 9999)]
    r = max(grid, key=lambda x: math.log(x) - w * math.log(D(x, mu, j, c)))
    return r
if __name__ == "__main__":
    PKT = 1500 * 8
    for name, mbps, p1, p2, chk in (("hsr-4g", 54.2, (.5, .27), (.1, .572), (.03, .766)),
                                    ("hwy-4g", 67.7, (.5, .305), (.1, .585), (.03, .854))):
        mu = mbps * 1e6 / PKT
        j, c = fit(mu, p1, p2)
        print(f"{name}: mu={mu:.0f} pkt/s  j={j*1e3:.2f} ms  c={c:.1f}  "
              f"predict rho(0.03)={rho_of(.03, mu, j, c):.3f} (sim {chk[1]})")
        for w in (1, 2, 5, 10):
            r = best(mu, j, c, w)
            print(f"   w={w:2d}: rho*={r:.3f} delta*={delta_of(r, mu, j, c):.3f} "
                  f"meanRTT={D(r, mu, j, c)*1e3:.1f} ms")
