#!/usr/bin/env python3
"""Shared ranking and Markdown/CSV report generation for ns-3 and Linux kernel lines benchmarks."""
from __future__ import annotations

import csv
import io
import math
import statistics
import sys
from pathlib import Path

_BENCH_DIR = str(Path(__file__).resolve().parent)
if _BENCH_DIR not in sys.path:
    sys.path.insert(0, _BENCH_DIR)

from lines_common import DEFAULT_ONE_WAY_DELAY_MS, atomic_write

RUNS_CSV_FIELDS = [
    'scenario', 'workload', 'cc', 'repeat', 'status', 'error', 'duration_s', 'capacity_mbps',
    'throughput_mbps', 'utilization', 'qdelay_avg_ms', 'qdelay_p95_ms', 'base_rtt_ms', 'est_rtt_ms',
    'util_power', 'norm_power', 'norm_power_p95', 'drop_rate', 'requests_expected', 'requests',
    'rct_mean_ms', 'rct_p50_ms', 'rct_p95_ms', 'fct_slowdown', 'fct_slowdown_p95', 'mortise_qoe',
]


def mean(rows: list[dict], field: str) -> str:
    values = [float(r[field]) for r in rows if r.get(field) not in ('', None) and math.isfinite(float(r[field]))]
    return f'{statistics.mean(values):.3f}' if values else '-'


def ranking_entries(
    rows: list[dict],
    scenario: str,
    workload_name: str,
    kind: str,
    *,
    ccs: list[str],
    workloads: list[str],
    repeats: int,
) -> list[dict]:
    """Rank complete runs using unrounded per-algorithm averages.

    kind:
      - 'bulk'         : bulk norm_power (higher is better)
      - 'bulk_p95'     : bulk norm_power_p95 (higher is better)
      - 'workload'     : workload fct_slowdown (lower is better)
      - 'workload_p95' : workload fct_slowdown_p95 (lower is better)
      - 'combined'     : sqrt(norm_power_p95 / geomean(workload fct_slowdown_p95)) (higher is better)
    """
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
        complete = (
            len(selected) == repeats
            and len(good) == repeats
            and {str(r['repeat']) for r in good} == {str(i) for i in range(1, repeats + 1)}
        )
        valid = complete and len(values) == repeats
        return (statistics.mean(values) if values else None), valid, len(good)

    is_workload = kind in ('workload', 'workload_p95')
    is_bulk = kind in ('bulk', 'bulk_p95')
    bulk_field = 'norm_power_p95' if kind in ('bulk_p95', 'combined') else 'norm_power'
    slow_field = 'fct_slowdown_p95' if kind in ('workload_p95', 'combined') else 'fct_slowdown'

    entries = []
    for cc in ccs:
        timeout = False
        norm, norm_ok, bulk_count = metric(cc, 'bulk', bulk_field, False)
        wk_stats = (
            {wk: metric(cc, wk, slow_field, True) for wk in (w for w in workloads if w != 'bulk')}
            if kind == 'combined'
            else {}
        )
        slow, slow_ok, workload_count = (
            metric(cc, workload_name, slow_field, True) if is_workload else (None, False, 0)
        )
        if is_bulk:
            value, valid, count = norm, norm_ok, f'{bulk_count}/{repeats}'
        elif is_workload:
            value, valid, count = slow, slow_ok, f'{workload_count}/{repeats}'
            selected = [r for r in rows if r['scenario'] == scenario and r['cc'] == cc and r['workload'] == workload_name]
            timeout = any(r['status'] == 'timeout' for r in selected)
        else:
            if wk_stats and all(v[0] is not None for v in wk_stats.values()):
                slow = math.exp(math.fsum(math.log(v[0]) for v in wk_stats.values()) / len(wk_stats))
            value = math.sqrt(norm / slow) if norm is not None and slow is not None else None
            valid = norm_ok and bool(wk_stats) and all(v[1] for v in wk_stats.values())
            workload_valid = True
            for wk in wk_stats:
                selected = [r for r in rows if r['scenario'] == scenario and r['cc'] == cc and r['workload'] == wk]
                assessed = (
                    len(selected) == repeats
                    and {str(r['repeat']) for r in selected} == {str(i) for i in range(1, repeats + 1)}
                    and all(r['status'] in ('success', 'timeout') for r in selected)
                )
                has_timeout = any(r['status'] == 'timeout' for r in selected)
                timeout |= has_timeout
                success_valid = True
                for r in selected:
                    if r['status'] == 'success':
                        try:
                            sample = float(r[slow_field])
                            success_valid &= math.isfinite(sample) and sample > 0
                        except (KeyError, TypeError, ValueError):
                            success_valid = False
                workload_valid &= assessed and success_valid
            if timeout and norm_ok and workload_valid:
                value, valid = 0.0, True
            count = '; '.join(
                [f'bulk {bulk_count}/{repeats}'] + [f'{wk} {v[2]}/{repeats}' for wk, v in wk_stats.items()]
            )
        entries.append({
            'cc': cc,
            'norm': norm,
            'slow': slow,
            'value': value,
            'workload_slows': {wk: v[0] for wk, v in wk_stats.items()},
            'valid': valid,
            'count': count,
            'rank': '-',
            'gap': '-',
            'status': (
                '超时（0分）' if timeout and (is_workload or valid)
                else '完整' if valid else '不完整或指标无效'
            ),
        })

    lower_better = is_workload
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


