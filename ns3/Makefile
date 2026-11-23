# Copa-Auto ns-3 Benchmark & Paper Reproduction Makefile
# Run "make help" for all targets and configurable variables.

REPO_ROOT    := $(abspath $(CURDIR)/..)
KERNEL_DIR   := $(REPO_ROOT)/kernel
SCRIPTS_DIR  := $(REPO_ROOT)/scripts
DATASETS_DIR := $(REPO_ROOT)/datasets
RESULTS_DIR  := $(REPO_ROOT)/results
NS3_ROOT     ?= $(CURDIR)/ns-3
NS3_BASE     := 84a73f9c00abf5e5b8aa676e1775261fb717c3ac
JOBS         ?= $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
export NS3_ROOT
export JOBS

# ---------------------------------------------------------------------------
# Benchmark suite configurable variables (override via `make bench SHARD=...`)
# ---------------------------------------------------------------------------
SHARD         ?= synth
BASIC_GROUPS  ?= ABCDE
BASIC_SIGMAS  ?= 0.1,0.4
BASIC_WORKLOADS ?= bulk,app
CCS           ?= copa_auto,copa_auto_v2,copa_auto_nc,copa_auto_nc_j0,copa_auto_nc_theta0.9,mortise_copa,bbr,bbr3,cubic,vegas,veno,yeah
WORKLOADS     ?= bulk,websearch
REPEATS       ?= 1
JITTER_US     ?= -1
CLOCK_MS      ?= 1.0
COPA_CLOCK_US ?= 1
V2_MEASUREMENT_RTTS ?= 16
QUEUES        ?=
PARALLEL      ?= $(JOBS)
RESUME        ?= 1

TRACE_DIR     ?= $(DATASETS_DIR)/traces
LINES_DIR     ?= $(DATASETS_DIR)/lines
WORKLOAD_DIR  ?= $(DATASETS_DIR)/workloads
CAP_CACHE     ?= $(RESULTS_DIR)/cache/phy_cap_cache.json
OUT_DIR       ?= $(RESULTS_DIR)/ns3/$(SHARD)
REPORT_DIR    ?= $(RESULTS_DIR)/reports

BENCH_BIN     ?= $(NS3_ROOT)/build/scratch/ns3-dev-tcp-capacity-trace-optimized
LTE_BIN       ?= $(NS3_ROOT)/build/scratch/ns3-dev-tcp-lte-highspeed-handover-optimized
WIFI_BIN      ?= $(NS3_ROOT)/build/scratch/ns3-dev-tcp-wifi-bench-optimized

WORKLOAD_MAP  := app=$(WORKLOAD_DIR)/app.csv,websearch=$(WORKLOAD_DIR)/websearch.csv,vl2=$(WORKLOAD_DIR)/vl2.csv,pageload=$(WORKLOAD_DIR)/pageload.csv,http2_mux_rr=$(WORKLOAD_DIR)/http2_mux_rr.csv,http2_mux_srpt=$(WORKLOAD_DIR)/http2_mux_srpt.csv
QUEUES_FLAG   := $(if $(QUEUES),--queues $(QUEUES),)
RESUME_FLAG   := $(if $(filter 1 true yes,$(RESUME)),--resume,)

# Legacy trace paths
LTE_TRACES := traces/mahimahi/Verizon-LTE-driving.down
LEO_TRACES := traces/leocc/4.8K.zip

.PHONY: help setup-bench test-core sync-bench build-bench workloads convert-mahimahi \
        trace_to_lines cache_to_workload \
        bench bench-wired bench-wifi bench-synth bench-4g bench-lte bench-5g bench-leo \
        bench-lines-quick bench-lines bench-datasets bench-full bench-basic \
        summary report clean-bench clean-all \
        setup smoke fetch fetch-mahimahi fetch-leocc 4g-paper lte leo mobile lines figures all clean-results

