#!/usr/bin/env python3
"""Focused tests for the Mahimahi Linux kernel lines CI integration."""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/bench'))
sys.path.insert(0, str(ROOT / 'scripts/data'))
sys.path.insert(0, str(ROOT / 'scripts/eval'))

import ci_runner  # noqa: E402
import lines_common  # noqa: E402
import lines_to_mahimahi  # noqa: E402
import run_kernel_lines  # noqa: E402
import run_mahimahi_suite  # noqa: E402


class KernelCiTests(unittest.TestCase):
    def test_v2_and_nc_j0_are_valid_kernel_client_options_and_quinn_removed(self):
        self.assertIn(('CopaAutoV2', 'copa_auto_v2'), ci_runner.EXTRA_CONGESTION_VARIANTS)
        self.assertIn(('CopaAutoNcJ0', 'copa_auto_nc_j0'), ci_runner.EXTRA_CONGESTION_VARIANTS)
        self.assertEqual(run_mahimahi_suite.resolve_mortise_cc('copa-auto-v2'), 'copa_auto_v2')
        self.assertEqual(run_kernel_lines.resolve_kernel_cc('copa-auto-v2'), 'copa_auto_v2')
        self.assertEqual(run_kernel_lines.resolve_kernel_cc('copa_auto_theta0.98'), 'copa_auto_t98')
        self.assertFalse(hasattr(ci_runner, 'cmd_run_quinn'))
        self.assertFalse(hasattr(run_mahimahi_suite, 'run_quinn_job'))

    def test_new_kernel_bpf_signature_patch_and_rust_enum(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bpf = root / 'algorithm/bpf-kern'
            (bpf / 'include').mkdir(parents=True)
            (bpf / 'include/vmlinux.h').write_text(
                'struct tcp_congestion_ops { void (*cong_control)(struct sock *, u32, int, const struct rate_sample *); };\n',
                encoding='utf-8',
            )
            (bpf / 'mortise_copa.bpf.c').write_text(
                'BPF_PROG(bpf_copa_main, struct sock *sk, const struct rate_sample *rs)\n',
                encoding='utf-8',
            )
            rust = root / 'mortise/mortise-common/src/congestion.rs'
            rust.parent.mkdir(parents=True)
            rust.write_text(
                '    Bbr,\n}\nCongestionOpt::Bbr => write!(f, "bbr"),\n'
                'CongestionOpt::Bbr => (0, b"bbr"),\n'
                '            CongestionOpt::Vegas => ManagerOperation::Load {\n'
                '                path: "".to_string(),\n'
                '                option: None,\n            },\n',
                encoding='utf-8',
            )
            self.assertEqual(ci_runner.cmd_patch_mortise(argparse.Namespace(mortise_dir=tmp)), 0)
            self.assertIn('u32 ack, int flag', (bpf / 'mortise_copa.bpf.c').read_text(encoding='utf-8'))
            self.assertIn('name = "copa_auto_v2"', rust.read_text(encoding='utf-8'))
            self.assertIn('CongestionOpt::CopaAutoV2 => (0, b"copa_auto_v2")', rust.read_text(encoding='utf-8'))

    def test_generate_bulk_workload_respects_mortise_frame_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            wk = Path(tmp) / 'bulk.wk'
            target = 10 * 1024**3  # 10 GiB
            run_kernel_lines.generate_bulk_workload(wk, target)
            lines = wk.read_text(encoding='utf-8').splitlines()
            self.assertGreaterEqual(len(lines), 5120)
            gap, size = map(int, lines[0].split())
            self.assertEqual(gap, 0)
            self.assertLessEqual(size, 2 * 1024 * 1024)
            self.assertGreaterEqual(len(lines) * size, target)

    def test_parse_queues_spec(self):
        self.assertEqual(run_kernel_lines.parse_queues_spec('500KB'), ['500000'])
        self.assertEqual(run_kernel_lines.parse_queues_spec('150KB, 2MB, infinite'), ['150000', '2000000', 'infinite'])
        self.assertEqual(run_kernel_lines.parse_queues_spec(''), ['500000'])
        with self.assertRaises(ValueError):
            run_kernel_lines.parse_queues_spec('0KB')
        with self.assertRaises(ValueError):
            run_kernel_lines.parse_queues_spec('500TB')

    def test_build_lines_plan_sharding_filtering_and_leocc_flag(self):
        matrix, meta = run_kernel_lines.build_lines_plan(
            dataset='basic',
            scenarios_filter=['elevator', 'asymmetric_delay', 'bandwidth_drop_severe'],
            workloads=['bulk', 'app'],
            ccs=['copa_auto_v2', 'leocc', 'cubic'],
            queues=['500KB'],
            repeats=1,
            max_bulk_s=0.0,
            workload_timeout_s=300.0,
            bulk_warmup_s=0.0,
            max_shards=4,
            root=ROOT,
        )
        self.assertTrue(meta['needs_leocc'])
        self.assertEqual(len(meta['scenarios']), 3)
        self.assertEqual(len(matrix['include']), 4)
        assigned = []
        for entry in matrix['include']:
            assigned.extend(json.loads(entry['units_json']))
        self.assertEqual(len(assigned), 6)  # 3 scenarios x 2 workloads
        self.assertEqual({u['scenario'] for u in assigned}, {'elevator', 'asymmetric_delay', 'bandwidth_drop_severe'})

    def test_build_lines_plan_all_datasets_fast(self):
        matrix, meta = run_kernel_lines.build_lines_plan(
            dataset='all',
            scenarios_filter=None,
            workloads=['bulk'],
            ccs=['cubic'],
            queues=['500KB'],
            repeats=1,
            max_bulk_s=60.0,
            workload_timeout_s=300.0,
            bulk_warmup_s=0.0,
            max_shards=8,
            root=ROOT,
        )
        self.assertFalse(meta['needs_leocc'])
        self.assertEqual(len(matrix['include']), 8)
        self.assertGreaterEqual(len(meta['scenarios']), 50)
        self.assertTrue(all(s['duration_s'] <= 60.0 for s in meta['scenarios']))

    def test_mahimahi_command_uses_asymmetric_delay_and_no_loss(self):
        conv = lines_to_mahimahi.MahimahiScenario(
            name='asym',
            period_ms=1000,
            duration_s=1.0,
            bulk_duration_s=1.0,
            mm_uplink_trace=Path('/tmp/ul.trace'),
            mm_downlink_trace=Path('/tmp/dl.trace'),
            mm_uplink_delay=Path('/tmp/ul.delay'),
            mm_downlink_delay=Path('/tmp/dl.delay'),
            delay_interval_ms=10,
            base_rtt_ms_bulk=85.0,
            base_rtt_ms_period=85.0,
            data_capacity_mbps_bulk=50.0,
            data_capacity_mbps_period=50.0,
            has_up_delay_file=True,
            has_down_delay_file=True,
        )
        cmd_q = run_kernel_lines.build_mahimahi_command(conv, '500000', Path('/tmp/out.log'), 'echo hi')
        self.assertEqual(cmd_q[:8], ['mm-delay', '10', '/tmp/ul.delay', '--downlink-delay', '/tmp/dl.delay', 'mm-link', '/tmp/ul.trace', '/tmp/dl.trace'])
        self.assertIn('--downlink-queue-args=bytes=500000', cmd_q)
        self.assertNotIn('mm-loss', cmd_q)

        cmd_inf = run_kernel_lines.build_mahimahi_command(conv, 'infinite', Path('/tmp/out.log'), 'echo hi')
        self.assertNotIn('--downlink-queue=droptail', cmd_inf)

    def test_evaluate_single_run_and_summarize_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene = root / 'scene'
            scene.mkdir()
            (scene / 'up.txt').write_text('0,12m\n2000,0\n', encoding='utf-8')
            (scene / 'down.txt').write_text('0,12m\n2000,0\n', encoding='utf-8')
            (scene / 'up.delay.txt').write_text('0,10\n# end_ms=2000\n', encoding='utf-8')
            (scene / 'down.delay.txt').write_text('0,10\n# end_ms=2000\n', encoding='utf-8')
            wk_file = root / 'tiny.wk'
            wk_file.write_text('0 15000\n10 15000\n', encoding='utf-8')

            conv = lines_to_mahimahi.convert_scenario(lines_common.discover(scene)[0], root / 'conv')
            out_dir = root / 'shard1'
            out_dir.mkdir()

            def fake_run_mm(mm_cmd, expected_out, link_log, *, allow_timeout=False):
                # Write realistic Mahimahi downlink.log
                link_log.write_text(
                    '0 # 1500\n0 - 1500 5\n1000 # 1500000\n1000 - 1350000 10\n',
                    encoding='utf-8',
                )
                if not allow_timeout:
                    if 'slow_cc' in mm_cmd[-1]:
                        # Simulate timeout with 1/2 requests completed
                        expected_out.write_text(
                            'id,size,client_send,server_recv,client_recv,rct\n'
                            '1,15000,1000000000,1010000000,1040000000,40.0\n',
                            encoding='utf-8',
                        )
                        return subprocess.CompletedProcess(mm_cmd, 124, '', 'timed out')
                    expected_out.write_text(
                        'id,size,client_send,server_recv,client_recv,rct\n'
                        '1,15000,1000000000,1010000000,1040000000,40.0\n'
                        '2,15000,1010000000,1020000000,1060000000,50.0\n'
                        '0,0,0,0,0,0\n',
                        encoding='utf-8',
                    )
                return subprocess.CompletedProcess(mm_cmd, 0, '', '')

            work_info = {'tiny': {'path': str(wk_file), 'count': 2, 'span_s': 0.01}}
            rows = []
            with patch.object(run_kernel_lines, 'run_mm_cmd', side_effect=fake_run_mm):
                for cc in ('fast_cc', 'slow_cc'):
                    for wk in ('bulk', 'tiny'):
                        rows.append(run_kernel_lines.evaluate_single_run(
                            conv=conv,
                            dataset_name='basic',
                            workload_name=wk,
                            cc=cc,
                            queue='500000',
                            repeat=1,
                            client_bin=Path('/bin/true'),
                            bulk_wk=wk_file,
                            work_info=work_info,
                            out_dir=out_dir,
                            workload_timeout_s=30.0,
                            bulk_warmup_s=0.0,
                            keep_logs=False,
                        ))

            # Verify statuses and raw downlink logs cleaned up
            by_key = {(r['cc'], r['workload']): r for r in rows}
            self.assertEqual(by_key[('fast_cc', 'bulk')]['status'], 'success')
            self.assertGreater(by_key[('fast_cc', 'bulk')]['norm_power'], 0)
            self.assertEqual(by_key[('fast_cc', 'tiny')]['status'], 'success')
            self.assertGreater(by_key[('fast_cc', 'tiny')]['fct_slowdown'], 0)
            self.assertEqual(by_key[('slow_cc', 'tiny')]['status'], 'timeout')
            self.assertEqual(by_key[('slow_cc', 'tiny')]['fct_slowdown'], '')
            self.assertEqual(list(out_dir.glob('*.downlink.log')), [])

            # Write shard_runs.csv and summarize
            run_kernel_lines.trace_report.write_runs_csv(
                out_dir / 'shard_runs.csv', rows, extra_fields=run_kernel_lines.KERNEL_CSV_EXTRA_FIELDS
            )
            plan_meta = {
                'dataset': 'basic',
                'scenarios': [{'name': 'scene', 'duration_s': 2.0, 'has_up_delay': True, 'has_down_delay': True}],
                'workloads': ['bulk', 'tiny'],
                'work_info': work_info,
                'ccs': ['fast_cc', 'slow_cc'],
                'queues': ['500000'],
                'repeats': 1,
                'max_bulk_s': 0.0,
                'workload_timeout_s': 30.0,
                'bulk_warmup_s': 0.0,
            }
            csv_path, md_path, report_md = run_kernel_lines.summarize_results(
                out_dir, root / 'summary', plan_meta=plan_meta
            )
            self.assertTrue(csv_path.is_file())
            self.assertTrue(md_path.is_file())
            self.assertIn('Mahimahi Linux Kernel Bench 实验报告', report_md)
            self.assertIn('所有场景总排名', report_md)
            self.assertIn('超时（0分）', report_md)
            self.assertIn('500KB', report_md)
            with csv_path.open(newline='', encoding='utf-8') as f:
                self.assertEqual(len(list(csv.DictReader(f))), 4)


if __name__ == '__main__':
    unittest.main()
