#!/usr/bin/env bash
# Run every experiment of the validation section and write the figure data.
# Finished runs are skipped, so the script can be restarted after an interruption.
# usage: NS3_ROOT=/path/to/ns-3 JOBS=9 ./run_all.sh [lte|leo|mobile|lines|figures|all]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export NS3_ROOT="${NS3_ROOT:-$HERE/ns-3}"
J="${JOBS:-$(($(getconf _NPROCESSORS_ONLN) - 1))}"
R="$HERE/results"; SC="$HERE/scripts/paper"; PY="python3 -I"
WHAT="${1:-all}"
want() { [ "$WHAT" = all ] || [ "$WHAT" = "$1" ]; }
mkdir -p "$R"

if want lte; then
    $PY "$SC/mahimahi_runner.py" "$HERE/traces/mahimahi" "$R/lte" "$J"
    $PY "$SC/mahimahi_analyze.py" "$R/lte" "$R/lte-summary"
fi
if want leo; then
    $PY "$SC/leocc_runner.py" "$HERE/traces/leocc/4.8K.zip" "$HERE/traces/leocc/extracted" "$R/leo" "$J"
    # Downlink BBRv1 crawls with a 64 MB send buffer: repeat those runs with 20 MB, then merge.
    $PY "$SC/leocc_bbr_rerun.py" "$HERE/traces/leocc/extracted" "$R/leo" "$R/leo-bbr20" "$J" 1800
    ( cd "$R/leo-bbr20"; for f in */*-metrics.json; do
        case "$f" in A-downlink-n1/t490*|C-downlink-n1/t411*) ;;
        *) [ -e "../leo/$f" ] || cp "$f" "../leo/$f";; esac; done )
    $PY "$SC/leocc_analyze.py" "$R/leo" "$HERE/traces/leocc/extracted" "$R/leo-summary"
fi
if want mobile; then
    # CUBIC, BBRv1 and the capacity probes, then the Copa family.
    export SWEEP_OUT="$R/sweep"
    for s in hsr-4g hwy-4g; do python3 "$SC/run.py" "$s" --ccs cubic,bbr --runs 3 --jobs "$J"; done
    for s in walk-line run-line; do python3 "$SC/run.py" "$s" --ccs cubic,bbr --runs 1 --jobs "$J"; done
    $PY "$SC/multiflow_runner.py" "$R/sweep/multiflow" "$HERE/config/hwy.args" n2,n4,n2bg,n1bg "$J" cubic,bbr
    $PY "$SC/mobile_runner.py" "$R/mobile" "$HERE/config/hwy.args" "$J"
    $PY "$SC/mobile_analyze.py" "$R/sweep" "$R/mobile" "$R/mobile-summary.json"
fi
if want lines; then
    # City and countryside lines: single flow, 3 runs. The capacity probes of the first
    # sweep are reused by the sweep without the competitive mode.
    LINES="walk-line run-line city-drive city-bus city-subway city-elevated city-mall city-stadium rural-road rural-mountain rural-village rural-edge hwy-line hsr-line"
    SWEEP_OUT="$R/lines" python3 "$SC/run.py" $LINES --ccs copa-auto,copa,cubic,bbr,copa-d0.05,copa-d0.02 --runs 3 --jobs "$J"
    ( cd "$R/lines"; find . \( -name 'r[0-9]-metrics.json' -o -name 'r[0-9]-rttprobe-metrics.json' \) | while read -r f; do
        mkdir -p "$R/lines-nc/$(dirname "$f")"; cp "$f" "$R/lines-nc/$f"; done )
    SWEEP_OUT="$R/lines-nc" python3 "$SC/run.py" $LINES --ccs copa-auto,copa --runs 3 --jobs "$J" --set ns3::TcpCopaKernel::ModeSwitching=false
    $PY "$SC/lines_analyze.py" "$R/lines" "$R/lines-nc" "$R/lines-summary.json"
    # Multi-flow runs on four lines. They reuse the capacity probes of the single-flow sweep.
    MULTI="city-drive city-subway rural-road hsr-line"
    for n in 2 4; do
        for v in sw nc; do
            for l in $MULTI; do for d in "$R/lines/$l/raw/"*; do
                t="$R/lines-multi/$v-n$n/$l/raw/$(basename "$d")"; mkdir -p "$t"
                cp "$d"/r[0-9]-metrics.json "$d"/r[0-9]-rttprobe-metrics.json "$t/"
            done; done
        done
        SWEEP_OUT="$R/lines-multi/sw-n$n" python3 "$SC/run.py" $MULTI --ccs copa-auto,copa,cubic,bbr --runs 3 --jobs "$J" --set numFlows=$n
        SWEEP_OUT="$R/lines-multi/nc-n$n" python3 "$SC/run.py" $MULTI --ccs copa-auto,copa --runs 3 --jobs "$J" --set numFlows=$n --set ns3::TcpCopaKernel::ModeSwitching=false
    done
    $PY "$SC/lines_multi_analyze.py" "$R/lines-multi" "$R/lines-multi-summary.json"
fi
if want figures; then
    # The tables and figures of the paper are written with pgfplots from these files.
    F="$R/figure-data"; mkdir -p "$F"
    $PY "$SC/sync_mobile.py" "$R/mobile" "$R/sweep"
    python3 "$SC/compare.py" "$R/sweep" "$R/compare"
    python3 "$SC/pgfdata.py" "$R/sweep" "$F" "$R/compare/compare.json"
    $PY "$SC/mahimahi_pgfdata.py" "$R/lte-summary/summary.json" "$F/trace-power.dat" "$F/trace-rows.tex"
    $PY "$SC/leocc_pgfdata.py" "$R/leo-summary/summary.json" "$F/leo-power.dat" "$F/leo-rows.tex"
fi