help:
	@echo "=== 1. Build, Unit Tests & Datasets ==="
	@echo "  make setup-bench       Clone ns-3 + overlay, build all 3 engines (capacity-trace, 4G PHY, Wi-Fi 802.11ax)"
	@echo "  make test-core         Run C unit tests for kernel/tcp_copa_auto/copa_auto_core.h"
	@echo "  make build-bench       Sync copa_auto_core.h + ns3-overlay and rebuild all 3 ns-3 binaries"
	@echo "  make workloads         Ensure datasets/workloads/*.csv exist"
	@echo "  make trace_to_lines    Run configured scripts/data/trace_to_lines.py"
	@echo "  make cache_to_workload Run configured scripts/data/cache_to_workload.py"
	@echo "  make convert-mahimahi  Convert Mahimahi traces to unified lines JSON (datasets/lines/mahimahi/)"
	@echo ""
	@echo "=== 2. Unified Benchmark Suite (results -> results/ns3/<shard>/) ==="
	@echo "  make bench             Run configured scripts/bench/run_trace_bench.py"
	@echo "  make legacy-bench      Run the previous suite for SHARD=$(SHARD)"
	@echo "  make bench-wired       Wired links (datasets/lines/wired/*.json: home, wan, dc, step)"
	@echo "  make bench-wifi        Real 802.11ax Wi-Fi MAC/PHY (datasets/lines/wifi/*.json: home, wall, cafe)"
	@echo "  make bench-lines-quick 3GPP 4G PHY quick subset (city-street, train-tunnel, highway-congestion)"
	@echo "  make bench-lines       All 14 3GPP 4G PHY real-world routes (datasets/lines/4g/*.json)"
	@echo "  make bench-basic       Basic A-E dynamic scenarios, bulk + app"
	@echo "  make bench-synth       Synthetic Mahimahi traces (2 traces)"
	@echo "  make bench-4g          Mahimahi 4G-driving traces (4 traces; alias: bench-lte)"
	@echo "  make bench-5g          Lumos5G traces"
	@echo "  make bench-leo         LeoCC Starlink fixed + dynamic traces"
	@echo "  make bench-datasets    nyc-subway + lumos-5g + leo"
	@echo "  make bench-full        All shards combined"
	@echo ""
	@echo "=== 3. Reports & Cleanup ==="
	@echo "  make summary           Re-run eval_report.py on OUT_DIR and print comparison_summary.md"
	@echo "  make report            Aggregate ALL results/ns3/* + results/kernel/* -> results/reports/all_summary.md"
	@echo "  make clean-bench       Delete results/ns3"
	@echo "  make clean-all         Delete results/ns3, results/kernel, results/reports, results/cache"
	@echo ""
	@echo "=== 4. Legacy Paper Scenarios ==="
	@echo "  make lines | mobile | 4g-paper | leo | figures | clean-results"
	@echo ""
	@echo "=== Configurable Variables ==="
	@echo "  SHARD=$(SHARD)  CCS=$(CCS)"
	@echo "  WORKLOADS=$(WORKLOADS)  REPEATS=$(REPEATS)"
	@echo "  JITTER_US=$(JITTER_US)  CLOCK_MS=$(CLOCK_MS)  COPA_CLOCK_US=$(COPA_CLOCK_US)  V2_MEASUREMENT_RTTS=$(V2_MEASUREMENT_RTTS)"
	@echo "  QUEUES=$(QUEUES)  PARALLEL=$(PARALLEL)  RESUME=$(RESUME)"
	@echo "  OUT_DIR=$(OUT_DIR)"

# ---------------------------------------------------------------------------
# Build & Core Tests
# ---------------------------------------------------------------------------
test-core:
	$(MAKE) -C $(KERNEL_DIR)/tcp_copa_auto test

