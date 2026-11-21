#!/usr/bin/env python3
"""Run TCP algorithms against all lines scenarios; edit the settings below."""
from __future__ import annotations

import csv
import concurrent.futures
import fcntl
import hashlib
import io
import json
import math
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Select a dataset directory or one of its scenario directories.
# Edit these settings, then run this script or `make bench`.
LINES_DIR = ROOT / 'datasets/lines/basic'
# WORKLOADS = ['bulk', 'websearch', 'pageload', 'app']
WORKLOADS = ['bulk']
CCS = ['cubic', 'bbr', 'copa_auto_nc','copa']
REPEATS = 2
RESUME = True  # False: rerun all combinations and replace their results.
MAX_WORKERS = os.cpu_count() or 1  # Maximum concurrent ns-3 processes; 1 runs serially.
WORKLOAD_DRAIN_TIMEOUT_S = 60.0
DEFAULT_ONE_WAY_DELAY_MS = 10.0
TCP_SEGMENT_SIZE_BYTES = 1448  # IPv4 + TCP timestamps with a 1500-byte MTU.
BINARY = ROOT / 'ns3/ns-3/build/scratch/tcp-trace-bench/ns3-dev-tcp-trace-bench-optimized'
RESULTS_DIR = ROOT / 'results/trace-bench'

# Bump this only when Python-side simulation semantics change. Reporting and
# concurrency changes do not invalidate previously completed simulations.
SIMULATION_SIGNATURE = '182f37063e9dd6a695b65af9d5419d7aaf98dcf7571b4f8f237a49730cb81f68'


def parse_line(path: Path, delay: bool = False) -> tuple[int, list[tuple[int, float]]]:
    points = []
    end = None
    with path.open(encoding='utf-8') as stream:
        for number, raw in enumerate(stream, 1):
            text = raw.strip()
            if not text:
                continue
            if text.startswith('#'):
                if text.startswith('# end_ms='):
                    if end is not None:
                        raise ValueError(f'{path}:{number}: duplicate end marker')
                    try:
                        end = int(text[len('# end_ms='):])
                    except ValueError:
                        raise ValueError(f'{path}:{number}: invalid end marker') from None
                continue
            try:
                t, raw_value = text.split(',')
                raw_value = raw_value.strip()
                mbps = not delay and raw_value.endswith('m')
                if mbps and raw_value[:-1] != raw_value[:-1].rstrip():
                    raise ValueError
                t, value = int(t), float(raw_value[:-1] if mbps else raw_value)
                if t < 0 or not math.isfinite(value) or value < 0 or (not delay and ((not mbps and not value.is_integer()) or value > (12_000_000 if mbps else 1_000_000))):
                    raise ValueError
                if mbps:
                    value /= 12.0
                if (not points and t != 0) or (points and t <= points[-1][0]):
                    raise ValueError
            except ValueError:
                raise ValueError(f'{path}:{number}: invalid time point or value') from None
            points.append((t, value))
    if not points:
        raise ValueError(f'{path}: no data')
    if delay:
        if end is None or end <= points[-1][0]:
            raise ValueError(f'{path}: missing or invalid # end_ms marker')
    else:
        if len(points) < 2 or points[-1][1] != 0 or points[-1][0] <= 0:
            raise ValueError(f'{path}: capacity must end with an explicit time,0')
        end = points[-1][0]
    return end, points


def discover(directory: Path) -> list[dict]:
    if not directory.is_dir():
        raise ValueError(f'dataset directory not found: {directory}')
    up = {(p.parent, p.name[:-len('.up.txt')]): p for p in directory.rglob('*.up.txt')}
    down = {(p.parent, p.name[:-len('.down.txt')]): p for p in directory.rglob('*.down.txt')}
    for direction, files in (('up', up), ('down', down)):
        for p in directory.rglob(f'{direction}.txt'):
            key = (p.parent, p.parent.name)
            if key in files:
                raise ValueError(f'ambiguous {direction} capacity files in {p.parent}')
            files[key] = p
    missing = set(up) ^ set(down)
    if missing:
        raise ValueError(f'missing up/down capacity for: {", ".join(str(parent / name) for parent, name in sorted(missing))}')
    if not up:
        raise ValueError(f'no line scenarios found in {directory}')
    scenarios = []
    seen = set()
    for parent, name in sorted(up):
        if name in seen:
            raise ValueError(f'duplicate scenario name: {name}')
        seen.add(name)
        files = {'up': up[parent, name], 'down': down[parent, name]}
        if (files['up'].name == 'up.txt') != (files['down'].name == 'down.txt'):
            raise ValueError(f'mixed capacity file naming in {parent}')
        for direction in ('up', 'down'):
            fixed_names = files[direction].name == f'{direction}.txt'
            p = parent / (f'{direction}.delay.txt' if fixed_names else f'{name}.{direction}.delay.txt')
            if p.is_file():
                files[direction + '_delay'] = p
        ends = [parse_line(p, key.endswith('_delay'))[0] for key, p in files.items()]
        scenarios.append({'name': name, 'files': files, 'duration_s': min(ends) / 1000.0})
    return scenarios


