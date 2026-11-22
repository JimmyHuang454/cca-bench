#!/usr/bin/env python3
"""Application workload semantics tests for tcp-trace-bench (MortiseAppSender/Receiver)."""
from __future__ import annotations

import statistics
import unittest

from scripts.bench.tests.harness import TraceBenchCase


class TraceBenchWorkloadTests(TraceBenchCase):
    """Verify workload scheduling, early termination, serialization bounds, HoL, and looping."""

    def test_01_workload_interarrival_timing_and_early_stop(self):
        scene = self.make_lines(
            up='0,50m\n10000,0\n',
            down='0,50m\n10000,0\n',
            up_delay='0,10\n# end_ms=10000\n',
            down_delay='0,10\n# end_ms=10000\n',
        )
        wk = self.make_workload('0 1000\n500 1000\n500 1000\n', name='three.wk')
        data = self.run_json(scene, stop_time=10.0, cc='cubic', workload_file=wk, line_period_ms=10000)
        self.assertEqual(data['requests'], 3)
        self.assertEqual(data['requests_expected'], 3)
        # Last chunk arrives at t=1.001s + ~10ms one-way delivery -> finishes well before stopTime=10s
        self.assertGreaterEqual(data['duration_s'], 1.005)
        self.assertLessEqual(data['duration_s'], 1.10)

    def test_02_chunk_transfer_duration_respects_serialization_bounds(self):
        scene = self.make_lines(
            up='0,10m\n5000,0\n',
            down='0,100m\n5000,0\n',
            up_delay='0,10\n# end_ms=5000\n',
            down_delay='0,10\n# end_ms=5000\n',
        )
        wk_small = self.make_workload('0 1000\n', name='small.wk')
        # 250 KB at 10 Mbps -> 200 ms pure serialization + handshake/slow-start
        wk_large = self.make_workload('0 250000\n', name='large.wk')
        d_small = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=wk_small, line_period_ms=5000)
        d_large = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=wk_large, line_period_ms=5000)

        self.assertLess(d_small['rct_mean_ms'], 60.0)
        self.assertGreater(d_large['rct_mean_ms'], 200.0)
        self.assertGreaterEqual(d_large['fct_slowdown'], 1.0)
        self.assertGreaterEqual(d_large['fct_slowdown_p95'], d_large['fct_slowdown'])

    def test_03_head_of_line_blocking_on_single_tcp_connection(self):
        scene = self.make_lines(
            up='0,10m\n5000,0\n',
            down='0,100m\n5000,0\n',
            up_delay='0,10\n# end_ms=5000\n',
            down_delay='0,10\n# end_ms=5000\n',
        )
        wk_hol = self.make_workload('0 500000\n10 1000\n', name='hol.wk')
        wk_sep = self.make_workload('0 500000\n1200 1000\n', name='sep.wk')

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
            up='0,50m\n1000,0\n',
            down='0,50m\n1000,0\n',
            up_delay='0,10\n500,60\n# end_ms=1000\n',
            down_delay='0,10\n# end_ms=1000\n',
            name='delay_loop',
        )
        wk = self.make_workload('\n'.join(f'{0 if i == 0 else 40} 6000' for i in range(70)) + '\n')
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

        # Part B: Capacity looping (period=1000ms: [0,500)->0m outage, [500,1000)->20m)
        c_scene = self.make_lines(
            up='0,0\n500,20m\n1000,0\n',
            down='0,20m\n1000,0\n',
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


if __name__ == '__main__':
    unittest.main()
