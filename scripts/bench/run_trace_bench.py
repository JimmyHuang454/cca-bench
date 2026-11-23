#!/usr/bin/env python3
"""Run TCP algorithms against all lines scenarios; edit the settings below."""
from __future__ import annotations

import concurrent.futures
import fcntl
import hashlib
import json
import math
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

_BENCH_DIR = str(Path(__file__).resolve().parent)
if _BENCH_DIR not in sys.path:
    sys.path.insert(0, _BENCH_DIR)

import lines_common
import trace_report
from lines_common import atomic_write, digest, discover, integrate_piecewise, parse_line, workload
from trace_report import mean

ROOT = Path(__file__).resolve().parents[2]

# Select a dataset directory or one of its scenario directories.
# Edit these settings, then run this script or `make bench`.
LINES_DIR = ROOT / 'datasets/lines/lumos'
# LINES_DIR = ROOT / 'datasets/lines/leocc'
WORKLOADS = ['bulk', 'websearch', 'pageload', 'app', 'http2_mux_rr', 'http2_mux_srpt']
CCS = ['westwood', 'illinois', 'veno', 'yeah', 'hybla']
CCS = ['copa_nc', 'copa', 'bbr', 'cubic','copa_auto_v3','copa_auto_v3_nc']
REPEATS = 1
TAIL_PERCENTILE = 98  # Tail percentile for qdelay, Norm Power, RCT, FCT Slowdown & rankings (e.g. 50, 90, 95, 99).
RESUME = True  # False: rerun all combinations and replace their results.
MAX_WORKERS = os.cpu_count() or 1  # Maximum concurrent ns-3 processes; 1 runs serially.
WORKLOAD_MAX_SIMULATION_S = 1800.0  # Maximum simulated time from the start, not wall time.
DEFAULT_ONE_WAY_DELAY_MS = lines_common.DEFAULT_ONE_WAY_DELAY_MS
TCP_SEGMENT_SIZE_BYTES = 1448  # IPv4 + TCP timestamps with a 1500-byte MTU.
TCP_SND_BUF_BYTES = 16 * 1024 * 1024  # 16 MiB: modern Linux/Android/iOS TCP auto-tuning ceiling.
TCP_RCV_BUF_BYTES = 16 * 1024 * 1024  # 16 MiB: avoids 128 KiB RWND cap on high-BDP 5G links.
QUEUE_BYTES: int | str = 'auto'  # 'auto' scales per scenario as clamp(max(1.5*BDP, 80ms), 500 KB, 16 MiB); or int bytes.
QUEUE_AUTO_BDP_MULT = 1.5
QUEUE_AUTO_DEPTH_MS = 80.0
QUEUE_AUTO_MIN_BYTES = 500_000
QUEUE_AUTO_MAX_BYTES = 16 * 1024 * 1024
BINARY = ROOT / 'ns3/ns-3/build/scratch/tcp-trace-bench/ns3-dev-tcp-trace-bench-optimized'
RESULTS_DIR = ROOT / 'results/trace-bench'

# Bump this only when Python-side simulation semantics change. Reporting and
# concurrency changes do not invalidate previously completed simulations.
SIMULATION_SIGNATURE = '182f37063e9dd6a695b65af9d5419d7aaf98dcf7571b4f8f237a49730cb81f68'


def result_directory(directory: Path) -> Path:
    try:
        relative = directory.resolve().relative_to((ROOT / 'datasets/lines').resolve())
        if relative.parts:
            return RESULTS_DIR / relative
    except ValueError:
        pass
    return RESULTS_DIR / directory.resolve().name


def ranking_entries(rows: list[dict], scenario: str, workload_name: str, kind: str) -> list[dict]:
    return trace_report.ranking_entries(
        rows, scenario, workload_name, kind,
        ccs=CCS, workloads=WORKLOADS, repeats=REPEATS, tail_percentile=TAIL_PERCENTILE,
    )


def overall_ranking(scenarios: list[dict], rows: list[dict]) -> tuple[list[dict], list[str]]:
    return trace_report.overall_ranking(
        scenarios, rows, ccs=CCS, workloads=WORKLOADS, repeats=REPEATS, tail_percentile=TAIL_PERCENTILE
    )


