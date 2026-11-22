#!/usr/bin/env python3
"""Link-layer physics tests for tcp-trace-bench: capacity gating, outages, queue drops, and delay."""
from __future__ import annotations

import statistics
import unittest

from scripts.bench.tests.harness import TraceBenchCase


class TraceBenchLinkCapacityTests(TraceBenchCase):
    """Verify MahimahiGate capacity scheduling, fractional pacing, outages, and queue limits."""

    def test_01_bandwidth_step_down_and_recovery(self):
        scene = self.make_lines(
            up='0,40m\n1500,4m\n3000,40m\n4500,0\n',
            down='0,100m\n4500,0\n',
            up_delay='0,10\n# end_ms=4500\n',
            down_delay='0,10\n# end_ms=4500\n',
        )
        out = self.run_full(scene, stop_time=4.5, cc='bbr', bin_ms=100.0, name='step')
        seg1 = [r for r in out.series if 0.5 <= r['time_s'] <= 1.4]
        seg2 = [r for r in out.series if 1.7 <= r['time_s'] <= 2.9]
        seg3 = [r for r in out.series if 3.5 <= r['time_s'] <= 4.4]

        self.assertAlmostEqual(statistics.mean(r['capacity_mbps'] for r in seg1), 40.0, delta=0.5)
        self.assertAlmostEqual(statistics.mean(r['capacity_mbps'] for r in seg2), 4.0, delta=0.2)
        self.assertAlmostEqual(statistics.mean(r['capacity_mbps'] for r in seg3), 40.0, delta=0.5)

        tput1 = statistics.mean(r['tput0_mbps'] for r in seg1)
        tput2 = statistics.mean(r['tput0_mbps'] for r in seg2)
        tput3 = statistics.mean(r['tput0_mbps'] for r in seg3)
        self.assertGreater(tput1, 25.0)
        self.assertLessEqual(tput2, 4.3)
        self.assertGreater(tput2, 2.0)
        self.assertGreater(tput3, 20.0)

    def test_02_fractional_mbps_accuracy_and_no_unused_credit_bank(self):
        # Mix fractional Mbps (< 12 Mbps = < 1 pkt/ms) and integer packet count
        scene = self.make_lines(
            up='0,0.5m\n1000,1.23m\n2000,2\n3000,0\n',
            down='0,50m\n3000,0\n',
            up_delay='0,5\n# end_ms=3000\n',
            down_delay='0,5\n# end_ms=3000\n',
        )
        data = self.run_json(scene, stop_time=3.0, cc='cubic')
        expected_cap = (0.5 + 1.23 + 24.0) / 3.0
        self.assertAlmostEqual(data['capacity_mbps'], expected_cap, places=4)
        self.assertGreater(data['throughput_mbps'], 6.0)

        # Verify unused integer opportunities during idle periods do NOT accumulate as burst credit:
        # 12 Mbps link idle for 500 ms, then send 150 KB (~100 pkts -> needs >= 100 ms at 12 Mbps)
        idle_scene = self.make_lines(
            up='0,12m\n2000,0\n',
            down='0,100m\n2000,0\n',
            up_delay='0,5\n# end_ms=2000\n',
            down_delay='0,5\n# end_ms=2000\n',
            name='idle',
        )
        wk = self.make_workload('500 150000\n', name='delayed_chunk.wk')
        wk_data = self.run_json(idle_scene, stop_time=2.0, cc='cubic', workload_file=wk, line_period_ms=2000)
        self.assertEqual(wk_data['requests'], 1)
        self.assertGreaterEqual(wk_data['rct_mean_ms'], 95.0)
        self.assertGreaterEqual(wk_data['duration_s'], 0.595)

    def test_03_bidirectional_outage_and_recovery(self):
        scene = self.make_lines(
            up='0,20m\n1000,0\n2000,20m\n3500,0\n',
            down='0,20m\n1000,0\n2000,20m\n3500,0\n',
            up_delay='0,10\n# end_ms=3500\n',
            down_delay='0,10\n# end_ms=3500\n',
        )
        out = self.run_full(scene, stop_time=3.5, cc='cubic', bin_ms=100.0, name='outage')
        during = [r for r in out.series if 1.15 <= r['time_s'] <= 1.95]
        after = [r for r in out.series if 2.6 <= r['time_s'] <= 3.4]
        self.assertTrue(during and after)
        self.assertTrue(all(r['capacity_mbps'] == 0.0 for r in during))
        self.assertTrue(all(r['tput0_mbps'] == 0.0 for r in during))
        self.assertGreater(statistics.mean(r['tput0_mbps'] for r in after), 5.0)

    def test_04_unidirectional_data_vs_ack_outage_directionality(self):
        # Case A: Data direction (up) outage only while ACK (down) stays 20m
        data_out = self.make_lines(
            up='0,20m\n800,0\n1800,20m\n3000,0\n',
            down='0,20m\n3000,0\n',
            up_delay='0,10\n# end_ms=3000\n',
            down_delay='0,10\n# end_ms=3000\n',
            name='data_out',
        )
        s_data = self.run_full(data_out, stop_time=3.0, cc='cubic', bin_ms=100.0, name='data_out_run').series
        mid_data = [r for r in s_data if 0.95 <= r['time_s'] <= 1.75]
        self.assertTrue(all(r['tput0_mbps'] == 0.0 for r in mid_data))
        self.assertGreater(statistics.mean(r['tput0_mbps'] for r in s_data if 2.3 <= r['time_s'] <= 2.9), 5.0)

        # Case B: ACK direction (down) outage only while data (up) stays 20m ->
        # sender stalls once flight fills cwnd, then resumes after ACK path returns
        ack_out = self.make_lines(
            up='0,20m\n3500,0\n',
            down='0,20m\n800,0\n2000,20m\n3500,0\n',
            up_delay='0,10\n# end_ms=3500\n',
            down_delay='0,10\n# end_ms=3500\n',
            name='ack_out',
        )
        s_ack = self.run_full(ack_out, stop_time=3.5, cc='cubic', bin_ms=100.0, name='ack_out_run').series
        stalled = [r for r in s_ack if 1.3 <= r['time_s'] <= 1.95]
        resumed = [r for r in s_ack if 2.6 <= r['time_s'] <= 3.4]
        self.assertTrue(all(r['tput0_mbps'] == 0.0 for r in stalled))
        self.assertGreater(statistics.mean(r['tput0_mbps'] for r in resumed), 5.0)

    def test_05_queue_buffer_limit_causes_drops_and_caps_qdelay(self):
        scene = self.make_lines(
            up='0,5m\n4000,0\n',
            down='0,50m\n4000,0\n',
            up_delay='0,10\n# end_ms=4000\n',
            down_delay='0,10\n# end_ms=4000\n',
        )
        small_q = self.run_json(scene, stop_time=4.0, cc='cubic', queue_bytes=15000)
        large_q = self.run_json(scene, stop_time=4.0, cc='cubic', queue_bytes=500000)

        # 15 KB at 5 Mbps -> max physical queuing delay is ~24 ms
        self.assertGreater(small_q['drop_rate'], 0.0)
        self.assertLess(small_q['qdelay_p95_ms'], 35.0)
        # 500 KB at 5 Mbps -> cubic builds significantly larger queue
        self.assertGreater(large_q['qdelay_p95_ms'], small_q['qdelay_p95_ms'] + 40.0)

    def test_06_bulk_does_not_loop_past_end_marker(self):
        scene = self.make_lines(
            up='0,20m\n1000,0\n',
            down='0,20m\n1000,0\n',
            up_delay='0,10\n# end_ms=1000\n',
            down_delay='0,10\n# end_ms=1000\n',
        )
        out = self.run_full(scene, stop_time=2.5, cc='cubic', bin_ms=100.0, name='noloop')
        after_end = [r for r in out.series if r['time_s'] >= 1.15]
        self.assertTrue(after_end)
        self.assertTrue(all(r['capacity_mbps'] == 0.0 for r in after_end))
        self.assertTrue(all(r['tput0_mbps'] == 0.0 for r in after_end))

        data = self.run_json(scene, stop_time=2.5, cc='cubic')
        self.assertAlmostEqual(data['capacity_mbps'], 20.0 * 1.0 / 2.5, places=3)


