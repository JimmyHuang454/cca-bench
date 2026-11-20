"""Write the pgfplots data and the power table of the Mahimahi replay.
usage: python3 mahimahi_pgfdata.py SUMMARY_JSON DAT_OUT TABLE_OUT"""
import json, sys
r = json.load(open(sys.argv[1]))
T = ["Verizon-LTE-driving", "TMobile-LTE-driving", "ATT-LTE-driving", "ATT-LTE-driving-2016"]
A = [("cubic", "CUBIC"), ("bbr", "BBRv1"), ("copa", "Copa"), ("copa+nosw", "Copa-NC"), ("copa-auto", "copa-auto"),
     ("copa-auto+nosw", "copa-auto-NC"), ("best", "最优 Copa-NC($\\delta$)")]
with open(sys.argv[2], "w") as f:
    f.write("x " + " ".join(f"p{c.replace('+', '').replace('-', '')}n{n}" for n in (1, 2, 4) for c, _ in A) + "\n")
    for i, t in enumerate(T):
        f.write(str(i) + " " + " ".join(f"{r[f'{t}|{n}|{c}']['power']:.4f}" for n in (1, 2, 4) for c, _ in A) + "\n")
rows = []
for n in (1, 2, 4):
    rows.append(f"\\multicolumn{{5}}{{@{{}}l}}{{{n} 条流}} \\\\")
    for c, lab in A:
        cells = []
        for t in T:
            top = max(round(r[f"{t}|{n}|{x}"]["power"], 2) for x, _ in A if x != "best")
            v = f"{r[f'{t}|{n}|{c}']['power']:.2f}"
            cells.append(f"\\bestval{{{v}}}" if c != "best" and round(r[f"{t}|{n}|{c}"]["power"], 2) == top else v)
        rows.append(f"{lab} & " + " & ".join(cells) + " \\\\")
    if n != 4:
        rows.append("\\midrule")
open(sys.argv[3], "w").write("\n".join("\t\t" + x for x in rows) + "\n")
