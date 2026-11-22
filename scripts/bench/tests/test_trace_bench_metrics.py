#!/usr/bin/env python3
"""Exact metric-calculation verification tests for tcp-trace-bench and eval_report."""
from __future__ import annotations

import math
import sys
import unittest

from scripts.bench.tests.harness import (
    ROOT,
    LineScenario,
    TraceBenchCase,
    compute_single_request_fct,
    line_integral_mbps,
    parse_line_points,
    recompute_power_from_qdelays,
    recompute_rtt_percentiles,
)

sys.path.insert(0, str(ROOT / 'scripts/eval'))
import eval_report  # noqa: E402


class TraceBenchCapacityMetricsTests(TraceBenchCase):
    """Verify exact piecewise and cross-period truncated LineIntegral capacity calculations."""

    def test_01_capacity_integral_piecewise_and_cross_period_truncation(self):
        up_text = '0,10m\n500,30m\n1000,0\n'
        up_pts = parse_line_points(up_text, is_capacity=True)
        scene = self.make_lines(
            up=up_text,
            down='0,100m\n1000,0\n',
            up_delay='0,10\n# end_ms=1000\n',
            down_delay='0,10\n# end_ms=1000\n',
        )

        # Case A: Fixed stopTime=2.25s over 1000ms period ->
        #   2 full cycles (2 * (10*500 + 30*500) = 40000) + partial 250ms at 10m (2500) = 42500 / 2250
        fixed = self.run_json(scene, stop_time=2.25, cc='cubic', line_period_ms=1000)
        expected_fixed = (2.0 * (10.0 * 500.0 + 30.0 * 500.0) + 10.0 * 250.0) / 2250.0
        self.assertAlmostEqual(fixed['capacity_mbps'], expected_fixed, places=5)
        self.assertAlmostEqual(fixed['capacity_mbps'], line_integral_mbps(up_pts, 2250.0, 1000), places=5)

        # Case B: Workload finishing mid-cycle (around ~1.76s -> 1 full cycle + 500ms@10m + ~260ms@30m)
        wk = self.make_workload('0 1000\n1750 1000\n', name='mid_cycle.wk')
        wk_data = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=wk, line_period_ms=1000)
        expected_wk_cap = line_integral_mbps(up_pts, wk_data['duration_s'] * 1000.0, 1000)
        self.assertGreater(wk_data['duration_s'], 1.75)
        self.assertLess(wk_data['duration_s'], 1.85)
        self.assertAlmostEqual(wk_data['capacity_mbps'], expected_wk_cap, places=5)


