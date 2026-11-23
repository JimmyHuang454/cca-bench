import importlib.util
import contextlib
import csv
import io
import tempfile
import json
import os
import signal
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('trace_bench', ROOT / 'scripts/bench/run_trace_bench.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class TraceBenchTests(unittest.TestCase):
    def test_mixed_bitrate_units_capacity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene = root / 'mixed.csv'
            scene.write_text('time,up_cap,down_cap\n0,24mbps,120mbps\n1s,40mbps,\n2s,50.11mbps,\n3s,0,0\n')
            parsed = bench.parse_line_csv(scene)
            self.assertEqual(parsed['end_ms'], 3000)
            self.assertEqual(parsed['up'][0][1], 2)
            self.assertAlmostEqual(parsed['up'][2][1] * 12, 50.11)
            for bad in (
                'time,cap\n0,2.5\n1s,0\n',
                'time,cap\n0,NaNmbps\n1s,0\n',
                'time,cap\n0,40m\n1s,0\n',
                'time,cap\n0,5\n1s,0\n',
            ):
                scene.write_text(bad)
                with self.assertRaises(ValueError):
                    bench.parse_line_csv(scene)
            scene.write_text('time,up_cap,down_cap\n0,24mbps,120mbps\n1s,40mbps,\n2s,50.11mbps,\n3s,0,0\n')
            if not bench.BINARY.is_file():
                self.skipTest('build tcp-trace-bench first')
            result = subprocess.run([str(bench.BINARY), '--profile=lines', f'--lineFile={scene}',
                                     '--stopTime=3', '--ccs=cubic',
                                     '--segmentSize=1448', '--evalJson=1'],
                                    text=True, capture_output=True, check=True)
            data = json.loads(result.stdout)
            self.assertAlmostEqual(data['capacity_mbps'], (24 + 40 + 50.11) / 3, places=5)
            self.assertGreater(data['throughput_mbps'], 10)

    def test_bitrate_unit_capacity_bps_kbps_mbps_gbps(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene = root / 'units.csv'
            csv_text = (
                'time,up_cap,down_cap\n'
                '0,24000000bps,100mbps\n'
                '1s,24000kbps,\n'
                '2s,64mbps,\n'
                '3s,0.1gbps,\n'
                '4s,40MBPS,\n'
                '5s,0kbps,0\n'
            )
            scene.write_text(csv_text)
            parsed = bench.parse_line_csv(scene)
            self.assertEqual(parsed['end_ms'], 5000)
            # 0->24Mbps, 1s->24000kbps (=24Mbps, deduped), 2s->64Mbps, 3s->100Mbps, 4s->40Mbps, 5s->0
            self.assertAlmostEqual(parsed['up'][0][1], 2.0, places=9)
            self.assertAlmostEqual(parsed['up'][1][1], 64.0 / 12.0, places=9)
            self.assertAlmostEqual(parsed['up'][2][1], 100.0 / 12.0, places=9)
            self.assertAlmostEqual(parsed['up'][3][1], 40.0 / 12.0, places=9)
            self.assertEqual(parsed['up'][4][1], 0.0)

            for bad in (
                'time,cap\n0,2.5\n1s,0\n',
                'time,cap\n0,2.0\n1s,0\n',
                'time,cap\n0,NaNmbps\n1s,0\n',
                'time,cap\n0,40m\n1s,0\n',
                'time,cap\n0,10.33kb\n1s,0\n',
                'time,cap\n0,266.88b\n1s,0\n',
                'time,cap\n0,40k\n1s,0\n',
                'time,cap\n0,40M\n1s,0\n',
                'time,cap\n0,10 mbps\n1s,0\n',
                'time,cap\n0,-1mbps\n1s,0\n',
            ):
                scene.write_text(bad)
                with self.assertRaises(ValueError, msg=f'expected ValueError for {bad!r}'):
                    bench.parse_line_csv(scene)

            scene.write_text(csv_text)
            if not bench.BINARY.is_file():
                self.skipTest('build tcp-trace-bench first')
            result = subprocess.run([str(bench.BINARY), '--profile=lines', f'--lineFile={scene}',
                                     '--stopTime=5', '--ccs=cubic',
                                     '--segmentSize=1448', '--evalJson=1'],
                                    text=True, capture_output=True, check=True)
            data = json.loads(result.stdout)
            expected_mbps = (24.0 + 24.0 + 64.0 + 100.0 + 40.0) / 5.0
            self.assertAlmostEqual(data['capacity_mbps'], expected_mbps, places=4)
            self.assertGreater(data['throughput_mbps'], 20)

    def test_default_delay_without_delay_columns_uses_python_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene = root / 'no_delay_5g.csv'
            # 800 Mbps; no delay column -> uses DEFAULT_ONE_WAY_DELAY_MS
            scene.write_text('time,cap\n0,800mbps\n2s,0\n')
            discovered = bench.discover(scene)[0]
            self.assertEqual(set(discovered['files']), {'line'})
            self.assertFalse(discovered['has_up_delay'])
            self.assertFalse(discovered['has_down_delay'])
            with patch.multiple(bench, QUEUE_BYTES='auto', DEFAULT_ONE_WAY_DELAY_MS=25.0):
                q25 = bench.scenario_queue_bytes(discovered)
            with patch.multiple(bench, QUEUE_BYTES='auto', DEFAULT_ONE_WAY_DELAY_MS=50.0):
                q50 = bench.scenario_queue_bytes(discovered)
            self.assertGreater(q50, q25)
            if not bench.BINARY.is_file():
                self.skipTest('build tcp-trace-bench first')
            result = subprocess.run([
                str(bench.BINARY), '--profile=lines', f'--lineFile={scene}',
                '--defaultDelayMs=25.0', '--stopTime=1', '--ccs=cubic',
                '--segmentSize=1448', '--evalJson=1',
            ], text=True, capture_output=True, check=True)
            data = json.loads(result.stdout)
            self.assertAlmostEqual(data['base_rtt_ms'], 50.0, places=3)

    def test_discover_csv_directory_and_flat_scenarios(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene = root / 'wired_fixed'
            scene.mkdir()
            (scene / 'line.csv').write_text('time,cap,delay\n0,12mbps,5\n100,0,\n')
            scenario = bench.discover(scene)[0]
            self.assertEqual(scenario['name'], 'wired_fixed')
            self.assertEqual(set(scenario['files']), {'line'})
            self.assertTrue(scenario['has_up_delay'])
            self.assertTrue(scenario['has_down_delay'])
            self.assertEqual(bench.discover(root), [scenario])
            (scene / 'line.csv').unlink()
            with self.assertRaisesRegex(ValueError, 'no CSV line scenarios'):
                bench.discover(root)

    def test_discover_dataset_and_individual_scenario(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'datasets/lines/example'
            dataset.mkdir(parents=True)
            (dataset / 'a.csv').write_text('time,cap,up_delay\n0,12mbps,20\n10,0,\n')
            scene_b = dataset / 'b'
            scene_b.mkdir()
            (scene_b / 'line.csv').write_text('time,cap\n0,12mbps\n10,0\n')
            all_scenes = bench.discover(dataset)
            self.assertEqual([s['name'] for s in all_scenes], ['a', 'b'])
            self.assertEqual(bench.discover(dataset / 'a.csv'), [all_scenes[0]])
            self.assertEqual(bench.discover(dataset / 'b'), [all_scenes[1]])
            with patch.multiple(bench, ROOT=root, RESULTS_DIR=root / 'results'):
                self.assertEqual(bench.result_directory(dataset), root / 'results/example')
                self.assertEqual(bench.result_directory(dataset / 'a.csv'), root / 'results/example/a')
                self.assertEqual(bench.result_directory(dataset / 'b'), root / 'results/example/b')

    def test_completed_task_output_includes_metrics_and_failures(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            progress = bench.Progress(2, 0, time.monotonic())
            progress.completed({'scenario': 'line-a', 'cc': 'copa', 'workload': 'bulk',
                                'repeat': 1, 'status': 'success',
                                'throughput_mbps': 10.5, 'norm_power': 0.8}, 0.12)
            progress.completed({'scenario': 'line-b', 'cc': 'bbr', 'workload': 'app',
                                'repeat': 2, 'status': 'failed', 'error': '超时'}, 0.34)
        self.assertIn('[完成] line-a  |  copa  |  bulk  |  第 1 次', out.getvalue())
        self.assertIn('吞吐 10.500 Mbps', out.getvalue())
        self.assertIn('Norm Power 0.800', out.getvalue())
        self.assertIn('[失败] line-b  |  bbr  |  app  |  第 2 次', err.getvalue())
        self.assertIn('超时', err.getvalue())

    def test_console_overview_uses_complete_scenarios(self):
        rows = []
        for scenario, norm, slow in [('a', 0.4, 1.0), ('b', 0.8, 4.0)]:
            rows.extend([
                {'scenario': scenario, 'cc': 'copa', 'workload': 'bulk', 'repeat': 1,
                 'status': 'success', 'norm_power': norm},
                {'scenario': scenario, 'cc': 'copa', 'workload': 'websearch', 'repeat': 1,
                 'status': 'success' if scenario == 'a' else 'failed', 'fct_slowdown': slow},
            ])
        out = io.StringIO()
        with patch.multiple(bench, CCS=['copa'], WORKLOADS=['bulk', 'websearch'], REPEATS=1), \
                contextlib.redirect_stdout(out):
            bench.print_overview([{'name': 'a'}, {'name': 'b'}], rows)
        self.assertIn('0.600 (2/2)', out.getvalue())
        self.assertIn('1.000x (1/2)', out.getvalue())

    def test_rankings_use_complete_unrounded_scenario_and_workload_values(self):
        rows = []
        for cc, norm, slow in [('best', 0.8, 2.0), ('tie', 0.8, 1.0), ('partial', 0.7, 3.0)]:
            for repeat in (1, 2):
                rows.append({'scenario': 'a', 'cc': cc, 'repeat': repeat, 'workload': 'bulk',
                             'status': 'success' if cc != 'partial' or repeat == 1 else 'failed',
                             'norm_power': norm, 'norm_power_p95': norm})
                rows.append({'scenario': 'a', 'cc': cc, 'repeat': repeat, 'workload': 'websearch',
                             'status': 'success', 'fct_slowdown': slow, 'fct_slowdown_p95': slow})
                rows.append({'scenario': 'a', 'cc': cc, 'repeat': repeat, 'workload': 'pageload',
                             'status': 'success', 'fct_slowdown': 4.0 if cc == 'tie' else 1.0,
                             'fct_slowdown_p95': 4.0 if cc == 'tie' else 1.0})
        with patch.multiple(bench, CCS=['best', 'tie', 'partial'], REPEATS=2,
                            WORKLOADS=['bulk', 'websearch', 'pageload']):
            bulk = bench.ranking_entries(rows, 'a', 'bulk', 'bulk')
            self.assertEqual([(e['cc'], e['rank']) for e in bulk],
                             [('best', '1'), ('tie', '1'), ('partial', '-')])
            self.assertEqual(bulk[2]['status'], '不完整或指标无效')
            web = bench.ranking_entries(rows, 'a', 'websearch', 'workload')
            self.assertEqual((web[0]['cc'], web[0]['gap']), ('tie', '基准'))
            self.assertEqual((web[1]['cc'], web[1]['gap']), ('best', '+100.0%'))
            page = bench.ranking_entries(rows, 'a', 'pageload', 'workload')
            self.assertEqual(page[-1]['cc'], 'tie')
            combined = bench.ranking_entries(rows, 'a', '', 'combined')
            self.assertEqual(combined[0]['cc'], 'best')
            self.assertAlmostEqual(combined[0]['slow'], (2.0 * 1.0) ** 0.5)
            self.assertAlmostEqual(combined[0]['value'], (0.8 / (2.0 ** 0.5)) ** 0.5)
            self.assertEqual(combined[-1]['rank'], '-')
            incomplete = [r for r in rows if not (r['cc'] == 'tie' and
                          r['workload'] == 'pageload' and r['repeat'] == 2)]
            combined = bench.ranking_entries(incomplete, 'a', '', 'combined')
            self.assertEqual(next(e for e in combined if e['cc'] == 'tie')['rank'], '-')
            tables = '\n'.join(bench.ranking_tables([{'name': 'a'}], rows))
            self.assertEqual(tables.count('### 综合排名（基于 P95）'), 1)
            self.assertIn('websearch P95 Slowdown', tables)
            self.assertIn('pageload P95 Slowdown', tables)

    def test_ranking_zero_and_missing_metrics_have_no_false_winner(self):
        rows = [{'scenario': 'a', 'cc': cc, 'repeat': 1, 'workload': 'bulk',
                 'status': 'success', 'norm_power': value}
                for cc, value in [('zero', 0), ('invalid', float('nan'))]]
        with patch.multiple(bench, CCS=['zero', 'invalid'], REPEATS=1):
            ranked = bench.ranking_entries(rows, 'a', 'bulk', 'bulk')
        self.assertEqual(ranked[0]['rank'], '-')
        self.assertEqual(ranked[0]['status'], '基准为零，无法排名')
        self.assertEqual(ranked[1]['rank'], '-')

    def test_rankings_bold_all_tied_baselines_only(self):
        rows = []
        for cc in ('a', 'b', 'incomplete'):
            rows.extend([
                {'scenario': 'scene', 'cc': cc, 'workload': 'bulk', 'repeat': 1,
                 'status': 'success' if cc != 'incomplete' else 'failed', 'norm_power': 0.5, 'norm_power_p95': 0.5},
                {'scenario': 'scene', 'cc': cc, 'workload': 'websearch', 'repeat': 1,
                 'status': 'success', 'fct_slowdown': 2.0, 'fct_slowdown_p95': 2.0},
            ])
        with patch.multiple(bench, CCS=['a', 'b', 'incomplete'], REPEATS=1,
                            WORKLOADS=['bulk', 'websearch']):
            tables = '\n'.join(bench.ranking_tables([{'name': 'scene'}], rows))
        # 5 tables before 各负载所有场景排名: bulk_p95(2) + bulk(2) + web_p95(3) + web(3) + combined(2) = 12
        self.assertEqual(tables.split('## 各负载所有场景排名')[0].count('**基准**'), 12)
        self.assertIn('| **scene** | **1** | **a** | **1/1** | **0.500** | **基准** | **完整** |', tables)
        self.assertIn('| scene | - | incomplete | 0/1 |', tables)

    def test_combined_ranking_uses_p95_metrics_not_means(self):
        rows = [
            # spiky has better mean norm_power & mean fct_slowdown, but worse P95 on both
            {'scenario': 'a', 'cc': 'spiky', 'workload': 'bulk', 'repeat': 1, 'status': 'success',
             'norm_power': 0.9, 'norm_power_p95': 0.2},
            {'scenario': 'a', 'cc': 'spiky', 'workload': 'app', 'repeat': 1, 'status': 'success',
             'fct_slowdown': 1.1, 'fct_slowdown_p95': 8.0},
            # steady has slightly lower mean, but much better P95 on both
            {'scenario': 'a', 'cc': 'steady', 'workload': 'bulk', 'repeat': 1, 'status': 'success',
             'norm_power': 0.7, 'norm_power_p95': 0.6},
            {'scenario': 'a', 'cc': 'steady', 'workload': 'app', 'repeat': 1, 'status': 'success',
             'fct_slowdown': 1.3, 'fct_slowdown_p95': 1.5},
        ]
        with patch.multiple(bench, CCS=['spiky', 'steady'], REPEATS=1, WORKLOADS=['bulk', 'app']):
            combined = bench.ranking_entries(rows, 'a', '', 'combined')
            self.assertEqual(combined[0]['cc'], 'steady')
            self.assertAlmostEqual(combined[0]['value'], (0.6 / 1.5) ** 0.5)
            overall, warnings = bench.overall_ranking([{'name': 'a'}], rows)
            self.assertFalse(warnings)
            self.assertEqual(overall[0]['cc'], 'steady')

    def test_workload_ranking_uses_correct_direction_and_timeout(self):
        rows = []
        for scene, norms, slows in [('a', (.8, .4), (1, 2)), ('b', (.4, .8), (4, 2))]:
            for cc, norm, slow in zip(('x', 'y'), norms, slows):
                rows.extend([{'scenario': scene, 'cc': cc, 'workload': 'bulk', 'repeat': 1, 'status': 'success',
                              'norm_power': norm, 'norm_power_p95': norm},
                             {'scenario': scene, 'cc': cc, 'workload': 'app', 'repeat': 1, 'status': 'success',
                              'fct_slowdown': slow, 'fct_slowdown_p95': slow}])
        scenes = [{'name': 'a'}, {'name': 'b'}]
        with patch.multiple(bench, CCS=['x', 'y'], REPEATS=1, WORKLOADS=['bulk', 'app']):
            for wk in ('bulk', 'app'):
                entries, warnings = bench.workload_ranking(scenes, rows, wk)
                self.assertFalse(warnings)
                self.assertEqual([e['rank'] for e in entries], ['1', '1'])
                self.assertAlmostEqual(entries[0]['score'], 2 ** -.5)
            next(r for r in rows if r['scenario'] == 'a' and r['cc'] == 'x' and r['workload'] == 'app')['status'] = 'timeout'
            entries, warnings = bench.workload_ranking(scenes, rows, 'app')
            self.assertFalse(warnings)
            self.assertEqual(entries[-1]['score'], 0)
            self.assertEqual(entries[-1]['complete'], 1)
            self.assertEqual(entries[-1]['status'], '含超时（0分）')
            next(r for r in rows if r['scenario'] == 'a' and r['cc'] == 'y' and r['workload'] == 'app')['status'] = 'timeout'
            self.assertIn('a', bench.workload_ranking(scenes, rows, 'app')[1][0])

    def test_overall_ranking_combines_scenarios_and_requires_completion(self):
        rows = []
        for scene, a, b in [('one', .8, .4), ('two', .3, .4)]:
            for cc, norm in [('a', a), ('b', b), ('partial', .6)]:
                rows.append({'scenario': scene, 'cc': cc, 'workload': 'bulk', 'repeat': 1,
                             'status': 'failed' if cc == 'partial' and scene == 'two' else 'success',
                             'norm_power': norm, 'norm_power_p95': norm})
                rows.append({'scenario': scene, 'cc': cc, 'workload': 'websearch', 'repeat': 1,
                             'status': 'success', 'fct_slowdown': 1.0, 'fct_slowdown_p95': 1.0})
        scenes = [{'name': 'one'}, {'name': 'two'}]
        with patch.multiple(bench, CCS=['a', 'b', 'partial'], REPEATS=1, WORKLOADS=['bulk', 'websearch']):
            entries, warnings = bench.overall_ranking(scenes, rows)
            self.assertEqual(warnings, [])
            self.assertEqual(entries[-1]['cc'], 'partial')
            self.assertEqual(entries[-1]['complete'], 1)
            self.assertIsNone(entries[-1]['score'])
            self.assertGreater(entries[0]['score'], entries[1]['score'])
            self.assertEqual(entries[0]['rank'], '1')
            rows = [r for r in rows if r['scenario'] != 'two']
            entries, warnings = bench.overall_ranking(scenes, rows)
            self.assertIn('two', warnings[0])
            self.assertTrue(all(e['score'] is None for e in entries))

    def test_overall_ranking_bulk_ties_zeros_and_missing_bulk(self):
        scenes = [{'name': 'a'}, {'name': 'b'}]
        rows = [{'scenario': scene, 'cc': cc, 'workload': 'bulk', 'repeat': 1,
                 'status': 'success', 'norm_power': value, 'norm_power_p95': value}
                for scene, vals in [('a', (1, .5, 0)), ('b', (.5, 1, 0))]
                for cc, value in zip(('x', 'y', 'zero'), vals)]
        with patch.multiple(bench, CCS=['x', 'y', 'zero'], REPEATS=1, WORKLOADS=['bulk']):
            entries, warnings = bench.overall_ranking(scenes, rows)
            self.assertFalse(warnings)
            self.assertEqual([e['rank'] for e in entries], ['1', '1', '3'])
            self.assertAlmostEqual(entries[0]['score'], 2 ** -.5)
            self.assertEqual(entries[-1]['score'], 0)
            self.assertEqual(entries[-1]['wins'], 0)
            table = '\n'.join(bench.overall_ranking_table(scenes, rows))
            self.assertEqual(table.count('**基准**'), 2)
        with patch.multiple(bench, WORKLOADS=['websearch']):
            self.assertIn('需要 bulk', bench.overall_ranking(scenes, rows)[1][0])

    def test_timeout_scores_zero_and_does_not_become_baseline(self):
        rows = []
        for cc, status in [('good', 'success'), ('slow', 'timeout')]:
            rows.append({'scenario': 'a', 'cc': cc, 'workload': 'bulk', 'repeat': 1,
                         'status': 'success', 'norm_power': .5, 'norm_power_p95': .5})
            rows.append({'scenario': 'a', 'cc': cc, 'workload': 'app', 'repeat': 1,
                         'status': status, 'fct_slowdown': 2 if status == 'success' else '',
                         'fct_slowdown_p95': 2 if status == 'success' else ''})
        with patch.multiple(bench, CCS=['good', 'slow'], REPEATS=1, WORKLOADS=['bulk', 'app']):
            scenario = bench.ranking_entries(rows, 'a', '', 'combined')
            slow = next(e for e in scenario if e['cc'] == 'slow')
            self.assertTrue(slow['valid'])
            self.assertEqual(slow['value'], 0)
            self.assertEqual(slow['status'], '超时（0分）')
            overall, warnings = bench.overall_ranking([{'name': 'a'}], rows)
            self.assertFalse(warnings)
            self.assertEqual(overall[-1]['score'], 0)
            self.assertEqual(overall[-1]['complete'], 0)
            self.assertEqual(overall[-1]['status'], '含超时（0分）')
            rows[1]['status'] = 'timeout'
            self.assertTrue(bench.overall_ranking([{'name': 'a'}], rows)[1])

    def test_timeout_is_recorded_and_resumed_without_fake_fct(self):
        if not bench.BINARY.exists():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = root / 'lines/sample'
            lines.mkdir(parents=True)
            (lines / 'line.csv').write_text('time,cap\n0,60mbps\n1s,0\n')
            workloads = root / 'datasets/workloads'
            workloads.mkdir(parents=True)
            (workloads / 'long.csv').write_text('time,size\n0,1000\n1500,2000\n')
            with patch.multiple(bench, ROOT=root, LINES_DIR=lines, RESULTS_DIR=root / 'results',
                                WORKLOADS=['bulk', 'long'], CCS=['cubic'], REPEATS=1,
                                WORKLOAD_MAX_SIMULATION_S=.5, RESUME=True), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(bench.main(), 1)
                with next((root / 'results').rglob('runs.csv')).open(newline='') as stream:
                    rows = {r['workload']: r for r in csv.DictReader(stream)}
                self.assertEqual(rows['long']['status'], 'timeout')
                self.assertEqual(rows['long']['fct_slowdown'], '')
                self.assertIn('0 分', rows['long']['error'])
                with patch.object(bench.ProcessController, 'run', side_effect=AssertionError('must resume')):
                    self.assertEqual(bench.main(), 1)

    def test_bulk_transfers_without_warmup_and_reports_power(self):
        binary = bench.BINARY
        if not binary.exists():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene = root / 'line.csv'
            scene.write_text('time,cap\n0,60mbps\n1s,0\n')
            result = subprocess.run(
                [str(binary), '--profile=lines', f'--lineFile={scene}',
                 '--stopTime=1', '--ccs=cubic', '--evalJson=1'],
                text=True, capture_output=True, check=True,
            )
            data = json.loads(next(line for line in result.stdout.splitlines() if line.startswith('{')))
            self.assertAlmostEqual(data['duration_s'], 1.0)
            self.assertAlmostEqual(data['capacity_mbps'], 60.0)
            self.assertGreater(data['throughput_mbps'], 0)
            self.assertAlmostEqual(data['utilization'], data['throughput_mbps'] / 60.0, places=5)
            self.assertGreater(data['est_rtt_ms'], 0)
            self.assertAlmostEqual(data['util_power'],
                                   data['utilization'] / (data['est_rtt_ms'] / 1000), places=4)
            self.assertAlmostEqual(data['norm_power'],
                                   data['utilization'] * data['base_rtt_ms'] / data['est_rtt_ms'], places=5)

    def test_reverse_link_is_required_for_tcp_handshake(self):
        binary = bench.BINARY
        if not binary.exists():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene = root / 'line.csv'
            scene.write_text('time,up_cap,down_cap\n0,60mbps,0\n500ms,0,0\n')
            result = subprocess.run([str(binary), '--profile=lines', f'--lineFile={scene}',
                                     '--stopTime=0.5', '--ccs=cubic', '--evalJson=1'],
                                    text=True, capture_output=True, check=True)
            data = json.loads(next(line for line in result.stdout.splitlines() if line.startswith('{')))
            self.assertEqual(data['throughput_mbps'], 0)

    def test_discovery_rejects_legacy_txt_and_bad_csv(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'a.up.txt').write_text('0,1\n1000,0\n')
            (p / 'a.down.txt').write_text('0,2\n900,0\n')
            with self.assertRaisesRegex(ValueError, 'no CSV line scenarios'):
                bench.discover(p)
            (p / 'a.csv').write_text('time,cap,up_delay\n0,12mbps,20\n800ms,0,\n')
            self.assertEqual(bench.discover(p)[0]['duration_s'], 0.8)
            (p / 'a.csv').write_text('time,cap,up_delay\n0,12mbps,\n400ms,12mbps,20\n800ms,0,\n')
            with self.assertRaises(ValueError):
                bench.discover(p)

    def test_end_to_end(self):
        binary = bench.BINARY
        if not binary.exists():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = root / 'lines' / 'sample'
            lines.mkdir(parents=True)
            (lines / 'a.csv').write_text('time,cap,up_delay,down_delay\n0,60mbps,5,8\n1s,0,,\n')
            work = root / 'datasets/workloads'
            work.mkdir(parents=True)
            (work / 'tiny.csv').write_text('time,size\n0,1000\n10,2000\n')
            with patch.multiple(bench, ROOT=root, LINES_DIR=lines, RESULTS_DIR=root / 'results',
                                BINARY=binary, CCS=['cubic'], WORKLOADS=['bulk', 'tiny'], REPEATS=1):
                self.assertEqual(bench.main(), 0)
            report = next((root / 'results').rglob('report.md')).read_text()
            self.assertIn('TCP 数据段大小：1448 字节', report)
            metadata = json.loads(next((root / 'results').rglob('metadata.json')).read_text())
            self.assertEqual(metadata['tcp_segment_size_bytes'], 1448)
            self.assertIn('Norm Power', report)
            self.assertIn('tiny', report)
            csv = next((root / 'results').rglob('runs.csv')).read_text()
            self.assertIn('2,2,', csv)

    def test_leocc_dataset_bulk_and_workload(self):
        lines = ROOT / 'datasets/lines/leocc'
        if not lines.is_dir():
            self.fail(f'LeoCC dataset is missing: {lines}')
        if not bench.BINARY.is_file():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            with patch.multiple(bench, LINES_DIR=lines, RESULTS_DIR=output,
                                CCS=['bbr'], WORKLOADS=['bulk', 'pageload'], REPEATS=1):
                self.assertEqual(bench.main(), 0)
            with next(output.rglob('runs.csv')).open(newline='') as stream:
                rows = {row['workload']: row for row in csv.DictReader(stream)}
            self.assertEqual(set(rows), {'bulk', 'pageload'})
            self.assertEqual(rows['bulk']['scenario'], 'leo-bw')
            self.assertEqual(rows['bulk']['status'], 'success')
            self.assertAlmostEqual(float(rows['bulk']['duration_s']), 119.99)
            self.assertGreater(float(rows['bulk']['throughput_mbps']), 0)
            self.assertGreater(float(rows['bulk']['util_power']), 0)
            self.assertGreater(float(rows['bulk']['norm_power']), 0)
            self.assertEqual(rows['pageload']['status'], 'success')
            self.assertEqual(rows['pageload']['requests'], rows['pageload']['requests_expected'])
            self.assertGreater(float(rows['pageload']['fct_slowdown']), 0)
            report = next(output.rglob('report.md')).read_text()
            self.assertIn('leo-bw', report)
            self.assertIn('Norm Power', report)
            self.assertIn('FCT Slowdown', report)
            self.assertIn('- Bulk 持续时间：119.990 s', report)
            bulk_table = report.split('## bulk', 1)[1].split('## ', 1)[0]
            self.assertNotIn('持续时间 s', bulk_table)
            self.assertNotIn('从传输开始统计，不设置预热', report)

    def test_leocc_cubic_workloads_complete_with_ethernet_mss(self):
        if not bench.BINARY.is_file():
            self.skipTest('build tcp-trace-bench first')
        lines = ROOT / 'datasets/lines/leocc'
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            with patch.multiple(bench, LINES_DIR=lines, RESULTS_DIR=output,
                                CCS=['cubic'], WORKLOADS=['websearch', 'pageload', 'app'], REPEATS=1):
                self.assertEqual(bench.main(), 0)
            with next(output.rglob('runs.csv')).open(newline='') as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 3)
            for row in rows:
                self.assertEqual(row['status'], 'success', row['workload'])
                self.assertEqual(row['requests'], row['requests_expected'])

    def test_report_lists_different_bulk_durations_by_scenario(self):
        with tempfile.TemporaryDirectory() as tmp:
            scenarios = [{'name': 'a', 'duration_s': 1.0, 'files': {}},
                         {'name': 'b', 'duration_s': 2.0, 'files': {}}]
            with patch.multiple(bench, WORKLOADS=['bulk'], CCS=['cubic'], REPEATS=1):
                bench.write_report(Path(tmp), scenarios, [], {})
            report = (Path(tmp) / 'report.md').read_text()
            self.assertIn('- Bulk 持续时间：a 1.000 s；b 2.000 s', report)

    def test_workload_continues_after_lines_end(self):
        binary = bench.BINARY
        if not binary.exists():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = root / 'lines' / 'cycle'
            lines.mkdir(parents=True)
            (lines / 'a.csv').write_text('time,cap,up_delay,down_delay\n0,60mbps,5,8\n1s,0,,\n')
            work = root / 'datasets/workloads'
            work.mkdir(parents=True)
            (work / 'long.csv').write_text('time,size\n0,1000\n1500,2000\n')
            with patch.multiple(bench, ROOT=root, LINES_DIR=lines, RESULTS_DIR=root / 'results',
                                BINARY=binary, CCS=['cubic'], WORKLOADS=['long'], REPEATS=1,
                                WORKLOAD_MAX_SIMULATION_S=3.5):
                self.assertEqual(bench.main(), 0)
            with next((root / 'results').rglob('runs.csv')).open(newline='') as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row['requests'], '2')
            self.assertGreater(float(row['duration_s']), 1.5)
            self.assertLess(float(row['duration_s']), 2.5)
            self.assertAlmostEqual(float(row['capacity_mbps']), 60.0, places=3)
            report = next((root / 'results').rglob('report.md')).read_text()
            self.assertIn('Workload 完成时间 s', report)

    def test_resume_reuses_matching_runs_and_replaces_changed_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = root / 'lines' / 'sample'
            lines.mkdir(parents=True)
            scene = lines / 'a.csv'
            scene.write_text('time,cap\n0,60mbps\n1s,0\n')
            binary = root / 'binary'
            binary.write_text('binary')
            result = (0, '{"throughput_mbps": 1}\n', '', '')
            with patch.multiple(bench, LINES_DIR=lines, RESULTS_DIR=root / 'results',
                                BINARY=binary, CCS=['cubic'], WORKLOADS=['bulk'], REPEATS=2, RESUME=True), \
                    patch.object(bench.ProcessController, 'run', return_value=result) as run:
                self.assertEqual(bench.main(), 0)
                self.assertEqual(run.call_count, 2)
                out = root / 'results/sample'
                self.assertTrue((out / 'report.md').is_file())
                self.assertEqual(bench.main(), 0)
                self.assertEqual(run.call_count, 2)
                (out / 'runs/a__bulk__cubic__r2.json').unlink()
                self.assertEqual(bench.main(), 0)
                self.assertEqual(run.call_count, 3)
                scene.write_text('time,cap\n0,72mbps\n1s,0\n')
                self.assertEqual(bench.main(), 0)
                self.assertEqual(run.call_count, 5)
                with patch.object(bench, 'RESUME', False):
                    self.assertEqual(bench.main(), 0)
                self.assertEqual(run.call_count, 7)
                self.assertEqual(len(list((root / 'results').iterdir())), 1)

    def test_parallel_respects_limit_and_retains_deterministic_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = root / 'lines' / 'sample'
            lines.mkdir(parents=True)
            (lines / 'a.csv').write_text('time,cap\n0,60mbps\n1s,0\n')
            binary = root / 'binary'
            binary.write_text('binary')
            lock = threading.Lock()
            active = 0
            peak = 0

            def simulate(controller, cmd, timeout):
                nonlocal active, peak
                self.assertIn('--segmentSize=1448', cmd)
                with lock:
                    active += 1
                    peak = max(peak, active)
                time.sleep(0.05)
                with lock:
                    active -= 1
                return (0, '{"throughput_mbps": 1}\n', '', '')

            console = io.StringIO()
            with patch.multiple(bench, LINES_DIR=lines, RESULTS_DIR=root / 'results',
                                BINARY=binary, CCS=['cubic'], WORKLOADS=['bulk'], REPEATS=4,
                                RESUME=False, MAX_WORKERS=2), \
                    patch.object(bench.ProcessController, 'run', simulate), \
                    contextlib.redirect_stdout(console):
                self.assertEqual(bench.main(), 0)
            self.assertEqual(peak, 2)
            self.assertEqual(console.getvalue().count('[完成] a  |  cubic  |  bulk'), 4)
            with (root / 'results/sample/runs.csv').open(newline='') as stream:
                self.assertEqual([r['repeat'] for r in csv.DictReader(stream)], ['1', '2', '3', '4'])

    def test_serial_and_parallel_results_match(self):
        if not bench.BINARY.exists():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = root / 'lines' / 'sample'
            lines.mkdir(parents=True)
            (lines / 'a.csv').write_text('time,cap\n0,60mbps\n500ms,0\n')
            results = []
            for workers in (1, 2):
                out = root / f'results-{workers}'
                with patch.multiple(bench, LINES_DIR=lines, RESULTS_DIR=out,
                                    CCS=['cubic'], WORKLOADS=['bulk'], REPEATS=2,
                                    RESUME=False, MAX_WORKERS=workers):
                    self.assertEqual(bench.main(), 0)
                with (out / 'sample/runs.csv').open(newline='') as stream:
                    results.append(list(csv.DictReader(stream)))
            self.assertEqual(results[0], results[1])

    def test_interrupted_parallel_run_stops_children_and_resumes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = root / 'lines' / 'sample'
            lines.mkdir(parents=True)
            (lines / 'a.csv').write_text('time,cap\n0,60mbps\n1s,0\n')
            marker = root / 'resume'
            binary = root / 'fake-bench'
            binary.write_text(
                '#!' + sys.executable + '\n'
                'import os, pathlib, sys, time\n'
                'run = next(a for a in sys.argv if a.startswith("--run=")).split("=")[1]\n'
                f'pathlib.Path({str(root)!r}, "pid-" + run).write_text(str(os.getpid()))\n'
                f'time.sleep(30 if run == "2" and not pathlib.Path({str(marker)!r}).exists() else 0.1)\n'
                'print("{\\"throughput_mbps\\": 1}")\n'
            )
            binary.chmod(0o755)
            driver = (
                f'import sys; sys.path.insert(0, {str(ROOT / "scripts/bench")!r}); '
                'import run_trace_bench as b; '
                f'b.LINES_DIR=b.Path({str(lines)!r}); b.RESULTS_DIR=b.Path({str(root / "results")!r}); '
                f'b.BINARY=b.Path({str(binary)!r}); '
                'b.CCS=["cubic"]; b.WORKLOADS=["bulk"]; b.REPEATS=2; b.MAX_WORKERS=2; '
                'raise SystemExit(b.main())'
            )
            proc = subprocess.Popen([sys.executable, '-c', driver], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            checkpoint = root / 'results/sample/runs/a__bulk__cubic__r1.json'
            deadline = time.monotonic() + 10
            try:
                while (not checkpoint.exists() or not (root / 'pid-2').exists()) and time.monotonic() < deadline:
                    if proc.poll() is not None:
                        self.fail('benchmark exited before the first checkpoint')
                    time.sleep(0.02)
                self.assertTrue(checkpoint.exists())
                pid = int((root / 'pid-2').read_text())
                proc.send_signal(signal.SIGINT)
                _, stderr = proc.communicate(timeout=5)
                self.assertEqual(proc.returncode, 130, stderr)
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
                marker.touch()
                resumed = subprocess.run([sys.executable, '-c', driver], text=True,
                                         capture_output=True, timeout=10, check=True)
                self.assertIn('已恢复：1', resumed.stdout)
                self.assertTrue((root / 'results/sample/runs/a__bulk__cubic__r2.json').exists())
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate()


    def test_custom_tail_percentile_updates_cli_report_overview_and_resume_cache(self):
        binary = bench.BINARY
        if not binary.exists():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = root / 'lines' / 'sample'
            lines.mkdir(parents=True)
            (lines / 'a.csv').write_text('time,cap\n0,60mbps\n1s,0\n')
            work = root / 'datasets/workloads'
            work.mkdir(parents=True)
            (work / 'tiny.csv').write_text('time,size\n0,1000\n10,2000\n')

            out99 = io.StringIO()
            with patch.multiple(bench, ROOT=root, LINES_DIR=lines, RESULTS_DIR=root / 'results',
                                BINARY=binary, CCS=['cubic'], WORKLOADS=['bulk', 'tiny'],
                                REPEATS=1, TAIL_PERCENTILE=99, RESUME=True), \
                    contextlib.redirect_stdout(out99):
                self.assertEqual(bench.main(), 0)
            self.assertIn('P99 Norm Power', out99.getvalue())
            self.assertIn('tiny P99 FCT', out99.getvalue())
            report99 = next((root / 'results').rglob('report.md')).read_text()
            self.assertIn('- 尾部分位点：P99', report99)
            self.assertIn('P99 排队延迟 ms', report99)
            self.assertIn('P99 Norm Power ↑', report99)
            self.assertIn('P99 数据块完成时间 ms', report99)
            self.assertIn('P99 FCT Slowdown ↓', report99)
            self.assertIn('### 综合排名（基于 P99）', report99)
            self.assertIn('### bulk：所有场景排名（P99）', report99)
            meta99 = json.loads(next((root / 'results').rglob('metadata.json')).read_text())
            self.assertEqual(meta99['tail_percentile'], 99.0)

            # Switching TAIL_PERCENTILE to 50 must invalidate RESUME cache and rerun with --tailPercentile=50
            out50 = io.StringIO()
            with patch.multiple(bench, ROOT=root, LINES_DIR=lines, RESULTS_DIR=root / 'results',
                                BINARY=binary, CCS=['cubic'], WORKLOADS=['bulk', 'tiny'],
                                REPEATS=1, TAIL_PERCENTILE=50, RESUME=True), \
                    contextlib.redirect_stdout(out50):
                self.assertEqual(bench.main(), 0)
            self.assertIn('已恢复：0  |  待运行：2', out50.getvalue())
            self.assertIn('P50 Norm Power', out50.getvalue())
            report50 = next((root / 'results').rglob('report.md')).read_text()
            self.assertIn('- 尾部分位点：P50', report50)
            self.assertIn('### 综合排名（基于 P50）', report50)


if __name__ == '__main__':
    unittest.main()
