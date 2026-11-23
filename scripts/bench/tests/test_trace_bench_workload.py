#!/usr/bin/env python3
"""Application workload semantics tests for tcp-trace-bench (MortiseAppSender/Receiver)."""
from __future__ import annotations

import statistics
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lines_common
from scripts.bench.tests.harness import BINARY, TraceBenchCase


class TraceBenchWorkloadTests(TraceBenchCase):
    """Verify workload scheduling, early termination, serialization bounds, HoL, and looping."""

    def test_01_workload_interarrival_timing_and_early_stop(self):
        scene = self.make_lines(
            up='0,50mbps\n10000,0\n',
            down='0,50mbps\n10000,0\n',
            up_delay='0,10\n# end_ms=10000\n',
            down_delay='0,10\n# end_ms=10000\n',
        )
        wk = self.make_workload('time,size\n0,1000\n500,1000\n1000,1000\n', name='three.csv')
        data = self.run_json(scene, stop_time=10.0, cc='cubic', workload_file=wk, line_period_ms=10000)
        self.assertEqual(data['requests'], 3)
        self.assertEqual(data['requests_expected'], 3)
        # Last chunk arrives at t=1.001s + ~10ms one-way delivery -> finishes well before stopTime=10s
        self.assertGreaterEqual(data['duration_s'], 1.005)
        self.assertLessEqual(data['duration_s'], 1.10)

    def test_02_chunk_transfer_duration_respects_serialization_bounds(self):
        scene = self.make_lines(
            up='0,10mbps\n5000,0\n',
            down='0,100mbps\n5000,0\n',
            up_delay='0,10\n# end_ms=5000\n',
            down_delay='0,10\n# end_ms=5000\n',
        )
        wk_small = self.make_workload('time,size\n0,1000\n', name='small.csv')
        # 250 KB at 10 Mbps -> 200 ms pure serialization + handshake/slow-start
        wk_large = self.make_workload('time,size\n0,250000\n', name='large.csv')
        d_small = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=wk_small, line_period_ms=5000)
        d_large = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=wk_large, line_period_ms=5000)

        self.assertLess(d_small['rct_mean_ms'], 60.0)
        self.assertGreater(d_large['rct_mean_ms'], 200.0)
        self.assertGreaterEqual(d_large['fct_slowdown'], 1.0)
        self.assertGreaterEqual(d_large['fct_slowdown_p95'], d_large['fct_slowdown'])

    def test_03_head_of_line_blocking_on_single_tcp_connection(self):
        scene = self.make_lines(
            up='0,10mbps\n5000,0\n',
            down='0,100mbps\n5000,0\n',
            up_delay='0,10\n# end_ms=5000\n',
            down_delay='0,10\n# end_ms=5000\n',
        )
        wk_hol = self.make_workload('time,size\n0,500000\n10,1000\n', name='hol.csv')
        wk_sep = self.make_workload('time,size\n0,500000\n1200,1000\n', name='sep.csv')

        d_hol = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=wk_hol, line_period_ms=5000)
        d_sep = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=wk_sep, line_period_ms=5000)

        self.assertEqual(d_hol['requests'], 2)
        self.assertEqual(d_sep['requests'], 2)
        # In HoL case, the 1 KB chunk waits ~400ms behind the 500 KB chunk -> huge P95 slowdown
        self.assertGreater(d_hol['fct_slowdown_p95'], d_sep['fct_slowdown_p95'] * 5.0)
        self.assertGreater(d_hol['rct_mean_ms'], d_sep['rct_mean_ms'] + 150.0)

    def test_04_workload_periodic_looping_of_both_delay_and_capacity(self):
        # Part A: Delay looping (period=1000ms: [0,500)->20ms RTT, [500,1000)->70ms RTT)
        d_scene = self.make_lines(
            up='0,50mbps\n1000,0\n',
            down='0,50mbps\n1000,0\n',
            up_delay='0,10\n500,60\n# end_ms=1000\n',
            down_delay='0,10\n# end_ms=1000\n',
            name='delay_loop',
        )
        wk = self.make_workload('time,size\n' + '\n'.join(f'{i * 40},6000' for i in range(70)) + '\n')
        full = self.run_full(
            d_scene, stop_time=3.0, cc='cubic',
            workload_file=wk, line_period_ms=1000, rtt_sample_log=True, name='delay_loop_run',
        )
        for cycle in (0, 1, 2):
            low = [r for t, r in full.rtts if cycle + 0.15 <= t <= cycle + 0.45]
            high = [r for t, r in full.rtts if cycle + 0.65 <= t <= cycle + 0.92]
            self.assertTrue(low and high, f'missing samples in cycle {cycle}')
            self.assertAlmostEqual(statistics.median(low), 22.0, delta=3.0, msg=f'cycle {cycle} low RTT')
            self.assertAlmostEqual(statistics.median(high), 72.0, delta=4.0, msg=f'cycle {cycle} high RTT')

        # Part B: Capacity looping (period=1000ms: [0,500)->0 outage, [500,1000)->20mbps)
        c_scene = self.make_lines(
            up='0,0\n500,20mbps\n1000,0\n',
            down='0,20mbps\n1000,0\n',
            up_delay='0,10\n# end_ms=1000\n',
            down_delay='0,10\n# end_ms=1000\n',
            name='cap_loop',
        )
        series = self.run_full(
            c_scene, stop_time=2.9, cc='cubic', bin_ms=100.0, line_period_ms=1000, name='cap_loop_run'
        ).series
        for cycle in (0, 1, 2):
            off_bins = [r['capacity_mbps'] for r in series if cycle + 0.1 <= r['time_s'] <= cycle + 0.4]
            on_bins = [r['capacity_mbps'] for r in series if cycle + 0.6 <= r['time_s'] <= cycle + 0.9]
            self.assertTrue(off_bins and on_bins)
            self.assertTrue(all(c == 0.0 for c in off_bins), f'cycle {cycle} expected 0 cap')
            self.assertAlmostEqual(statistics.mean(on_bins), 20.0, delta=1.0)

    def test_05_csv_format_units_column_order_comments_and_mortise_export(self):
        csv_text = (
            '\ufefftime,size,comment\n'
            '0,2048,"initial, ""boot"" chunk"\n'
            '20ms,1.5kb,small metadata\n'
            '20000us,16KB,same-ms burst\n'
            '0.5s,512b,\n'
            '1s,0.25mb,quarter MiB\n'
            '1500ms,0.5M,half MiB\n'
        )
        reordered_text = (
            'COMMENT,SIZE,TIME\n'
            '"initial, ""boot"" chunk",2048,0ms\n'
            'small metadata,1.5k,20\n'
            'same-ms burst,16kb,20\n'
            ',512b,500ms\n'
            'quarter MiB,256kb,1000\n'
            'half MiB,0.5mb,1.5s\n'
        )
        expected = [
            (0.0, 2048),
            (20.0, 1536),
            (20.0, 16384),
            (500.0, 512),
            (1000.0, 262144),
            (1500.0, 524288),
        ]
        csv_wk = self.make_workload(csv_text, name='csv_units.csv')
        reordered_wk = self.make_workload(reordered_text, name='reordered.csv')
        self.assertEqual(lines_common.parse_workload(csv_wk), expected)
        self.assertEqual(lines_common.parse_workload(reordered_wk), expected)
        self.assertEqual(lines_common.workload(csv_wk), (6, 1.5))

        # Round-trip write_workload_csv
        rt_wk = csv_wk.parent / 'roundtrip.csv'
        lines_common.write_workload_csv(rt_wk, expected)
        self.assertEqual(lines_common.parse_workload(rt_wk), expected)

        # Temporary Mortise export for upstream Rust client produces gap_ms<TAB>size_bytes
        # and is rejected by parse_workload (legacy .wk format removed).
        mortise_wk = csv_wk.parent / 'converted.mortise.wk'
        lines_common.write_mortise_workload(csv_wk, mortise_wk)
        self.assertEqual(
            mortise_wk.read_text(encoding='utf-8').splitlines(),
            ['0\t2048', '20\t1536', '0\t16384', '480\t512', '500\t262144', '500\t524288'],
        )
        with self.assertRaises(ValueError):
            lines_common.parse_workload(mortise_wk)

        scene = self.make_lines(
            up='0,50mbps\n5000,0\n',
            down='0,50mbps\n5000,0\n',
            up_delay='0,10\n# end_ms=5000\n',
            down_delay='0,10\n# end_ms=5000\n',
            name='csv_order_parity',
        )
        d_csv = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=csv_wk, line_period_ms=5000)
        d_reord = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=reordered_wk, line_period_ms=5000)
        self.assertEqual(d_csv['requests'], 6)
        self.assertEqual(d_csv['requests_expected'], 6)
        for key in ('requests', 'duration_s', 'rct_mean_ms', 'rct_p95_ms', 'fct_slowdown', 'fct_slowdown_p95'):
            self.assertAlmostEqual(d_csv[key], d_reord[key], places=6, msg=f'mismatch in {key}')

    def test_06_csv_workload_validation_rejects_legacy_and_malformed_in_python_and_ns3(self):
        scene = self.make_lines(
            up='0,50mbps\n2000,0\n',
            down='0,50mbps\n2000,0\n',
            up_delay='0,10\n# end_ms=2000\n',
            down_delay='0,10\n# end_ms=2000\n',
            name='bad_workloads',
        )
        bad_cases = {
            'legacy_ws': '0 1000\n500 1000\n',
            'headerless_csv': '0,1000\n500,1000\n',
            'missing_size': 'time,comment\n0,note\n',
            'missing_time': 'size,comment\n1000,note\n',
            'unknown_col': 'time,size,extra\n0,1000,1\n',
            'duplicate_col': 'time,size,time\n0,1000,0\n',
            'backwards_time': 'time,size\n100,1kb\n50,1kb\n',
            'negative_time': 'time,size\n-1,1kb\n',
            'zero_size': 'time,size\n0,0kb\n',
            'negative_size': 'time,size\n0,-10\n',
            'oversize': 'time,size\n0,100mb\n',
            'space_before_size_unit': 'time,size\n0,10 kb\n',
            'space_before_time_unit': 'time,size\n10 ms,1kb\n',
            'unsuffixed_float_size': 'time,size\n0,1.5\n',
            'header_only': 'time,size\n',
            'unclosed_quote': 'time,size,comment\n0,1kb,"unclosed\n',
            'col_count_mismatch': 'time,size\n0,1kb,extra\n',
        }
        for label, bad in bad_cases.items():
            bad_wk = self.make_workload(bad, name=f'{label}.csv')
            with self.assertRaises(ValueError, msg=f'Python should reject {label}: {bad!r}'):
                lines_common.parse_workload(bad_wk)
            proc = subprocess.run([
                str(BINARY), '--profile=lines',
                f'--lineFile={scene.line}',
                f'--workloadFile={bad_wk}', '--stopTime=2', '--ccs=cubic', '--evalJson=1',
            ], text=True, capture_output=True)
            self.assertNotEqual(proc.returncode, 0, f'ns-3 should reject {label}: {bad!r}')


if __name__ == '__main__':
    unittest.main()
