# Reproduction of the simulations. Run "make help" for the targets.
NS3_ROOT ?= $(CURDIR)/ns-3
JOBS ?= $(shell echo $$(($$(getconf _NPROCESSORS_ONLN) - 1)))
export NS3_ROOT
export JOBS

LTE_TRACES := traces/mahimahi/Verizon-LTE-driving.down
LEO_TRACES := traces/leocc/4.8K.zip

.PHONY: help setup smoke fetch fetch-mahimahi fetch-leocc basic lte leo mobile lines figures all clean-results

help:
	@echo "make setup           fetch ns-3 at the base commit, copy ns3-overlay, build"
	@echo "make smoke           unit test and one short run of each program"
	@echo "make fetch           download all traces (fetch-mahimahi, fetch-leocc)"
	@echo "make basic           basic scenario"
	@echo "make lte             LTE trace replay (downloads the traces if needed)"
	@echo "make leo             Starlink trace replay (downloads the traces if needed)"
	@echo "make mobile          LTE mobile scenarios with handover"
	@echo "make lines           city and countryside lines"
	@echo "make figures         figure data, after the four experiments above"
	@echo "make all             all experiments, then the figure data"
	@echo "make clean-results   delete results/ (the simulation results)"
	@echo "variables: NS3_ROOT=$(NS3_ROOT) JOBS=$(JOBS)"

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