def result_directory(directory: Path) -> Path:
    try:
        relative = directory.resolve().relative_to((ROOT / 'datasets/lines').resolve())
        if relative.parts:
            return RESULTS_DIR / relative
    except ValueError:
        pass
    return RESULTS_DIR / directory.resolve().name


def workload(path: Path) -> tuple[int, float]:
    elapsed, count = 0.0, 0
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        try:
            gap, size = line.split()
            gap, size = float(gap), int(size)
            if not math.isfinite(gap) or gap < 0 or size <= 0 or size > 100000000:
                raise ValueError
        except ValueError:
            raise ValueError(f'{path}:{number}: invalid workload entry') from None
        elapsed += gap / 1000
        count += 1
    if not count:
        raise ValueError(f'{path}: empty workload')
    return count, elapsed


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for part in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(part)
    return h.hexdigest()


def mean(rows: list[dict], field: str) -> str:
    values = [float(r[field]) for r in rows if r.get(field) not in ('', None) and math.isfinite(float(r[field]))]
    return f'{statistics.mean(values):.3f}' if values else '-'


def ranking_entries(rows: list[dict], scenario: str, workload_name: str, kind: str) -> list[dict]:
    """Rank complete runs using unrounded per-algorithm averages."""
    def metric(cc: str, wk: str, field: str, positive: bool) -> tuple[float | None, bool, int]:
        selected = [r for r in rows if r['scenario'] == scenario and r['cc'] == cc and r['workload'] == wk]
        good = [r for r in selected if r['status'] == 'success']
        values = []
        for row in good:
            try:
                value = float(row[field])
                if math.isfinite(value) and (value > 0 if positive else value >= 0):
                    values.append(value)
            except (KeyError, TypeError, ValueError):
                pass
        complete = (len(selected) == REPEATS and len(good) == REPEATS and
                    {str(r['repeat']) for r in good} == {str(i) for i in range(1, REPEATS + 1)})
        valid = complete and len(values) == REPEATS
        return (statistics.mean(values) if values else None), valid, len(good)

    entries = []
    for cc in CCS:
        norm, norm_ok, bulk_count = metric(cc, 'bulk', 'norm_power', False)
        wk_stats = {wk: metric(cc, wk, 'fct_slowdown', True)
                    for wk in (w for w in WORKLOADS if w != 'bulk')} if kind == 'combined' else {}
        slow, slow_ok, workload_count = (metric(cc, workload_name, 'fct_slowdown', True)
                                          if kind == 'workload' else (None, False, 0))
        if kind == 'bulk':
            value, valid, count = norm, norm_ok, f'{bulk_count}/{REPEATS}'
        elif kind == 'workload':
            value, valid, count = slow, slow_ok, f'{workload_count}/{REPEATS}'
        else:
            if wk_stats and all(v[0] is not None for v in wk_stats.values()):
                slow = math.exp(math.fsum(math.log(v[0]) for v in wk_stats.values()) / len(wk_stats))
            value = math.sqrt(norm / slow) if norm is not None and slow is not None else None
            valid = norm_ok and bool(wk_stats) and all(v[1] for v in wk_stats.values())
            count = '; '.join([f'bulk {bulk_count}/{REPEATS}'] +
                              [f'{wk} {v[2]}/{REPEATS}' for wk, v in wk_stats.items()])
        entries.append({'cc': cc, 'norm': norm, 'slow': slow, 'value': value,
                        'workload_slows': {wk: v[0] for wk, v in wk_stats.items()},
                        'valid': valid, 'count': count, 'rank': '-', 'gap': '-',
                        'status': '完整' if valid else '不完整或指标无效'})

    lower_better = kind == 'workload'
    qualified = [e for e in entries if e['valid'] and e['value'] is not None]
    qualified.sort(key=lambda e: (e['value'] if lower_better else -e['value'], e['cc']))
    if qualified and qualified[0]['value'] > 0:
        best = qualified[0]['value']
        previous = None
        rank = 0
        for index, entry in enumerate(qualified, 1):
            value = entry['value']
            if previous is None or value != previous:
                rank = index
            entry['rank'] = str(rank)
            entry['gap'] = '基准' if value == best else f'{(value - best) / best * 100:+.1f}%'
            previous = value
    elif qualified:
        for entry in qualified:
            entry['status'] = '基准为零，无法排名'
    return qualified + [e for e in entries if not e['valid'] or e['value'] is None]


