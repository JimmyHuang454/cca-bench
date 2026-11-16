#!/usr/bin/env bash
# Download the public traces (not stored in this repository).
# usage: ./fetch_traces.sh [mahimahi|leocc|all]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WHAT="${1:-all}"
if [ "$WHAT" = mahimahi ] || [ "$WHAT" = all ]; then
    D="$HERE/traces/mahimahi"; mkdir -p "$D"
    for t in Verizon-LTE-driving TMobile-LTE-driving ATT-LTE-driving ATT-LTE-driving-2016; do
        for dir in down up; do
            curl -fsSL --retry 3 -o "$D/$t.$dir" \
                "https://raw.githubusercontent.com/ravinet/mahimahi/master/traces/$t.$dir"
        done
    done
fi
if [ "$WHAT" = leocc ] || [ "$WHAT" = all ]; then
    # 732 MB; only the sampled traces are extracted (the whole set is about 60 GB).
    D="$HERE/traces/leocc"; mkdir -p "$D"
    curl -fL -C - -o "$D/4.8K.zip" \
        "https://cloud.tsinghua.edu.cn/d/9fc6fd096e764f57bd25/files/?p=%2F4.8K.zip&dl=1"
fi