def overall_ranking(
    scenarios: list[dict],
    rows: list[dict],
    *,
    ccs: list[str],
    workloads: list[str],
    repeats: int,
) -> tuple[list[dict], list[str]]:
    """Compare algorithms against each scenario's best complete P95 result."""
    if 'bulk' not in workloads:
        return [], ['总排名需要 bulk 结果。']
    if not scenarios:
        return [], ['没有可比较的场景。']
    kind = 'combined' if any(w != 'bulk' for w in workloads) else 'bulk_p95'
    values = {cc: [] for cc in ccs}
    wins = {cc: 0 for cc in ccs}
    completed = {cc: 0 for cc in ccs}
    missing_baselines = []
    for scenario in scenarios:
        entries = ranking_entries(rows, scenario['name'], '', kind, ccs=ccs, workloads=workloads, repeats=repeats)
        qualified = [e for e in entries if e['valid'] and e['value'] is not None]
        best = max((e['value'] for e in qualified), default=0.0)
        if best <= 0:
            missing_baselines.append(scenario['name'])
        for e in qualified:
            values[e['cc']].append(e['value'] / best if best > 0 else None)
            if e['status'] == '完整':
                completed[e['cc']] += 1
            if best > 0 and e['value'] == best:
                wins[e['cc']] += 1
    entries = []
    for cc in ccs:
        ratios = values[cc]
        complete = len(ratios) == len(scenarios)
        score = None
        if complete and not missing_baselines:
            score = (
                0.0 if any(v == 0 for v in ratios)
                else math.exp(math.fsum(math.log(v) for v in ratios) / len(ratios))
            )
        status = ('场景不完整' if not complete else '含超时（0分）' if completed[cc] < len(scenarios) else '完整')
        entries.append({
            'cc': cc, 'complete': completed[cc], 'wins': wins[cc], 'score': score,
            'rank': '-', 'gap': '-', 'status': status,
        })
    if missing_baselines:
        return entries, ['无法计算总排名：以下场景没有有效的正数基准：' + '、'.join(missing_baselines) + '。']
    ranked = sorted((e for e in entries if e['score'] is not None), key=lambda e: (-e['score'], e['cc']))
    if ranked and ranked[0]['score'] > 0:
        best = ranked[0]['score']
        previous = None
        rank = 0
        for index, e in enumerate(ranked, 1):
            if previous is None or e['score'] != previous:
                rank = index
            e['rank'] = str(rank)
            e['gap'] = '基准' if e['score'] == best else f"{(e['score'] - best) / best * 100:+.1f}%"
            previous = e['score']
    elif ranked:
        for e in ranked:
            e['status'] = '总分基准为零，无法排名'
    return ranked + [e for e in entries if e['score'] is None], []


