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
    def test_lumos_uses_same_down_trace_in_both_directions(self):
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
            dest = root / 'datasets/lines/lumos/5g_trace_100_walking'
            self.assertEqual((dest / 'down.txt').read_text(),
                             '0,1\n1,0\n2,1\n3,0\n4,1\n5,0\n')
            self.assertEqual((dest / 'up.txt').read_text(),
                             (dest / 'down.txt').read_text())
            self.assertEqual((dest / 'up.delay.txt').read_text(),
                             '0,20\n# end_ms=5\n')
            self.assertEqual((dest / 'down.delay.txt').read_text(),
                             '0,20\n# end_ms=5\n')

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
            self.assertTrue((root / 'datasets/lines/example/a/up.txt').is_file())
            with patch.multiple(module, ROOT=root, DATASET_DIR=None, UPLINK_TRACE=up,
                                DOWNLINK_TRACE=down, DELAY_TRACE=delay), \
                    patch('sys.argv', ['trace_to_lines.py']), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(module.main(), 0)
            self.assertTrue((root / 'datasets/lines/converted/a/down.delay.txt').is_file())

    def test_capacity_boundaries_and_gaps(self):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / 'input'
            trace.write_text('0\n5\n5\n5\n5\n5\n6\n6\n6\n6\n6\n')
            output = io.StringIO()
            module.write_capacity(trace, output)
            self.assertEqual(output.getvalue(), '0,1\n1,0\n5,5\n7,0\n')
            trace.write_text('1\n1\n2\n3\n')
            output = io.StringIO()
            module.write_capacity(trace, output)
            self.assertEqual(output.getvalue(), '0,0\n1,2\n2,1\n4,0\n')

    def test_existing_legacy_output_is_not_mixed_with_new_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'traces' / 'sample'
            dataset.mkdir(parents=True)
            (dataset / 'one.up').write_text('0\n')
            (dataset / 'one.down').write_text('0\n')
            output = root / 'datasets/lines/sample/one'
            output.mkdir(parents=True)
            (output / 'one.up.txt').write_text('existing\n')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 1)
            self.assertFalse((output / 'down.txt').exists())

    def test_delay_and_bad_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / 'input'
            trace.write_text('20\n20\n25.5\n18\n')
            output = io.StringIO()
            module.write_delay(trace, output)
            self.assertEqual(output.getvalue(), '0,20\n20,25.5\n30,18\n# end_ms=40\n')
            trace.write_text('2\n1\n')
            with self.assertRaisesRegex(ValueError, 'nondecreasing'):
                module.write_capacity(trace, io.StringIO())
            trace.write_text('nan\n')
            with self.assertRaisesRegex(ValueError, 'invalid'):
                module.write_delay(trace, io.StringIO())

    def test_directory_pairs_delay_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'traces' / 'sample'
            dataset.mkdir(parents=True)
            (dataset / 'one.up').write_text('0\n2\n')
            (dataset / 'one.down').write_text('1\n3\n')
            (dataset / 'one.delay').write_text('20\n25\n')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 0)
                target = root / 'datasets/lines/sample/one'
                self.assertEqual((target / 'up.txt').read_text(), '0,1\n1,0\n2,1\n3,0\n')
                self.assertEqual((target / 'up.delay.txt').read_text(), '0,20\n10,25\n# end_ms=20\n')
                self.assertEqual((target / 'down.delay.txt').read_text(), (target / 'up.delay.txt').read_text())
                self.assertEqual(module.main(), 1)

    def test_leo_and_batch_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = root / 'leocc'
            dataset.mkdir()
            (dataset / 'bw_a.txt').write_text('1\n2\n')
            (dataset / 'delay_a.txt').write_text('20\n')
            (dataset / 'bad.down').write_text('1\n')
            with patch.object(module, 'ROOT', root), patch('sys.argv', ['trace_to_lines.py', str(dataset)]), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(), 1)
                self.assertFalse((root / 'datasets/lines/leocc').exists())
                (dataset / 'bad.down').unlink()
                self.assertEqual(module.main(), 0)
                dest = root / 'datasets/lines/leocc/bw_a'
                self.assertEqual((dest / 'up.txt').read_text(), (dest / 'down.txt').read_text())
                self.assertEqual((dest / 'up.delay.txt').read_text(), '0,20\n# end_ms=10\n')
                self.assertEqual((dest / 'down.delay.txt').read_text(), (dest / 'up.delay.txt').read_text())


if __name__ == '__main__':
    unittest.main()
