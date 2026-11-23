#!/usr/bin/env python3
"""Comprehensive unit & ns-3 parity tests for unified CSV lines format."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BINARY = ROOT / 'ns3/ns-3/build/scratch/tcp-trace-bench/ns3-dev-tcp-trace-bench-optimized'
if str(ROOT / 'scripts/bench') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts/bench'))
import lines_common  # noqa: E402


class LinesCsvFormatTests(unittest.TestCase):
    def _run_ns3(self, csv_path: Path, *, stop_s: float, default_delay_ms: float = 10.0) -> dict:
        if not BINARY.is_file():
            self.skipTest('build tcp-trace-bench first')
        res = subprocess.run([
            str(BINARY), '--profile=lines', f'--lineFile={csv_path}',
            f'--defaultDelayMs={default_delay_ms}', f'--stopTime={stop_s}',
            '--ccs=cubic', '--segmentSize=1448', '--evalJson=1',
        ], text=True, capture_output=True, check=True)
        return json.loads(next(line for line in reversed(res.stdout.splitlines()) if line.startswith('{')))

    def test_01_arbitrary_column_order_parity(self):
        """Reordering header columns produces identical Python parses and ns-3 results."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            canonical = root / 'canonical.csv'
            reordered = root / 'reordered.csv'
            canonical.write_text(
                'time,up_cap,down_cap,up_delay,down_delay,comment\n'
                '0,40m,100m,10,20,start\n'
                '1s,20m,,,35,"spike, down"\n'
                '2s,0,0,,,end\n',
                encoding='utf-8',
            )
            reordered.write_text(
                'comment,down_delay,up_cap,time,up_delay,down_cap\n'
                'start,20,40m,0,10,100m\n'
                '"spike, down",35,20m,1s,,\n'
                'end,,0,2s,,0\n',
                encoding='utf-8',
            )
            p1 = lines_common.parse_line_csv(canonical)
            p2 = lines_common.parse_line_csv(reordered)
            self.assertEqual(p1, p2)

            d1 = self._run_ns3(canonical, stop_s=2.0)
            d2 = self._run_ns3(reordered, stop_s=2.0)
            for key in ('capacity_mbps', 'base_rtt_ms', 'throughput_mbps', 'utilization'):
                self.assertAlmostEqual(d1[key], d2[key], places=6, msg=key)

    def test_02_unified_fixed_8_column_template_fallback(self):
        """A single fixed 8-column header supports mixing cap/up_cap/down_cap and delay/up_delay/down_delay."""
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'fixed8.csv'
            p.write_text(
                'time,cap,up_cap,down_cap,delay,up_delay,down_delay,comment\n'
                '0,60m,,,10,,,symmetric 60m & 10ms each way\n'
                '1s,,20m,,,40,,only up drops to 20m & 40ms; down keeps 60m & 10ms\n'
                '2s,60m,,,10,,,both directions recover together\n'
                '3s,0,,,,,,end\n',
                encoding='utf-8',
            )
            parsed = lines_common.parse_line_csv(p)
            self.assertEqual(parsed['end_ms'], 3000)
            self.assertEqual([(t, round(v * 12, 4)) for t, v in parsed['up']],
                             [(0, 60.0), (1000, 20.0), (2000, 60.0), (3000, 0.0)])
            self.assertEqual([(t, round(v * 12, 4)) for t, v in parsed['down']],
                             [(0, 60.0), (3000, 0.0)])
            self.assertEqual(parsed['up_delay'], [(0, 10.0), (1000, 40.0), (2000, 10.0)])
            self.assertEqual(parsed['down_delay'], [(0, 10.0)])

            d = self._run_ns3(p, stop_s=3.0)
            # Capacity avg = (60 + 20 + 60) / 3 = 140/3 Mbps
            # Base RTT avg = (20 + 50 + 20) / 3 = 30 ms
            self.assertAlmostEqual(d['capacity_mbps'], 140.0 / 3.0, places=4)
            self.assertAlmostEqual(d['base_rtt_ms'], 30.0, places=3)

    def test_03_delay_precedence_matrix_python_and_ns3(self):
        """Verify all combinations of delay, up_delay, down_delay, and defaultDelayMs fallback."""
        cases = [
            # (name, csv_text, default_delay, exp_up_delay, exp_down_delay, exp_rtt_ms)
            (
                'delay_plus_up_delay',
                'time,cap,delay,up_delay\n0,40m,10,30\n1s,40m,15,40\n2s,0,,\n',
                99.0,
                [(0, 30.0), (1000, 40.0)],
                [(0, 10.0), (1000, 15.0)],
                (40.0 + 55.0) / 2.0,
            ),
            (
                'delay_plus_down_delay',
                'time,cap,delay,down_delay\n0,40m,12,28\n1s,0,,\n',
                99.0,
                [(0, 12.0)],
                [(0, 28.0)],
                40.0,
            ),
            (
                'only_up_delay_down_uses_default',
                'time,cap,up_delay\n0,40m,25\n1s,0,\n',
                15.0,
                [(0, 25.0)],
                None,
                40.0,
            ),
            (
                'only_down_delay_up_uses_default',
                'time,cap,down_delay\n0,40m,18\n1s,0,\n',
                12.0,
                None,
                [(0, 18.0)],
                30.0,
            ),
            (
                'delay_column_present_but_all_empty',
                'time,cap,delay\n0,40m,\n1s,0,\n',
                22.5,
                None,
                None,
                45.0,
            ),
            (
                'no_delay_columns_at_all',
                'time,cap\n0,40m\n1s,0\n',
                17.5,
                None,
                None,
                35.0,
            ),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, text, default_d, exp_ud, exp_dd, exp_rtt in cases:
                p = root / f'{name}.csv'
                p.write_text(text, encoding='utf-8')
                parsed = lines_common.parse_line_csv(p)
                self.assertEqual(parsed['up_delay'], exp_ud, name)
                self.assertEqual(parsed['down_delay'], exp_dd, name)
                info = lines_common.peek_line_csv_info(p)
                self.assertEqual(info, (parsed['end_ms'], exp_ud is not None, exp_dd is not None), name)

                d = self._run_ns3(p, stop_s=parsed['end_ms'] / 1000.0, default_delay_ms=default_d)
                self.assertAlmostEqual(d['base_rtt_ms'], exp_rtt, places=3, msg=name)

    def test_04_capacity_precedence_and_units_python_and_ns3(self):
        """Verify cap + up_cap/down_cap overrides and mixed packet/b/kb/mb/m units."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # cap + up_cap (no down_cap): up overrides cap when present, down always uses cap
            p1 = root / 'cap_plus_up.csv'
            p1.write_text(
                'time,cap,up_cap\n'
                '0,100m,2\n'          # up = 2 pkts/ms = 24 Mbps, down = 100 Mbps
                '1s,100m,10.33kb\n'   # up = 10.33 KiB/ms, down = 100 Mbps
                '2s,50m,\n'           # up falls back to cap = 50 Mbps, down = 50 Mbps
                '3s,0,\n',
                encoding='utf-8',
            )
            parsed1 = lines_common.parse_line_csv(p1)
            self.assertAlmostEqual(parsed1['up'][0][1], 2.0, places=9)
            self.assertAlmostEqual(parsed1['up'][1][1], 10.33 * 1024 / 1500.0, places=9)
            self.assertAlmostEqual(parsed1['up'][2][1], 50.0 / 12.0, places=9)
            self.assertAlmostEqual(parsed1['down'][0][1], 100.0 / 12.0, places=9)
            self.assertAlmostEqual(parsed1['down'][1][1], 50.0 / 12.0, places=9)
            d1 = self._run_ns3(p1, stop_s=3.0)
            exp_mbps = (24.0 + 10.33 * 1024 * 8 / 1000.0 + 50.0) / 3.0
            self.assertAlmostEqual(d1['capacity_mbps'], exp_mbps, places=4)

            # cap + down_cap (no up_cap): up uses cap, down overrides with 0 -> no TCP handshake
            p2 = root / 'cap_plus_down_zero.csv'
            p2.write_text('time,cap,down_cap\n0,50m,0\n500ms,0,\n', encoding='utf-8')
            d2 = self._run_ns3(p2, stop_s=0.5)
            self.assertEqual(d2['throughput_mbps'], 0.0)

    def test_05_time_and_delay_units_python_and_ns3(self):
        """Verify time (default ms, ms, s) and delay (default ms, us, ms, s) unit conversions."""
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'units_time_delay.csv'
            p.write_text(
                'time,cap,up_delay,down_delay,comment\n'
                '0ms,40m,500us,9.5ms,RTT 10ms\n'
                '0.5s,40m,0.03s,10ms,RTT 40ms\n'
                '1500MS,40m,20,20,RTT 40ms\n'
                '2S,0,,,end\n',
                encoding='utf-8',
            )
            parsed = lines_common.parse_line_csv(p)
            self.assertEqual(parsed['end_ms'], 2000)
            self.assertEqual(parsed['up_delay'], [(0, 0.5), (500, 30.0), (1500, 20.0)])
            self.assertEqual(parsed['down_delay'], [(0, 9.5), (500, 10.0), (1500, 20.0)])

            d = self._run_ns3(p, stop_s=2.0)
            # [0,0.5s): 10ms, [0.5,1.5s): 40ms, [1.5,2.0s): 40ms -> avg = (5 + 40 + 20)/2 = 32.5 ms
            self.assertAlmostEqual(d['base_rtt_ms'], 32.5, places=3)

    def test_06_comments_bom_and_rfc4180_quoting(self):
        """UTF-8 BOM, full-line # comments, quoted comments with commas & escaped quotes, short rows."""
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'quoted.csv'
            p.write_text(
                '\ufeff# Leading comment outside table\n'
                'time,cap,delay,comment\n'
                '0,24m,10,"start, ""phase 1"""\n'
                '# Mid-file comment\n'
                '1s,48m,20\n'  # omitted trailing comment cell
                '2s,0,,end\n',
                encoding='utf-8',
            )
            parsed = lines_common.parse_line_csv(p)
            self.assertEqual(parsed['end_ms'], 2000)
            d = self._run_ns3(p, stop_s=2.0)
            self.assertAlmostEqual(d['capacity_mbps'], 36.0, places=4)
            self.assertAlmostEqual(d['base_rtt_ms'], 30.0, places=3)

    def test_07_invalid_csv_rejected_by_both_python_and_ns3(self):
        """Malformed schemas, units, precedence violations, and quotes fail in both Python and ns-3."""
        bad_cases = [
            ('unknown_col', 'time,unknown\n0,10m\n1s,0\n'),
            ('dup_col', 'time,cap,cap\n0,10m,10m\n1s,0,0\n'),
            ('missing_time', 'cap\n10m\n0\n'),
            ('only_up_cap', 'time,up_cap\n0,10m\n1s,0\n'),
            ('only_down_cap', 'time,down_cap\n0,10m\n1s,0\n'),
            ('nonzero_start', 'time,cap\n10,10m\n1s,0\n'),
            ('nonmonotonic_time', 'time,cap\n0,10m\n1s,20m\n500ms,10m\n2s,0\n'),
            ('dup_time_units', 'time,cap\n0,10m\n1000,20m\n1s,30m\n2s,0\n'),
            ('sub_ms_time_unsuffixed', 'time,cap\n0,10m\n1.5,20m\n2s,0\n'),
            ('sub_ms_time_ms', 'time,cap\n0,10m\n1.5ms,20m\n2s,0\n'),
            ('sub_ms_time_s', 'time,cap\n0,10m\n0.0005s,20m\n2s,0\n'),
            ('space_in_time', 'time,cap\n0,10m\n1 s,0\n'),
            ('space_in_delay', 'time,cap,delay\n0,10m,10 ms\n1s,0,\n'),
            ('space_in_cap', 'time,cap\n0,10 kb\n1s,0\n'),
            ('unsuffixed_float_cap', 'time,cap\n0,2.5\n1s,0\n'),
            ('bad_cap_unit_mbps', 'time,cap\n0,40mbps\n1s,0\n'),
            ('bad_cap_unit_k', 'time,cap\n0,40k\n1s,0\n'),
            ('bad_cap_unit_upper_M', 'time,cap\n0,40M\n1s,0\n'),
            ('neg_delay', 'time,cap,delay\n0,10m,-5ms\n1s,0,\n'),
            ('delay_missing_at_t0', 'time,cap,delay\n0,10m,\n500ms,10m,15\n1s,0,\n'),
            ('up_delay_missing_at_t0', 'time,cap,up_delay\n0,10m,\n500ms,10m,15\n1s,0,\n'),
            ('end_not_zero', 'time,cap\n0,10m\n1s,5m\n'),
            ('end_down_not_zero', 'time,up_cap,down_cap\n0,10m,10m\n1s,0,5m\n'),
            ('bad_overridden_cap', 'time,cap,up_cap,down_cap\n0,badcap,10m,10m\n1s,0,0,0\n'),
            ('too_many_cols', 'time,cap\n0,10m,extra\n1s,0\n'),
            ('unclosed_quote', 'time,cap,comment\n0,10m,"unclosed\n1s,0,\n'),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, content in bad_cases:
                p = root / f'{name}.csv'
                p.write_text(content, encoding='utf-8')
                with self.assertRaises(ValueError, msg=f'Python failed to reject {name}'):
                    lines_common.parse_line_csv(p)
                if BINARY.is_file():
                    proc = subprocess.run([
                        str(BINARY), '--profile=lines', f'--lineFile={p}',
                        '--stopTime=1', '--ccs=cubic', '--evalJson=1',
                    ], text=True, capture_output=True)
                    self.assertNotEqual(proc.returncode, 0, f'ns-3 failed to reject {name}')

    def test_08_legacy_txt_ignored_and_csv_discovery_order(self):
        """Legacy .txt files are no longer discovered; flat and directory CSVs sort alphabetically."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            legacy_dir = root / 'legacy_only'
            legacy_dir.mkdir()
            (legacy_dir / 'up.txt').write_text('0,10m\n1000,0\n')
            (legacy_dir / 'down.txt').write_text('0,10m\n1000,0\n')
            with self.assertRaisesRegex(ValueError, 'no CSV line scenarios'):
                lines_common.discover(legacy_dir)

            mix = root / 'mix'
            (mix / 'alpha').mkdir(parents=True)
            (mix / 'alpha' / 'line.csv').write_text('time,cap\n0,10m\n1s,0\n')
            (mix / 'beta.csv').write_text('time,cap\n0,10m\n2s,0\n')
            (mix / 'gamma').mkdir(parents=True)
            (mix / 'gamma' / 'line.csv').write_text('time,cap,delay\n0,10m,15ms\n3s,0,\n')
            scenes = lines_common.discover(mix)
            self.assertEqual([s['name'] for s in scenes], ['alpha', 'beta', 'gamma'])
            self.assertEqual([s['period_ms'] for s in scenes], [1000, 2000, 3000])
            self.assertEqual([s['has_up_delay'] for s in scenes], [False, False, True])


if __name__ == '__main__':
    unittest.main()