def overall_ranking_table(
    scenarios: list[dict],
    rows: list[dict],
    *,
    ccs: list[str],
    workloads: list[str],
    repeats: int,
) -> list[str]:
    entries, warnings = overall_ranking(scenarios, rows, ccs=ccs, workloads=workloads, repeats=repeats)
    metric = (
        '单场景综合评分（基于 P95 Norm Power 与 P95 FCT Slowdown）'
        if any(w != 'bulk' for w in workloads)
        else '单场景 Bulk P95 Norm Power'
    )
    result = [
        '## 所有场景总排名', '',
        f'每个场景等权：先将 {metric} 除以该场景最优值，再对所有场景的相对得分取几何均值。',
        '全部场景的所选负载及重复均有有效结果时参与排名；workload 超时按零分计算，并列场景最优各计一次。', '',
    ]
    if warnings:
        result.extend(warnings + [''])
    if entries:
        result.extend([
            '| 总排名 | 算法 | 完整场景 | 场景最优次数 | 总评分 ↑ | 相对第一名差距 | 状态 |',
            '|---:|---|---:|---:|---:|---:|---|',
        ])
        for e in entries:
            cells = [
                e['rank'], e['cc'], f"{e['complete']}/{len(scenarios)}", str(e['wins']),
                '-' if e['score'] is None else f"{e['score']:.3f}", e['gap'], e['status'],
            ]
            if e['gap'] == '基准':
                cells = [f'**{cell}**' for cell in cells]
            result.append('| ' + ' | '.join(cells) + ' |')
        result.append('')
    return result


def workload_ranking(
    scenarios: list[dict],
    rows: list[dict],
    workload_name: str,
    *,
    ccs: list[str],
    workloads: list[str],
    repeats: int,
) -> tuple[list[dict], list[str]]:
    """Compare one workload across scenarios using P95 metrics against each scenario's best."""
    if not scenarios:
        return [], ['没有可比较的场景。']
    bulk = workload_name == 'bulk'
    ratios = {cc: [] for cc in ccs}
    completed = {cc: 0 for cc in ccs}
    wins = {cc: 0 for cc in ccs}
    missing = []
    for scene in scenarios:
        name = scene['name']
        entries = ranking_entries(
            rows, name, workload_name, 'bulk_p95' if bulk else 'workload_p95',
            ccs=ccs, workloads=workloads, repeats=repeats,
        )
        valid = [e for e in entries if e['valid'] and e['value'] is not None]
        best = (max(e['value'] for e in valid) if bulk else min(e['value'] for e in valid)) if valid else 0
        if best <= 0:
            missing.append(name)
        for e in entries:
            cc = e['cc']
            if e['valid'] and e['value'] is not None:
                completed[cc] += 1
                ratios[cc].append((e['value'] / best if bulk else best / e['value']) if best > 0 else None)
                if best > 0 and e['value'] == best:
                    wins[cc] += 1
            elif not bulk:
                selected = [r for r in rows if r['scenario'] == name and r['cc'] == cc and r['workload'] == workload_name]
                assessed = (
                    len(selected) == repeats
                    and {str(r['repeat']) for r in selected} == {str(i) for i in range(1, repeats + 1)}
                    and all(r['status'] in ('success', 'timeout') for r in selected)
                )
                good_values = []
                for r in selected:
                    if r['status'] == 'success':
                        try:
                            v = float(r['fct_slowdown_p95'])
                            if math.isfinite(v) and v > 0:
                                good_values.append(v)
                        except (KeyError, TypeError, ValueError):
                            pass
                if (
                    assessed
                    and any(r['status'] == 'timeout' for r in selected)
                    and len(good_values) == sum(r['status'] == 'success' for r in selected)
                ):
                    ratios[cc].append(0.0)
    result = []
    for cc in ccs:
        values = ratios[cc]
        assessed = len(values) == len(scenarios)
        score = None
        if assessed and not missing:
            score = (
                0.0 if any(v == 0 for v in values)
                else math.exp(math.fsum(math.log(v) for v in values) / len(values))
            )
        result.append({
            'cc': cc, 'score': score, 'complete': completed[cc], 'wins': wins[cc],
            'rank': '-', 'gap': '-',
            'status': ('场景不完整' if not assessed else '含超时（0分）' if completed[cc] < len(scenarios) else '完整'),
        })
    if missing:
        return result, ['无法计算总分：以下场景没有有效的正数基准：' + '、'.join(missing) + '。']
    ranked = sorted((e for e in result if e['score'] is not None), key=lambda e: (-e['score'], e['cc']))
    if ranked and ranked[0]['score'] > 0:
        best = ranked[0]['score']
        previous = None
        rank = 0
        for index, e in enumerate(ranked, 1):
            if previous is None or e['score'] != previous:
                rank = index
            e['rank'] = str(rank)
            e['gap'] = '基准' if e['score'] == best else f"{(e['score'] - best) / best * 100:+.1f}%"
            previous = e['score']
    elif ranked:
        for e in ranked:
            e['status'] = '总分基准为零，无法排名'
    return ranked + [e for e in result if e['score'] is None], []