class TraceBenchPowerAndRttMetricsTests(TraceBenchCase):
    """Independently recompute per-packet qdelay, time-weighted base RTT, RTT percentiles, and power."""

    def test_02_power_and_qdelay_exact_recomputation_static_link(self):
        up_delay_text = '0,10\n# end_ms=3000\n'
        down_delay_text = '0,20\n# end_ms=3000\n'
        scene = self.make_lines(
            up='0,5m\n3000,0\n',
            down='0,50m\n3000,0\n',
            up_delay=up_delay_text,
            down_delay=down_delay_text,
        )
        # Tight 25 KB buffer builds genuine queue + drops under cubic so qdelay & P95 are non-trivial
        js, full = self.run_pair(scene, stop_time=3.0, cc='cubic', queue_bytes=25000, name='static_power')
        self.assertGreater(len(full.qdelays), 100)
        self.assertGreater(len(full.rtts), 50)

        ref = recompute_power_from_qdelays(
            full.qdelays,
            up_delay_pts=parse_line_points(up_delay_text, is_capacity=False),
            down_delay_pts=parse_line_points(down_delay_text, is_capacity=False),
            duration_ms=js['duration_s'] * 1000.0,
            period_ms=0,
            capacity_mbps=js['capacity_mbps'],
            throughput_mbps=js['throughput_mbps'],
        )
        rtt_ref = recompute_rtt_percentiles(full.rtts)

        # Verify evalJson=1 metrics against per-packet receiver qdelay log recomputation
        self.assertAlmostEqual(js['base_rtt_ms'], 30.0, places=5)
        self.assertAlmostEqual(js['base_rtt_ms'], ref['base_rtt_ms'], places=5)
        self.assertAlmostEqual(js['qdelay_avg_ms'], ref['qdelay_avg_ms'], places=4)
        self.assertAlmostEqual(js['qdelay_p95_ms'], ref['qdelay_p95_ms'], places=4)
        self.assertAlmostEqual(js['qdelay_tail_ms'], ref['qdelay_tail_ms'], places=4)
        self.assertAlmostEqual(js['est_rtt_ms'], ref['est_rtt_ms'], places=4)
        self.assertAlmostEqual(js['utilization'], ref['utilization'], places=5)
        self.assertAlmostEqual(js['util_power'], ref['util_power'], places=4)
        self.assertAlmostEqual(js['norm_power'], ref['norm_power'], places=5)
        self.assertAlmostEqual(js['norm_power_p95'], ref['norm_power_p95'], places=5)
        self.assertAlmostEqual(js['norm_power_tail'], ref['norm_power_tail'], places=5)
        self.assertAlmostEqual(js['tput_power'], ref['tput_power'], places=4)
        # With cubic queue buildup, P95 queuing delay exceeds mean -> P95 norm power < mean norm power
        self.assertGreater(js['qdelay_p95_ms'], js['qdelay_avg_ms'])
        self.assertLess(js['norm_power_p95'], js['norm_power'])

        # Cross-check --evalJson=0 -metrics.json fields too
        flow0 = full.metrics['flows'][0]
        self.assertEqual(flow0['rtt_samples'], len(full.rtts))
        self.assertAlmostEqual(full.metrics['avg_capacity_mbps'], js['capacity_mbps'], places=3)
        self.assertAlmostEqual(full.metrics['total_throughput_mbps'], js['throughput_mbps'], places=3)
        self.assertAlmostEqual(flow0['rtt_mean_ms'], rtt_ref['rtt_mean_ms'], places=3)
        self.assertAlmostEqual(flow0['rtt_p50_ms'], rtt_ref['rtt_p50_ms'], places=3)
        self.assertAlmostEqual(flow0['rtt_p95_ms'], rtt_ref['rtt_p95_ms'], places=3)
        self.assertAlmostEqual(flow0['rtt_p99_ms'], rtt_ref['rtt_p99_ms'], places=3)

    def test_03_per_packet_qdelay_immune_to_delay_spikes_delayed_ack_and_fair_base_rtt(self):
        # Base RTT jumps 20ms -> 100ms at t=1s, then back to 20ms at t=2s.
        # Exact time-weighted average base RTT = (1000*20 + 1000*100 + 1000*20) / 3000 = 46.666667 ms.
        up_delay_text = '0,10\n1000,90\n2000,10\n# end_ms=3000\n'
        down_delay_text = '0,10\n# end_ms=3000\n'
        scene = self.make_lines(
            up='0,20m\n3000,0\n',
            down='0,50m\n3000,0\n',
            up_delay=up_delay_text,
            down_delay=down_delay_text,
        )
        up_pts = parse_line_points(up_delay_text, is_capacity=False)
        down_pts = parse_line_points(down_delay_text, is_capacity=False)
        expected_base_rtt = (1000.0 * 20.0 + 1000.0 * 100.0 + 1000.0 * 20.0) / 3000.0

        # Part A: Time-weighted base_rtt_ms is IDENTICAL across all CCs and per-packet qdelay matches log
        for cc in ('copa_nc', 'bbr', 'cubic'):
            js, full = self.run_pair(scene, stop_time=3.0, cc=cc, name=f'dyn_{cc}')
            self.assertGreater(len(full.qdelays), 100)
            ref = recompute_power_from_qdelays(
                full.qdelays,
                up_delay_pts=up_pts,
                down_delay_pts=down_pts,
                duration_ms=js['duration_s'] * 1000.0,
                period_ms=0,
                capacity_mbps=js['capacity_mbps'],
                throughput_mbps=js['throughput_mbps'],
            )
            self.assertAlmostEqual(js['base_rtt_ms'], expected_base_rtt, places=5, msg=cc)
            self.assertAlmostEqual(js['qdelay_avg_ms'], ref['qdelay_avg_ms'], places=4, msg=cc)
            self.assertAlmostEqual(js['qdelay_p95_ms'], ref['qdelay_p95_ms'], places=4, msg=cc)
            self.assertAlmostEqual(js['norm_power'], ref['norm_power'], places=5, msg=cc)
            self.assertAlmostEqual(js['norm_power_p95'], ref['norm_power_p95'], places=5, msg=cc)

        # Part B: Uncongested 2-MSS probes every 40ms across +80ms/-80ms delay jump & drop:
        # TCP Delayed ACK inflates ACK RTT by +40ms AND transition ACKs lag behind the -80ms drop,
        # yet per-packet receiver queuing delay (t_recv - t_enq) stays strictly near zero!
        wk = self.make_workload('\n'.join(f'{0 if i == 0 else 40} 2000' for i in range(65)) + '\n')
        probe_js = self.run_json(scene, stop_time=3.0, cc='cubic', workload_file=wk, line_period_ms=3000)
        self.assertLess(probe_js['qdelay_avg_ms'], 1.5)
        self.assertLess(probe_js['qdelay_p95_ms'], 2.0)


