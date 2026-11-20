# Copa-Auto ns-3 Benchmark & Paper Reproduction Makefile
# Run "make help" for all targets and configurable variables.

REPO_ROOT    := $(abspath $(CURDIR)/..)
KERNEL_DIR   := $(REPO_ROOT)/kernel
SCRIPTS_DIR  := $(KERNEL_DIR)/scripts
NS3_ROOT     ?= $(CURDIR)/ns-3
NS3_BASE     := 84a73f9c00abf5e5b8aa676e1775261fb717c3ac
JOBS         ?= $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
export NS3_ROOT
export JOBS

# ---------------------------------------------------------------------------
# Benchmark suite configurable variables (override via `make bench SHARD=...`)
# ---------------------------------------------------------------------------
SHARD        ?= synth
CCS          ?= copa_auto,copa_auto_nc,copa_auto_nc_j0,mortise_copa,bbr,bbr3,cubic,vegas,veno,yeah
WORKLOADS    ?= bulk,websearch
REPEATS      ?= 1
JITTER_US    ?= -1
BIN_MS       ?= 5.0
CLOCK_MS     ?= 1.0
QUEUES       ?=
PARALLEL     ?= $(JOBS)
RESUME       ?= 1
TRACE_DIR    ?= $(CURDIR)/traces/cache
WORKLOAD_DIR ?= $(CURDIR)/results/workloads
OUT_DIR      ?= $(CURDIR)/results/bench/$(SHARD)
SUMMARY_DIR  ?= $(OUT_DIR)/summary
BENCH_BIN    ?= $(NS3_ROOT)/build/scratch/ns3-dev-tcp_ccas_bench-optimized

WORKLOAD_MAP := app=$(WORKLOAD_DIR)/app.wk,websearch=$(WORKLOAD_DIR)/websearch.wk,vl2=$(WORKLOAD_DIR)/vl2.wk,pageload=$(WORKLOAD_DIR)/pageload.wk
QUEUES_FLAG  := $(if $(QUEUES),--queues $(QUEUES),)
RESUME_FLAG  := $(if $(filter 1 true yes,$(RESUME)),--resume,)

# Legacy trace paths
LTE_TRACES := traces/mahimahi/Verizon-LTE-driving.down
LEO_TRACES := traces/leocc/4.8K.zip

.PHONY: help setup-bench test-core sync-bench build-bench workloads \
        bench bench-synth bench-lte bench-leo bench-datasets bench-full \
        summary clean-bench \
        setup smoke fetch fetch-mahimahi fetch-leocc basic lte leo mobile lines figures all clean-results

help:
	@echo "=== Copa-Auto ns-3 Benchmark Targets ==="
	@echo "  make setup-bench     Clone ns-3 base + overlay, configure 6-module optimized build, build tcp_ccas_bench"
	@echo "  make test-core       Run C unit tests for kernel/tcp_copa_auto/copa_auto_core.h"
	@echo "  make build-bench     Sync copa_auto_core.h + tcp_ccas_bench.cc and rebuild tcp_ccas_bench"
	@echo "  make workloads       Generate websearch/vl2/pageload .wk files (and download app.wk if missing)"
	@echo "  make bench           Run run_ns3_suite.py + mahimahi_eval.py for SHARD=$(SHARD)"
	@echo "  make bench-synth     Shortcut: make bench SHARD=synth"
	@echo "  make bench-lte       Shortcut: make bench SHARD=lte"
	@echo "  make bench-leo       Shortcut: make bench SHARD=leo"
	@echo "  make bench-datasets  Shortcut: make bench SHARD=datasets"
	@echo "  make bench-full      Shortcut: make bench SHARD=full (all 44 shards / 329 traces)"
	@echo "  make summary         Re-run mahimahi_eval.py on OUT_DIR and print comparison_summary.md"
	@echo "  make clean-bench     Delete results/bench"
	@echo ""
	@echo "=== Configurable Variables ==="
	@echo "  SHARD=$(SHARD)"
	@echo "  CCS=$(CCS)"
	@echo "  WORKLOADS=$(WORKLOADS)"
	@echo "  REPEATS=$(REPEATS)  JITTER_US=$(JITTER_US)  BIN_MS=$(BIN_MS)  CLOCK_MS=$(CLOCK_MS)"
	@echo "  QUEUES=$(QUEUES)  PARALLEL=$(PARALLEL)"
	@echo "  OUT_DIR=$(OUT_DIR)"
	@echo ""
	@echo "=== Legacy Paper Reproduction Targets ==="
	@echo "  make setup | smoke | fetch | basic | lte | leo | mobile | lines | figures | all | clean-results"

# ---------------------------------------------------------------------------
# Copa-Auto ns-3 Benchmark Targets
# ---------------------------------------------------------------------------
test-core:
	$(MAKE) -C $(KERNEL_DIR)/tcp_copa_auto test

