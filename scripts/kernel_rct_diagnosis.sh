#!/usr/bin/env bash
set -euo pipefail
root=/tmp/rct-diag
mkdir -p "$root"
uname -a > "$root/environment.txt"
git rev-parse HEAD >> "$root/environment.txt"
git -C /tmp/Mortise rev-parse HEAD >> "$root/environment.txt"
sysctl net.ipv4.tcp_slow_start_after_idle net.core.default_qdisc >> "$root/environment.txt"
run_pair() {
    local dest="$root/$SHARD-$1"
    # Mortise binds port 5000 without SO_REUSEADDR. Wait out TIME_WAIT
    # from the preceding pair before starting another server on this runner.
    python3 - <<'PYPORT'
import errno, socket, time
deadline = time.monotonic() + 100
while True:
    with socket.socket() as probe:
        try:
            probe.bind(("0.0.0.0", 5000))
            break
        except OSError as exc:
            if exc.errno != errno.EADDRINUSE or time.monotonic() >= deadline:
                raise
    time.sleep(1)
PYPORT
    python3 scripts/ci_runner.py run-kernel --out-dir "$dest"
    python3 - "$dest" <<'PY'
import csv, sys
from pathlib import Path
folder = Path(sys.argv[1])
for path in sorted(folder.glob("*.rct.csv")):
    with path.open() as f:
        rows = {int(r["id"]): r for r in csv.DictReader(f)}
    assert set(range(1, 81)) <= rows.keys(), path
    gap = (int(rows[36]["client_recv"]) - int(rows[35]["client_recv"])) / 1e9
    print(path.name, gap)
    with (folder.parent / "gaps.tsv").open("a") as f:
        print(folder.name, path.name.split("__")[-1].split(".")[0], f"{gap:.3f}", sep="\t", file=f)
PY
    gzip -1 "$dest"/*.downlink.log
}
for trial in 1 2 3 4 5; do run_pair "$trial"; done
if ! awk '$2=="copa_auto" && $3>=10 {found=1} END {exit !found}' "$root/gaps.tsv"; then
    export SHARD=diag-111
    run_pair fallback
fi
echo Y | sudo tee /sys/module/tcp_copa_auto/parameters/diagnose
sudo tcpdump -i any -U -s 96 -w "$root/tcp.pcap" 'tcp port 5000' >"$root/tcpdump.log" 2>&1 &
cap=$!
(
    while true; do
        date +%s.%N
        ss -tinmo '( sport = :5000 )'
        sleep .2
    done
) > "$root/ss.txt" &
sampler=$!
trap 'kill "$sampler" 2>/dev/null || true; sudo pkill -INT -x tcpdump || true' EXIT
run_pair observed
kill "$sampler"
sudo pkill -INT -x tcpdump || true
wait "$cap" || true
trap - EXIT
sudo dmesg | grep copa_sample > "$root/copa-samples.txt"
gzip -1 "$root/ss.txt" "$root/tcp.pcap"
result=$(awk '{printf "%s %s=%ss ",$1,$2,$3}' "$root/gaps.tsv")
echo "result=${result:0:245}" >> "$GITHUB_OUTPUT"