sync-bench:
	cp "$(KERNEL_DIR)/tcp_copa_auto/copa_auto_v3.h" "$(CURDIR)/ns3-overlay/src/internet/model/copa_auto_v3.h"
	cp "$(KERNEL_DIR)/tcp_copa_auto/copa_auto_v2.h" "$(CURDIR)/ns3-overlay/src/internet/model/copa_auto_v2.h"
	cp "$(KERNEL_DIR)/tcp_copa_auto/copa_auto_core.h" "$(CURDIR)/ns3-overlay/src/internet/model/copa_auto_core.h"
	cp "$(KERNEL_DIR)/tcp_copa_auto/copa_auto_math.h" "$(CURDIR)/ns3-overlay/src/internet/model/copa_auto_math.h"
	cp -R "$(CURDIR)/ns3-overlay/." "$(NS3_ROOT)/"

setup-bench:
	@if [ ! -f "$(NS3_ROOT)/ns3" ]; then \
		mkdir -p "$(NS3_ROOT)"; \
		git -C "$(NS3_ROOT)" init -q; \
		git -C "$(NS3_ROOT)" remote add origin https://github.com/nsnam/ns-3-dev-git.git; \
		git -C "$(NS3_ROOT)" fetch --depth 1 origin "$(NS3_BASE)"; \
		git -C "$(NS3_ROOT)" checkout -q FETCH_HEAD; \
	fi
	$(MAKE) sync-bench
	cd "$(NS3_ROOT)" && ./ns3 configure --build-profile=optimized --disable-python --disable-werror \
		--enable-modules=core,network,internet,point-to-point,applications,traffic-control,lte,wifi,mobility,buildings,propagation,spectrum,flow-monitor
	cd "$(NS3_ROOT)" && ./ns3 build tcp-trace-bench tcp-capacity-trace tcp-lte-highspeed-handover tcp-wifi-bench
	ln -sf ns3-dev-tcp-capacity-trace-optimized "$(NS3_ROOT)/build/scratch/ns3-dev-tcp-capacity-trace-default"
	ln -sf ns3-dev-tcp-lte-highspeed-handover-optimized "$(NS3_ROOT)/build/scratch/ns3-dev-tcp-lte-highspeed-handover-default"
	ln -sf ns3-dev-tcp-wifi-bench-optimized "$(NS3_ROOT)/build/scratch/ns3-dev-tcp-wifi-bench-default"

build-bench: sync-bench
	cd "$(NS3_ROOT)" && ./ns3 build tcp-trace-bench tcp-capacity-trace tcp-lte-highspeed-handover tcp-wifi-bench
	ln -sf ns3-dev-tcp-capacity-trace-optimized "$(NS3_ROOT)/build/scratch/ns3-dev-tcp-capacity-trace-default"
	ln -sf ns3-dev-tcp-lte-highspeed-handover-optimized "$(NS3_ROOT)/build/scratch/ns3-dev-tcp-lte-highspeed-handover-default"
	ln -sf ns3-dev-tcp-wifi-bench-optimized "$(NS3_ROOT)/build/scratch/ns3-dev-tcp-wifi-bench-default"

workloads:
	@mkdir -p "$(WORKLOAD_DIR)"
	@if [ ! -f "$(WORKLOAD_DIR)/websearch.csv" ] || [ ! -f "$(WORKLOAD_DIR)/vl2.csv" ] || [ ! -f "$(WORKLOAD_DIR)/pageload.csv" ] || [ ! -f "$(WORKLOAD_DIR)/http2_mux_rr.csv" ] || [ ! -f "$(WORKLOAD_DIR)/http2_mux_srpt.csv" ]; then \
		python3 "$(SCRIPTS_DIR)/data/gen_workloads.py" --out-dir "$(WORKLOAD_DIR)"; \
	fi

convert-mahimahi:
	@mkdir -p "$(TRACE_DIR)" "$(LINES_DIR)/mahimahi"
	python3 "$(SCRIPTS_DIR)/data/mahimahi_to_lines.py" \
		--shard "$(SHARD)" \
		--trace-dir "$(TRACE_DIR)" \
		--out-dir "$(LINES_DIR)/mahimahi"

# ---------------------------------------------------------------------------
# Unified ns-3 Benchmark Suite
# ---------------------------------------------------------------------------
bench:
	python3 "$(SCRIPTS_DIR)/bench/run_trace_bench.py"

