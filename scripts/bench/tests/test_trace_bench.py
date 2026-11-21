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
    def test_discover_dataset_and_individual_scenario(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'datasets/lines/example'
            for name in ('a', 'b'):
                scene = dataset / name
                scene.mkdir(parents=True)
                for direction in ('up', 'down'):
                    (scene / f'{name}.{direction}.txt').write_text('0,1\n10,0\n')
                (scene / f'{name}.up.delay.txt').write_text('0,20\n# end_ms=10\n')
            all_scenes = bench.discover(dataset)
            self.assertEqual([s['name'] for s in all_scenes], ['a', 'b'])
            self.assertEqual(bench.discover(dataset / 'a'), [all_scenes[0]])
            with patch.multiple(bench, ROOT=root, RESULTS_DIR=root / 'results'):
                self.assertEqual(bench.result_directory(dataset), root / 'results/example')
                self.assertEqual(bench.result_directory(dataset / 'a'), root / 'results/example/a')
            (dataset / 'b/b.down.txt').unlink()
            with self.assertRaisesRegex(ValueError, 'missing up/down'):
                bench.discover(dataset)

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
                             'norm_power': norm})
                rows.append({'scenario': 'a', 'cc': cc, 'repeat': repeat, 'workload': 'websearch',
                             'status': 'success', 'fct_slowdown': slow})
                rows.append({'scenario': 'a', 'cc': cc, 'repeat': repeat, 'workload': 'pageload',
                             'status': 'success', 'fct_slowdown': 4.0 if cc == 'tie' else 1.0})
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
            self.assertEqual(tables.count('### 综合排名'), 1)
            self.assertIn('websearch Slowdown', tables)
            self.assertIn('pageload Slowdown', tables)

    def test_ranking_zero_and_missing_metrics_have_no_false_winner(self):
        rows = [{'scenario': 'a', 'cc': cc, 'repeat': 1, 'workload': 'bulk',
                 'status': 'success', 'norm_power': value}
                for cc, value in [('zero', 0), ('invalid', float('nan'))]]
        with patch.multiple(bench, CCS=['zero', 'invalid'], REPEATS=1):
            ranked = bench.ranking_entries(rows, 'a', 'bulk', 'bulk')
        self.assertEqual(ranked[0]['rank'], '-')
        self.assertEqual(ranked[0]['status'], '基准为零，无法排名')
        self.assertEqual(ranked[1]['rank'], '-')

    def test_bulk_transfers_without_warmup_and_reports_power(self):
        binary = bench.BINARY
        if not binary.exists():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            up, down = root / 'up', root / 'down'
            up.write_text('0,5\n1000,0\n')
            down.write_text('0,5\n1000,0\n')
            result = subprocess.run(
                [str(binary), '--profile=lines', f'--upTrace={up}', f'--downTrace={down}',
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
            up, down = root / 'up', root / 'down'
            up.write_text('0,5\n500,0\n')
            down.write_text('0,0\n500,0\n')
            result = subprocess.run([str(binary), '--profile=lines', f'--upTrace={up}',
                                     f'--downTrace={down}', '--stopTime=0.5', '--ccs=cubic', '--evalJson=1'],
                                    text=True, capture_output=True, check=True)
            data = json.loads(next(line for line in result.stdout.splitlines() if line.startswith('{')))
            self.assertEqual(data['throughput_mbps'], 0)

    def test_discovery_and_bad_delay(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'a.up.txt').write_text('0,1\n1000,0\n')
            with self.assertRaisesRegex(ValueError, 'missing up/down'):
                bench.discover(p)
            (p / 'a.down.txt').write_text('0,2\n900,0\n')
            (p / 'a.up.delay.txt').write_text('0,20\n# end_ms=800\n')
            self.assertEqual(bench.discover(p)[0]['duration_s'], 0.8)
            (p / 'a.up.delay.txt').write_text('0,20\n')
            with self.assertRaisesRegex(ValueError, 'end_ms'):
                bench.discover(p)

    def test_end_to_end(self):
        binary = bench.BINARY
        if not binary.exists():
            self.skipTest('build tcp-trace-bench first')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = root / 'lines' / 'sample'
            lines.mkdir(parents=True)
            (lines / 'a.up.txt').write_text('0,5\n1000,0\n')
            (lines / 'a.down.txt').write_text('0,5\n1000,0\n')
            (lines / 'a.up.delay.txt').write_text('0,5\n# end_ms=1000\n')
            (lines / 'a.down.delay.txt').write_text('0,8\n# end_ms=1000\n')
            work = root / 'datasets/workloads'
            work.mkdir(parents=True)
            (work / 'tiny.wk').write_text('0 1000\n10 2000\n')
            with patch.multiple(bench, ROOT=root, LINES_DIR=lines, RESULTS_DIR=root / 'results',
                                BINARY=binary, CCS=['cubic'], WORKLOADS=['bulk', 'tiny'], REPEATS=1):
                self.assertEqual(bench.main(), 0)
            report = next((root / 'results').rglob('report.md')).read_text()
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
            (lines / 'a.up.txt').write_text('0,5\n1000,0\n')
            (lines / 'a.down.txt').write_text('0,5\n1000,0\n')
            (lines / 'a.up.delay.txt').write_text('0,5\n# end_ms=1000\n')
            (lines / 'a.down.delay.txt').write_text('0,8\n# end_ms=1000\n')
            work = root / 'datasets/workloads'
            work.mkdir(parents=True)
            (work / 'long.wk').write_text('0 1000\n1500 2000\n')
            with patch.multiple(bench, ROOT=root, LINES_DIR=lines, RESULTS_DIR=root / 'results',
                                BINARY=binary, CCS=['cubic'], WORKLOADS=['long'], REPEATS=1,
                                WORKLOAD_DRAIN_TIMEOUT_S=2.0):
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
            up = lines / 'a.up.txt'
            up.write_text('0,5\n1000,0\n')
            (lines / 'a.down.txt').write_text('0,5\n1000,0\n')
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
                up.write_text('0,6\n1000,0\n')
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
            (lines / 'a.up.txt').write_text('0,5\n1000,0\n')
            (lines / 'a.down.txt').write_text('0,5\n1000,0\n')
            binary = root / 'binary'
            binary.write_text('binary')
            lock = threading.Lock()
            active = 0
            peak = 0

            def simulate(controller, cmd, timeout):
                nonlocal active, peak
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
            (lines / 'a.up.txt').write_text('0,5\n500,0\n')
            (lines / 'a.down.txt').write_text('0,5\n500,0\n')
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
            (lines / 'a.up.txt').write_text('0,5\n1000,0\n')
            (lines / 'a.down.txt').write_text('0,5\n1000,0\n')
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


if __name__ == '__main__':
    unittest.main()
