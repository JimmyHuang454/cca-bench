import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

path = Path(__file__).resolve().parents[1] / 'trace_to_lines.py'
spec = importlib.util.spec_from_file_location('trace_to_lines', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ConversionTest(unittest.TestCase):
    def test_lumos_uses_symmetric_csv_without_delay_column(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'traces' / 'lumos'
            dataset.mkdir(parents=True)
            (dataset / '5g_trace_100_walking.down').write_text('0\n2\n4\n')
            (dataset / '5g_trace_100_walking.mbps').write_text('not used\n')
            (dataset / 'lumos-fixed-ack-100mbps.up').write_text('0\n0\n1\n')
            with patch.multiple(module, ROOT=root, DATASET_DIR=dataset), \
                    patch('sys.argv', ['trace_to_lines.py']), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(module.main(), 0)
            dest = root / 'datasets/lines/lumos/5g_trace_100_walking.csv'
            self.assertEqual(
                dest.read_text(),
                'time,cap\n0,12mbps\n1,0\n2,12mbps\n3,0\n4,12mbps\n5,0\n',
            )

    def test_script_configuration_directory_and_single_pair(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'traces' / 'example'
            dataset.mkdir(parents=True)
            up, down, delay = (dataset / name for name in ('a.up', 'a.down', 'a.delay'))
            up.write_text('0\n1\n')
            down.write_text('0\n2\n')
            delay.write_text('20\n')
            with patch.multiple(module, ROOT=root, DATASET_DIR=dataset), \
                    patch('sys.argv', ['trace_to_lines.py']), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(module.main(), 0)
            out_dir = root / 'datasets/lines/example/a.csv'
            self.assertTrue(out_dir.is_file())
            self.assertEqual(
                out_dir.read_text(),
                'time,up_cap,down_cap,delay\n0,12mbps,12mbps,20\n1,,0,\n2,0,0,\n',
            )
            with patch.multiple(module, ROOT=root, DATASET_DIR=None, UPLINK_TRACE=up,
                                DOWNLINK_TRACE=down, DELAY_TRACE=delay), \
                    patch('sys.argv', ['trace_to_lines.py']), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(module.main(), 0)
            self.assertTrue((root / 'datasets/lines/converted/a.csv').is_file())

    def test_capacity_boundaries_and_gaps(self):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / 'input'
            trace.write_text('0\n5\n5\n5\n5\n5\n6\n6\n6\n6\n6\n')
            output = io.StringIO()
            module.write_scene_csv(trace, trace, None, output)
            self.assertEqual(output.getvalue(), 'time,cap\n0,12mbps\n1,0\n5,60mbps\n7,0\n')
            trace.write_text('1\n1\n2\n3\n')
            output = io.StringIO()
            module.write_scene_csv(trace, trace, None, output)
            self.assertEqual(output.getvalue(), 'time,cap\n0,0\n1,24mbps\n2,12mbps\n4,0\n')

    def test_format_capacity_units_and_precision(self):
        self.assertEqual(module.format_capacity_bps(0), '0')
        self.assertEqual(module.format_capacity_bps(500), '500bps')
        self.assertEqual(module.format_capacity_bps(307_000), '307kbps')
        self.assertEqual(module.format_capacity_bps(2_135_000), '2.135mbps')
        self.assertEqual(module.format_capacity_bps(2_135_000, decimals=2), '2.13mbps')
        self.assertEqual(module.format_capacity_bps(1_200_000_000), '1.2gbps')
        self.assertEqual(module.format_capacity(7), '84mbps')
        self.assertEqual(module.format_capacity(100), '1.2gbps')

    def test_existing_directory_csv_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'traces' / 'sample'
            dataset.mkdir(parents=True)
            (dataset / 'one.up').write_text('0\n')
            (dataset / 'one.down').write_text('0\n')
            output = root / 'datasets/lines/sample/one'
            output.mkdir(parents=True)
            (output / 'line.csv').write_text('existing\n')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 1)
            self.assertFalse((root / 'datasets/lines/sample/one.csv').exists())

    def test_delay_and_bad_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / 'input'
            trace.write_text('20\n20\n25.5\n18\n')
            events = list(module.iter_delay_events(trace))
            self.assertEqual(events, [(0, '20'), (20, '25.5'), (30, '18'), (40, '__END__')])
            trace.write_text('2\n1\n')
            with self.assertRaisesRegex(ValueError, 'nondecreasing'):
                list(module.iter_capacity_events(trace))
            trace.write_text('nan\n')
            with self.assertRaisesRegex(ValueError, 'invalid'):
                list(module.iter_delay_events(trace))

    def test_directory_pairs_delay_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'traces' / 'sample'
            dataset.mkdir(parents=True)
            (dataset / 'one.up').write_text('0\n2\n')
            (dataset / 'one.down').write_text('1\n3\n')
            (dataset / 'one.delay').write_text('20\n25\n')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 0)
                target = root / 'datasets/lines/sample/one.csv'
                self.assertEqual(
                    target.read_text(),
                    'time,up_cap,down_cap,delay\n'
                    '0,12mbps,0,20\n'
                    '1,0,12mbps,\n'
                    '2,12mbps,0,\n'
                    '3,0,0,\n',
                )
                self.assertEqual(module.main(), 1)

    def test_leo_and_batch_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'leocc'
            dataset.mkdir()
            (dataset / 'bw_a.txt').write_text('1\n2\n')
            (dataset / 'delay_a.txt').write_text('20\n')
            (dataset / 'bad.down').write_text('1\n')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 1)
                self.assertFalse((root / 'datasets/lines/leocc').exists())
                (dataset / 'bad.down').unlink()
                self.assertEqual(module.main(), 0)
                dest = root / 'datasets/lines/leocc/bw_a.csv'
                self.assertEqual(dest.read_text(), 'time,cap,delay\n0,0,20\n1,12mbps,\n3,0,\n')

    def test_leo_without_delay_omits_delay_column(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'leocc'
            dataset.mkdir()
            (dataset / 'bw_nodelay.txt').write_text('0\n0\n1\n')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 0)
            dest = root / 'datasets/lines/leocc/bw_nodelay.csv'
            self.assertEqual(dest.read_text(), 'time,cap\n0,24mbps\n1,12mbps\n2,0\n')

    def test_single_file_dot_trace_conversion_directory_and_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'traces' / 'cellular-traces-nyc'
            dataset.mkdir(parents=True)
            trace_a = dataset / 'trace-taxi.trace'
            trace_a.write_text('0\n0\n2\n')
            # Directory mode
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 0)
            dest_dir = root / 'datasets/lines/cellular-traces-nyc/trace-taxi.csv'
            self.assertEqual(dest_dir.read_text(), 'time,cap\n0,24mbps\n1,0\n2,12mbps\n3,0\n')

            # Single-file CLI mode
            single = root / 'custom.trace'
            single.write_text('1\n2\n2\n')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(single)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 0)
            dest_single = root / 'datasets/lines/converted/custom.csv'
            self.assertEqual(dest_single.read_text(), 'time,cap\n0,0\n1,12mbps\n2,24mbps\n3,0\n')

    def test_pantheon_calibrated_directory_conversion(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'traces' / 'pantheon'
            dataset.mkdir(parents=True)
            (dataset / '0.57mbps-poisson.trace').write_text('100\n200\n')
            (dataset / '77.72mbps.trace').write_text('0\n0\n59999\n')
            (dataset / '12mbps.trace').write_text('1\n')  # stub skipped in calibrated Pantheon dir
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 0)
            out_dir = root / 'datasets/lines/pantheon'
            self.assertEqual(
                sorted(p.name for p in out_dir.glob('*.csv')),
                ['aws_korea_to_china.csv', 'nepal_to_aws_india.csv'],
            )
            self.assertEqual(
                (out_dir / 'aws_korea_to_china.csv').read_text(),
                'time,cap,delay,comment\n'
                '0,77.72mbps,51,"Pantheon calibrated: AWS Korea to China wired (77.72mbps, mm-delay 51ms)"\n'
                '60000,0,,\n',
            )
            self.assertEqual(
                (out_dir / 'nepal_to_aws_india.csv').read_text(),
                'time,cap,delay,comment\n'
                '0,0,28,"Pantheon calibrated: Nepal cellular to AWS India (0.57mbps-poisson, mm-delay 28ms)"\n'
                '100,12mbps,,\n'
                '101,0,,\n'
                '200,12mbps,,\n'
                '201,0,,\n',
            )

    def test_opennetlab_json_symmetric_and_asymmetric_conversion(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'traces' / 'opennetlab-sample'
            dataset.mkdir(parents=True)
            sym_json = {
                'from': 'sigcomm-5g',
                'delay_from': 'sigcomm-ping',
                'downlink': {},
                'uplink': {
                    'trace_pattern': [
                        {'duration': 100.0, 'capacity': 307, 'rtt': 40.5},
                        {'duration': 100.0, 'capacity': 2135, 'rtt': 60.0},
                        {'duration': 100.0, 'capacity': 2135, 'rtt': 60.0},
                    ]
                },
            }
            (dataset / 'mm5g.json').write_text(json.dumps(sym_json), encoding='utf-8')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 0)
            out_csv = root / 'datasets/lines/opennetlab-sample/mm5g.csv'
            self.assertEqual(
                out_csv.read_text(encoding='utf-8'),
                'time,cap,delay,comment\n'
                '0,307kbps,20.25,OpenNetLab (from=sigcomm-5g; delay_from=sigcomm-ping)\n'
                '100,2.135mbps,30,\n'
                '300,0,,\n',
            )

            # Single-file asymmetric JSON CLI mode
            asym_path = root / 'asym.json'
            asym_json = {
                'uplink': {'trace_pattern': [{'duration': 500.0, 'capacity': 500, 'rtt': 20.0}]},
                'downlink': {'trace_pattern': [{'duration': 500.0, 'capacity': 12000, 'rtt': 40.0}]},
            }
            asym_path.write_text(json.dumps(asym_json), encoding='utf-8')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(asym_path)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 0)
            out_asym = root / 'datasets/lines/converted/asym.csv'
            self.assertEqual(
                out_asym.read_text(encoding='utf-8'),
                'time,up_cap,down_cap,up_delay,down_delay\n'
                '0,500kbps,12mbps,10,20\n'
                '500,0,0,,\n',
            )


if __name__ == '__main__':
    unittest.main()
