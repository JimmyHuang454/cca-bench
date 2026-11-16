"""Write the pgfplots data and the power table of the LeoCC replay.
usage: python3 leocc_pgfdata.py SUMMARY_JSON DAT_OUT TABLE_OUT"""
import json, sys
r = json.load(open(sys.argv[1]))
A = [("cubic", "CUBIC"), ("bbr", "BBRv1"), ("copa", "Copa"), ("copa+nosw", "Copa-NC"), ("copa-auto", "copa-auto"),
     ("copa-auto+nosw", "copa-auto-NC"), ("best", "最优 Copa-NC($\\delta$)")]
K = lambda c: c.replace("+", "").replace("-", "")
with open(sys.argv[2], "w") as f:
    f.write("x " + " ".join(K(c) + d[0] for d in ("downlink", "uplink") for c, _ in A) + "\n")
    for i, n in enumerate((1, 2, 4)):
        f.write(str(i) + " " + " ".join(f"{r[f'{d}|{n}|{c}']['power']:.4f}" for d in ("downlink", "uplink") for c, _ in A) + "\n")
rows = []
for c, lab in A:
    cells = []
    for d in ("downlink", "uplink"):
        for n in (1, 2, 4):
            top = max(round(r[f"{d}|{n}|{x}"]["power"], 2) for x, _ in A if x != "best")
            v = f"{r[f'{d}|{n}|{c}']['power']:.2f}"
            cells.append(f"\\bestval{{{v}}}" if c != "best" and round(r[f"{d}|{n}|{c}"]["power"], 2) == top else v)
    rows.append(f"{lab} & " + " & ".join(cells) + " \\\\")
open(sys.argv[3], "w").write("\n".join("\t\t" + x for x in rows) + "\n")