def overall_ranking_table(scenarios: list[dict], rows: list[dict]) -> list[str]:
    return trace_report.overall_ranking_table(
        scenarios, rows, ccs=CCS, workloads=WORKLOADS, repeats=REPEATS, tail_percentile=TAIL_PERCENTILE
    )


def workload_ranking(scenarios: list[dict], rows: list[dict], workload_name: str) -> tuple[list[dict], list[str]]:
    return trace_report.workload_ranking(
        scenarios, rows, workload_name,
        ccs=CCS, workloads=WORKLOADS, repeats=REPEATS, tail_percentile=TAIL_PERCENTILE,
    )


def workload_ranking_tables(scenarios: list[dict], rows: list[dict]) -> list[str]:
    return trace_report.workload_ranking_tables(
        scenarios, rows, ccs=CCS, workloads=WORKLOADS, repeats=REPEATS, tail_percentile=TAIL_PERCENTILE
    )


def ranking_tables(scenarios: list[dict], rows: list[dict]) -> list[str]:
    return trace_report.ranking_tables(
        scenarios, rows, ccs=CCS, workloads=WORKLOADS, repeats=REPEATS, tail_percentile=TAIL_PERCENTILE
    )


def print_overview(scenarios: list[dict], rows: list[dict]) -> None:
    trace_report.print_overview(
        scenarios, rows, ccs=CCS, workloads=WORKLOADS, repeats=REPEATS, tail_percentile=TAIL_PERCENTILE
    )


class Progress:
    def __init__(self, total: int, resumed: int, started: float):
        self.total = total
        self.resumed = resumed
        self.started = started
        self.tty = sys.stdout.isatty()
        self.last_bucket = -1
        self.last_draw = 0.0
        self.visible = False
        self.last_state = None

    def update(self, done: int, failures: int, running: int, force: bool = False) -> None:
        percent = 100 * done // self.total if self.total else 100
        state = (done, failures, running)
        if force and self.last_state == state:
            return
        bucket = percent // 10
        now = time.monotonic()
        if not force and ((self.tty and now - self.last_draw < 0.2) or
                          (not self.tty and bucket <= self.last_bucket)):
            return
        filled = percent * 20 // 100
        bar = '#' * filled + '-' * (20 - filled)
        line = (f'[{bar}] {percent:3d}%  {done}/{self.total}  '
                f'运行中 {running}  失败 {failures}  已恢复 {self.resumed}  '
                f'耗时 {now - self.started:.1f}s')
        if self.tty:
            print('\r\033[K' + line, end='', flush=True)
            self.visible = True
        else:
            print(line, flush=True)
        self.last_bucket = bucket
        self.last_draw = now
        self.last_state = state

    def finish(self) -> None:
        if self.tty and self.visible:
            print(flush=True)
            self.visible = False

    def completed(self, row: dict, elapsed: float) -> None:
        if self.tty and self.visible:
            print('\r\033[K', end='', flush=True)
            self.visible = False
        label = (f"{row['scenario']}  |  {row['cc']}  |  {row['workload']}  |  "
                 f"第 {row['repeat']} 次  |  {elapsed:.2f}s")
        if row['status'] != 'success':
            print(f"[失败] {label}  |  {row['error']}", file=sys.stderr, flush=True)
            return
        fields = [('throughput_mbps', '吞吐', ' Mbps'), ('norm_power', 'Norm Power', '')] if row['workload'] == 'bulk' else [
            ('duration_s', '完成时间', ' s'), ('fct_slowdown', 'FCT Slowdown', 'x')]
        metrics = []
        for key, name, unit in fields:
            try:
                value = float(row[key])
                if math.isfinite(value):
                    metrics.append(f'{name} {value:.3f}{unit}')
            except (KeyError, TypeError, ValueError):
                pass
        suffix = '  |  ' + '  |  '.join(metrics) if metrics else ''
        print(f'[完成] {label}{suffix}', flush=True)


