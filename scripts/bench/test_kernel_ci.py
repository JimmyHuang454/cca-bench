#!/usr/bin/env python3
"""Focused tests for the Mahimahi kernel CI integration."""
import argparse
from pathlib import Path
import tempfile
import unittest

import ci_runner
import run_mahimahi_suite


class KernelCiTests(unittest.TestCase):
    def test_v2_is_a_valid_kernel_client_option(self):
        self.assertIn(("CopaAutoV2", "copa_auto_v2"), ci_runner.EXTRA_CONGESTION_VARIANTS)
        self.assertEqual(run_mahimahi_suite.resolve_mortise_cc("copa-auto-v2"), "copa_auto_v2")

    def test_dataset_plan_and_kernel_override(self):
        mode, matrix = ci_runner.build_plan("datasets", "app,websearch")
        self.assertEqual(mode, "datasets")
        self.assertEqual({m["shard"] for m in matrix["include"]}, {"nyc-subway", "lumos-5g", "leo"})
        self.assertEqual(ci_runner.resolve_kernel_ccs(mode, "leo", "0.98", "copa_auto_v2,mortise_copa", set(), True),
                         "copa_auto_v2,mortise_copa")

    def test_new_kernel_bpf_signature_patch_and_rust_enum(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bpf = root / "algorithm/bpf-kern"
            (bpf / "include").mkdir(parents=True)
            (bpf / "include/vmlinux.h").write_text(
                "struct tcp_congestion_ops { void (*cong_control)(struct sock *, u32, int, const struct rate_sample *); };\n")
            (bpf / "mortise_copa.bpf.c").write_text(
                "BPF_PROG(bpf_copa_main, struct sock *sk, const struct rate_sample *rs)\n")
            rust = root / "mortise/mortise-common/src/congestion.rs"
            rust.parent.mkdir(parents=True)
            rust.write_text('    Bbr,\n}\nCongestionOpt::Bbr => write!(f, "bbr"),\n'
                            'CongestionOpt::Bbr => (0, b"bbr"),\n'
                            '            CongestionOpt::Vegas => ManagerOperation::Load {\n'
                            '                path: "".to_string(),\n'
                            '                option: None,\n            },\n')
            self.assertEqual(ci_runner.cmd_patch_mortise(argparse.Namespace(mortise_dir=tmp)), 0)
            self.assertIn('u32 ack, int flag', (bpf / "mortise_copa.bpf.c").read_text())
            self.assertIn('name = "copa_auto_v2"', rust.read_text())
            self.assertIn('CongestionOpt::CopaAutoV2 => (0, b"copa_auto_v2")', rust.read_text())


if __name__ == "__main__":
    unittest.main()