def ranking_tables(scenarios: list[dict], rows: list[dict]) -> list[str]:
    result = ['## 算法排名', '',
              '每个场景分别排名；指标为各次成功运行的算术平均，使用未四舍五入的值计算。',
              '只有对应组合的所有重复均成功且指标有效时才参与排名；并列值共享排名。', '']

    def fmt(value: float | None, suffix: str = '') -> str:
        return '-' if value is None else f'{value:.3f}{suffix}'

    if 'bulk' in WORKLOADS:
        result.extend(['### Bulk：Norm Power', '',
                       '| 场景 | 排名 | 算法 | 成功/总次数 | Norm Power ↑ | 相对最优差距 | 状态 |',
                       '|---|---:|---|---:|---:|---:|---|'])
        for scenario in scenarios:
            for e in ranking_entries(rows, scenario['name'], 'bulk', 'bulk'):
                result.append(f"| {scenario['name']} | {e['rank']} | {e['cc']} | {e['count']} | {fmt(e['norm'])} | {e['gap']} | {e['status']} |")
        result.append('')
    for wk in (w for w in WORKLOADS if w != 'bulk'):
        result.extend([f'### {wk}：FCT Slowdown', '',
                       '| 场景 | 排名 | 算法 | 成功/总次数 | FCT Slowdown ↓ | 相对最优差距 | 状态 |',
                       '|---|---:|---|---:|---:|---:|---|'])
        for scenario in scenarios:
            for e in ranking_entries(rows, scenario['name'], wk, 'workload'):
                result.append(f"| {scenario['name']} | {e['rank']} | {e['cc']} | {e['count']} | {fmt(e['slow'], 'x')} | {e['gap']} | {e['status']} |")
        result.append('')
    workloads = [w for w in WORKLOADS if w != 'bulk']
    if 'bulk' in WORKLOADS and workloads:
        headers = ['场景', '排名', '算法', '成功/总次数', 'Norm Power']
        headers.extend(f'{wk} Slowdown' for wk in workloads)
        headers.extend(['Slowdown 几何均值', '综合评分 ↑', '相对最优差距', '状态'])
        result.extend(['### 综合排名', '',
                       '各 workload 的重复运行先取算术平均，再对所有 workload 等权取几何均值；综合评分 = sqrt(Norm Power / Slowdown 几何均值)。',
                       '所有 workload 和 bulk 均需完成全部重复，才参与综合排名。', '',
                       '| ' + ' | '.join(headers) + ' |',
                       '|' + '|'.join(['---'] * len(headers)) + '|'])
        for scenario in scenarios:
            for e in ranking_entries(rows, scenario['name'], '', 'combined'):
                cells = [scenario['name'], e['rank'], e['cc'], e['count'], fmt(e['norm'])]
                cells.extend(fmt(e['workload_slows'].get(wk), 'x') for wk in workloads)
                cells.extend([fmt(e['slow'], 'x'), fmt(e['value']), e['gap'], e['status']])
                result.append('| ' + ' | '.join(cells) + ' |')
        result.append('')
    return result


def print_overview(scenarios: list[dict], rows: list[dict]) -> None:
    """Print equal-weight scenario summaries, excluding incomplete groups."""
    groups: dict[tuple[str, str, str], list[dict]] = {}
    for row in rows:
        groups.setdefault((row['scenario'], row['cc'], row['workload']), []).append(row)
    expected_repeats = {str(i) for i in range(1, REPEATS + 1)}
    columns = [('Norm Power', 'bulk', 'norm_power')] if 'bulk' in WORKLOADS else []
    columns += [(f'{wk} FCT', wk, 'fct_slowdown') for wk in WORKLOADS if wk != 'bulk']
    table = [['Algorithm'] + [col[0] for col in columns]]
    for cc in CCS:
        cells = [cc]
        for _, wk, metric in columns:
            values = []
            for scenario in scenarios:
                group = groups.get((scenario['name'], cc, wk), [])
                if (len(group) != REPEATS or any(r['status'] != 'success' for r in group) or
                        {str(r['repeat']) for r in group} != expected_repeats):
                    continue
                try:
                    samples = [float(r[metric]) for r in group]
                except (KeyError, TypeError, ValueError):
                    continue
                if any(not math.isfinite(v) or (v < 0 if wk == 'bulk' else v <= 0) for v in samples):
                    continue
                values.append(statistics.mean(samples))
            if values:
                value = (statistics.mean(values) if wk == 'bulk' else
                         math.exp(math.fsum(math.log(v) for v in values) / len(values)))
                cells.append(f'{value:.3f}{"" if wk == "bulk" else "x"} ({len(values)}/{len(scenarios)})')
            else:
                cells.append(f'- (0/{len(scenarios)})')
        table.append(cells)
    widths = [max(len(row[i]) for row in table) for i in range(len(table[0]))]
    print('\n结果概览  ·  数值后为完整场景数 / 总场景数')
    print('  Norm Power：场景算术均值；FCT Slowdown：场景几何均值。')
    print('  覆盖场景数不同时，算法结果不宜直接比较。')
    for index, row in enumerate(table):
        print('  ' + '  '.join(value.ljust(widths[i]) for i, value in enumerate(row)))
        if index == 0:
            print('  ' + '  '.join('-' * width for width in widths))


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