def workload_ranking_tables(
    scenarios: list[dict],
    rows: list[dict],
    *,
    ccs: list[str],
    workloads: list[str],
    repeats: int,
) -> list[str]:
    result = [
        '## 各负载所有场景排名', '',
        '每个场景等权，先将 P95 指标换算为相对该场景最优值的得分，再对所有场景取几何均值。',
        'Bulk 得分为算法 P95 Norm Power / 最优 P95 Norm Power；其他负载得分为最优 P95 FCT Slowdown / 算法 P95 FCT Slowdown。',
        'workload 超时按 0 分计算；进程错误或缺失结果不参与排名。', '',
    ]
    for wk in workloads:
        entries, warnings = workload_ranking(scenarios, rows, wk, ccs=ccs, workloads=workloads, repeats=repeats)
        result.extend([f'### {wk}：所有场景排名（P95）', ''])
        if warnings:
            result.extend(warnings + [''])
        if entries:
            result.extend([
                '| 排名 | 算法 | 完成场景 | 场景最优次数 | 总评分 ↑ | 相对第一名差距 | 状态 |',
                '|---:|---|---:|---:|---:|---:|---|',
            ])
            for e in entries:
                cells = [
                    e['rank'], e['cc'], f"{e['complete']}/{len(scenarios)}", str(e['wins']),
                    '-' if e['score'] is None else f"{e['score']:.3f}", e['gap'], e['status'],
                ]
                if e['gap'] == '基准':
                    cells = [f'**{cell}**' for cell in cells]
                result.append('| ' + ' | '.join(cells) + ' |')
            result.append('')
    return result


