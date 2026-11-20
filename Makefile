# Top-level Makefile for copa-delta-repro
# Delegates ns-3 benchmark targets to ns3/Makefile and kernel unit tests to kernel/tcp_copa_auto/Makefile.

.PHONY: help test-core setup-bench build-bench workloads \
        bench bench-synth bench-lte bench-leo bench-datasets bench-full \
        summary clean-bench

help:
	@$(MAKE) -C ns3 help

test-core:
	$(MAKE) -C kernel/tcp_copa_auto test

setup-bench build-bench workloads bench bench-synth bench-lte bench-leo bench-datasets bench-full summary clean-bench:
	$(MAKE) -C ns3 $@