def atomic_write(path: Path, content: str) -> None:
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                     prefix=f'.{path.name}.', delete=False) as stream:
        temp = Path(stream.name)
        try:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            temp.unlink(missing_ok=True)
            raise
    try:
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


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


def write_report(output: Path, scenarios: list[dict], rows: list[dict], work: dict) -> None:
    ok = sum(row['status'] == 'success' for row in rows)
    expected = len(scenarios) * len(CCS) * len(WORKLOADS) * REPEATS
    text = ['# TCP Trace Bench 实验报告', '', f'- 数据集：{LINES_DIR}',
            f'- 算法：{", ".join(CCS)}', f'- 负载：{", ".join(WORKLOADS)}',
            f'- TCP 数据段大小：{TCP_SEGMENT_SIZE_BYTES} 字节',
            f'- 每组重复：{REPEATS}；计划：{expected}；已运行或恢复：{len(rows)}；成功：{ok}；失败或未完成：{len(rows)-ok}']
    if 'bulk' in WORKLOADS:
        durations = {s['duration_s'] for s in scenarios}
        if len(durations) == 1:
            text.append(f'- Bulk 持续时间：{next(iter(durations)):.3f} s')
        else:
            text.append('- Bulk 持续时间：' + '；'.join(
                f"{s['name']} {s['duration_s']:.3f} s" for s in scenarios))
    text.append('')
    for wk in WORKLOADS:
        text.extend([f'## {wk}', ''])
        if wk == 'bulk':
            fields = ['capacity_mbps', 'throughput_mbps', 'utilization', 'qdelay_avg_ms', 'qdelay_p95_ms', 'util_power', 'norm_power', 'drop_rate']
            text.append('| 场景 | 算法 | 成功/总次数 | 容量 Mbps | 吞吐 Mbps | 利用率 | 平均排队延迟 ms | P95 排队延迟 ms | Power ↑ | Norm Power ↑ | 门控丢包率 |')
        else:
            info = work[wk]
            text.extend([f"文件：`{info['path']}`；数据块数：{info['count']}；到达跨度：{info['span_s']:.3f} s", ''])
            fields = ['duration_s', 'requests', 'rct_mean_ms', 'rct_p95_ms', 'fct_slowdown', 'mortise_qoe']
            text.append('| 场景 | 算法 | 成功/总次数 | Workload 完成时间 s | 完成数据块 | 平均数据块完成时间 ms | P95 数据块完成时间 ms | FCT Slowdown ↓ | Mortise QoE |')
        text.append('|' + '|'.join(['---'] * (3 + len(fields))) + '|')
        for scenario in scenarios:
            for cc in CCS:
                subset = [r for r in rows if r['scenario'] == scenario['name'] and r['cc'] == cc and r['workload'] == wk]
                good = [r for r in subset if r['status'] == 'success']
                text.append('| ' + ' | '.join([scenario['name'], cc, f'{len(good)}/{REPEATS}'] + [mean(good, f) for f in fields]) + ' |')
        text.append('')
    text.extend(ranking_tables(scenarios, rows))
    text.extend(['## 场景与运行配置', ''])
    if any('up_delay' not in s['files'] or 'down_delay' not in s['files'] for s in scenarios):
        text.append(f'- 缺失延迟文件时，单向基础延迟：{DEFAULT_ONE_WAY_DELAY_MS:g} ms。')
    if any(wk != 'bulk' for wk in WORKLOADS):
        text.append(f'- Workload 循环回放 lines，完成时间为全部数据块收到的仿真时刻；最后一块产生后最多等待 {WORKLOAD_DRAIN_TIMEOUT_S:g} s。')
    text.extend(['', '## 失败与未完成', ''])
    failed = [r for r in rows if r['status'] != 'success']
    text.extend([f"- {r['scenario']} / {r['workload']} / {r['cc']} / 第 {r['repeat']} 次：{r['error']}" for r in failed] or ['无。'])
    atomic_write(output / 'report.md', '\n'.join(text) + '\n')


