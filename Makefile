# Top-level Makefile for copa-delta-repro
# Delegates ns-3 benchmark targets to ns3/Makefile and kernel unit tests to kernel/tcp_copa_auto/Makefile.

.PHONY: help test test-core test-bench setup-bench build-bench workloads convert-mahimahi \
        trace_to_lines cache_to_workload \
        bench bench-wired bench-wifi bench-synth bench-4g bench-lte bench-5g bench-leo \
        bench-lines-quick bench-lines bench-datasets bench-full \
        summary report clean-bench clean-all \
        lines mobile basic 4g-paper lte leo figures clean-results

help:
	@$(MAKE) -C ns3 help

test: test-core test-bench

test-core:
	$(MAKE) -C kernel/tcp_copa_auto test

test-bench:
	python3 -m unittest discover -s scripts/bench/tests -p 'test_*.py' -v
	python3 -m unittest scripts/bench/test_kernel_ci.py scripts/data/tests/test_lines_to_mahimahi.py scripts/data/tests/test_trace_to_lines.py scripts/data/tests/test_cache_to_workload.py -v

setup-bench build-bench workloads convert-mahimahi trace_to_lines cache_to_workload \
bench bench-wired bench-wifi bench-synth bench-4g bench-lte bench-5g bench-leo \
bench-lines-quick bench-lines bench-datasets bench-full \
summary report clean-bench clean-all \
lines mobile basic 4g-paper lte leo figures clean-results:
	$(MAKE) -C ns3 $@