class TraceBenchFctMetricsTests(TraceBenchCase):
    """Exact verification of RCT percentiles, FCT slowdown, P95 FCT slowdown, clamping, and QoE."""

    def test_04_single_request_fct_slowdown_qoe_and_throughput_exact(self):
        scene = self.make_lines(
            up='0,10m\n5000,0\n',
            down='0,100m\n5000,0\n',
            up_delay='0,15\n# end_ms=5000\n',
            down_delay='0,25\n# end_ms=5000\n',
        )
        size = 200000
        wk = self.make_workload(f'0 {size}\n', name='single.wk')
        js = self.run_json(scene, stop_time=5.0, cc='cubic', workload_file=wk, line_period_ms=5000)

        # Single chunk arrives at arrSec[0] = 0.001s and stops simulation immediately on completion
        ref = compute_single_request_fct(
            duration_s=js['duration_s'],
            arr_sec=0.001,
            size_bytes=size,
            request_base_rtt_ms=40.0,
            capacity_mbps=js['capacity_mbps'],
        )
        self.assertEqual(js['requests'], 1)
        self.assertEqual(js['requests_expected'], 1)
        self.assertAlmostEqual(js['capacity_mbps'], 10.0, places=5)
        self.assertAlmostEqual(js['throughput_mbps'], ref['throughput_mbps'], places=5)
        self.assertAlmostEqual(js['rct_mean_ms'], ref['rct_ms'], places=4)
        self.assertAlmostEqual(js['rct_p50_ms'], ref['rct_ms'], places=4)
        self.assertAlmostEqual(js['rct_p95_ms'], ref['rct_ms'], places=4)
        # Slow-start + handshake makes actual RCT > ideal (40 + 160 = 200ms) -> non-clamped slowdown
        self.assertGreater(ref['fct_slowdown'], 1.1)
        self.assertAlmostEqual(js['fct_slowdown'], ref['fct_slowdown'], places=5)
        self.assertAlmostEqual(js['fct_slowdown_p95'], ref['fct_slowdown'], places=5)
        self.assertAlmostEqual(js['mortise_qoe'], ref['mortise_qoe'], places=4)

    def test_05_fct_slowdown_lower_clamp_and_p95_nearest_rank_boundary(self):
        # Part A: Lower clamp sd = max(rct / ideal, 1.0)
        # Asymmetric link: up=5ms, down=95ms -> base_rtt=100ms, ideal(1KB) ~= 100.08ms.
        # After chunk 0 establishes TCP handshake (~106ms, sd ~= 1.06), chunks 1..20 arrive on the
        # open connection and complete in ~6ms (one-way up delay) << 100ms -> clamped to sd = 1.0.
        # With N=21 chunks, ceil(0.95*21)=20 -> 20th sorted sd is 1.0, while geometric mean > 1.0!
        asym = self.make_lines(
            up='0,100m\n10000,0\n',
            down='0,100m\n10000,0\n',
            up_delay='0,5\n# end_ms=10000\n',
            down_delay='0,95\n# end_ms=10000\n',
            name='clamp',
        )
        wk_clamp = self.make_workload('\n'.join(f'{0 if i == 0 else 200} 1000' for i in range(21)) + '\n')
        d_clamp = self.run_json(asym, stop_time=10.0, cc='cubic', workload_file=wk_clamp, line_period_ms=10000)
        self.assertEqual(d_clamp['requests'], 21)
        self.assertLess(d_clamp['rct_p50_ms'], 15.0)
        self.assertGreater(d_clamp['fct_slowdown'], 1.0)
        self.assertAlmostEqual(d_clamp['fct_slowdown_p95'], 1.0, places=6)

        # Part B: Nearest-rank P95 exact step transition (N=19 vs N=20) & geometric vs arithmetic mean
        # Chunk 0: 400KB (~340ms ideal, sd ~= 1.2)
        # Chunk 1: 1KB arriving 10ms later -> HoL-blocked behind 400KB -> huge outlier (sd > 12x)
        # Remaining chunks: spaced 1KB after full drain -> sd = 1.0
        #   N=19 -> ceil(0.95*19) = 19 (rank 19/19 = MAX = outlier > 10x)
        #   N=20 -> ceil(0.95*20) = 19 (rank 19/20 = 2nd largest = Chunk 0 < 2x!)
        scene = self.make_lines(
            up='0,10m\n10000,0\n',
            down='0,100m\n10000,0\n',
            up_delay='0,10\n# end_ms=10000\n',
            down_delay='0,10\n# end_ms=10000\n',
            name='p95_step',
        )
        base_lines = ['0 400000', '10 1000', '1200 1000']
        wk19 = self.make_workload('\n'.join(base_lines + ['80 1000'] * 16) + '\n', name='n19.wk')
        wk20 = self.make_workload('\n'.join(base_lines + ['80 1000'] * 17) + '\n', name='n20.wk')

        d19 = self.run_json(scene, stop_time=10.0, cc='cubic', workload_file=wk19, line_period_ms=10000)
        d20 = self.run_json(scene, stop_time=10.0, cc='cubic', workload_file=wk20, line_period_ms=10000)

        self.assertEqual(d19['requests'], 19)
        self.assertEqual(d20['requests'], 20)
        self.assertGreater(d19['fct_slowdown_p95'], 10.0)
        self.assertLess(d20['fct_slowdown_p95'], 2.0)

        # Verify geometric mean (NOT arithmetic mean) on N=19:
        # Arithmetic mean >= (outlier + 18 * 1.0) / 19; geometric mean is far smaller
        arith_lower_bound = (d19['fct_slowdown_p95'] + 18.0) / 19.0
        self.assertLess(d19['fct_slowdown'], arith_lower_bound - 0.3)
        # Also verify rct percentile ordering
        self.assertLessEqual(d19['rct_p50_ms'], d19['rct_mean_ms'])
        self.assertLessEqual(d19['rct_mean_ms'], d19['rct_p95_ms'])

    def test_06_fct_ideal_ms_uses_periodic_dynamic_delay_at_request_arrival(self):
        # Period = 2000ms: [0, 1000) -> 20ms RTT, [1000, 2000) -> 110ms RTT
        scene = self.make_lines(
            up='0,50m\n2000,0\n',
            down='0,50m\n2000,0\n',
            up_delay='0,10\n1000,100\n# end_ms=2000\n',
            down_delay='0,10\n# end_ms=2000\n',
        )
        # Single 100 KB request arriving at t = 3.201s -> wraps 3201 % 2000 = 1201ms -> base RTT = 110ms!
        # Handshake already completed at t ~= 0.02s during the 20ms phase, so cwnd = IW10 and
        # transferring 100 KB takes multiple 110ms RTTs (> ideal = 110 + 16 = 126ms -> not clamped).
        size = 100000
        wk = self.make_workload(f'3200 {size}\n', name='wrapped_delay.wk')
        js = self.run_json(scene, stop_time=6.0, cc='cubic', workload_file=wk, line_period_ms=2000)

        ref_dynamic = compute_single_request_fct(
            duration_s=js['duration_s'],
            arr_sec=3.201,
            size_bytes=size,
            request_base_rtt_ms=110.0,
            capacity_mbps=js['capacity_mbps'],
        )
        ref_wrong_initial = compute_single_request_fct(
            duration_s=js['duration_s'],
            arr_sec=3.201,
            size_bytes=size,
            request_base_rtt_ms=20.0,
            capacity_mbps=js['capacity_mbps'],
        )
        self.assertGreater(ref_dynamic['fct_slowdown'], 1.2)
        self.assertAlmostEqual(js['rct_mean_ms'], ref_dynamic['rct_ms'], places=4)
        self.assertAlmostEqual(js['fct_slowdown'], ref_dynamic['fct_slowdown'], places=5)
        self.assertAlmostEqual(js['fct_slowdown_p95'], ref_dynamic['fct_slowdown'], places=5)
        # Explicitly confirm it does NOT match the initial 20ms base RTT
        self.assertGreater(abs(js['fct_slowdown'] - ref_wrong_initial['fct_slowdown']), 2.0)