def ranking_tables(
    scenarios: list[dict],
    rows: list[dict],
    *,
    ccs: list[str],
    workloads: list[str],
    repeats: int,
) -> list[str]:
    result = [
        '## 算法排名', '',
        '每个场景分别排名；指标为各次成功运行的算术平均，使用未四舍五入的值计算。',
        '只有对应组合的所有重复均成功且指标有效时才参与排名；并列值共享排名。', '',
    ]

    def fmt(value: float | None, suffix: str = '') -> str:
        return '-' if value is None else f'{value:.3f}{suffix}'

    def row(cells: list[str], entry: dict) -> str:
        if entry['gap'] == '基准':
            cells = [f'**{cell}**' for cell in cells]
        return '| ' + ' | '.join(cells) + ' |'

    if 'bulk' in workloads:
        result.extend([
            '### Bulk：P95 Norm Power', '',
            '| 场景 | 排名 | 算法 | 成功/总次数 | P95 Norm Power ↑ | 相对最优差距 | 状态 |',
            '|---|---:|---|---:|---:|---:|---|',
        ])
        for scenario in scenarios:
            for e in ranking_entries(rows, scenario['name'], 'bulk', 'bulk_p95', ccs=ccs, workloads=workloads, repeats=repeats):
                result.append(row([scenario['name'], e['rank'], e['cc'], e['count'], fmt(e['norm']), e['gap'], e['status']], e))
        result.append('')
        result.extend([
            '### Bulk：Norm Power（均值参考）', '',
            '| 场景 | 排名 | 算法 | 成功/总次数 | Norm Power ↑ | 相对最优差距 | 状态 |',
            '|---|---:|---|---:|---:|---:|---|',
        ])
        for scenario in scenarios:
            for e in ranking_entries(rows, scenario['name'], 'bulk', 'bulk', ccs=ccs, workloads=workloads, repeats=repeats):
                result.append(row([scenario['name'], e['rank'], e['cc'], e['count'], fmt(e['norm']), e['gap'], e['status']], e))
        result.append('')
    for wk in (w for w in workloads if w != 'bulk'):
        result.extend([
            f'### {wk}：P95 FCT Slowdown', '',
            '| 场景 | 排名 | 算法 | 成功/总次数 | P95 FCT Slowdown ↓ | 相对最优差距 | 状态 |',
            '|---|---:|---|---:|---:|---:|---|',
        ])
        for scenario in scenarios:
            for e in ranking_entries(rows, scenario['name'], wk, 'workload_p95', ccs=ccs, workloads=workloads, repeats=repeats):
                result.append(row([scenario['name'], e['rank'], e['cc'], e['count'], fmt(e['slow'], 'x'), e['gap'], e['status']], e))
        result.append('')
        result.extend([
            f'### {wk}：FCT Slowdown（几何均值参考）', '',
            '| 场景 | 排名 | 算法 | 成功/总次数 | FCT Slowdown ↓ | 相对最优差距 | 状态 |',
            '|---|---:|---|---:|---:|---:|---|',
        ])
        for scenario in scenarios:
            for e in ranking_entries(rows, scenario['name'], wk, 'workload', ccs=ccs, workloads=workloads, repeats=repeats):
                result.append(row([scenario['name'], e['rank'], e['cc'], e['count'], fmt(e['slow'], 'x'), e['gap'], e['status']], e))
        result.append('')
    non_bulk = [w for w in workloads if w != 'bulk']
    if 'bulk' in workloads and non_bulk:
        headers = ['场景', '排名', '算法', '成功/总次数', 'P95 Norm Power']
        headers.extend(f'{wk} P95 Slowdown' for wk in non_bulk)
        headers.extend(['P95 Slowdown 几何均值', '综合评分 ↑', '相对最优差距', '状态'])
        result.extend([
            '### 综合排名（基于 P95）', '',
            '各 workload 的 P95 FCT Slowdown 重复运行先取算术平均，再对所有 workload 等权取几何均值；综合评分 = sqrt(P95 Norm Power / P95 Slowdown 几何均值)。',
            '所有 workload 和 bulk 均需完成全部重复；workload 超时的场景综合评分为 0。', '',
            '| ' + ' | '.join(headers) + ' |',
            '|' + '|'.join(['---'] * len(headers)) + '|',
        ])
        for scenario in scenarios:
            for e in ranking_entries(rows, scenario['name'], '', 'combined', ccs=ccs, workloads=workloads, repeats=repeats):
                cells = [scenario['name'], e['rank'], e['cc'], e['count'], fmt(e['norm'])]
                cells.extend(fmt(e['workload_slows'].get(wk), 'x') for wk in non_bulk)
                cells.extend([fmt(e['slow'], 'x'), fmt(e['value']), e['gap'], e['status']])
                result.append(row(cells, e))
        result.append('')
    result.extend(workload_ranking_tables(scenarios, rows, ccs=ccs, workloads=workloads, repeats=repeats))
    result.extend(overall_ranking_table(scenarios, rows, ccs=ccs, workloads=workloads, repeats=repeats))
    return result


