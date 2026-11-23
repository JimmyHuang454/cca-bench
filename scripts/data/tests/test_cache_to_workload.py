#!/usr/bin/env python3
"""Unit and ns-3 integration tests for scripts/data/cache_to_workload.py."""
from __future__ import annotations

import contextlib
import gzip
import importlib.util
import io
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = ROOT / 'scripts/data/cache_to_workload.py'
spec = importlib.util.spec_from_file_location('cache_to_workload', SCRIPT_PATH)
assert spec is not None and spec.loader is not None
mod = importlib.util.module_from_spec(spec)
sys.modules['cache_to_workload'] = mod
spec.loader.exec_module(mod)

if str(ROOT / 'scripts/bench') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts/bench'))
import lines_common  # noqa: E402

BINARY = ROOT / 'ns3/ns-3/build/scratch/tcp-trace-bench/ns3-dev-tcp-trace-bench-optimized'


def _write_oracle_bin(path: Path, records: list[tuple[int, int, int, int]]) -> None:
    with path.open('wb') as f:
        for ts, oid, sz, nxt in records:
            f.write(struct.pack('<IQIq', ts, oid, sz, nxt))


def _make_synthetic_wiki_records(
    *,
    start_ts: int = 1_551_744_000,
    seconds: int = 120,
    per_sec: int = 200,
) -> list[tuple[int, int, int, int]]:
    """Create realistic heavy-tailed oracleGeneral records including 0-byte & oversize noise."""
    sizes = [4096, 8192, 16384, 32768, 65536, 131072, 524288, 1_572_864]
    out: list[tuple[int, int, int, int]] = []
    seq = 1
    for sec in range(seconds):
        ts = start_ts + sec
        for k in range(per_sec):
            if k == 0:
                sz = 0  # invalid miss
            elif k == 1:
                sz = 500  # below 1 KB
            elif k == 2:
                sz = 50_000_000  # above 10 MB
            else:
                sz = sizes[(sec * 7 + k) % len(sizes)]
            out.append((ts, 100_000 + seq, sz, -1))
            seq += 1
    return out


