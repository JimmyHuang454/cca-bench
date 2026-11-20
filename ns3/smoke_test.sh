#!/usr/bin/env bash
# Quick check of a build: the unit test and one short run of each simulation program.
# usage: NS3_ROOT=/path/to/ns-3 ./smoke_test.sh [OUT_DIR]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export NS3_ROOT="${NS3_ROOT:-$HERE/ns-3}"
O="$(cd "$(dirname "${1:-$HERE/smoke}")" && pwd)/$(basename "${1:-$HERE/smoke}")"; mkdir -p "$O"
cd "$NS3_ROOT"
./test.py -s tcp-copa-test 2>&1 | grep -E "tests passed|FAIL"
B=build/scratch/ns3-dev-tcp-capacity-trace-default
seq 1 1000 > "$O/const.txt"
$B --ccs=copa-auto+nosw --profile=mahimahi --downTrace="$O/const.txt" --stopTime=30 --warmup=5 \
    --queueBytes=2000000 --sndBuf=4194304 --prefix="$O/capacity" > /dev/null
grep -E 'utilization' "$O/capacity-metrics.json"
build/scratch/ns3-dev-tcp-lte-highspeed-handover-default --run=1 --prefix="$O/lte" --cc=copa-auto \
    --numFlows=1 --bgMbps=0 $(cat "$HERE/config/hwy.args") --simTime=20 > /dev/null
grep -E 'total_throughput_mbps' "$O/lte-copa-auto-metrics.json"
echo ok
