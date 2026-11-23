import contextlib
import importlib.util
import io
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
                'time,cap\n0,1.46kb\n1,0\n2,1.46kb\n3,0\n4,1.46kb\n5,0\n',
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
                'time,up_cap,down_cap,delay\n0,1.46kb,1.46kb,20\n1,,0,\n2,0,0,\n',
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
            self.assertEqual(output.getvalue(), 'time,cap\n0,1.46kb\n1,0\n5,7.32kb\n7,0\n')
            output3 = io.StringIO()
            module.write_scene_csv(trace, trace, None, output3, decimals=3)
            self.assertEqual(output3.getvalue(), 'time,cap\n0,1.465kb\n1,0\n5,7.324kb\n7,0\n')
            trace.write_text('1\n1\n2\n3\n')
            output = io.StringIO()
            module.write_scene_csv(trace, trace, None, output)
            self.assertEqual(output.getvalue(), 'time,cap\n0,0\n1,2.93kb\n2,1.46kb\n4,0\n')

    def test_format_capacity_units_and_precision(self):
        self.assertEqual(module.format_capacity_bytes(0), '0')
        self.assertEqual(module.format_capacity_bytes(512), '512b')
        self.assertEqual(module.format_capacity_bytes(10.33 * 1024), '10.33kb')
        self.assertEqual(module.format_capacity_bytes(1024 * 1024), '1mb')
        self.assertEqual(module.format_capacity(7), '10.25kb')
        self.assertEqual(module.format_capacity(1000), '1.43mb')

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
                    '0,1.46kb,0,20\n'
                    '1,0,1.46kb,\n'
                    '2,1.46kb,0,\n'
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
                self.assertEqual(dest.read_text(), 'time,cap,delay\n0,0,20\n1,1.46kb,\n3,0,\n')

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
            self.assertEqual(dest.read_text(), 'time,cap\n0,2.93kb\n1,1.46kb\n2,0\n')


if __name__ == '__main__':
    unittest.main()