class CacheToWorkloadTests(unittest.TestCase):
    def test_01_oracle_general_bin_and_zst_thin_by_target_mbps_and_determinism(self):
        records = _make_synthetic_wiki_records(seconds=100, per_sec=120)
        with tempfile.TemporaryDirectory() as tmp:
            tmp_p = Path(tmp)
            bin_path = tmp_p / 'wiki_2019u.oracleGeneral.bin'
            _write_oracle_bin(bin_path, records)

            out_bin = tmp_p / 'from_bin.csv'
            cfg = mod.ConvertConfig(
                input_source=str(bin_path),
                output_csv=out_bin,
                mode='thin',
                skip_seconds=10.0,
                duration_s=60.0,
                target_mbps=4.0,
                min_bytes=1024,
                max_bytes=10 * 1024 * 1024,
                subsecond_mode='uniform',
                seed=123,
            )
            stats_bin = mod.convert_to_workload(cfg)
            self.assertEqual(stats_bin.detected_format, 'oracleGeneral')
            self.assertGreater(stats_bin.out_requests, 20)
            # Span should fit inside 60s window and average bitrate should be close to 4 Mbps
            self.assertLessEqual(stats_bin.out_span_s, 60.0)
            self.assertGreater(stats_bin.out_span_s, 50.0)
            self.assertAlmostEqual(stats_bin.out_avg_mbps, 4.0, delta=1.5)
            # Verify parsed workload obeys strict bounds & non-decreasing timestamps
            parsed = lines_common.parse_workload(out_bin)
            self.assertEqual(parsed[0][0], 0.0)
            self.assertTrue(all(1024 <= sz <= 10 * 1024 * 1024 for _, sz in parsed))

            # Compress with real zstd if available and verify byte-for-byte identical output
            if shutil.which('zstd'):
                zst_path = tmp_p / 'wiki_2019u.oracleGeneral.zst'
                subprocess.run(['zstd', '-q', '-f', str(bin_path), '-o', str(zst_path)], check=True)
                out_zst = tmp_p / 'from_zst.csv'
                cfg_zst = mod.ConvertConfig(
                    input_source=str(zst_path),
                    output_csv=out_zst,
                    mode='thin',
                    skip_seconds=10.0,
                    duration_s=60.0,
                    target_mbps=4.0,
                    min_bytes=1024,
                    max_bytes=10 * 1024 * 1024,
                    subsecond_mode='uniform',
                    seed=123,
                )
                mod.convert_to_workload(cfg_zst)
                self.assertEqual(out_bin.read_text(encoding='utf-8'), out_zst.read_text(encoding='utf-8'))

    def test_02_thin_by_target_requests_sample_rate_and_subsecond_modes(self):
        records = _make_synthetic_wiki_records(seconds=30, per_sec=60)
        with tempfile.TemporaryDirectory() as tmp:
            tmp_p = Path(tmp)
            bin_path = tmp_p / 'wiki.oracleGeneral.bin'
            _write_oracle_bin(bin_path, records)

            # Exact target request count via reservoir sampling + even subsecond spacing
            out_req = tmp_p / 'reqs.csv'
            stats_req = mod.convert_to_workload(mod.ConvertConfig(
                input_source=str(bin_path),
                output_csv=out_req,
                mode='thin',
                duration_s=20.0,
                target_mbps=None,
                target_requests=75,
                subsecond_mode='even',
                include_comment=False,
                seed=7,
            ))
            self.assertEqual(stats_req.out_requests, 75)
            header = out_req.read_text(encoding='utf-8').splitlines()[0]
            self.assertEqual(header, 'time,size')

            # Fixed sample rate + burst subsecond mode (same-ms bursts)
            out_burst = tmp_p / 'burst.csv'
            stats_burst = mod.convert_to_workload(mod.ConvertConfig(
                input_source=str(bin_path),
                output_csv=out_burst,
                mode='thin',
                duration_s=20.0,
                sample_rate=0.2,
                subsecond_mode='burst',
                burst_prob=0.8,
                seed=9,
            ))
            parsed = lines_common.parse_workload(out_burst)
            zero_gaps = sum(1 for i in range(1, len(parsed)) if parsed[i][0] == parsed[i - 1][0])
            self.assertGreater(zero_gaps, 0)
            self.assertGreater(stats_burst.out_requests, 100)

    def test_03_cdf_mode_poisson_and_bursty_even_with_zero_timestamps(self):
        # All timestamps == 0 (common in anonymized cache traces without wall-clock time)
        records = [(0, i, 4096 if i % 4 else 524288, -1) for i in range(1, 500)]
        with tempfile.TemporaryDirectory() as tmp:
            tmp_p = Path(tmp)
            bin_path = tmp_p / 'notime.oracleGeneral.bin'
            _write_oracle_bin(bin_path, records)

            # Thin mode must reject non-advancing timestamps with a helpful error
            with self.assertRaisesRegex(ValueError, 'MODE="cdf"'):
                mod.convert_to_workload(mod.ConvertConfig(
                    input_source=str(bin_path),
                    output_csv=tmp_p / 'fail.csv',
                    mode='thin',
                    duration_s=60.0,
                    target_mbps=3.0,
                ))

            # CDF Poisson mode works cleanly
            out_p = tmp_p / 'cdf_poisson.csv'
            st_p = mod.convert_to_workload(mod.ConvertConfig(
                input_source=str(bin_path),
                output_csv=out_p,
                mode='cdf',
                duration_s=120.0,
                target_mbps=2.0,
                arrival_model='poisson',
                seed=42,
            ))
            self.assertGreater(st_p.out_requests, 50)
            self.assertLessEqual(st_p.out_span_s, 120.0)
            self.assertAlmostEqual(st_p.out_avg_mbps, 2.0, delta=0.8)

            # CDF Bursty mode with fixed TARGET_REQUESTS
            out_b = tmp_p / 'cdf_bursty.csv'
            st_b = mod.convert_to_workload(mod.ConvertConfig(
                input_source=str(bin_path),
                output_csv=out_b,
                mode='cdf',
                duration_s=60.0,
                target_mbps=None,
                target_requests=60,
                arrival_model='bursty',
                burst_size_min=4,
                burst_size_max=4,
                burst_inner_gap_ms=0.0,
                seed=42,
            ))
            self.assertEqual(st_b.out_requests, 60)
            parsed_b = lines_common.parse_workload(out_b)
            same_ms = sum(1 for i in range(1, len(parsed_b)) if parsed_b[i][0] == parsed_b[i - 1][0])
            self.assertGreaterEqual(same_ms, 30)

    def test_04_wikimedia_raw_tsv_gz_and_general_csv_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_p = Path(tmp)
            # Wikimedia cache-u 5-column headerless .tsv.gz
            tsv_gz = tmp_p / 'cache-u-20190305.tsv.gz'
            with gzip.open(tsv_gz, 'wt', encoding='utf-8') as f:
                for sec in range(25):
                    for k in range(10):
                        f.write(f'{1000 + sec}\t{9000 + sec * 10 + k}\timage/jpeg\t{8192 * (k + 1)}\t0.012\n')
            out_tsv = tmp_p / 'wiki_tsv.csv'
            st_tsv = mod.convert_to_workload(mod.ConvertConfig(
                input_source=str(tsv_gz),
                output_csv=out_tsv,
                mode='thin',
                duration_s=20.0,
                sample_rate=0.5,
                seed=11,
            ))
            self.assertEqual(st_tsv.detected_format, 'wiki_tsv')
            self.assertGreater(st_tsv.out_requests, 40)

            # General headered CSV with millisecond timestamps
            gen_csv = tmp_p / 'custom_cdn.csv'
            rows = ['timestamp,key,val_size']
            for i in range(100):
                rows.append(f'{i * 250},item_{i},{4096 + (i % 5) * 8192}')
            gen_csv.write_text('\n'.join(rows) + '\n', encoding='utf-8')
            out_csv = tmp_p / 'from_csv.csv'
            st_csv = mod.convert_to_workload(mod.ConvertConfig(
                input_source=str(gen_csv),
                output_csv=out_csv,
                mode='thin',
                duration_s=20.0,
                sample_rate=1.0,
                subsecond_mode='raw',
                csv_time_col='timestamp',
                csv_size_col='val_size',
                csv_id_col='key',
                csv_time_unit='ms',
            ))
            self.assertEqual(st_csv.detected_format, 'csv')
            self.assertEqual(st_csv.out_requests, 80)
            self.assertAlmostEqual(st_csv.out_span_s, 19.75, places=3)

    def test_05_top_level_script_config_and_overwrite_guard(self):
        records = _make_synthetic_wiki_records(seconds=20, per_sec=40)
        with tempfile.TemporaryDirectory() as tmp:
            tmp_p = Path(tmp)
            bin_path = tmp_p / 'sample.oracleGeneral.bin'
            out_path = tmp_p / 'tuned.csv'
            _write_oracle_bin(bin_path, records)

            # Top-level config mode (user edits constants at top of file and runs `make cache_to_workload`)
            buf = io.StringIO()
            with patch.multiple(
                mod,
                INPUT_SOURCE=bin_path,
                OUTPUT_CSV=out_path,
                MODE='thin',
                DURATION_S=15.0,
                TARGET_MBPS=2.0,
                TARGET_REQUESTS=None,
                SAMPLE_RATE=None,
                OVERWRITE=True,
                SEED=99,
            ), contextlib.redirect_stdout(buf):
                rc = mod.main()
            self.assertEqual(rc, 0)
            self.assertTrue(out_path.is_file())
            self.assertIn('已生成标准 CSV 负载', buf.getvalue())

            # OVERWRITE=False blocks overwriting existing output
            err = io.StringIO()
            with patch.multiple(
                mod,
                INPUT_SOURCE=bin_path,
                OUTPUT_CSV=out_path,
                OVERWRITE=False,
            ), contextlib.redirect_stderr(err):
                rc_dup = mod.main()
            self.assertEqual(rc_dup, 1)
            self.assertIn('already exists', err.getvalue())

            # Switching TARGET_MBPS=None and TARGET_REQUESTS=25 via script constants
            out_reqs = tmp_p / 'reqs_only.csv'
            with patch.multiple(
                mod,
                INPUT_SOURCE=bin_path,
                OUTPUT_CSV=out_reqs,
                DURATION_S=15.0,
                TARGET_MBPS=None,
                TARGET_REQUESTS=25,
                SAMPLE_RATE=None,
                OVERWRITE=True,
            ), contextlib.redirect_stdout(io.StringIO()):
                rc_req = mod.main()
            self.assertEqual(rc_req, 0)
            self.assertEqual(len(lines_common.parse_workload(out_reqs)), 25)

            # Preset name resolution: remote URL by default, local cache hit when file exists,
            # and OUTPUT_CSV=None auto-names datasets/workloads/<preset>.csv
            src_url, stem = mod.resolve_input_source('meta_reag', cache_dir=tmp_p)
            self.assertTrue(src_url.startswith('https://ftp.pdl.cmu.edu/'))
            self.assertEqual(stem, 'meta_reag')
            local_cached = tmp_p / 'meta_reag.oracleGeneral.zst'
            local_cached.write_bytes(b'dummy')
            src_local, stem2 = mod.resolve_input_source('meta_reag', cache_dir=tmp_p)
            self.assertEqual(src_local, str(local_cached))
            self.assertEqual(stem2, 'meta_reag')
            with patch.multiple(mod, INPUT_SOURCE='wiki_2019u', LOCAL_CACHE_DIR=tmp_p, OUTPUT_CSV=None):
                cfg_auto = mod.build_config()
            self.assertEqual(cfg_auto.input_source, mod.PRESET_SOURCES['wiki_2019u'])
            self.assertEqual(cfg_auto.output_csv, mod.ROOT / 'datasets/workloads/wiki_2019u.csv')

    def test_06_generated_workload_runs_cleanly_in_ns3_trace_bench(self):
        if not BINARY.is_file():
            self.skipTest(f'build tcp-trace-bench first: {BINARY}')
        records = _make_synthetic_wiki_records(seconds=10, per_sec=30)
        with tempfile.TemporaryDirectory() as tmp:
            tmp_p = Path(tmp)
            bin_path = tmp_p / 'mini.oracleGeneral.bin'
            _write_oracle_bin(bin_path, records)
            wk_csv = tmp_p / 'wiki_mini.csv'
            mod.convert_to_workload(mod.ConvertConfig(
                input_source=str(bin_path),
                output_csv=wk_csv,
                mode='thin',
                duration_s=5.0,
                target_mbps=None,
                target_requests=12,
                seed=5,
            ))
            line_csv = tmp_p / 'line.csv'
            line_csv.write_text('time,cap,delay\n0,50mbps,10\n5s,0,\n', encoding='utf-8')
            res = subprocess.run([
                str(BINARY), '--profile=lines',
                f'--lineFile={line_csv}',
                f'--workloadFile={wk_csv}',
                '--linePeriodMs=5000',
                '--stopTime=20',
                '--ccs=bbr',
                '--evalJson=1',
            ], text=True, capture_output=True, check=True)
            data = json.loads(next(l for l in reversed(res.stdout.splitlines()) if l.startswith('{')))
            self.assertEqual(data['requests'], 12)
            self.assertEqual(data['requests_expected'], 12)
            self.assertGreaterEqual(data['fct_slowdown'], 1.0)


if __name__ == '__main__':
    unittest.main()