trace_to_lines:
	python3 "$(SCRIPTS_DIR)/../scripts/data/trace_to_lines.py"

cache_to_workload:
	python3 "$(SCRIPTS_DIR)/data/cache_to_workload.py"

legacy-bench: workloads
	@if [ ! -x "$(BENCH_BIN)" ]; then $(MAKE) build-bench; fi
	@mkdir -p "$(OUT_DIR)" "$(RESULTS_DIR)/cache"
	python3 "$(SCRIPTS_DIR)/bench/run_ns3_suite.py" \
		--bin "$(BENCH_BIN)" \
		--lte-bin "$(LTE_BIN)" \
		--wifi-bin "$(WIFI_BIN)" \
		--shard "$(SHARD)" \
		--basic-groups "$(BASIC_GROUPS)" \
		--basic-sigmas "$(BASIC_SIGMAS)" \
		--ccs "$(CCS)" \
		--workloads "$(WORKLOADS)" \
		--workload-map "$(WORKLOAD_MAP)" \
		--repeats "$(REPEATS)" \
		--jitter-us "$(JITTER_US)" \
		--clock-ms "$(CLOCK_MS)" \
		--copa-clock-us "$(COPA_CLOCK_US)" \
		--v2-measurement-rtts "$(V2_MEASUREMENT_RTTS)" \
		--parallel "$(PARALLEL)" \
		--trace-dir "$(TRACE_DIR)" \
		--lines-dir "$(LINES_DIR)" \
		--cap-cache "$(CAP_CACHE)" \
		$(QUEUES_FLAG) \
		$(RESUME_FLAG) \
		--out-dir "$(OUT_DIR)"
	$(MAKE) summary OUT_DIR="$(OUT_DIR)"

bench-basic:
	$(MAKE) legacy-bench SHARD=basic WORKLOADS="$(if $(filter command line environment,$(origin WORKLOADS)),$(WORKLOADS),$(BASIC_WORKLOADS))"

bench-wired:
	$(MAKE) legacy-bench SHARD=wired

bench-wifi:
	$(MAKE) legacy-bench SHARD=wifi

bench-lines-quick:
	$(MAKE) legacy-bench SHARD=lines-quick

bench-lines:
	$(MAKE) legacy-bench SHARD=lines

bench-synth:
	$(MAKE) legacy-bench SHARD=synth

bench-4g:
	$(MAKE) legacy-bench SHARD=4g

bench-lte: bench-4g

bench-5g:
	$(MAKE) legacy-bench SHARD=lumos-5g

bench-leo:
	$(MAKE) legacy-bench SHARD=leo

bench-datasets:
	$(MAKE) legacy-bench SHARD=datasets

bench-full:
	$(MAKE) legacy-bench SHARD=full

summary:
	@mkdir -p "$(OUT_DIR)"
	python3 "$(SCRIPTS_DIR)/eval/eval_report.py" \
		--input-dir "$(OUT_DIR)" \
		--out-csv "$(OUT_DIR)/comparison_summary.csv" \
		--out-md "$(OUT_DIR)/comparison_summary.md"
	@echo ""
	@cat "$(OUT_DIR)/comparison_summary.md"

report:
	@mkdir -p "$(REPORT_DIR)"
	python3 "$(SCRIPTS_DIR)/eval/eval_report.py" \
		--input-dir "$(RESULTS_DIR)/ns3" "$(RESULTS_DIR)/kernel" \
		--out-csv "$(REPORT_DIR)/all_summary.csv" \
		--out-md "$(REPORT_DIR)/all_summary.md"
	@echo ""
	@cat "$(REPORT_DIR)/all_summary.md"

clean-bench:
	rm -rf "$(RESULTS_DIR)/ns3"

clean-all:
	rm -rf "$(RESULTS_DIR)/ns3" "$(RESULTS_DIR)/kernel" "$(RESULTS_DIR)/reports" "$(RESULTS_DIR)/cache"

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

4g-paper: lte

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

all: fetch
	./run_all.sh all

clean-results:
	rm -rf results