def render_report(
    scenarios: list[dict],
    rows: list[dict],
    work: dict,
    *,
    title: str = 'TCP Trace Bench 实验报告',
    dataset_label: str,
    ccs: list[str],
    workloads: list[str],
    repeats: int,
    tcp_segment_size_bytes: int | None = 1448,
    default_one_way_delay_ms: float = DEFAULT_ONE_WAY_DELAY_MS,
    workload_max_s: float = 1800.0,
    workload_timeout_label: str = '仿真时间',
    extra_header_lines: list[str] | None = None,
    extra_config_lines: list[str] | None = None,
) -> str:
    ok = sum(row['status'] == 'success' for row in rows)
    expected = len(scenarios) * len(ccs) * len(workloads) * repeats
    text = [
        f'# {title}', '',
        f'- 数据集：{dataset_label}',
        f'- 算法：{", ".join(ccs)}',
        f'- 负载：{", ".join(workloads)}',
    ]
    if tcp_segment_size_bytes is not None:
        text.append(f'- TCP 数据段大小：{tcp_segment_size_bytes} 字节')
    if extra_header_lines:
        text.extend(extra_header_lines)
    text.append(
        f'- 每组重复：{repeats}；计划：{expected}；已运行或恢复：{len(rows)}；成功：{ok}；失败或未完成：{len(rows) - ok}'
    )
    if 'bulk' in workloads and scenarios:
        durations = {s['duration_s'] for s in scenarios}
        if len(durations) == 1:
            text.append(f'- Bulk 持续时间：{next(iter(durations)):.3f} s')
        else:
            text.append(
                '- Bulk 持续时间：' + '；'.join(f"{s['name']} {s['duration_s']:.3f} s" for s in scenarios)
            )
    text.append('')
    for wk in workloads:
        text.extend([f'## {wk}', ''])
        if wk == 'bulk':
            fields = [
                'capacity_mbps', 'throughput_mbps', 'utilization', 'qdelay_avg_ms',
                'qdelay_p95_ms', 'util_power', 'norm_power', 'norm_power_p95', 'drop_rate',
            ]
            text.append(
                '| 场景 | 算法 | 成功/总次数 | 容量 Mbps | 吞吐 Mbps | 利用率 | 平均排队延迟 ms | P95 排队延迟 ms | Power ↑ | Norm Power ↑ | P95 Norm Power ↑ | 门控丢包率 |'
            )
        else:
            info = work.get(wk)
            if info:
                text.extend([f"文件：`{info['path']}`；数据块数：{info['count']}；到达跨度：{info['span_s']:.3f} s", ''])
            fields = ['duration_s', 'requests', 'rct_mean_ms', 'rct_p95_ms', 'fct_slowdown', 'fct_slowdown_p95', 'mortise_qoe']
            text.append(
                '| 场景 | 算法 | 成功/总次数 | Workload 完成时间 s | 完成数据块 | 平均数据块完成时间 ms | P95 数据块完成时间 ms | FCT Slowdown ↓ | P95 FCT Slowdown ↓ | Mortise QoE |'
            )
        text.append('|' + '|'.join(['---'] * (3 + len(fields))) + '|')
        for scenario in scenarios:
            for cc in ccs:
                subset = [r for r in rows if r['scenario'] == scenario['name'] and r['cc'] == cc and r['workload'] == wk]
                good = [r for r in subset if r['status'] == 'success']
                text.append(
                    '| ' + ' | '.join([scenario['name'], cc, f'{len(good)}/{repeats}'] + [mean(good, f) for f in fields]) + ' |'
                )
        text.append('')
    text.extend(ranking_tables(scenarios, rows, ccs=ccs, workloads=workloads, repeats=repeats))
    text.extend(['## 场景与运行配置', ''])
    if any('up_delay' not in s.get('files', {}) or 'down_delay' not in s.get('files', {}) for s in scenarios):
        text.append(f'- 缺失延迟文件时，单向基础延迟：{default_one_way_delay_ms:g} ms。')
    if any(wk != 'bulk' for wk in workloads):
        text.append(
            f'- Workload 循环回放 lines；全部数据块收到时立即结束，最多运行 {workload_max_s:g} s {workload_timeout_label}，超时记 0 分。'
        )
    if extra_config_lines:
        text.extend(extra_config_lines)
    text.extend(['', '## 失败与未完成', ''])
    failed = [r for r in rows if r['status'] != 'success']
    text.extend(
        [f"- {r['scenario']} / {r['workload']} / {r['cc']} / 第 {r['repeat']} 次：{r['error']}" for r in failed]
        or ['无。']
    )
    return '\n'.join(text) + '\n'