def run_fingerprint(command: list[str], binary_sha: str, scenario_files: dict,
                    workload_sha: str | None, runner_sha: str) -> str:
    payload = {'version': 1, 'command': command, 'binary_sha256': binary_sha,
               'scenario_files': scenario_files, 'workload_sha256': workload_sha,
               'runner_sha256': runner_sha}
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()


class ProcessController:
    """Track independent ns-3 processes so an interrupted bench stops them."""

    def __init__(self):
        self.lock = threading.Lock()
        self.active: set[subprocess.Popen] = set()
        self.cancelled = False

    def run(self, cmd: list[str], timeout: int) -> tuple[int | None, str, str, str]:
        with self.lock:
            if self.cancelled:
                return None, '', '', '已中断'
            try:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, start_new_session=True)
            except OSError as error:
                return None, '', '', str(error)
            self.active.add(proc)
        try:
            try:
                stdout, stderr = proc.communicate(timeout=timeout)
                return proc.returncode, stdout, stderr, ''
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate()
                return None, stdout, stderr, f'运行超时（{timeout} s）'
        finally:
            with self.lock:
                self.active.discard(proc)

    def cancel(self) -> None:
        with self.lock:
            self.cancelled = True
            for proc in self.active:
                try:
                    proc.kill()
                except ProcessLookupError:
                    pass


def _format_bytes(size: int) -> str:
    if size >= 1024 * 1024 and size % (1024 * 1024) == 0:
        return f'{size // (1024 * 1024)} MiB'
    if size >= 1_000_000:
        return f'{size / 1_000_000:.2f} MB'
    return f'{size / 1_000:.0f} KB'


def scenario_queue_bytes(scenario: dict) -> int:
    if QUEUE_BYTES != 'auto':
        if isinstance(QUEUE_BYTES, bool) or not isinstance(QUEUE_BYTES, int) or QUEUE_BYTES <= 0:
            raise ValueError(f'invalid QUEUE_BYTES: {QUEUE_BYTES!r}')
        return QUEUE_BYTES
    files = scenario.get('files') or {}
    up_path = files.get('up')
    if not up_path:
        return QUEUE_AUTO_MIN_BYTES
    end_ms, up_pts = parse_line(Path(up_path), delay=False)
    avg_cap_mbps = integrate_piecewise(up_pts, end_ms) * 12.0 / end_ms if end_ms > 0 else 0.0
    delays_ms = []
    for key in ('up_delay', 'down_delay'):
        dpath = files.get(key)
        if dpath:
            d_end, d_pts = parse_line(Path(dpath), delay=True)
            delays_ms.append(integrate_piecewise(d_pts, d_end) / d_end if d_end > 0 else DEFAULT_ONE_WAY_DELAY_MS)
        else:
            delays_ms.append(DEFAULT_ONE_WAY_DELAY_MS)
    avg_rtt_ms = sum(delays_ms)
    bytes_per_sec = avg_cap_mbps * 1_000_000.0 / 8.0
    bdp_bytes = bytes_per_sec * (avg_rtt_ms / 1000.0)
    depth_bytes = bytes_per_sec * (QUEUE_AUTO_DEPTH_MS / 1000.0)
    raw = max(int(round(QUEUE_AUTO_BDP_MULT * bdp_bytes)), int(round(depth_bytes)))
    return max(QUEUE_AUTO_MIN_BYTES, min(QUEUE_AUTO_MAX_BYTES, raw))