def write_results(output: Path, scenarios: list[dict], rows: list[dict], work: dict) -> None:
    fields = ['scenario', 'workload', 'cc', 'repeat', 'status', 'error', 'duration_s', 'capacity_mbps',
              'throughput_mbps', 'utilization', 'qdelay_avg_ms', 'qdelay_p95_ms', 'base_rtt_ms', 'est_rtt_ms',
              'util_power', 'norm_power', 'drop_rate', 'requests_expected', 'requests', 'rct_mean_ms', 'rct_p50_ms',
              'rct_p95_ms', 'fct_slowdown', 'mortise_qoe']
    stream = io.StringIO(newline='')
    writer = csv.DictWriter(stream, fieldnames=fields, extrasaction='ignore')
    writer.writeheader()
    writer.writerows(rows)
    atomic_write(output / 'runs.csv', stream.getvalue())
    write_report(output, scenarios, rows, work)


def main() -> int:
    lock_handle = None
    progress = None
    started = time.monotonic()
    try:
        if not BINARY.is_file():
            raise ValueError(f'benchmark binary missing: {BINARY}; run make build-bench')
        if (REPEATS < 1 or not isinstance(MAX_WORKERS, int) or MAX_WORKERS < 1
                or not CCS or len(set(CCS)) != len(CCS) or not WORKLOADS or len(set(WORKLOADS)) != len(WORKLOADS)
                or not math.isfinite(WORKLOAD_DRAIN_TIMEOUT_S) or WORKLOAD_DRAIN_TIMEOUT_S <= 0):
            raise ValueError('invalid algorithms, workloads or repeats')
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
        metadata = {'lines_dir': str(LINES_DIR), 'ccs': CCS, 'workloads': {k: {**v, 'path': str(v['path'])} for k, v in work.items()},
                    'repeats': REPEATS, 'max_workers': MAX_WORKERS, 'default_one_way_delay_ms': DEFAULT_ONE_WAY_DELAY_MS,
                    'workload_drain_timeout_s': WORKLOAD_DRAIN_TIMEOUT_S,
                    'scenarios': [{'name': s['name'], 'duration_s': s['duration_s'],
                                   'files': {k: {'path': str(v), 'sha256': digest(v)} for k, v in s['files'].items()}} for s in scenarios],
                    'binary_sha256': digest(BINARY), 'queue_bytes_per_direction': 500000,
                    'tcp_segment_size_bytes': TCP_SEGMENT_SIZE_BYTES,
                    'tcp_configuration': 'ns-3 defaults except segment size; congestion-control implementations may enable pacing'}
        atomic_write(output / 'metadata.json', json.dumps(metadata, indent=2) + '\n')
        scenario_hashes = {s['name']: {k: v['sha256'] for k, v in s['files'].items()}
                           for s in metadata['scenarios']}
        rows_by_index: dict[int, dict] = {}
        pending = []
        index = 0
        for scenario in scenarios:
            for wk in WORKLOADS:
                for cc in CCS:
                    for repeat in range(1, REPEATS + 1):
                        row = {'scenario': scenario['name'], 'workload': wk, 'cc': cc, 'repeat': repeat,
                               'duration_s': scenario['duration_s'], 'status': 'failed', 'error': ''}
                        files = scenario['files']
                        stop_s = (scenario['duration_s'] if wk == 'bulk'
                                  else work[wk]['span_s'] + WORKLOAD_DRAIN_TIMEOUT_S + 0.001)
                        cmd = [str(BINARY), '--profile=lines', f'--upTrace={files["up"]}', f'--downTrace={files["down"]}',
                               f'--stopTime={stop_s}', f'--ccs={cc}', f'--run={repeat}',
                               f'--defaultDelayMs={DEFAULT_ONE_WAY_DELAY_MS}',
                               f'--segmentSize={TCP_SEGMENT_SIZE_BYTES}', '--evalJson=1']
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
                                if saved['fingerprint'] == fingerprint and saved['row']['status'] == 'success':
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
            write_results(output, scenarios, [rows_by_index[i] for i in sorted(rows_by_index)], work)

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
                    row['error'] = f"数据块完成 {data.get('requests', 0)}/{job['expected']}"
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
