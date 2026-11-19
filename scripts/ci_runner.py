#!/usr/bin/env python3
"""CI orchestration helper for Mahimahi Comparison GitHub Actions workflow.

Replaces embedded bash/YAML logic with testable Python subcommands:
  - plan          : compute GitHub Actions matrix JSON and effective mode
  - patch-mortise : patch Mortise BPF ABI (Linux >= 6.10) & CongestionOpt enum
  - run-quinn     : prepare workloads and invoke run_mahimahi_suite.py (--stack quinn)
  - run-kernel    : start Mortise daemons, prepare workloads, run --stack kernel
  - summarize     : run mahimahi_eval.py + mahimahi_rct.py and write step summary
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
import urllib.request

SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

EXTRA_CONGESTION_VARIANTS: list[tuple[str, str]] = [
    ("CopaAuto", "copa_auto"),
    ("CopaAutoNc", "copa_auto_nc"),
    ("CopaAutoT90", "copa_auto_t90"),
    ("CopaAutoT98", "copa_auto_t98"),
    ("CopaAutoT", "copa_auto_t"),
    ("CopaOrig", "copa_orig"),
    ("Leocc", "leocc"),
    ("Cdg", "cdg"),
    ("Veno", "veno"),
    ("Yeah", "yeah"),
    ("Illinois", "illinois"),
    ("Westwood", "westwood"),
    ("Hybla", "hybla"),
    ("Reno", "reno"),
]

WORKLOAD_MAP_ARG = (
    "app=/tmp/workloads/app.wk,"
    "websearch=/tmp/workloads/websearch.wk,"
    "vl2=/tmp/workloads/vl2.wk,"
    "pageload=/tmp/workloads/pageload.wk"
)


def parse_thetas(theta_raw: str) -> tuple[str, list[str]]:
    vals = [t.strip() for t in theta_raw.split(",") if t.strip()]
    if not vals:
        vals = ["0.98"]
    return vals[0], vals


def parse_queues_spec(spec: str) -> str:
    """Convert human-readable queue sizes like '150KB,2MB' or '2M' to comma-separated bytes."""
    units = {
        "": 1,
        "b": 1,
        "k": 1000,
        "kb": 1000,
        "m": 1000000,
        "mb": 1000000,
        "g": 1000000000,
        "gb": 1000000000,
    }
    out: list[str] = []
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        m = re.match(r"^([0-9]*\.?[0-9]+)\s*([a-zA-Z]*)$", tok)
        if not m:
            raise ValueError(f"Invalid queue size {tok!r}; expected e.g. 150KB, 2MB, or bytes")
        val = float(m.group(1))
        unit = m.group(2).lower()
        if unit not in units:
            raise ValueError(f"Unknown queue size unit {unit!r} in {tok!r}")
        out.append(str(int(round(val * units[unit]))))
    if not out:
        raise ValueError("Empty queue size specification")
    return ",".join(out)


def build_plan(mode: str, rct_workloads_raw: str) -> tuple[str, dict[str, list[dict[str, str]]]]:
    lumos_5g_shards = [f"lumos-5g-s{i}-of-16" for i in range(1, 17)]
    lumos_4g_shards = [f"lumos-4g-s{i}-of-18" for i in range(1, 19)]
    base_shards = [
        "lte", "nyc-stat", "nyc-walk-1", "nyc-walk-2", "nyc-walk-3",
        "nyc-drive-1", "nyc-drive-2", "nyc-subway", "synth", "leo",
    ]
    shards = {
        "quick": ["smoke"],
        "rct": ["smoke"],
        "rct-datasets": ["lte", "nyc-rct", "lumos-rct", "leo"],
        "lumos-5g-all": lumos_5g_shards,
        "lumos-4g-all": lumos_4g_shards,
        "lumos-all": lumos_5g_shards + lumos_4g_shards,
        "full": base_shards + lumos_5g_shards + lumos_4g_shards,
        "datasets": ["nyc-subway", "lumos-5g", "leo"],
    }.get(mode, [mode])

    rct_workloads = [w.strip() for w in rct_workloads_raw.split(",") if w.strip()] or ["app"]
    include: list[dict[str, str]] = []
    for shard in shards:
        if mode == "rct":
            workloads = ["app"]
        elif mode == "rct-datasets":
            workloads = rct_workloads
        elif mode == "full":
            workloads = ["bulk", *[w for w in rct_workloads if w != "bulk"]]
        else:
            workloads = ["bulk"] if mode == "datasets" or shard == "leo" else ["bulk", "app"]
        for workload in workloads:
            include.append({"shard": shard, "workload": workload})
    return mode, {"include": include}


def cmd_plan(_args: argparse.Namespace) -> int:
    mode = os.environ.get("MODE", "datasets").strip() or "datasets"
    rct_workloads = os.environ.get("RCT_WORKLOADS", "app,websearch,vl2,pageload")
    eff_mode, matrix = build_plan(mode, rct_workloads)
    lines = [
        f"matrix={json.dumps(matrix)}",
        f"mode={eff_mode}",
    ]
    gh_out = os.environ.get("GITHUB_OUTPUT")
    if gh_out:
        with open(gh_out, "a", encoding="utf-8") as f:
            for line in lines:
                f.write(line + "\n")
    for line in lines:
        print(line)
    return 0


def cmd_patch_mortise(args: argparse.Namespace) -> int:
    mortise_dir = pathlib.Path(args.mortise_dir)

    # 1. Patch BPF cong_control signature if kernel >= 6.10 (4-arg ABI)
    bpf_dir = mortise_dir / "algorithm" / "bpf-kern"
    vml_path = bpf_dir / "include" / "vmlinux.h"
    vml = vml_path.read_text(errors="ignore")
    m = re.search(r"struct\s+tcp_congestion_ops\s*\{(.*?)\};", vml, re.S)
    if not m:
        raise RuntimeError(f"struct tcp_congestion_ops not found in {vml_path}")
    for line in m.group(1).splitlines():
        if "cong_control" in line:
            print("Kernel cong_control signature:", line.strip())
    if re.search(r"\(\*cong_control\)\s*\([^)]*u32[^)]*int[^)]*rate_sample", m.group(1)):
        bpf_c = bpf_dir / "mortise_copa.bpf.c"
        src = bpf_c.read_text(encoding="utf-8")
        old = "BPF_PROG(bpf_copa_main, struct sock *sk, const struct rate_sample *rs)"
        new = "BPF_PROG(bpf_copa_main, struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)"
        if new not in src:
            if old not in src:
                raise RuntimeError("expected bpf_copa_main signature not found")
            bpf_c.write_text(src.replace(old, new, 1), encoding="utf-8")
            print("Patched bpf_copa_main for 4-arg cong_control ABI (Linux >= 6.10)")
    else:
        print("Keeping 2-arg bpf_copa_main ABI")

    # 2. Patch mortise-common/src/congestion.rs with copa_auto*, leocc, and mainline kernel CCs
    cong_rs = mortise_dir / "mortise" / "mortise-common" / "src" / "congestion.rs"
    src = cong_rs.read_text(encoding="utf-8")
    if "CopaAuto," not in src:
        enum_add = "".join(
            f'    #[value(name = "{name}")]\n    {var},\n'
            for var, name in EXTRA_CONGESTION_VARIANTS
        )
        disp_add = "".join(
            f'            CongestionOpt::{var} => write!(f, "{name}"),\n'
            for var, name in EXTRA_CONGESTION_VARIANTS
        )
        ca_add = "".join(
            f'            CongestionOpt::{var} => (0, b"{name}"),\n'
            for var, name in EXTRA_CONGESTION_VARIANTS
        )
        load_add = "".join(
            f'            CongestionOpt::{var} => ManagerOperation::Load {{ path: "".to_string(), option: None }},\n'
            for var, _ in EXTRA_CONGESTION_VARIANTS
        )

        if "    Bbr,\n}" not in src:
            raise RuntimeError("expected enum CongestionOpt end")
        src = src.replace("    Bbr,\n}", f"    Bbr,\n{enum_add}}}", 1)
        src = src.replace(
            'CongestionOpt::Bbr => write!(f, "bbr"),\n',
            f'CongestionOpt::Bbr => write!(f, "bbr"),\n{disp_add}',
            1,
        )
        src = src.replace(
            'CongestionOpt::Bbr => (0, b"bbr"),\n',
            f'CongestionOpt::Bbr => (0, b"bbr"),\n{ca_add}',
            1,
        )
        marker = (
            '            CongestionOpt::Vegas => ManagerOperation::Load {\n'
            '                path: "".to_string(),\n'
            '                option: None,\n'
            '            },\n'
        )
        if marker not in src:
            raise RuntimeError("expected get_load_option end")
        src = src.replace(marker, marker + load_add, 1)
        cong_rs.write_text(src, encoding="utf-8")
        print(f"Patched {cong_rs} with {len(EXTRA_CONGESTION_VARIANTS)} extra kernel CCs")
    return 0


def prepare_workloads(workloads_dir: pathlib.Path, bulk_lines: int, mortise_app_wk: pathlib.Path | None = None) -> None:
    workloads_dir.mkdir(parents=True, exist_ok=True)
    app_dst = workloads_dir / "app.wk"
    if mortise_app_wk and mortise_app_wk.is_file():
        shutil.copyfile(mortise_app_wk, app_dst)
    elif not app_dst.exists():
        url = "https://raw.githubusercontent.com/BobAnkh/Mortise/main/mortise/workload/app.wk"
        for attempt in range(3):
            try:
                urllib.request.urlretrieve(url, app_dst)
                break
            except Exception:
                if attempt == 2:
                    raise
                time.sleep(2)
    subprocess.run(
        [sys.executable, str(SCRIPTS_DIR / "gen_workloads.py"), "--out-dir", str(workloads_dir)],
        check=True,
    )
    (workloads_dir / "bulk.wk").write_text("0\t2097152\n" * bulk_lines, encoding="utf-8")


def auto_parallel(shard: str, cpus: int | None = None) -> int:
    """Choose safe concurrency from CPU count without introducing Mahimahi scheduling jitter."""
    n_cpu = cpus if cpus is not None else (os.cpu_count() or 4)
    high_rate = shard.startswith("lumos-5g") or shard in ("lumos-rct", "leo", "local-leo")
    if high_rate:
        return max(2, min(16, (n_cpu * 3) // 4))
    return max(2, min(24, (n_cpu * 3) // 2))


def resolve_shard_tuning(
    mode: str,
    shard: str,
    workload: str,
    parallel_override: str = "",
    queues_override: str = "",
    cpus: int | None = None,
) -> tuple[int, int, str, int, bool]:
    """Return (bulk_secs, app_secs, queues_bytes_csv, parallel, high_bdp_conn_win)."""
    bulk_secs = 30 if shard == "smoke" else 45
    app_secs = 0 if mode in ("rct-datasets", "full") else 300
    parallel = auto_parallel(shard, cpus=cpus)
    if parallel_override.strip():
        parallel = max(1, int(parallel_override.strip()))
    high_bdp = False

    # Dataset-specific default shallow / deep queues (aligned with each dataset's BDP / paper):
    #   LTE / NYC 4G / Synth / Lumos 4G : 150 KB, 2 MB
    #   Lumos 5G (100-800 Mbps)         : 2 MB, 16 MB
    #   LeoCC Starlink (official 50k p) : 2 MB, 75 MB
    if shard.startswith("lumos-5g") or shard == "lumos-rct":
        queues = "2MB,16MB"
        high_bdp = True
        if mode == "datasets" or (mode.startswith("lumos-") and workload == "bulk"):
            queues = "2MB"
        elif mode == "rct-datasets" or (mode.startswith("lumos-") and workload != "bulk"):
            queues = "16MB"
    elif shard.startswith("lumos-4g"):
        queues = "150KB,2MB" if mode == "full" else "2MB"
    elif shard == "leo":
        bulk_secs = 115
        queues = "2MB,75MB"
        high_bdp = True
        if mode in ("datasets", "rct-datasets"):
            queues = "2MB"
    else:
        queues = "150KB,2MB"
        if mode in ("quick", "datasets"):
            queues = "150KB"
        elif mode in ("rct", "rct-datasets"):
            queues = "2MB"

    if queues_override.strip():
        queues = queues_override.strip()

    return bulk_secs, app_secs, parse_queues_spec(queues), parallel, high_bdp


def resolve_quinn_ccs(mode: str, theta_raw: str, override: str) -> str:
    first_theta, theta_list = parse_thetas(theta_raw)
    if override.strip():
        return override.strip().replace("{theta}", first_theta)
    theta_ccs = [f"copa-auto-theta{tv}" for tv in theta_list]
    if mode in ("quick", "datasets"):
        ccs = ["copa", "copa-auto", "copa-auto-nc", f"copa-auto-theta{first_theta}"]
    elif mode == "rct":
        ccs = ["copa-auto", "copa-auto-theta0.9", "copa-auto-theta0.98"]
    elif mode == "rct-datasets":
        ccs = ["copa-auto", *theta_ccs, "copa-auto-nc", "copa", "bbr1", "bbr3", "cubic"]
    else:
        ccs = [
            "copa-auto", "copa-auto-nc", f"copa-auto-theta{first_theta}",
            f"copa-auto-nc-theta{first_theta}", "copa", "bbr1", "bbr3", "cubic",
        ]
    return ",".join(ccs)


def resolve_kernel_ccs(mode: str, shard: str, theta_raw: str, override: str, avail_ccs: set[str], copa_kmod: bool) -> str:
    first_theta, theta_list = parse_thetas(theta_raw)
    if override.strip():
        return override.strip().replace("{theta}", first_theta)
    theta_ccs = [f"copa_auto_theta{tv}" for tv in theta_list]
    if mode in ("quick", "datasets"):
        ccs = ["copa_auto", "copa_auto_nc", f"copa_auto_theta{first_theta}", "mortise_copa", 'bbr']
    elif mode == "rct":
        ccs = ["copa_auto", "copa_auto_theta0.9", "copa_auto_theta0.98", "copa_orig", "mortise_copa"]
    elif mode in ("rct-datasets", "full"):
        ccs = ["copa_auto", *theta_ccs, "copa_auto_nc", "copa_orig", "mortise_copa", "bbr", "cubic"]
        for extra in ("vegas", "cdg"):
            if extra in avail_ccs:
                ccs.append(extra)
    else:
        ccs = ["copa_auto", "copa_auto_nc", f"copa_auto_theta{first_theta}", "copa_orig", "mortise_copa", "bbr", "cubic"]
        if copa_kmod and "copa" in avail_ccs:
            ccs.append("copa")
    if shard == "leo" and "leocc" in avail_ccs and "leocc" not in ccs:
        ccs.append("leocc")
    return ",".join(ccs)


def read_available_kernel_ccs() -> set[str]:
    try:
        out = subprocess.check_output(
            ["sysctl", "-n", "net.ipv4.tcp_available_congestion_control"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        return set(out.split())
    except Exception:
        return {"cubic", "reno", "bbr"}


def cmd_run_quinn(args: argparse.Namespace) -> int:
    mode = os.environ["MODE"]
    theta = os.environ.get("THETA", "0.98")
    repeats = os.environ.get("REPEATS", "1")
    override = os.environ.get("QUINN_CCS", "")
    shard = os.environ["SHARD"]
    workload = os.environ["WORKLOAD"]
    send_fairness = os.environ.get("SEND_FAIRNESS", "false")
    stream_rw = os.environ.get("STREAM_RECEIVE_WINDOW", "8M")

    prepare_workloads(pathlib.Path("/tmp/workloads"), bulk_lines=200)
    bulk_secs, app_secs, queues, parallel, high_bdp = resolve_shard_tuning(
        mode, shard, workload, os.environ.get("PARALLEL", ""), os.environ.get("QUEUES", "")
    )
    ccs = resolve_quinn_ccs(mode, theta, override)

    env = os.environ.copy()
    if mode == "rct":
        env["RUST_LOG"] = "warn,quinn_proto::congestion::copa=debug"

    cmd = [
        sys.executable, str(SCRIPTS_DIR / "run_mahimahi_suite.py"),
        "--stack", "quinn",
        "--shard", shard,
        "--ccs", ccs,
        "--queues", queues,
        "--workloads", workload,
        "--bulk-secs", str(bulk_secs),
        "--app-secs", str(app_secs),
        "--repeats", repeats,
        "--send-fairness", send_fairness,
        "--parallel", str(parallel),
        "--bin", args.bin,
        "--trace-dir", "/tmp/traces",
        "--app-workload", "/tmp/workloads/app.wk",
        "--app-workloads", WORKLOAD_MAP_ARG,
        "--bulk-workload", "/tmp/workloads/bulk.wk",
        "--out-dir", args.out_dir,
    ]
    if high_bdp:
        cmd += ["--connection-window", "64M"]
    if stream_rw:
        cmd += ["--stream-receive-window", stream_rw]

    print("Running:", " ".join(cmd), flush=True)
    return subprocess.run(cmd, env=env).returncode


def cmd_run_kernel(args: argparse.Namespace) -> int:
    mode = os.environ["MODE"]
    theta = os.environ.get("THETA", "0.98")
    repeats = os.environ.get("REPEATS", "1")
    override = os.environ.get("KERNEL_CCS", "")
    shard = os.environ["SHARD"]
    workload = os.environ["WORKLOAD"]
    copa_kmod = os.environ.get("COPA_KMOD", "0") == "1"

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    mortise_dir = pathlib.Path(args.mortise_dir)
    mortise_root = mortise_dir / "mortise"

    prepare_workloads(
        pathlib.Path("/tmp/workloads"),
        bulk_lines=8192,
        mortise_app_wk=mortise_root / "workload" / "app.wk",
    )
    bulk_secs, app_secs, queues, parallel, _ = resolve_shard_tuning(
        mode, shard, workload, os.environ.get("PARALLEL", ""), os.environ.get("QUEUES", "")
    )

    py_log = (out_dir / "process_report.log").open("w", encoding="utf-8")
    mgr_log = (out_dir / "manager.log").open("w", encoding="utf-8")
    srv_log = (out_dir / "server.log").open("w", encoding="utf-8")
    procs: list[tuple[subprocess.Popen, bool]] = []

    try:
        py_env = os.environ.copy()
        py_env["LOG_LEVEL"] = "warn"
        py_proc = subprocess.Popen(
            ["/home/vagrant/mortise-venv/bin/python3", "process-report.py"],
            cwd=mortise_root, env=py_env, stdout=py_log, stderr=subprocess.STDOUT,
        )
        procs.append((py_proc, False))
        time.sleep(2)

        mgr_env = os.environ.copy()
        mgr_env["RUST_LOG"] = "info"
        mgr_proc = subprocess.Popen(
            ["sudo", "-E", "./target/release/manager"],
            cwd=mortise_root, env=mgr_env, stdout=mgr_log, stderr=subprocess.STDOUT,
        )
        procs.append((mgr_proc, True))
        for _ in range(20):
            if pathlib.Path("/tmp/mortise.sock").exists():
                break
            time.sleep(1)
        time.sleep(2)

        mgr_log.flush()
        mgr_text = (out_dir / "manager.log").read_text(encoding="utf-8", errors="ignore")
        print(mgr_text, flush=True)
        if "Fail to load" in mgr_text or "Load mortise_copa" not in mgr_text:
            tail = " ".join(mgr_text.strip().splitlines()[-4:])
            print(f"::error::mortise_copa BPF failed to load: {tail}", flush=True)
            return 1

        srv_proc = subprocess.Popen(
            ["./target/release/server"],
            cwd=mortise_root, stdout=srv_log, stderr=subprocess.STDOUT,
        )
        procs.append((srv_proc, False))
        time.sleep(2)

        avail_ccs = read_available_kernel_ccs()
        ccs = resolve_kernel_ccs(mode, shard, theta, override, avail_ccs, copa_kmod)
        cmd = [
            sys.executable, str(SCRIPTS_DIR / "run_mahimahi_suite.py"),
            "--stack", "kernel",
            "--shard", shard,
            "--ccs", ccs,
            "--queues", queues,
            "--workloads", workload,
            "--bulk-secs", str(bulk_secs),
            "--app-secs", str(app_secs),
            "--repeats", repeats,
            "--parallel", str(parallel),
            "--bin", str(mortise_root / "target" / "release" / "client"),
            "--trace-dir", "/tmp/traces",
            "--app-workload", "/tmp/workloads/app.wk",
            "--app-workloads", WORKLOAD_MAP_ARG,
            "--bulk-workload", "/tmp/workloads/bulk.wk",
            "--out-dir", str(out_dir),
        ]
        print("Running:", " ".join(cmd), flush=True)
        return subprocess.run(cmd, cwd=mortise_root).returncode
    finally:
        for proc, is_sudo in reversed(procs):
            if is_sudo:
                subprocess.run(["sudo", "kill", str(proc.pid)], stderr=subprocess.DEVNULL)
            else:
                proc.terminate()
        for f in (py_log, mgr_log, srv_log):
            f.close()


def cmd_summarize(args: argparse.Namespace) -> int:
    mode = os.environ.get("MODE", "")
    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    summary_md = out_dir / "comparison_summary.md"

    subprocess.run(
        [
            sys.executable, str(SCRIPTS_DIR / "mahimahi_eval.py"),
            "--input-dir", args.input_dir,
            "--out-csv", str(out_dir / "comparison_summary.csv"),
            "--out-md", str(summary_md),
            "--min-rtt-ms", "20.0",
            "--bulk-warmup-s", "5.0",
        ],
        check=True,
    )

    rct_cmd = [
        sys.executable, str(SCRIPTS_DIR / "mahimahi_rct.py"),
        "--input-dir", args.input_dir,
        "--out-dir", str(out_dir),
    ]
    if mode in ("rct", "rct-datasets", "full", "lumos-5g-all", "lumos-4g-all", "lumos-all"):
        rct_cmd.append("--print-summary")
    rct_out = subprocess.check_output(rct_cmd, text=True)

    combined = summary_md.read_text(encoding="utf-8") + "\n" + rct_out
    gh_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if gh_summary:
        with open(gh_summary, "a", encoding="utf-8") as f:
            f.write(combined)
    else:
        print(combined)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="CI helper for Mahimahi Comparison workflow.")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("plan")

    p_patch = sub.add_parser("patch-mortise")
    p_patch.add_argument("--mortise-dir", default="/tmp/Mortise")

    p_quinn = sub.add_parser("run-quinn")
    p_quinn.add_argument("--bin", default="./quinn-jls/target/release/quinn-perf")
    p_quinn.add_argument("--out-dir", default="/tmp/quinn_out")

    p_kern = sub.add_parser("run-kernel")
    p_kern.add_argument("--mortise-dir", default="/tmp/Mortise")
    p_kern.add_argument("--out-dir", default="/tmp/kernel_out")

    p_sum = sub.add_parser("summarize")
    p_sum.add_argument("--input-dir", default="/tmp/all_results")
    p_sum.add_argument("--out-dir", default="/tmp/summary")

    args = parser.parse_args()
    return {
        "plan": cmd_plan,
        "patch-mortise": cmd_patch_mortise,
        "run-quinn": cmd_run_quinn,
        "run-kernel": cmd_run_kernel,
        "summarize": cmd_summarize,
    }[args.cmd](args)


if __name__ == "__main__":
    raise SystemExit(main())