def write_runs_csv(path: Path, rows: list[dict], extra_fields: list[str] | None = None) -> None:
    fields = list(RUNS_CSV_FIELDS)
    if extra_fields:
        for f in extra_fields:
            if f not in fields:
                fields.append(f)
    stream = io.StringIO(newline='')
    writer = csv.DictWriter(stream, fieldnames=fields, extrasaction='ignore')
    writer.writeheader()
    writer.writerows(rows)
    atomic_write(path, stream.getvalue())


def print_overview(
    scenarios: list[dict],
    rows: list[dict],
    *,
    ccs: list[str],
    workloads: list[str],
    repeats: int,
) -> None:
    """Print equal-weight scenario summaries, excluding incomplete groups."""
    groups: dict[tuple[str, str, str], list[dict]] = {}
    for row in rows:
        groups.setdefault((row['scenario'], row['cc'], row['workload']), []).append(row)
    expected_repeats = {str(i) for i in range(1, repeats + 1)}
    columns = [('Norm Power', 'bulk', 'norm_power'), ('P95 Norm Power', 'bulk', 'norm_power_p95')] if 'bulk' in workloads else []
    for wk in workloads:
        if wk != 'bulk':
            columns.append((f'{wk} FCT', wk, 'fct_slowdown'))
            columns.append((f'{wk} P95 FCT', wk, 'fct_slowdown_p95'))
    table = [['Algorithm'] + [col[0] for col in columns]]
    for cc in ccs:
        cells = [cc]
        for _, wk, metric in columns:
            values = []
            for scenario in scenarios:
                group = groups.get((scenario['name'], cc, wk), [])
                if (
                    len(group) != repeats
                    or any(r['status'] != 'success' for r in group)
                    or {str(r['repeat']) for r in group} != expected_repeats
                ):
                    continue
                try:
                    samples = [float(r[metric]) for r in group]
                except (KeyError, TypeError, ValueError):
                    continue
                if any(not math.isfinite(v) or (v < 0 if wk == 'bulk' else v <= 0) for v in samples):
                    continue
                values.append(statistics.mean(samples))
            if values:
                value = (
                    statistics.mean(values) if wk == 'bulk'
                    else math.exp(math.fsum(math.log(v) for v in values) / len(values))
                )
                cells.append(f'{value:.3f}{"" if wk == "bulk" else "x"} ({len(values)}/{len(scenarios)})')
            else:
                cells.append(f'- (0/{len(scenarios)})')
        table.append(cells)
    widths = [max(len(row[i]) for row in table) for i in range(len(table[0]))]
    print('\n结果概览  ·  数值后为完整场景数 / 总场景数')
    print('  Norm Power / P95 Norm Power：场景算术均值；FCT / P95 FCT Slowdown：场景几何均值。')
    print('  覆盖场景数不同时，算法结果不宜直接比较。')
    for index, row in enumerate(table):
        print('  ' + '  '.join(value.ljust(widths[i]) for i, value in enumerate(row)))
        if index == 0:
            print('  ' + '  '.join('-' * width for width in widths))
