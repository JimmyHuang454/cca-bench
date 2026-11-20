"""Copy the final copa-auto runs over the sweep tree, which compare.py and pgfdata.py read.
usage: python3 sync_mobile.py MOBILE_DIR SWEEP_DIR"""
import os, shutil, sys
SRC, DST = os.path.join(sys.argv[1], "sw"), sys.argv[2]
n = 0
for root, _, files in os.walk(SRC):
    for f in files:
        if "copa-auto" in f:
            d = os.path.join(DST, os.path.relpath(root, SRC))
            os.makedirs(d, exist_ok=True)
            shutil.copy(os.path.join(root, f), d)
            n += 1
print(n, "files copied")
