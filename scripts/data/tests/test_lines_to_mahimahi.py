#!/usr/bin/env python3
"""Unit tests for datasets/lines CSV -> Mahimahi conversion."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts/bench'))
sys.path.insert(0, str(ROOT / 'scripts/data'))

import lines_common  # noqa: E402
import lines_to_mahimahi  # noqa: E402


class LinesToMahimahiTests(unittest.TestCase):
    def test_direction_mapping_and_asymmetric_delay(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene_dir = root / 'asym'
            scene_dir.mkdir()
            # lines up_cap/up_delay = TCP data (50 Mbps, 5 ms delay)
            # lines down_cap/down_delay = TCP ACK (100 Mbps, 80 ms delay)
            (scene_dir / 'line.csv').write_text(
                'time,up_cap,down_cap,up_delay,down_delay,comment\n'
                '0,50mbps,100mbps,5ms,80ms,asymmetric link\n'
                '1s,0,0,,,end\n',
                encoding='utf-8',
            )

            scenario = lines_common.discover(scene_dir)[0]
            conv = lines_to_mahimahi.convert_scenario(scenario, root / 'out')

            self.assertEqual(conv.period_ms, 1000)
            self.assertAlmostEqual(conv.duration_s, 1.0)
            self.assertAlmostEqual(conv.base_rtt_ms_bulk, 85.0)
            self.assertAlmostEqual(conv.base_rtt_ms_period, 85.0)
            self.assertAlmostEqual(conv.data_capacity_mbps_bulk, 50.0, places=4)

            # lines up (data) -> Mahimahi downlink (server -> client)
            dl_pkts = conv.mm_downlink_trace.read_text(encoding='utf-8').splitlines()
            ul_pkts = conv.mm_uplink_trace.read_text(encoding='utf-8').splitlines()
            # 50 Mbps over 1s = 50_000_000 / 8 / 1500 = 4166.67 -> 4166 packets
            self.assertEqual(len(dl_pkts), 4166)
            # 100 Mbps over 1s = 8333.33 -> 8333 packets
            self.assertEqual(len(ul_pkts), 8333)
            # Constant delays compress to 1 line with explicit direction mapping
            self.assertEqual(conv.mm_downlink_delay.read_text(encoding='utf-8').splitlines(), ['5'])
            self.assertEqual(conv.mm_uplink_delay.read_text(encoding='utf-8').splitlines(), ['80'])

    def test_mixed_count_mbps_outage_and_dynamic_delay(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene_csv = root / 'dyn.csv'
            # 0..100ms: 24mbps (=200 pkts); 100..200ms: outage (0 pkts); 200..300ms: 12mbps (=100 pkts)
            scene_csv.write_text(
                'time,up_cap,down_cap,delay,up_delay\n'
                '0,24mbps,12mbps,10,10\n'
                '100,0,,,50\n'
                '200,12mbps,,,10\n'
                '300,0,0,,\n',
                encoding='utf-8',
            )

            scenario = lines_common.discover(scene_csv)[0]
            conv = lines_to_mahimahi.convert_scenario(scenario, root / 'out')

            dl_ts = [int(x) for x in conv.mm_downlink_trace.read_text(encoding='utf-8').splitlines()]
            self.assertEqual(len(dl_ts), 300)
            self.assertTrue(all(t < 100 or t >= 200 for t in dl_ts))

            dl_delays = [int(x) for x in conv.mm_downlink_delay.read_text(encoding='utf-8').splitlines()]
            self.assertEqual(len(dl_delays), 30)  # 300ms / 10ms
            self.assertEqual(dl_delays[:10], [10] * 10)
            self.assertEqual(dl_delays[10:20], [50] * 10)
            self.assertEqual(dl_delays[20:30], [10] * 10)
            # Downlink ACK delay fell back to delay=10ms -> compressed to 1 line
            self.assertEqual(conv.mm_uplink_delay.read_text(encoding='utf-8').splitlines(), ['10'])
            # Average base RTT = (10*100 + 50*100 + 10*100)/300 + 10 = 33.333ms
            self.assertAlmostEqual(conv.base_rtt_ms_bulk, 100.0 / 3.0, places=4)

    def test_missing_delay_uses_default_and_all_zero_capacity_has_sentinel(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene_csv = root / 'zero.csv'
            scene_csv.write_text('time,up_cap,down_cap\n0,0,12mbps\n500,0,0\n', encoding='utf-8')

            scenario = lines_common.discover(scene_csv)[0]
            conv = lines_to_mahimahi.convert_scenario(scenario, root / 'out', default_one_way_delay_ms=10.0)
            self.assertFalse(conv.has_up_delay_file)
            self.assertFalse(conv.has_down_delay_file)
            self.assertAlmostEqual(conv.base_rtt_ms_bulk, 20.0)
            dl_ts = [int(x) for x in conv.mm_downlink_trace.read_text(encoding='utf-8').splitlines()]
            self.assertEqual(len(dl_ts), 1)
            self.assertGreater(dl_ts[0], 500)

    def test_max_bulk_s_computes_truncated_window_base_rtt(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene_csv = root / 'trunc.csv'
            scene_csv.write_text(
                'time,cap,delay,up_delay\n'
                '0,24mbps,10,\n'
                '1s,,,50\n'
                '2s,0,,\n',
                encoding='utf-8',
            )

            scenario = lines_common.discover(scene_csv)[0]
            conv = lines_to_mahimahi.convert_scenario(scenario, root / 'out', max_bulk_s=1.0)
            self.assertAlmostEqual(conv.bulk_duration_s, 1.0)
            self.assertAlmostEqual(conv.duration_s, 2.0)
            self.assertAlmostEqual(conv.base_rtt_ms_bulk, 20.0)
            self.assertAlmostEqual(conv.base_rtt_ms_period, 40.0)

    def test_real_repo_basic_and_leocc_scenarios(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            asym = lines_common.discover(ROOT / 'datasets/lines/basic/asymmetric_delay')[0]
            conv_asym = lines_to_mahimahi.convert_scenario(asym, out)
            self.assertAlmostEqual(conv_asym.base_rtt_ms_bulk, 85.0)
            self.assertEqual(conv_asym.mm_downlink_delay.read_text(encoding='utf-8').strip(), '5')
            self.assertEqual(conv_asym.mm_uplink_delay.read_text(encoding='utf-8').strip(), '80')

            leo = lines_common.discover(ROOT / 'datasets/lines/leocc/leo-bw.csv')[0]
            conv_leo = lines_to_mahimahi.convert_scenario(leo, out)
            self.assertGreater(conv_leo.period_ms, 100_000)
            self.assertGreater(conv_leo.base_rtt_ms_bulk, 10.0)
            self.assertGreater(conv_leo.data_capacity_mbps_bulk, 10.0)


if __name__ == '__main__':
    unittest.main()