class TraceBenchConsistencyAndParityTests(TraceBenchCase):
    """Verify throughput/drop/retx consistency, Python eval_report exact formulas, and real datasets."""

    def test_07_throughput_drop_rate_and_retrans_ratio_consistency(self):
        scene = self.make_lines(
            up='0,5m\n6000,0\n',
            down='0,50m\n6000,0\n',
            up_delay='0,10\n# end_ms=6000\n',
            down_delay='0,10\n# end_ms=6000\n',
        )
        # Uncongested workload: zero drops and zero retransmissions, exact throughput identity
        wk_clean = self.make_workload('0 10000\n200 15000\n200 25000\n', name='clean.wk')
        d_clean = self.run_json(
            scene, stop_time=6.0, cc='cubic', queue_bytes=500000, workload_file=wk_clean, line_period_ms=6000
        )
        total_clean_bytes = 10000 + 15000 + 25000
        self.assertAlmostEqual(
            d_clean['throughput_mbps'], total_clean_bytes * 8.0 / d_clean['duration_s'] / 1e6, places=5
        )
        self.assertEqual(d_clean['drop_rate'], 0.0)
        self.assertEqual(d_clean['retrans_pkts'], 0)
        self.assertEqual(d_clean['retrans_ratio'], 0.0)

        # Tight-buffer large workload: genuine drops and retransmissions, still exact throughput identity
        wk_heavy = self.make_workload('0 600000\n', name='heavy.wk')
        d_heavy = self.run_json(
            scene, stop_time=6.0, cc='cubic', queue_bytes=10000, workload_file=wk_heavy, line_period_ms=6000
        )
        self.assertAlmostEqual(
            d_heavy['throughput_mbps'], 600000 * 8.0 / d_heavy['duration_s'] / 1e6, places=5
        )
        self.assertGreater(d_heavy['drop_rate'], 0.0)
        self.assertLess(d_heavy['drop_rate'], 1.0)
        self.assertGreater(d_heavy['retrans_pkts'], 0)
        self.assertGreater(d_heavy['retrans_ratio'], 0.0)
        self.assertLess(d_heavy['retrans_ratio'], 1.0)

    def test_08_eval_report_python_power_and_fct_p95_exact(self):
        # Independently verify Linux-kernel Python evaluator (eval_report.py) exact formulas
        d = self.subdir('eval_py')
        dl_log = d / 'downlink.log'
        # 20 delivered packets (1000 B each) across t=0..1000ms (1.0s), 25 opportunities (1000 B each),
        # 1 dropped packet (1000 B); qdelays = [10]*19 + [110]; sorted chronologically like Mahimahi logs.
        events: list[tuple[int, str]] = []
        for i in range(25):
            events.append((i * 40, f'{i * 40} # 1000'))
        for i in range(19):
            events.append((i * 50, f'{i * 50} - 1000 10'))
        events.append((500, '500 d 1 1000'))
        events.append((1000, '1000 - 1000 110'))
        events.sort(key=lambda item: item[0])
        dl_log.write_text('\n'.join(line for _, line in events) + '\n', encoding='utf-8')

        m = eval_report.parse_downlink_log(dl_log, min_rtt_ms=20.0, warmup_s=0.0)
        # duration = 1.0s, delivered = 20000 B (0.16 Mbps), opportunity = 25000 B (0.20 Mbps) -> util = 0.8
        # qdelays sorted: 19x 10ms + 1x 110ms -> avg = 15.0ms, linear p95 = (19*0.95=18.05) -> 10*0.95 + 110*0.05 = 15.0ms
        # Let's verify exact math:
        self.assertAlmostEqual(m['duration_s'], 1.0, places=6)
        self.assertAlmostEqual(m['capacity_mbps'], 0.20, places=6)
        self.assertAlmostEqual(m['throughput_mbps'], 0.16, places=6)
        self.assertAlmostEqual(m['utilization'], 0.80, places=6)
        self.assertAlmostEqual(m['qdelay_avg_ms'], 15.0, places=6)
        self.assertAlmostEqual(m['qdelay_p95_ms'], 15.0, places=6)
        self.assertAlmostEqual(m['norm_power'], 0.80 * 20.0 / 35.0, places=6)
        self.assertAlmostEqual(m['norm_power_p95'], 0.80 * 20.0 / 35.0, places=6)
        self.assertAlmostEqual(m['drop_rate'], 1000.0 / 21000.0, places=6)

        # Verify RCT CSV slowdown geometric mean & linear-interpolated P95
        rct_csv = d / 'rct.csv'
        # base_rtt=20ms, cap=8Mbps (1000 B/ms), size=10000B -> ideal = 20 + 10 = 30ms
        # 20 requests: 18 with rct=30ms (sd=1.0), 1 with rct=60ms (sd=2.0), 1 with rct=300ms (sd=10.0)
        rows = ['id,size,client_send,server_recv,client_recv,rct']
        rcts = [30.0] * 18 + [60.0, 300.0]
        for idx, rct in enumerate(rcts, 1):
            rows.append(f'{idx},10000,0,0,0,{rct}')
        rows.append('0,0,0,0,0,0')
        rct_csv.write_text('\n'.join(rows) + '\n', encoding='utf-8')

        r = eval_report.parse_rct_csv(rct_csv, delivered_bytes=200000, base_rtt_ms=20.0, capacity_mbps=8.0)
        expected_gmean = math.exp((math.log(2.0) + math.log(10.0)) / 20.0)
        # Sorted sds = [1.0]*18 + [2.0, 10.0]; k = 19 * 0.95 = 18.05 -> 2.0*0.95 + 10.0*0.05 = 2.40
        expected_p95 = 2.0 * 0.95 + 10.0 * 0.05
        self.assertEqual(r['requests'], 20)
        self.assertAlmostEqual(r['rct_mean_ms'], sum(rcts) / 20.0, places=6)
        self.assertAlmostEqual(r['rct_p50_ms'], 30.0, places=6)
        self.assertAlmostEqual(r['fct_slowdown'], expected_gmean, places=6)
        self.assertAlmostEqual(r['fct_slowdown_p95'], expected_p95, places=6)
        self.assertAlmostEqual(r['fct_slowdown_tail'], expected_p95, places=6)

        # Also verify configurable tail_percentile=50 and 99 on both downlink.log and rct.csv
        m99 = eval_report.parse_downlink_log(dl_log, min_rtt_ms=20.0, warmup_s=0.0, tail_percentile=99.0)
        # k = 19 * 0.99 = 18.81 -> 10*0.19 + 110*0.81 = 91.0ms
        self.assertAlmostEqual(m99['tail_percentile'], 99.0, places=6)
        self.assertAlmostEqual(m99['qdelay_tail_ms'], 91.0, places=6)
        self.assertAlmostEqual(m99['norm_power_tail'], 0.80 * 20.0 / 111.0, places=6)
        self.assertAlmostEqual(m99['qdelay_p95_ms'], 15.0, places=6)  # true P95 stays unchanged

        r50 = eval_report.parse_rct_csv(rct_csv, delivered_bytes=200000, base_rtt_ms=20.0, capacity_mbps=8.0, tail_percentile=50.0)
        r99 = eval_report.parse_rct_csv(rct_csv, delivered_bytes=200000, base_rtt_ms=20.0, capacity_mbps=8.0, tail_percentile=99.0)
        self.assertAlmostEqual(r50['fct_slowdown_tail'], 1.0, places=6)
        self.assertAlmostEqual(r50['rct_tail_ms'], 30.0, places=6)
        # k = 19 * 0.99 = 18.81 -> 2.0*0.19 + 10.0*0.81 = 8.48
        self.assertAlmostEqual(r99['fct_slowdown_tail'], 2.0 * 0.19 + 10.0 * 0.81, places=6)
        self.assertAlmostEqual(r99['fct_slowdown_p95'], expected_p95, places=6)  # true P95 stays unchanged

        with self.assertRaises(ValueError):
            eval_report.parse_downlink_log(dl_log, tail_percentile=0.0)
        with self.assertRaises(ValueError):
            eval_report.parse_rct_csv(rct_csv, tail_percentile=101.0)

    def test_10_configurable_tail_percentile_p50_p95_p99_exact(self):
        # Part A: Bulk flow with queue buildup -> verify P50, P95, P99 exact raw-RTT recomputation
        up_delay_text = '0,10\n# end_ms=3000\n'
        down_delay_text = '0,10\n# end_ms=3000\n'
        bulk_scene = self.make_lines(
            up='0,5m\n3000,0\n',
            down='0,50m\n3000,0\n',
            up_delay=up_delay_text,
            down_delay=down_delay_text,
            name='bulk_pct',
        )
        full = self.run_full(bulk_scene, stop_time=3.0, cc='cubic', queue_bytes=40000, rtt_sample_log=True, name='bulk_pct_rtts')
        up_pts = parse_line_points(up_delay_text, is_capacity=False)
        down_pts = parse_line_points(down_delay_text, is_capacity=False)

        results = {}
        for p in (50.0, 95.0, 99.0):
            js = self.run_json(bulk_scene, stop_time=3.0, cc='cubic', queue_bytes=40000, tail_percentile=p)
            ref = recompute_power_from_qdelays(
                full.qdelays,
                up_delay_pts=up_pts,
                down_delay_pts=down_pts,
                duration_ms=js['duration_s'] * 1000.0,
                period_ms=0,
                capacity_mbps=js['capacity_mbps'],
                throughput_mbps=js['throughput_mbps'],
                tail_percentile=p,
            )
            self.assertAlmostEqual(js['tail_percentile'], p, places=5)
            self.assertAlmostEqual(js['qdelay_tail_ms'], ref['qdelay_tail_ms'], places=4)
            self.assertAlmostEqual(js['norm_power_tail'], ref['norm_power_tail'], places=5)
            # True P95 fields stay constant regardless of --tailPercentile
            self.assertAlmostEqual(js['qdelay_p95_ms'], ref['qdelay_p95_ms'], places=4)
            self.assertAlmostEqual(js['norm_power_p95'], ref['norm_power_p95'], places=5)
            results[p] = js

        self.assertLessEqual(results[50.0]['qdelay_tail_ms'], results[95.0]['qdelay_tail_ms'])
        self.assertLessEqual(results[95.0]['qdelay_tail_ms'], results[99.0]['qdelay_tail_ms'])
        self.assertGreaterEqual(results[50.0]['norm_power_tail'], results[95.0]['norm_power_tail'])
        self.assertGreaterEqual(results[95.0]['norm_power_tail'], results[99.0]['norm_power_tail'])
        self.assertGreater(results[99.0]['qdelay_tail_ms'], results[50.0]['qdelay_tail_ms'])

        # Part B: 20-request workload with 1 huge HoL outlier ->
        #   P50 (rank 10/20) == 1.0, P95 (rank 19/20) < 2.0, P99 (rank 20/20 = max) > 10.0!
        wk_scene = self.make_lines(
            up='0,10m\n10000,0\n',
            down='0,100m\n10000,0\n',
            up_delay='0,10\n# end_ms=10000\n',
            down_delay='0,10\n# end_ms=10000\n',
            name='wk_pct',
        )
        wk20 = self.make_workload('\n'.join(['0 400000', '10 1000', '1200 1000'] + ['80 1000'] * 17) + '\n', name='wk20.wk')
        w50 = self.run_json(wk_scene, stop_time=10.0, cc='cubic', workload_file=wk20, line_period_ms=10000, tail_percentile=50.0)
        w95 = self.run_json(wk_scene, stop_time=10.0, cc='cubic', workload_file=wk20, line_period_ms=10000, tail_percentile=95.0)
        w99 = self.run_json(wk_scene, stop_time=10.0, cc='cubic', workload_file=wk20, line_period_ms=10000, tail_percentile=99.0)

        self.assertAlmostEqual(w50['fct_slowdown_tail'], 1.0, places=6)
        self.assertAlmostEqual(w50['rct_tail_ms'], w50['rct_p50_ms'], places=5)
        self.assertAlmostEqual(w95['fct_slowdown_tail'], w95['fct_slowdown_p95'], places=6)
        self.assertLess(w95['fct_slowdown_tail'], 2.0)
        self.assertGreater(w99['fct_slowdown_tail'], 10.0)
        self.assertAlmostEqual(w99['fct_slowdown_p95'], w95['fct_slowdown_p95'], places=6)

    def test_09_real_basic_dataset_scenarios_invariants_and_formulas(self):
        cases = [
            ('asymmetric_delay', 85.0, 120.0),
            ('delay_spike', 23.75, 120.0),
            ('bandwidth_drop_severe', 20.0, 120.0),
            ('long_outage', 20.0, 120.0),
        ]
        pageload_wk = ROOT / 'datasets/workloads/pageload.wk'
        for name, expected_base_rtt, dur_s in cases:
            scene_dir = ROOT / 'datasets/lines/basic' / name
            scene = LineScenario(
                up=scene_dir / 'up.txt',
                down=scene_dir / 'down.txt',
                up_delay=scene_dir / 'up.delay.txt',
                down_delay=scene_dir / 'down.delay.txt',
            )
            bulk = self.run_json(scene, stop_time=dur_s, cc='bbr')
            self.assertAlmostEqual(bulk['base_rtt_ms'], expected_base_rtt, places=3, msg=name)
            self.assertGreater(bulk['throughput_mbps'], 0.0, name)
            self.assertAlmostEqual(
                bulk['utilization'], bulk['throughput_mbps'] / bulk['capacity_mbps'], places=5, msg=name
            )
            self.assertAlmostEqual(
                bulk['est_rtt_ms'], bulk['base_rtt_ms'] + bulk['qdelay_avg_ms'], places=4, msg=name
            )
            expected_np = bulk['utilization'] * bulk['base_rtt_ms'] / bulk['est_rtt_ms']
            expected_p95_np = bulk['utilization'] * bulk['base_rtt_ms'] / (
                bulk['base_rtt_ms'] + bulk['qdelay_p95_ms']
            )
            self.assertAlmostEqual(bulk['norm_power'], expected_np, places=4, msg=name)
            self.assertAlmostEqual(bulk['norm_power_p95'], expected_p95_np, places=4, msg=name)
            self.assertGreaterEqual(bulk['norm_power_p95'], 0.0, name)
            self.assertLessEqual(bulk['norm_power_p95'], 1.001, name)
            self.assertLessEqual(bulk['norm_power'], 1.001, name)

            wk = self.run_json(
                scene, stop_time=300.0, cc='bbr',
                workload_file=pageload_wk, line_period_ms=int(dur_s * 1000),
            )
            self.assertEqual(wk['requests'], wk['requests_expected'], name)
            self.assertGreaterEqual(wk['fct_slowdown'], 1.0, name)
            self.assertGreaterEqual(wk['fct_slowdown_p95'], wk['fct_slowdown'] - 1e-6, name)
            self.assertLessEqual(wk['rct_p50_ms'], wk['rct_p95_ms'], name)


if __name__ == '__main__':
    unittest.main()
