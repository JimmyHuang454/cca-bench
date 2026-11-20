"""Capacity/loss schedule of the basic scenario (no handover) for tcp-capacity-trace --profile=file.

usage: python3 gen_schedule.py GROUP SEED OUT [SIGMA]   (GROUP: A = capacity varies, B = loss varies, C = both,
D = base RTT varies, E = capacity, loss and base RTT vary)
Lines: time_s rate_mbps loss [one-way bottleneck delay ms]; access links add 5 ms each way. Every 30 s a new segment (mean capacity, loss); within a
segment the capacity changes every 20 ms, log-normal around the segment mean (sigma: 0.1 by
default, near-wired; 0.4 for a wireless-like link).
"""
import math
import random
import sys

SEGMENTS = [(40, 0.0), (20, 0.01), (60, 0.005), (10, 0.05), (40, 0.02), (30, 0.10)]  # (Mbit/s, loss)
R0_MS = [50, 100, 20, 150, 30, 80]  # base RTT of each segment (groups D, E)
SEG_LEN, TICK, CONST_CAP = 30.0, 0.02, 40.0

group, seed, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
SIGMA = float(sys.argv[4]) if len(sys.argv) > 4 else 0.1
rng = random.Random(seed)
with open(out, "w") as f:
    f.write("# time_s rate_mbps loss  (group %s, seed %d)\n" % (group, seed))
    for k, (cap, loss) in enumerate(SEGMENTS):
        mean = cap if group in ("A", "C", "E") else CONST_CAP
        p = loss if group in ("B", "C", "E") else 0.0
        delay = R0_MS[k] / 2 - 5 if group in ("D", "E") else None
        for i in range(int(SEG_LEN / TICK)):
            t = k * SEG_LEN + i * TICK
            r = mean * math.exp(SIGMA * rng.gauss(0, 1) - SIGMA ** 2 / 2)
            f.write("%.3f %.4f %g" % (t, r, p) + ("" if delay is None else " %g" % delay) + "\n")
