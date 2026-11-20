# Top-level Makefile for copa-delta-repro
# Delegates ns-3 benchmark targets to ns3/Makefile and kernel unit tests to kernel/tcp_copa_auto/Makefile.

.PHONY: help test-core setup-bench build-bench workloads convert-mahimahi \
        bench bench-wired bench-wifi bench-synth bench-4g bench-lte bench-5g bench-leo \
        bench-lines-quick bench-lines bench-datasets bench-full \
        summary report clean-bench clean-all \
        lines mobile basic 4g-paper lte leo figures clean-results

help:
	@$(MAKE) -C ns3 help

test-core:
	$(MAKE) -C kernel/tcp_copa_auto test

setup-bench build-bench workloads convert-mahimahi \
bench bench-wired bench-wifi bench-synth bench-4g bench-lte bench-5g bench-leo \
bench-lines-quick bench-lines bench-datasets bench-full \
summary report clean-bench clean-all \
lines mobile basic 4g-paper lte leo figures clean-results:
	$(MAKE) -C ns3 $@