def write_report(
    output: Path,
    scenarios: list[dict],
    rows: list[dict],
    work: dict,
    queue_by_scenario: dict[str, int] | None = None,
) -> None:
    extra_header = [
        f'- TCP 端侧收发缓冲：SndBuf {_format_bytes(TCP_SND_BUF_BYTES)} / RcvBuf {_format_bytes(TCP_RCV_BUF_BYTES)}',
    ]
    extra_config: list[str] = []
    if queue_by_scenario:
        uniq = sorted(set(queue_by_scenario.values()))
        if len(uniq) == 1:
            extra_header.append(f'- 瓶颈单向队列缓冲：{_format_bytes(uniq[0])}（{uniq[0]} 字节）')
        else:
            extra_header.append(
                f'- 瓶颈单向队列缓冲（auto）：{_format_bytes(uniq[0])}–{_format_bytes(uniq[-1])}'
            )
            extra_config.append(
                '- 各场景瓶颈单向队列缓冲：'
                + '；'.join(f"{s['name']} {_format_bytes(queue_by_scenario[s['name']])}" for s in scenarios if s['name'] in queue_by_scenario)
                + '。'
            )
    content = trace_report.render_report(
        scenarios,
        rows,
        work,
        title='TCP Trace Bench 实验报告',
        dataset_label=str(LINES_DIR),
        ccs=CCS,
        workloads=WORKLOADS,
        repeats=REPEATS,
        tail_percentile=TAIL_PERCENTILE,
        tcp_segment_size_bytes=TCP_SEGMENT_SIZE_BYTES,
        default_one_way_delay_ms=DEFAULT_ONE_WAY_DELAY_MS,
        workload_max_s=WORKLOAD_MAX_SIMULATION_S,
        workload_timeout_label='仿真时间',
        extra_header_lines=extra_header,
        extra_config_lines=extra_config,
    )
    atomic_write(output / 'report.md', content)


def write_results(
    output: Path,
    scenarios: list[dict],
    rows: list[dict],
    work: dict,
    queue_by_scenario: dict[str, int] | None = None,
) -> None:
    trace_report.write_runs_csv(output / 'runs.csv', rows)
    write_report(output, scenarios, rows, work, queue_by_scenario=queue_by_scenario)


