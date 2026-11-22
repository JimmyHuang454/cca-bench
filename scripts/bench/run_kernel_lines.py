#!/usr/bin/env python3
"""Run and summarize Linux kernel TCP benchmarks over datasets/lines via Mahimahi + Mortise."""
from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import math
import os
import platform
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

_BENCH_DIR = Path(__file__).resolve().parent
_SCRIPTS_DIR = _BENCH_DIR.parent
_DATA_DIR = _SCRIPTS_DIR / 'data'
_EVAL_DIR = _SCRIPTS_DIR / 'eval'
for _p in (str(_BENCH_DIR), str(_DATA_DIR), str(_EVAL_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import eval_report  # noqa: E402
import lines_to_mahimahi  # noqa: E402
import trace_report  # noqa: E402
from lines_common import (  # noqa: E402
    DEFAULT_ONE_WAY_DELAY_MS,
    ROOT,
    atomic_write,
    digest,
    discover_dataset,
    workload as load_workload,
)

MM_LAUNCH_LOCK = threading.Lock()

MAINLINE_TCP_MODULES: dict[str, str] = {
    'bbr': 'tcp_bbr',
    'vegas': 'tcp_vegas',
    'westwood': 'tcp_westwood',
    'illinois': 'tcp_illinois',
    'veno': 'tcp_veno',
    'yeah': 'tcp_yeah',
    'hybla': 'tcp_hybla',
    'cdg': 'tcp_cdg',
    'cubic': 'tcp_cubic',
    'reno': 'tcp_reno',
}

KERNEL_CSV_EXTRA_FIELDS = ['dataset', 'queue_bytes', 'stack', 'retrans_pkts', 'retrans_ratio']


def parse_queues_spec(spec: str) -> list[str]:
    """Parse queue list like '500KB', '150KB,2MB', or 'infinite' into canonical tokens."""
    units = {
        '': 1, 'b': 1,
        'k': 1000, 'kb': 1000,
        'm': 1_000_000, 'mb': 1_000_000,
        'g': 1_000_000_000, 'gb': 1_000_000_000,
    }
    raw = spec.strip() or '500KB'
    out: list[str] = []
    for tok in raw.split(','):
        tok = tok.strip()
        if not tok:
            continue
        if tok.lower() in ('infinite', 'inf', 'unlimited'):
            token = 'infinite'
        else:
            m = re.match(r'^([0-9]*\.?[0-9]+)\s*([a-zA-Z]*)$', tok)
            if not m:
                raise ValueError(f'Invalid queue size {tok!r}; expected e.g. 500KB, 2MB, bytes, or infinite')
            val = float(m.group(1))
            unit = m.group(2).lower()
            if unit not in units or val <= 0:
                raise ValueError(f'Invalid queue size {tok!r}')
            token = str(int(round(val * units[unit])))
        if token not in out:
            out.append(token)
    if not out:
        raise ValueError('Empty queue size specification')
    return out


def format_queue_label(q: str) -> str:
    if q == 'infinite':
        return 'infinite'
    return eval_report.format_queue_size(q)


def parse_csv_list(raw: str, *, default: list[str] | None = None) -> list[str]:
    items = [tok.strip() for tok in raw.split(',') if tok.strip()]
    if not items:
        return list(default or [])
    dedup: list[str] = []
    for item in items:
        if item not in dedup:
            dedup.append(item)
    return dedup


def resolve_kernel_cc(cc: str) -> str:
    """Map suite CC label (e.g. copa, copa-auto-v2, copa_auto_theta0.98) to Linux kernel TCP CCA name."""
    norm = cc.strip().replace('-', '_')
    if norm == 'copa':
        return 'copa_orig'
    m = re.match(r'^copa_auto_theta([0-9]*\.?[0-9]+)$', norm)
    if m:
        theta_milli = int(round(float(m.group(1)) * 1000))
        if not (10 <= theta_milli <= 995):
            raise ValueError(f'theta out of [0.01, 0.995] in {cc!r}')
        if theta_milli == 500:
            return 'copa_auto'
        if theta_milli == 900:
            return 'copa_auto_t90'
        if theta_milli == 980:
            return 'copa_auto_t98'
        param = Path('/sys/module/tcp_copa_auto/parameters/theta_milli')
        if param.exists():
            try:
                param.write_text(f'{theta_milli}\n', encoding='utf-8')
            except PermissionError:
                subprocess.run(
                    ['sudo', 'tee', str(param)],
                    input=f'{theta_milli}\n',
                    text=True,
                    check=True,
                    stdout=subprocess.DEVNULL,
                )
        return 'copa_auto_t'
    return norm


def read_available_kernel_ccs() -> set[str]:
    try:
        out = subprocess.check_output(
            ['sysctl', '-n', 'net.ipv4.tcp_available_congestion_control'],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        return set(out.split())
    except Exception:
        return {'cubic', 'reno'}


def ensure_kernel_ccs_loaded(ccs: list[str]) -> set[str]:
    avail = read_available_kernel_ccs()
    for cc in ccs:
        kcc = resolve_kernel_cc(cc)
        if kcc in avail:
            continue
        mod = MAINLINE_TCP_MODULES.get(kcc)
        if mod:
            subprocess.run(['sudo', 'modprobe', mod], stderr=subprocess.DEVNULL)
    return read_available_kernel_ccs()


def collect_host_sysctls() -> dict[str, str]:
    keys = (
        'net.ipv4.tcp_available_congestion_control',
        'net.ipv4.tcp_congestion_control',
        'net.ipv4.tcp_slow_start_after_idle',
        'net.ipv4.tcp_rmem',
        'net.ipv4.tcp_wmem',
        'net.core.default_qdisc',
    )
    info: dict[str, str] = {'kernel': platform.release(), 'platform': platform.platform()}
    for key in keys:
        try:
            info[key] = subprocess.check_output(['sysctl', '-n', key], text=True, stderr=subprocess.DEVNULL).strip()
        except Exception:
            pass
    return info


def build_lines_plan(
    *,
    dataset: str,
    scenarios_filter: list[str] | None,
    workloads: list[str],
    ccs: list[str],
    queues: list[str],
    repeats: int,
    max_bulk_s: float,
    workload_timeout_s: float,
    bulk_warmup_s: float,
    max_shards: int,
    root: Path = ROOT,
) -> tuple[dict, dict]:
    """Compute balanced GitHub Actions matrix shards and full plan metadata."""
    if repeats < 1:
        raise ValueError('repeats must be >= 1')
    if max_shards < 1:
        raise ValueError('max_shards must be >= 1')
    if not ccs:
        raise ValueError('kernel_ccs must not be empty')
    if not workloads:
        raise ValueError('workloads must not be empty')

    scenarios = discover_dataset(dataset, scenarios_filter, root=root, fast_duration=True)
    work_info: dict[str, dict] = {}
    for wk in workloads:
        if wk == 'bulk':
            continue
        if not wk.replace('_', '').replace('-', '').isalnum():
            raise ValueError(f'invalid workload name: {wk}')
        wk_path = root / 'datasets' / 'workloads' / f'{wk}.wk'
        if not wk_path.is_file():
            raise ValueError(f'workload file not found: {wk_path}')
        count, span_s = load_workload(wk_path)
        total_bytes = sum(
            int(line.split()[1])
            for line in wk_path.read_text(encoding='utf-8').splitlines()
            if line.strip() and not line.strip().startswith('#')
        )
        work_info[wk] = {
            'path': str(wk_path.relative_to(root)),
            'count': count,
            'span_s': span_s,
            'total_bytes': total_bytes,
        }

    multiplier = len(ccs) * len(queues) * repeats
    units: list[dict] = []
    for sc in scenarios:
        eff_bulk_s = min(sc['duration_s'], max_bulk_s) if max_bulk_s > 0 else sc['duration_s']
        for wk in workloads:
            if wk == 'bulk':
                est_s = max(eff_bulk_s, 10.0) * multiplier
            else:
                span_s = work_info[wk]['span_s']
                base_est = max(span_s * 1.5, 45.0)
                if sc['name'] == 'slow_uplink':
                    base_est = max(base_est, work_info[wk]['total_bytes'] * 8.0 / 1e6 * 1.1)
                est_s = min(workload_timeout_s, base_est) * multiplier
            units.append({
                'dataset': sc.get('dataset', dataset),
                'scenario': sc['name'],
                'workload': wk,
                'est_s': round(est_s, 1),
            })

    # Greedy LPT bin packing into <= max_shards shards.
    n_shards = min(max_shards, len(units))
    bins: list[list[dict]] = [[] for _ in range(n_shards)]
    loads = [0.0] * n_shards
    for unit in sorted(units, key=lambda u: (-u['est_s'], u['scenario'], u['workload'])):
        idx = min(range(n_shards), key=lambda i: loads[i])
        bins[idx].append(unit)
        loads[idx] += unit['est_s']

    # Keep deterministic ordering within each shard.
    include: list[dict[str, str]] = []
    for idx, bucket in enumerate(bins, 1):
        bucket.sort(key=lambda u: (u['dataset'], u['scenario'], workloads.index(u['workload'])))
        shard_id = f's{idx}-of-{n_shards}'
        uniq_scenes = list(dict.fromkeys(u['scenario'] for u in bucket))
        uniq_wks = list(dict.fromkeys(u['workload'] for u in bucket))
        scene_desc = uniq_scenes[0] if len(uniq_scenes) == 1 else f'{len(uniq_scenes)} scenes'
        label = f'{shard_id}: {scene_desc} ({",".join(uniq_wks)})'
        include.append({
            'shard_id': shard_id,
            'label': label,
            'units_json': json.dumps(
                [{'dataset': u['dataset'], 'scenario': u['scenario'], 'workload': u['workload']} for u in bucket],
                separators=(',', ':'),
            ),
        })

    plan_meta = {
        'dataset': dataset,
        'scenarios': [
            {
                'name': s['name'],
                'dataset': s.get('dataset', dataset),
                'duration_s': (min(s['duration_s'], max_bulk_s) if max_bulk_s > 0 else s['duration_s']),
                'period_ms': s['period_ms'],
                'has_up_delay': 'up_delay' in s['files'],
                'has_down_delay': 'down_delay' in s['files'],
            }
            for s in scenarios
        ],
        'workloads': workloads,
        'work_info': work_info,
        'ccs': ccs,
        'queues': queues,
        'repeats': repeats,
        'max_bulk_s': max_bulk_s,
        'workload_timeout_s': workload_timeout_s,
        'bulk_warmup_s': bulk_warmup_s,
        'needs_leocc': any(c.replace('-', '_') == 'leocc' for c in ccs),
    }
    return {'include': include}, plan_meta


def generate_bulk_workload(path: Path, max_bytes: int) -> None:
    """Create a Mortise .wk file with enough 2 MiB chunks to saturate the entire bulk run.

    Mortise's tokio LengthDelimitedCodec uses the default 8 MiB max_frame_length, so
    each request chunk must stay well below 8 MiB (2 MiB matches upstream Mortise).
    """
    chunk_bytes = 2 * 1024 * 1024
    min_bytes = max(max_bytes, 4 * 1024**3)
    n_lines = max(2048, math.ceil(min_bytes / chunk_bytes))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f'0\t{chunk_bytes}\n' * n_lines, encoding='utf-8')


def build_mahimahi_command(
    conv: lines_to_mahimahi.MahimahiScenario,
    queue: str,
    log_path: Path,
    inner_cmd: str,
) -> list[str]:
    """Build mm-delay + mm-link command with explicit lines->Mahimahi direction mapping."""
    cmd = [
        'mm-delay',
        str(conv.delay_interval_ms),
        str(conv.mm_uplink_delay),
        '--downlink-delay',
        str(conv.mm_downlink_delay),
        'mm-link',
        str(conv.mm_uplink_trace),
        str(conv.mm_downlink_trace),
    ]
    if queue != 'infinite':
        cmd.extend([
            '--uplink-queue=droptail', f'--uplink-queue-args=bytes={queue}',
            '--downlink-queue=droptail', f'--downlink-queue-args=bytes={queue}',
        ])
    cmd.extend([f'--downlink-log={log_path}', '--', 'bash', '-c', inner_cmd])
    return cmd


def has_deliveries(path: Path) -> bool:
    if not path.is_file():
        return False
    with path.open(encoding='utf-8', errors='ignore') as stream:
        return any(line.split(maxsplit=2)[1:2] == ['-'] for line in stream if not line.startswith('#'))


def run_mm_cmd(
    mm_cmd: list[str],
    expected_out: Path,
    link_log: Path,
    *,
    allow_timeout: bool = False,
) -> subprocess.CompletedProcess[str]:
    """Launch mm-delay/mm-link with staggered startup and 1 retry on transient setup failure."""
    res: subprocess.CompletedProcess[str] | None = None
    for attempt in range(2):
        with MM_LAUNCH_LOCK:
            proc = subprocess.Popen(mm_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(1.5)
        stdout, stderr = proc.communicate()
        res = subprocess.CompletedProcess(mm_cmd, proc.returncode, stdout, stderr)
        ok_code = res.returncode == 0 or (allow_timeout and res.returncode in (124, 130, 137, 143))
        if ok_code and expected_out.exists() and expected_out.stat().st_size > 0:
            return res
        if link_log.exists() and link_log.stat().st_size > 0:
            break
        if attempt == 0:
            time.sleep(2.0)
    assert res is not None
    return res


def rct_wall_duration_s(rct_path: Path) -> float | None:
    if not rct_path.is_file():
        return None
    sends: list[int] = []
    recvs: list[int] = []
    with rct_path.open('r', encoding='utf-8', errors='ignore') as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header:
            return None
        for row in reader:
            if len(row) < 6 or row[0] == '0':
                continue
            try:
                sends.append(int(float(row[2])))
                recvs.append(int(float(row[4])))
            except ValueError:
                continue
    if not sends or not recvs:
        return None
    span_ns = max(recvs) - min(sends)
    return max(0.0, span_ns / 1e9) if span_ns >= 0 else None


def summarize_process_error(stdout: str, stderr: str) -> str:
    lines = [l.strip() for l in (stdout + '\n' + stderr).splitlines() if l.strip()]
    useful = [l for l in lines if 'Died on std::runtime_error:' not in l]
    chosen = useful[-4:] if useful else lines[-4:]
    return ' | '.join(chosen)[-500:]


def evaluate_single_run(
    *,
    conv: lines_to_mahimahi.MahimahiScenario,
    dataset_name: str,
    workload_name: str,
    cc: str,
    queue: str,
    repeat: int,
    client_bin: Path,
    bulk_wk: Path,
    work_info: dict[str, dict],
    out_dir: Path,
    workload_timeout_s: float,
    bulk_warmup_s: float,
    keep_logs: bool,
) -> dict:
    kernel_cc = resolve_kernel_cc(cc)
    stem = f'{conv.name}__q{queue}__{workload_name}__{cc}__r{repeat}'
    log_path = out_dir / f'{stem}.downlink.log'
    rct_path = out_dir / f'{stem}.rct.csv'
    err_path = out_dir / f'{stem}.err.log'
    timeout_marker = out_dir / f'{stem}.timeout'
    timeout_marker.unlink(missing_ok=True)

    row: dict = {
        'dataset': dataset_name,
        'scenario': conv.name,
        'queue_bytes': queue,
        'stack': 'kernel',
        'workload': workload_name,
        'cc': cc,
        'repeat': repeat,
        'status': 'failed',
        'error': '',
        'duration_s': conv.bulk_duration_s if workload_name == 'bulk' else '',
    }

    started = time.monotonic()
    try:
        if workload_name == 'bulk':
            bulk_secs = max(1, int(math.ceil(conv.bulk_duration_s)))
            client_cmd = (
                f'timeout -k 5 {bulk_secs} "{client_bin}" --connect "${{MAHIMAHI_BASE}}" '
                f'--congestion "{kernel_cc}" --workload "{bulk_wk}" --output "{rct_path}"; '
                'status=$?; if [ "$status" -eq 124 ] || [ "$status" -eq 130 ] || '
                '[ "$status" -eq 137 ] || [ "$status" -eq 143 ]; then exit 0; fi; exit "$status"'
            )
            mm_cmd = build_mahimahi_command(conv, queue, log_path, client_cmd)
            res = run_mm_cmd(mm_cmd, log_path, log_path, allow_timeout=True)
            ok_code = res.returncode in (0, 124, 130, 137, 143)
            if not ok_code or not log_path.is_file() or not has_deliveries(log_path):
                detail = summarize_process_error(res.stdout, res.stderr)
                err_path.write_text(res.stdout + '\n' + res.stderr, encoding='utf-8')
                row['error'] = f'exit {res.returncode}: {detail or "no packet deliveries in downlink.log"}'
                return row

            stats = eval_report.parse_downlink_log(
                log_path,
                min_rtt_ms=conv.base_rtt_ms_bulk,
                warmup_s=bulk_warmup_s,
                max_s=conv.bulk_duration_s,
            )
            row.update({
                'duration_s': stats['duration_s'],
                'capacity_mbps': stats['capacity_mbps'],
                'throughput_mbps': stats['throughput_mbps'],
                'utilization': stats['utilization'],
                'qdelay_avg_ms': stats['qdelay_avg_ms'],
                'qdelay_p95_ms': stats['qdelay_p95_ms'],
                'base_rtt_ms': conv.base_rtt_ms_bulk,
                'est_rtt_ms': stats['est_rtt_ms'],
                'util_power': stats['util_power'],
                'norm_power': stats['norm_power'],
                'drop_rate': stats['drop_rate'],
            })
            if math.isnan(stats['throughput_mbps']) or math.isnan(stats['norm_power']):
                row['error'] = 'invalid bulk metrics parsed from downlink.log'
                return row
            row['status'] = 'success'
            return row

        # Request/object workload (app, websearch, pageload, vl2, ...)
        wk_meta = work_info[workload_name]
        wk_path = Path(wk_meta['path'])
        expected_reqs = int(wk_meta['count'])
        row['requests_expected'] = expected_reqs
        timeout_secs = max(10, int(math.ceil(workload_timeout_s)))

        # Mahimahi packetshell turns any non-zero child exit code (including timeout's 124)
        # into exit status 1. Record timeout explicitly via a marker file so we can
        # cleanly distinguish genuine timeouts from client crashes.
        client_cmd = (
            f'timeout -s INT -k 10 {timeout_secs} "{client_bin}" --connect "${{MAHIMAHI_BASE}}" '
            f'--congestion "{kernel_cc}" --workload "{wk_path}" --output "{rct_path}"; '
            'status=$?; if [ "$status" -eq 124 ] || [ "$status" -eq 130 ] || '
            '[ "$status" -eq 137 ] || [ "$status" -eq 143 ]; then '
            f'touch "{timeout_marker}"; exit 0; fi; exit "$status"'
        )
        mm_cmd = build_mahimahi_command(conv, queue, log_path, client_cmd)
        res = run_mm_cmd(mm_cmd, rct_path, log_path, allow_timeout=True)
        elapsed_s = time.monotonic() - started
        timed_out = timeout_marker.is_file() or res.returncode in (124, 130, 137, 143)

        link_stats = (
            eval_report.parse_downlink_log(log_path, min_rtt_ms=conv.base_rtt_ms_period, warmup_s=0.0)
            if log_path.is_file()
            else {}
        )
        cap_mbps = link_stats.get('capacity_mbps', math.nan)
        if not isinstance(cap_mbps, (int, float)) or math.isnan(cap_mbps) or cap_mbps <= 0:
            cap_mbps = conv.data_capacity_mbps_period

        rct_stats = eval_report.parse_rct_csv(
            rct_path,
            delivered_bytes=link_stats.get('delivered_bytes', 0),
            base_rtt_ms=conv.base_rtt_ms_period,
            capacity_mbps=cap_mbps,
        )
        completed = int(rct_stats.get('requests', 0))
        row['requests'] = completed
        row['capacity_mbps'] = cap_mbps
        row['base_rtt_ms'] = conv.base_rtt_ms_period
        if 'drop_rate' in link_stats:
            row['drop_rate'] = link_stats['drop_rate']
        if 'retrans_pkts' in rct_stats:
            row['retrans_pkts'] = rct_stats['retrans_pkts']
        if 'retrans_ratio' in rct_stats:
            row['retrans_ratio'] = rct_stats['retrans_ratio']

        if timed_out or (res.returncode == 0 and completed < expected_reqs):
            row['status'] = 'timeout'
            row['duration_s'] = elapsed_s
            row['error'] = (
                f'达到最大运行时间 {workload_timeout_s:g} s，数据块完成 {completed}/{expected_reqs}，计 0 分'
            )
            for field in ('rct_mean_ms', 'rct_p50_ms', 'rct_p95_ms', 'fct_slowdown', 'mortise_qoe'):
                row[field] = ''
            return row

        if res.returncode != 0 or completed != expected_reqs:
            detail = summarize_process_error(res.stdout, res.stderr)
            err_path.write_text(res.stdout + '\n' + res.stderr, encoding='utf-8')
            row['error'] = f'exit {res.returncode} (completed {completed}/{expected_reqs}): {detail}'
            return row

        wall_s = rct_wall_duration_s(rct_path)
        row.update({
            'duration_s': wall_s if wall_s is not None else elapsed_s,
            'throughput_mbps': link_stats.get('throughput_mbps', ''),
            'utilization': link_stats.get('utilization', ''),
            'qdelay_avg_ms': link_stats.get('qdelay_avg_ms', ''),
            'qdelay_p95_ms': link_stats.get('qdelay_p95_ms', ''),
            'est_rtt_ms': link_stats.get('est_rtt_ms', ''),
            'rct_mean_ms': rct_stats.get('rct_mean_ms', ''),
            'rct_p50_ms': rct_stats.get('rct_p50_ms', ''),
            'rct_p95_ms': rct_stats.get('rct_p95_ms', ''),
            'fct_slowdown': rct_stats.get('fct_slowdown', ''),
            'mortise_qoe': rct_stats.get('mortise_qoe', ''),
            'status': 'success',
        })
        return row
    finally:
        timeout_marker.unlink(missing_ok=True)
        if not keep_logs and log_path.is_file():
            log_path.unlink(missing_ok=True)
        if not keep_logs and workload_name == 'bulk' and rct_path.is_file():
            rct_path.unlink(missing_ok=True)


def start_mortise_daemons(mortise_root: Path, out_dir: Path) -> tuple[list[tuple[subprocess.Popen, bool]], list]:
    py_log = (out_dir / 'process_report.log').open('w', encoding='utf-8')
    mgr_log = (out_dir / 'manager.log').open('w', encoding='utf-8')
    srv_log = (out_dir / 'server.log').open('w', encoding='utf-8')
    logs = [py_log, mgr_log, srv_log]
    procs: list[tuple[subprocess.Popen, bool]] = []

    py_env = os.environ.copy()
    py_env['LOG_LEVEL'] = 'warn'
    py_proc = subprocess.Popen(
        [os.environ.get('MORTISE_PYTHON', '/home/vagrant/mortise-venv/bin/python3'), 'process-report.py'],
        cwd=mortise_root,
        env=py_env,
        stdout=py_log,
        stderr=subprocess.STDOUT,
    )
    procs.append((py_proc, False))
    for _ in range(30):
        if Path('/tmp/mortise-py.sock').exists():
            break
        if py_proc.poll() is not None:
            raise RuntimeError('Mortise process-report.py exited; inspect process_report.log')
        time.sleep(1)
    else:
        raise RuntimeError('Mortise Python socket was not ready; inspect process_report.log')

    mgr_env = os.environ.copy()
    mgr_env['RUST_LOG'] = 'info'
    mgr_proc = subprocess.Popen(
        ['sudo', '-E', './target/release/manager'],
        cwd=mortise_root,
        env=mgr_env,
        stdout=mgr_log,
        stderr=subprocess.STDOUT,
    )
    procs.append((mgr_proc, True))
    for _ in range(20):
        if Path('/tmp/mortise.sock').exists():
            break
        time.sleep(1)
    time.sleep(2)

    mgr_log.flush()
    mgr_text = (out_dir / 'manager.log').read_text(encoding='utf-8', errors='ignore')
    print(mgr_text, flush=True)
    if 'Fail to load' in mgr_text or 'Load mortise_copa' not in mgr_text:
        tail = ' '.join(mgr_text.strip().splitlines()[-4:])
        raise RuntimeError(f'mortise_copa BPF failed to load: {tail}')

    srv_proc = subprocess.Popen(
        ['./target/release/server'],
        cwd=mortise_root,
        stdout=srv_log,
        stderr=subprocess.STDOUT,
    )
    procs.append((srv_proc, False))
    time.sleep(2)
    return procs, logs


def stop_mortise_daemons(procs: list[tuple[subprocess.Popen, bool]], logs: list) -> None:
    for proc, is_sudo in reversed(procs):
        if is_sudo:
            subprocess.run(['sudo', 'kill', str(proc.pid)], stderr=subprocess.DEVNULL)
        else:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    for handle in logs:
        handle.close()


def cmd_plan(args: argparse.Namespace) -> int:
    dataset = (args.dataset or os.environ.get('DATASET', 'basic')).strip() or 'basic'
    scenarios_raw = args.scenarios if args.scenarios is not None else os.environ.get('SCENARIOS', '')
    workloads_raw = args.workloads or os.environ.get('WORKLOADS', 'bulk,app')
    ccs_raw = args.ccs or os.environ.get('KERNEL_CCS', 'copa_auto_v2,copa_auto_nc,mortise_copa,bbr,cubic')
    queues_raw = args.queues if args.queues is not None else os.environ.get('QUEUES', '500KB')
    repeats = int(args.repeats or os.environ.get('REPEATS', '1') or '1')
    max_bulk_s = float(args.max_bulk_s if args.max_bulk_s is not None else (os.environ.get('MAX_BULK_S', '0') or '0'))
    workload_timeout_s = float(
        args.workload_timeout_s if args.workload_timeout_s is not None
        else (os.environ.get('WORKLOAD_TIMEOUT_S', '600') or '600')
    )
    bulk_warmup_s = float(
        args.bulk_warmup_s if args.bulk_warmup_s is not None else (os.environ.get('BULK_WARMUP_S', '0') or '0')
    )
    max_shards = int(args.max_shards or os.environ.get('MAX_SHARDS', '8') or '8')

    scenarios_filter = parse_csv_list(scenarios_raw) if scenarios_raw.strip() else None
    workloads = parse_csv_list(workloads_raw, default=['bulk'])
    ccs = parse_csv_list(ccs_raw)
    queues = parse_queues_spec(queues_raw)

    matrix, plan_meta = build_lines_plan(
        dataset=dataset,
        scenarios_filter=scenarios_filter,
        workloads=workloads,
        ccs=ccs,
        queues=queues,
        repeats=repeats,
        max_bulk_s=max_bulk_s,
        workload_timeout_s=workload_timeout_s,
        bulk_warmup_s=bulk_warmup_s,
        max_shards=max_shards,
    )

    outputs = {
        'matrix': json.dumps(matrix, separators=(',', ':')),
        'plan_json': json.dumps(plan_meta, separators=(',', ':')),
        'needs_leocc': 'true' if plan_meta['needs_leocc'] else 'false',
    }
    gh_out = os.environ.get('GITHUB_OUTPUT')
    if gh_out:
        with open(gh_out, 'a', encoding='utf-8') as f:
            for key, val in outputs.items():
                f.write(f'{key}={val}\n')
    for key, val in outputs.items():
        print(f'{key}={val}')
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    units_raw = args.units_json or os.environ.get('UNITS_JSON', '')
    if not units_raw.strip():
        raise ValueError('UNITS_JSON or --units-json is required')
    units: list[dict] = json.loads(units_raw)
    ccs = parse_csv_list(args.ccs or os.environ.get('KERNEL_CCS', 'copa_auto_v2,copa_auto_nc,mortise_copa,bbr,cubic'))
    queues = parse_queues_spec(args.queues if args.queues is not None else os.environ.get('QUEUES', '500KB'))
    repeats = int(args.repeats or os.environ.get('REPEATS', '1') or '1')
    parallel = max(1, int(args.parallel or os.environ.get('PARALLEL', '1') or '1'))
    max_bulk_s = float(args.max_bulk_s if args.max_bulk_s is not None else (os.environ.get('MAX_BULK_S', '0') or '0'))
    workload_timeout_s = float(
        args.workload_timeout_s if args.workload_timeout_s is not None
        else (os.environ.get('WORKLOAD_TIMEOUT_S', '600') or '600')
    )
    bulk_warmup_s = float(
        args.bulk_warmup_s if args.bulk_warmup_s is not None else (os.environ.get('BULK_WARMUP_S', '0') or '0')
    )
    keep_logs = args.keep_logs or os.environ.get('KEEP_LOGS', '0') == '1'

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    mortise_root = Path(args.mortise_dir) / 'mortise'
    client_bin = mortise_root / 'target' / 'release' / 'client'

    # Discover and convert unique scenarios for this shard.
    by_dataset: dict[str, list[str]] = {}
    for unit in units:
        by_dataset.setdefault(unit['dataset'], [])
        if unit['scenario'] not in by_dataset[unit['dataset']]:
            by_dataset[unit['dataset']].append(unit['scenario'])

    scenario_objs: dict[tuple[str, str], dict] = {}
    for ds, names in by_dataset.items():
        for sc in discover_dataset(ds, names):
            scenario_objs[(ds, sc['name'])] = sc

    work_info: dict[str, dict] = {}
    for wk in sorted({u['workload'] for u in units if u['workload'] != 'bulk'}):
        wk_path = ROOT / 'datasets' / 'workloads' / f'{wk}.wk'
        count, span_s = load_workload(wk_path)
        work_info[wk] = {'path': str(wk_path), 'count': count, 'span_s': span_s, 'sha256': digest(wk_path)}

    with tempfile.TemporaryDirectory(prefix='mahimahi-lines-') as tmp_root:
        tmp_path = Path(tmp_root)
        converted: dict[tuple[str, str], lines_to_mahimahi.MahimahiScenario] = {}
        max_bulk_bytes = 4 * 1024**3
        for key, sc in scenario_objs.items():
            conv = lines_to_mahimahi.convert_scenario(
                sc,
                tmp_path / 'traces' / key[0],
                default_one_way_delay_ms=DEFAULT_ONE_WAY_DELAY_MS,
                max_bulk_s=max_bulk_s,
            )
            converted[key] = conv
            est_bytes = int(conv.data_capacity_mbps_bulk * 1e6 / 8.0 * conv.bulk_duration_s * 2.5)
            max_bulk_bytes = max(max_bulk_bytes, est_bytes)

        bulk_wk = tmp_path / 'bulk.wk'
        generate_bulk_workload(bulk_wk, max_bulk_bytes)

        procs: list[tuple[subprocess.Popen, bool]] = []
        logs: list = []
        try:
            procs, logs = start_mortise_daemons(mortise_root, out_dir)
            avail_ccs = ensure_kernel_ccs_loaded(ccs)
            print(f'Kernel congestion controls available: {sorted(avail_ccs)}', flush=True)
            unavailable = [
                cc for cc in ccs
                if resolve_kernel_cc(cc) not in avail_ccs
            ]
            if unavailable:
                raise RuntimeError(f'Requested kernel congestion controls unavailable: {unavailable}')

            jobs = []
            for unit in units:
                key = (unit['dataset'], unit['scenario'])
                conv = converted[key]
                for queue in queues:
                    for cc in ccs:
                        for rep in range(1, repeats + 1):
                            jobs.append((conv, unit['dataset'], unit['workload'], cc, queue, rep))

            print(
                f'[kernel-lines] Running {len(jobs)} jobs '
                f'(units={len(units)}, ccs={len(ccs)}, queues={len(queues)}, repeats={repeats}, parallel={parallel})',
                flush=True,
            )

            rows: list[dict] = [{} for _ in jobs]

            def run_one(idx: int, item: tuple) -> tuple[int, dict]:
                conv, ds_name, wk, cc, queue, rep = item
                t0 = time.monotonic()
                row = evaluate_single_run(
                    conv=conv,
                    dataset_name=ds_name,
                    workload_name=wk,
                    cc=cc,
                    queue=queue,
                    repeat=rep,
                    client_bin=client_bin,
                    bulk_wk=bulk_wk,
                    work_info=work_info,
                    out_dir=out_dir,
                    workload_timeout_s=workload_timeout_s,
                    bulk_warmup_s=bulk_warmup_s,
                    keep_logs=keep_logs,
                )
                dt = time.monotonic() - t0
                tag = 'OK' if row['status'] == 'success' else row['status'].upper()
                extra = (
                    f"tput={row.get('throughput_mbps', '-')}Mbps norm_power={row.get('norm_power', '-')}"
                    if wk == 'bulk' and row['status'] == 'success'
                    else f"slowdown={row.get('fct_slowdown', '-')}x"
                    if row['status'] == 'success'
                    else row.get('error', '')
                )
                print(
                    f'[{tag}] {conv.name} | q={format_queue_label(queue)} | {wk} | {cc} | r{rep} | {dt:.1f}s | {extra}',
                    flush=True,
                )
                return idx, row

            with concurrent.futures.ThreadPoolExecutor(max_workers=parallel) as pool:
                futures = [pool.submit(run_one, idx, item) for idx, item in enumerate(jobs)]
                for fut in concurrent.futures.as_completed(futures):
                    idx, row = fut.result()
                    rows[idx] = row
        finally:
            stop_mortise_daemons(procs, logs)

    trace_report.write_runs_csv(out_dir / 'shard_runs.csv', rows, extra_fields=KERNEL_CSV_EXTRA_FIELDS)
    meta = {
        'host': collect_host_sysctls(),
        'units': units,
        'ccs': ccs,
        'queues': queues,
        'repeats': repeats,
        'parallel': parallel,
        'max_bulk_s': max_bulk_s,
        'workload_timeout_s': workload_timeout_s,
        'bulk_warmup_s': bulk_warmup_s,
        'direction_mapping': 'lines/up.* (TCP data) -> Mahimahi downlink (server->client); lines/down.* (TCP ACK) -> Mahimahi uplink (client->server)',
    }
    atomic_write(out_dir / 'shard_metadata.json', json.dumps(meta, indent=2) + '\n')

    failed = [r for r in rows if r['status'] == 'failed']
    print(f'[kernel-lines] Shard complete: {len(rows) - len(failed)}/{len(rows)} non-error ({len(failed)} failed)', flush=True)
    return 0 if not failed else 1


def load_shard_rows(input_dir: Path) -> list[dict]:
    rows: list[dict] = []
    seen: set[tuple] = set()
    for csv_path in sorted(input_dir.rglob('shard_runs.csv')):
        with csv_path.open('r', newline='', encoding='utf-8') as f:
            for row in csv.DictReader(f):
                key = (
                    row.get('dataset', ''),
                    row.get('scenario', ''),
                    row.get('queue_bytes', ''),
                    row.get('workload', ''),
                    row.get('cc', ''),
                    str(row.get('repeat', '1')),
                )
                if key in seen:
                    continue
                seen.add(key)
                rows.append(row)
    return rows


def summarize_results(
    input_dir: Path,
    out_dir: Path,
    *,
    plan_meta: dict | None = None,
) -> tuple[Path, Path, str]:
    rows = load_shard_rows(input_dir)
    if not rows:
        raise ValueError(f'No shard_runs.csv files with rows found under {input_dir}')

    shard_metas: list[dict] = []
    for meta_path in sorted(input_dir.rglob('shard_metadata.json')):
        try:
            shard_metas.append(json.loads(meta_path.read_text(encoding='utf-8')))
        except (OSError, ValueError):
            pass

    if plan_meta:
        ccs = list(plan_meta['ccs'])
        workloads = list(plan_meta['workloads'])
        queues = list(plan_meta['queues'])
        repeats = int(plan_meta['repeats'])
        dataset_label = f"datasets/lines/{plan_meta['dataset']}"
        max_bulk_s = float(plan_meta.get('max_bulk_s', 0.0))
        workload_timeout_s = float(plan_meta.get('workload_timeout_s', 300.0))
        bulk_warmup_s = float(plan_meta.get('bulk_warmup_s', 0.0))
        raw_scenarios = plan_meta['scenarios']
        work_info = plan_meta.get('work_info', {})
    else:
        ccs = list(dict.fromkeys(r['cc'] for r in rows))
        workloads = list(dict.fromkeys(r['workload'] for r in rows))
        queues = list(dict.fromkeys(r.get('queue_bytes', '500000') for r in rows))
        repeats = max(int(r.get('repeat', 1)) for r in rows)
        dataset_label = ', '.join(sorted({r.get('dataset', 'lines') for r in rows}))
        max_bulk_s = 0.0
        workload_timeout_s = 300.0
        bulk_warmup_s = 0.0
        raw_scenarios = [
            {'name': name, 'duration_s': 0.0, 'has_up_delay': True, 'has_down_delay': True}
            for name in dict.fromkeys(r['scenario'] for r in rows)
        ]
        work_info = {}

    multi_queue = len(queues) > 1
    report_scenarios: list[dict] = []
    for sc in raw_scenarios:
        files_stub = {}
        if sc.get('has_up_delay', True):
            files_stub['up_delay'] = True
        if sc.get('has_down_delay', True):
            files_stub['down_delay'] = True
        for q in queues:
            disp_name = f"{sc['name']} [q={format_queue_label(q)}]" if multi_queue else sc['name']
            report_scenarios.append({
                'name': disp_name,
                'duration_s': float(sc.get('duration_s', 0.0)),
                'files': files_stub,
            })

    report_rows: list[dict] = []
    for r in rows:
        item = dict(r)
        if multi_queue:
            item['scenario'] = f"{r['scenario']} [q={format_queue_label(r.get('queue_bytes', ''))}]"
        report_rows.append(item)

    host_info = shard_metas[0].get('host', {}) if shard_metas else {}
    queue_labels = ', '.join(format_queue_label(q) for q in queues)
    extra_header = [
        f'- 运行栈：Linux Kernel TCP + Mahimahi（增强版 LeoReplayer）+ Mortise',
        f'- 瓶颈链路队列（每方向）：{queue_labels}（端主机保持 Linux 默认参数）',
    ]
    if host_info.get('kernel'):
        extra_header.append(f"- Runner 内核：`{host_info['kernel']}`（默认 qdisc：`{host_info.get('net.core.default_qdisc', 'unknown')}`）")
    if max_bulk_s > 0:
        extra_header.append(f'- Bulk 最大时长截断：{max_bulk_s:g} s')

    extra_config = [
        '- 方向映射：`lines/up.*`（TCP 数据）→ Mahimahi downlink（`server → client`）；`lines/down.*`（TCP ACK）→ Mahimahi uplink（`client → server`）。',
        f'- Bulk 统计预热：{bulk_warmup_s:g} s；动态与非对称单向延迟由 10 ms 步长保序 FIFO `mm-delay` 回放，不额外注入随机丢包。',
    ]

    report_md = trace_report.render_report(
        report_scenarios,
        report_rows,
        work_info,
        title='Mahimahi Linux Kernel Bench 实验报告',
        dataset_label=dataset_label,
        ccs=ccs,
        workloads=workloads,
        repeats=repeats,
        tcp_segment_size_bytes=None,
        default_one_way_delay_ms=DEFAULT_ONE_WAY_DELAY_MS,
        workload_max_s=workload_timeout_s,
        workload_timeout_label='实际运行时间',
        extra_header_lines=extra_header,
        extra_config_lines=extra_config,
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / 'runs.csv'
    md_path = out_dir / 'report.md'
    meta_path = out_dir / 'metadata.json'

    trace_report.write_runs_csv(csv_path, rows, extra_fields=KERNEL_CSV_EXTRA_FIELDS)
    atomic_write(md_path, report_md)
    atomic_write(
        meta_path,
        json.dumps(
            {
                'plan': plan_meta,
                'host': host_info,
                'shard_count': len(shard_metas),
                'total_rows': len(rows),
            },
            indent=2,
        ) + '\n',
    )
    return csv_path, md_path, report_md


def format_step_summary(report_md: str, max_bytes: int = 900_000) -> str:
    """Keep full report in GitHub Step Summary unless it nears GitHub's 1 MiB limit."""
    encoded = report_md.encode('utf-8')
    if len(encoded) <= max_bytes:
        return report_md
    marker = '\n## 算法排名\n'
    head = report_md.split('\n## ', 1)[0]
    if marker in report_md:
        tail = marker + report_md.split(marker, 1)[1]
        compact = (
            head
            + '\n\n> 完整逐场景明细表较长，已保存至 `mahimahi-kernel-lines-summary` artifact 中的 `report.md` 和 `runs.csv`。\n'
            + tail
        )
        if len(compact.encode('utf-8')) <= max_bytes:
            return compact
    return report_md[:max_bytes // 2] + '\n\n> 内容过长已截断，完整结果见 `report.md` artifact。\n'


def cmd_summarize(args: argparse.Namespace) -> int:
    plan_raw = args.plan_json or os.environ.get('PLAN_JSON', '')
    plan_meta = json.loads(plan_raw) if plan_raw.strip() else None
    csv_path, md_path, report_md = summarize_results(
        Path(args.input_dir),
        Path(args.out_dir),
        plan_meta=plan_meta,
    )
    print(f'Wrote {csv_path} and {md_path}', flush=True)
    gh_summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if gh_summary:
        with open(gh_summary, 'a', encoding='utf-8') as f:
            f.write(format_step_summary(report_md))
    else:
        print(report_md)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='cmd', required=True)

    p_plan = sub.add_parser('plan')
    p_plan.add_argument('--dataset', default=None)
    p_plan.add_argument('--scenarios', default=None)
    p_plan.add_argument('--workloads', default=None)
    p_plan.add_argument('--ccs', default=None)
    p_plan.add_argument('--queues', default=None)
    p_plan.add_argument('--repeats', type=int, default=None)
    p_plan.add_argument('--max-bulk-s', type=float, default=None)
    p_plan.add_argument('--workload-timeout-s', type=float, default=None)
    p_plan.add_argument('--bulk-warmup-s', type=float, default=None)
    p_plan.add_argument('--max-shards', type=int, default=None)

    p_run = sub.add_parser('run')
    p_run.add_argument('--mortise-dir', required=True)
    p_run.add_argument('--out-dir', required=True)
    p_run.add_argument('--units-json', default=None)
    p_run.add_argument('--ccs', default=None)
    p_run.add_argument('--queues', default=None)
    p_run.add_argument('--repeats', type=int, default=None)
    p_run.add_argument('--parallel', type=int, default=None)
    p_run.add_argument('--max-bulk-s', type=float, default=None)
    p_run.add_argument('--workload-timeout-s', type=float, default=None)
    p_run.add_argument('--bulk-warmup-s', type=float, default=None)
    p_run.add_argument('--keep-logs', action='store_true')

    p_sum = sub.add_parser('summarize')
    p_sum.add_argument('--input-dir', required=True)
    p_sum.add_argument('--out-dir', required=True)
    p_sum.add_argument('--plan-json', default=None)

    args = parser.parse_args()
    return {'plan': cmd_plan, 'run': cmd_run, 'summarize': cmd_summarize}[args.cmd](args)


if __name__ == '__main__':
    raise SystemExit(main())