class TraceBenchLinkDelayTests(TraceBenchCase):
    """Verify propagation delay fallback, asymmetry, dynamic spikes, and FIFO packet ordering."""

    def test_07_default_delay_when_delay_files_missing(self):
        scene = self.make_lines(up='0,50m\n1000,0\n', down='0,50m\n1000,0\n')
        wk = self.make_workload('0 1448\n100 1448\n100 1448\n', name='probe.wk')
        data = self.run_json(
            scene, stop_time=1.0, cc='cubic', default_delay_ms=15.0, workload_file=wk, line_period_ms=1000
        )
        self.assertAlmostEqual(data['base_rtt_ms'], 30.0, places=2)
        self.assertAlmostEqual(data['est_rtt_ms'], 30.0, delta=3.0)

    def test_08_asymmetric_one_way_delay_sums_both_directions(self):
        scene = self.make_lines(
            up='0,50m\n2000,0\n',
            down='0,50m\n2000,0\n',
            up_delay='0,5\n# end_ms=2000\n',
            down_delay='0,80\n# end_ms=2000\n',
        )
        wk = self.make_workload('\n'.join(f'{0 if i == 0 else 150} 1448' for i in range(6)) + '\n')
        data, full = self.run_pair(
            scene, stop_time=2.0, cc='cubic', workload_file=wk, line_period_ms=2000, name='asym'
        )
        self.assertAlmostEqual(data['base_rtt_ms'], 85.0, places=2)
        self.assertTrue(full.rtts)
        min_rtt = min(r for _, r in full.rtts)
        self.assertGreaterEqual(min_rtt, 84.5)
        self.assertLessEqual(min_rtt, 88.0)

    def test_09_dynamic_delay_spike_and_recovery_timeline(self):
        # Base RTT: 0..1s = 20ms, 1..2s = 90ms (80+10), 2..3s = 20ms
        scene = self.make_lines(
            up='0,50m\n3000,0\n',
            down='0,50m\n3000,0\n',
            up_delay='0,10\n1000,80\n2000,10\n# end_ms=3000\n',
            down_delay='0,10\n# end_ms=3000\n',
        )
        # Multi-segment probes (6000 B = 5 MSS) ensure every probe yields majority
        # immediate intra-probe ACKs under TCP DelAckCount=2 so median RTT tracks link delay.
        wk = self.make_workload('\n'.join(f'{0 if i == 0 else 40} 6000' for i in range(70)) + '\n')
        full = self.run_full(
            scene, stop_time=3.0, cc='cubic', workload_file=wk,
            line_period_ms=3000, rtt_sample_log=True, name='spike',
        )
        before = [r for t, r in full.rtts if 0.15 <= t <= 0.95]
        during = [r for t, r in full.rtts if 1.15 <= t <= 1.95]
        after = [r for t, r in full.rtts if 2.15 <= t <= 2.85]
        self.assertTrue(before and during and after)
        self.assertAlmostEqual(statistics.median(before), 22.0, delta=3.0)
        self.assertAlmostEqual(statistics.median(during), 92.0, delta=4.0)
        self.assertAlmostEqual(statistics.median(after), 22.0, delta=3.0)

    def test_10_sharp_delay_drop_preserves_packet_order_without_spurious_retx(self):
        # Sharp drop from 100ms -> 5ms at t=500ms while sending rapid small packets
        scene = self.make_lines(
            up='0,50m\n1500,0\n',
            down='0,50m\n1500,0\n',
            up_delay='0,100\n500,5\n# end_ms=1500\n',
            down_delay='0,10\n# end_ms=1500\n',
        )
        wk = self.make_workload('\n'.join(f'{0 if i == 0 else 5} 1448' for i in range(180)) + '\n')
        data = self.run_json(scene, stop_time=1.5, cc='cubic', workload_file=wk, line_period_ms=1500)
        self.assertEqual(data['requests'], 180)
        self.assertEqual(data['retrans_pkts'], 0)


if __name__ == '__main__':
    unittest.main()