sync-bench:
	@mkdir -p "$(NS3_ROOT)/scratch"
	cp "$(CURDIR)/tcp_ccas_bench.cc" "$(NS3_ROOT)/scratch/tcp_ccas_bench.cc"
	cp "$(CURDIR)/mortise_copa_ns3.h" "$(NS3_ROOT)/scratch/mortise_copa_ns3.h"
	cp "$(KERNEL_DIR)/tcp_copa_auto/copa_auto_core.h" "$(NS3_ROOT)/scratch/copa_auto_core.h"
	cp "$(KERNEL_DIR)/tcp_copa_auto/copa_auto_math.h" "$(NS3_ROOT)/scratch/copa_auto_math.h"

setup-bench:
	@if [ ! -f "$(NS3_ROOT)/ns3" ]; then \
		mkdir -p "$(NS3_ROOT)"; \
		git -C "$(NS3_ROOT)" init -q; \
		git -C "$(NS3_ROOT)" remote add origin https://github.com/nsnam/ns-3-dev-git.git; \
		git -C "$(NS3_ROOT)" fetch --depth 1 origin "$(NS3_BASE)"; \
		git -C "$(NS3_ROOT)" checkout -q FETCH_HEAD; \
	fi
	cp -R "$(CURDIR)/ns3-overlay/." "$(NS3_ROOT)/"
	$(MAKE) sync-bench
	cd "$(NS3_ROOT)" && ./ns3 configure --build-profile=optimized --disable-python --disable-werror \
		--enable-modules=core,network,internet,point-to-point,applications,traffic-control
	cd "$(NS3_ROOT)" && ./ns3 build tcp_ccas_bench

build-bench: sync-bench
	cd "$(NS3_ROOT)" && ./ns3 build tcp_ccas_bench

workloads:
	@mkdir -p "$(WORKLOAD_DIR)"
	python3 "$(SCRIPTS_DIR)/gen_workloads.py" --out-dir "$(WORKLOAD_DIR)"
	@if [ ! -f "$(WORKLOAD_DIR)/app.wk" ]; then \
		echo "Downloading Mortise app.wk..."; \
		curl -fsSL https://raw.githubusercontent.com/BobAnkh/Mortise/main/mortise/workload/app.wk -o "$(WORKLOAD_DIR)/app.wk" \
			|| echo "Warning: could not download app.wk (offline?); websearch/vl2/pageload are ready."; \
	fi

bench: workloads
	@if [ ! -x "$(BENCH_BIN)" ]; then $(MAKE) build-bench; fi
	@mkdir -p "$(OUT_DIR)" "$(SUMMARY_DIR)"
	python3 "$(SCRIPTS_DIR)/run_ns3_suite.py" \
		--bin "$(BENCH_BIN)" \
		--shard "$(SHARD)" \
		--ccs "$(CCS)" \
		--workloads "$(WORKLOADS)" \
		--workload-map "$(WORKLOAD_MAP)" \
		--repeats "$(REPEATS)" \
		--jitter-us "$(JITTER_US)" \
		--bin-ms "$(BIN_MS)" \
		--clock-ms "$(CLOCK_MS)" \
		--parallel "$(PARALLEL)" \
		--trace-dir "$(TRACE_DIR)" \
		$(QUEUES_FLAG) \
		$(RESUME_FLAG) \
		--out-dir "$(OUT_DIR)"
	$(MAKE) summary OUT_DIR="$(OUT_DIR)" SUMMARY_DIR="$(SUMMARY_DIR)"

bench-synth:
	$(MAKE) bench SHARD=synth

bench-lte:
	$(MAKE) bench SHARD=lte

bench-leo:
	$(MAKE) bench SHARD=leo

bench-datasets:
	$(MAKE) bench SHARD=datasets

bench-full:
	$(MAKE) bench SHARD=full

summary:
	@mkdir -p "$(SUMMARY_DIR)"
	python3 "$(SCRIPTS_DIR)/mahimahi_eval.py" \
		--input-dir "$(OUT_DIR)" \
		--out-csv "$(SUMMARY_DIR)/comparison_summary.csv" \
		--out-md "$(SUMMARY_DIR)/comparison_summary.md"
	@echo ""
	@cat "$(SUMMARY_DIR)/comparison_summary.md"

clean-bench:
	rm -rf "$(CURDIR)/results/bench"

# ---------------------------------------------------------------------------
# Legacy Paper Reproduction Targets
# ---------------------------------------------------------------------------
setup:
	./setup_ns3.sh "$(NS3_ROOT)"

smoke:
	./smoke_test.sh

fetch: fetch-mahimahi fetch-leocc

fetch-mahimahi:
	./fetch_traces.sh mahimahi

fetch-leocc:
	./fetch_traces.sh leocc

$(LTE_TRACES):
	./fetch_traces.sh mahimahi

$(LEO_TRACES):
	./fetch_traces.sh leocc

basic:
	./run_all.sh basic

lte: $(LTE_TRACES)
	./run_all.sh lte

leo: $(LEO_TRACES)
	./run_all.sh leo

mobile:
	./run_all.sh mobile

lines:
	./run_all.sh lines

figures:
	./run_all.sh figures

all: basic lte leo mobile lines figures

clean-results:
	rm -rf results smoke
