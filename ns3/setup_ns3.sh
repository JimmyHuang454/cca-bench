#!/usr/bin/env bash
# Fetch ns-3 at the base commit, copy our files over it and build.
# usage: ./setup_ns3.sh [NS3_DIR]   (NS3_REPO overrides the source of the clone)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:-$HERE/ns-3}"
REPO="${NS3_REPO:-https://github.com/nsnam/ns-3-dev-git.git}"
BASE=84a73f9c00abf5e5b8aa676e1775261fb717c3ac
if [ ! -d "$DIR/.git" ]; then
    git clone "$REPO" "$DIR"
fi
git -C "$DIR" checkout --quiet "$BASE"
cp -R "$HERE/ns3-overlay/." "$DIR/"
cd "$DIR"
./ns3 configure --enable-tests --disable-python --disable-werror -d default
./ns3 build tcp-capacity-trace tcp-lte-highspeed-handover
./ns3 build internet-test >/dev/null 2>&1 || true
echo "built: $DIR"