def main() -> int:
    lock_handle = None
    progress = None
    started = time.monotonic()
    try:
        if not BINARY.is_file():
            raise ValueError(f'benchmark binary missing: {BINARY}; run make build-bench')
        if (REPEATS < 1 or not isinstance(MAX_WORKERS, int) or MAX_WORKERS < 1
                or not CCS or len(set(CCS)) != len(CCS) or not WORKLOADS or len(set(WORKLOADS)) != len(WORKLOADS)
                or not math.isfinite(WORKLOAD_MAX_SIMULATION_S) or WORKLOAD_MAX_SIMULATION_S <= 0):
            raise ValueError('invalid algorithms, workloads or repeats')
        tail_p = trace_report.validate_tail_percentile(TAIL_PERCENTILE)
        print('\nTCP Trace Bench\n' + '=' * 40, flush=True)
        print(f'数据集：{LINES_DIR}\n正在读取并校验输入…', flush=True)
        scenarios = discover(LINES_DIR)
        work = {}
        for wk in WORKLOADS:
            if wk == 'bulk':
                continue
            if not wk.replace('_', '').replace('-', '').isalnum():
                raise ValueError(f'invalid workload name: {wk}')
            path = ROOT / 'datasets/workloads' / f'{wk}.wk'
            count, span = workload(path)
            work[wk] = {'path': path, 'count': count, 'span_s': span, 'sha256': digest(path)}
        output = result_directory(LINES_DIR)
        output.mkdir(parents=True, exist_ok=True)
        lock_handle = (output / '.lock').open('a')
        try:
            fcntl.flock(lock_handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError(f'benchmark already running for {output}') from None
        raw_dir = output / 'runs'
        raw_dir.mkdir(exist_ok=True)
        queue_by_scenario = {s['name']: scenario_queue_bytes(s) for s in scenarios}
        metadata = {'lines_dir': str(LINES_DIR), 'ccs': CCS, 'workloads': {k: {**v, 'path': str(v['path'])} for k, v in work.items()},
                    'repeats': REPEATS, 'tail_percentile': tail_p, 'max_workers': MAX_WORKERS, 'default_one_way_delay_ms': DEFAULT_ONE_WAY_DELAY_MS,
                    'workload_max_simulation_s': WORKLOAD_MAX_SIMULATION_S,
                    'scenarios': [{'name': s['name'], 'duration_s': s['duration_s'],
                                   'queue_bytes': queue_by_scenario[s['name']],
                                   'files': {k: {'path': str(v), 'sha256': digest(v)} for k, v in s['files'].items()}} for s in scenarios],
                    'binary_sha256': digest(BINARY), 'queue_bytes_mode': QUEUE_BYTES,
                    'tcp_segment_size_bytes': TCP_SEGMENT_SIZE_BYTES,
                    'tcp_snd_buf_bytes': TCP_SND_BUF_BYTES,
                    'tcp_rcv_buf_bytes': TCP_RCV_BUF_BYTES,
                    'tcp_configuration': 'ns-3 defaults with explicit segment size and 16 MiB SndBuf/RcvBuf; congestion-control implementations may enable pacing'}
        atomic_write(output / 'metadata.json', json.dumps(metadata, indent=2) + '\n')
        scenario_hashes = {s['name']: {k: v['sha256'] for k, v in s['files'].items()}
                           for s in metadata['scenarios']}
        rows_by_index: dict[int, dict] = {}
        pending = []
        index = 0
        for scenario in scenarios:
            q_bytes = queue_by_scenario[scenario['name']]
            for wk in WORKLOADS:
                for cc in CCS:
                    for repeat in range(1, REPEATS + 1):
                        row = {'scenario': scenario['name'], 'workload': wk, 'cc': cc, 'repeat': repeat,
                               'duration_s': scenario['duration_s'], 'status': 'failed', 'error': ''}
                        files = scenario['files']
                        stop_s = scenario['duration_s'] if wk == 'bulk' else WORKLOAD_MAX_SIMULATION_S
                        cmd = [str(BINARY), '--profile=lines', f'--upTrace={files["up"]}', f'--downTrace={files["down"]}',
                               f'--stopTime={stop_s}', f'--ccs={cc}', f'--run={repeat}',
                               f'--defaultDelayMs={DEFAULT_ONE_WAY_DELAY_MS}',
                               f'--segmentSize={TCP_SEGMENT_SIZE_BYTES}',
                               f'--sndBuf={TCP_SND_BUF_BYTES}',
                               f'--rcvBuf={TCP_RCV_BUF_BYTES}',
                               f'--queueBytes={q_bytes}',
                               f'--tailPercentile={tail_p:g}', '--evalJson=1']
                        for direction in ('up', 'down'):
                            if direction + '_delay' in files:
                                cmd.append(f'--{direction}DelayFile={files[direction + "_delay"]}')
                        if wk != 'bulk':
                            cmd.append(f'--workloadFile={work[wk]["path"]}')
                            cmd.append(f'--linePeriodMs={round(scenario["duration_s"] * 1000)}')
                        label = f'{scenario["name"]}__{wk}__{cc}__r{repeat}'
                        checkpoint = raw_dir / f'{label}.json'
                        fingerprint = run_fingerprint(cmd, metadata['binary_sha256'],
                                                      scenario_hashes[scenario['name']],
                                                      work[wk]['sha256'] if wk != 'bulk' else None,
                                                      SIMULATION_SIGNATURE)
                        if RESUME and checkpoint.is_file():
                            try:
                                saved = json.loads(checkpoint.read_text(encoding='utf-8'))
                                if saved['fingerprint'] == fingerprint and saved['row']['status'] in ('success', 'timeout'):
                                    rows_by_index[index] = saved['row']
                                    index += 1
                                    continue
                            except (OSError, ValueError, KeyError, TypeError):
                                pass
                        checkpoint.unlink(missing_ok=True)
                        pending.append({'index': index, 'row': row, 'cmd': cmd, 'label': label,
                                        'checkpoint': checkpoint, 'fingerprint': fingerprint,
                                        'timeout': max(120, int(stop_s * 20)),
                                        'expected': work[wk]['count'] if wk != 'bulk' else None})
                        index += 1

        def snapshot() -> None:
            write_results(output, scenarios, [rows_by_index[i] for i in sorted(rows_by_index)], work,
                          queue_by_scenario=queue_by_scenario)

        def save_result(job: dict, result: tuple[int | None, str, str, str]) -> None:
            returncode, stdout, stderr, error = result
            label, row = job['label'], job['row']
            atomic_write(raw_dir / f'{label}.stdout', stdout)
            atomic_write(raw_dir / f'{label}.stderr', stderr)
            try:
                if error:
                    raise RuntimeError(error)
                if returncode:
                    raise RuntimeError(f'exit {returncode}: {stderr.strip()[-300:]}')
                data = json.loads(next(line for line in reversed(stdout.splitlines()) if line.startswith('{')))
                row.update(data)
                if job['expected'] is not None and int(data.get('requests', 0)) != job['expected']:
                    row['status'] = 'timeout'
                    row['error'] = f"达到最大仿真时长 {WORKLOAD_MAX_SIMULATION_S:g} s，数据块完成 {data.get('requests', 0)}/{job['expected']}，计 0 分"
                    for field in (
                        'rct_mean_ms', 'rct_p50_ms', 'rct_p95_ms', 'rct_tail_ms',
                        'fct_slowdown', 'fct_slowdown_p95', 'fct_slowdown_tail', 'mortise_qoe',
                    ):
                        row[field] = ''
                else:
                    row['status'] = 'success'
            except (RuntimeError, ValueError, StopIteration) as exc:
                row['error'] = str(exc)
            atomic_write(job['checkpoint'], json.dumps({'fingerprint': job['fingerprint'], 'row': row},
                                                       ensure_ascii=False, allow_nan=False) + '\n')
            rows_by_index[job['index']] = row
            snapshot()
            progress.completed(row, job['finished_at'] - job['started_at'])
            progress.update(len(rows_by_index),
                            sum(r['status'] != 'success' for r in rows_by_index.values()),
                            len(active), force=progress.tty)

        snapshot()
        controller = ProcessController()
        worker_count = min(MAX_WORKERS, len(pending)) if pending else 0
        print(f'场景：{len(scenarios)}  |  算法：{", ".join(CCS)}  |  负载：{", ".join(WORKLOADS)}')
        print(f'重复：{REPEATS}  |  并发：{worker_count}  |  已恢复：{len(rows_by_index)}  |  待运行：{len(pending)}\n', flush=True)
        progress = Progress(index, len(rows_by_index), started)
        progress.update(len(rows_by_index), 0, worker_count, force=True)
        if pending:
            executor = concurrent.futures.ThreadPoolExecutor(max_workers=worker_count)
            active: dict[concurrent.futures.Future, dict] = {}
            jobs = iter(pending)

            def run_job(job: dict) -> tuple[int | None, str, str, str]:
                try:
                    return controller.run(job['cmd'], job['timeout'])
                finally:
                    job['finished_at'] = time.monotonic()

            def submit_one() -> bool:
                job = next(jobs, None)
                if job is None:
                    return False
                job['started_at'] = time.monotonic()
                active[executor.submit(run_job, job)] = job
                return True

            try:
                for _ in range(worker_count):
                    submit_one()
                while active:
                    completed, _ = concurrent.futures.wait(active, timeout=1.0,
                                                           return_when=concurrent.futures.FIRST_COMPLETED)
                    if not completed:
                        progress.update(len(rows_by_index),
                                        sum(r['status'] != 'success' for r in rows_by_index.values()),
                                        len(active))
                    for future in completed:
                        job = active.pop(future)
                        save_result(job, future.result())
                        submit_one()
            except BaseException:
                controller.cancel()
                executor.shutdown(wait=True, cancel_futures=True)
                for future, job in active.items():
                    if future.done() and not future.cancelled():
                        result = future.result()
                        if result[0] == 0 and not result[3]:
                            save_result(job, result)
                raise
            else:
                executor.shutdown(wait=True)
        rows = [rows_by_index[i] for i in sorted(rows_by_index)]
        failures = sum(row['status'] != 'success' for row in rows)
        progress.update(len(rows), failures, 0, force=True)
        progress.finish()
        print(f'\n{"完成" if failures == 0 else "已完成，存在失败任务"}  ·  成功 {len(rows) - failures}/{index}  ·  耗时 {time.monotonic() - started:.1f}s')
        print_overview(scenarios, rows)
        print(f'\n详细报告：{output / "report.md"}\n')
        return 0 if failures == 0 else 1
    except (OSError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        if progress is not None:
            progress.finish()
        print('\n已中断；已完成的结果已保存，再次运行可继续。', file=sys.stderr)
        return 130
    finally:
        if lock_handle is not None:
            lock_handle.close()


if __name__ == '__main__':
    raise SystemExit(main())
