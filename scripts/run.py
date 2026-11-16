"""Run scratch/run-sweep.py with its output root redirected (repo results untouched)."""
import importlib.util, os, sys
ROOT = os.environ["NS3_ROOT"]
spec = importlib.util.spec_from_file_location("rs", os.path.join(ROOT, "scratch", "run-sweep.py"))
rs = importlib.util.module_from_spec(spec)
sys.argv = ["run-sweep.py"] + sys.argv[1:]
spec.loader.exec_module(rs)
rs.OUT = os.environ["SWEEP_OUT"]
rs.main()
