#!/usr/bin/env bash
set -euo pipefail

# Build and install enhanced LeoReplayer Mahimahi (mm-delay, mm-link, mm-loss).
# Supports:
#   - Standard MAHIMAHI_BASE outer host addressing in nested shells
#   - Independent uplink/downlink dynamic delay traces:
#       mm-delay <interval_ms> <uplink_delay_file> [--downlink-delay <downlink_delay_file>] [cmd...]
#   - Order-preserving FIFO DelayQueue (no packet reordering when delay decreases)
#
# Usage:
#   bash scripts/env/install_mahimahi.sh [leo|standard] [--install-dir DIR]

install_dir="/usr/local/bin"
setuid_install=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        leo|standard)
            # Always build the unified enhanced LeoReplayer so dynamic and
            # asymmetric lines scenarios work identically everywhere.
            shift
            ;;
        --install-dir)
            install_dir="$2"
            setuid_install=0
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

source_dir=$(mktemp -d)
trap 'rm -rf "$source_dir"' EXIT

if [[ "$setuid_install" -eq 1 ]]; then
    sudo sysctl -w net.ipv4.ip_forward=1
fi

revision=69df6406e9985a764b4d52b506cd6ee77596ffb2
sudo apt-get install -y -qq apache2-bin apache2-dev ssl-cert protobuf-compiler libprotobuf-dev
curl -fsSL --retry 3 "https://codeload.github.com/SpaceNetLab/LeoCC/tar.gz/$revision" \
    | tar -xz --strip-components=1 -C "$source_dir"
cd "$source_dir/leoreplayer/replayer"

# The upstream replayer was built on Ubuntu 22.04. Keep warnings visible on newer compilers.
sed -i 's/ -Werror"/"/' configure.ac

python3 - <<'PY'
from pathlib import Path

# 1. Preserve outer host address for MAHIMAHI_BASE in nested shells.
ps_path = Path("src/packet/packetshell.cc")
ps_src = ps_path.read_text(encoding="utf-8")
before_ps = 'ingress_addr().ip().c_str(),\n                                          true ) );'
after_ps = 'egress_addr().ip().c_str(),\n                                          false ) );'
if ps_src.count(before_ps) != 1:
    raise SystemExit("Unexpected LeoReplayer MAHIMAHI_BASE implementation")
ps_path.write_text(ps_src.replace(before_ps, after_ps), encoding="utf-8")

# 2. Extend mm-delay to accept optional `--downlink-delay <file>` for asymmetric delays.
ds_path = Path("src/frontend/delayshell.cc")
ds_src = ds_path.read_text(encoding="utf-8").replace("\r\n", "\n")
before_ds = """        const string delay_filename = argv[2];

        vector< string > command;

        if ( argc == 3 ) {
            command.push_back( shell_path() );
        } else {
            for ( int i = 3; i < argc; i++ ) {
                command.push_back( argv[ i ] );
            }
        }

        PacketShell<DelayQueue> delay_shell_app( "delay", user_environment, passthrough_until_signal );

        delay_shell_app.start_uplink( "[delay " + to_string( interval_ms ) + " ms-int] ",
                                      command,
                                      interval_ms, delay_filename );
        delay_shell_app.start_downlink( interval_ms, delay_filename );"""

after_ds = """        if ( argc < 3 ) {
            throw runtime_error( "Usage: " + string( argv[ 0 ] )
                + " interval-milliseconds uplink-delay-tracefile [--downlink-delay downlink-delay-tracefile] [command...]" );
        }

        const string uplink_delay_filename = argv[ 2 ];
        string downlink_delay_filename = uplink_delay_filename;
        int cmd_start = 3;

        if ( argc >= 5 && string( argv[ 3 ] ) == "--downlink-delay" ) {
            downlink_delay_filename = argv[ 4 ];
            cmd_start = 5;
        }

        vector< string > command;

        if ( argc <= cmd_start ) {
            command.push_back( shell_path() );
        } else {
            for ( int i = cmd_start; i < argc; i++ ) {
                command.push_back( argv[ i ] );
            }
        }

        PacketShell<DelayQueue> delay_shell_app( "delay", user_environment, passthrough_until_signal );

        delay_shell_app.start_uplink( "[delay " + to_string( interval_ms ) + " ms-int] ",
                                      command,
                                      interval_ms, uplink_delay_filename );
        delay_shell_app.start_downlink( interval_ms, downlink_delay_filename );"""

if ds_src.count(before_ds) != 1:
    raise SystemExit("Unexpected LeoReplayer delayshell.cc implementation")
ds_path.write_text(ds_src.replace(before_ds, after_ds), encoding="utf-8")

# 3. Make DelayQueue strictly FIFO (monotonic release timestamps) so dropping delay
#    or same-millisecond bursts never reorder packets.
dq_hh = Path("src/frontend/delay_queue.hh")
dq_cc = Path("src/frontend/delay_queue.cc")
if "class DelayQueue" not in dq_hh.read_text(encoding="utf-8"):
    raise SystemExit("Unexpected delay_queue.hh")

dq_hh.write_text("""/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef DELAY_QUEUE_HH
#define DELAY_QUEUE_HH

#include <queue>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>

#include "file_descriptor.hh"
#include "ezio.hh"

class DelayQueue
{
private:
    uint64_t base_timestamp_;
    uint64_t interval_ms_;
    uint64_t last_release_;
    std::queue< std::pair<uint64_t, std::string> > packet_queue_;
    std::vector<uint64_t> delays_ms_;

public:
    DelayQueue(uint64_t interval_ms, const std::string & filename);

    void read_packet( const std::string & contents );

    void write_packets( FileDescriptor & fd );

    unsigned int wait_time( void ) const;

    bool pending_output( void ) const { return wait_time() <= 0; }

    static bool finished( void ) { return false; }
};

#endif /* DELAY_QUEUE_HH */
""", encoding="utf-8")

dq_cc.write_text("""/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <limits>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include "delay_queue.hh"
#include "timestamp.hh"

using namespace std;

DelayQueue::DelayQueue(uint64_t interval_ms, const string &filename)
    : base_timestamp_(0),
      interval_ms_(interval_ms),
      last_release_(0),
      packet_queue_(),
      delays_ms_() {
    const char *env_base = getenv("BASE_TIMESTAMP");
    base_timestamp_ = env_base ? std::stoull(env_base) : raw_timestamp();

    ifstream trace_file(filename);
    if ( not trace_file.good() ) {
        throw runtime_error(filename + ": error opening for reading");
    }

    string line;
    while ( getline(trace_file, line) ) {
        if ( line.empty() ) {
            continue;
        }
        const long long parsed = stoll(line);
        if ( parsed < 0 ) {
            throw runtime_error(filename + ": negative delay value");
        }
        delays_ms_.emplace_back(static_cast<uint64_t>(parsed));
    }
    if ( delays_ms_.empty() ) {
        throw runtime_error(filename + ": no valid delay samples found");
    }
}

void DelayQueue::read_packet( const string & contents )
{
    const uint64_t now = raw_timestamp();
    const uint64_t elapsed = now >= base_timestamp_ ? (now - base_timestamp_) : 0;
    const size_t delay_idx = (elapsed / interval_ms_) % delays_ms_.size();
    uint64_t release = now + delays_ms_[delay_idx];
    if ( release < last_release_ ) {
        release = last_release_;
    }
    last_release_ = release;
    packet_queue_.emplace( release, contents );
}

void DelayQueue::write_packets( FileDescriptor & fd )
{
    while ( (!packet_queue_.empty())
            && (packet_queue_.front().first <= raw_timestamp()) ) {
        fd.write( packet_queue_.front().second );
        packet_queue_.pop();
    }
}

unsigned int DelayQueue::wait_time( void ) const
{
    if ( packet_queue_.empty() ) {
        return numeric_limits<uint16_t>::max();
    }

    const auto now = raw_timestamp();
    if ( packet_queue_.front().first <= now ) {
        return 0;
    }
    return static_cast<unsigned int>(packet_queue_.front().first - now);
}
""", encoding="utf-8")
PY

./autogen.sh
./configure
for directory in util packet graphing; do
    make -C "src/$directory" -j"$(nproc)"
done
make -C src/frontend -j"$(nproc)" mm-delay mm-link mm-loss

mkdir -p "$install_dir"
if [[ "$setuid_install" -eq 1 ]]; then
    sudo install -o root -g root -m 4755 src/frontend/{mm-delay,mm-link,mm-loss} "$install_dir/"
else
    install -m 755 src/frontend/{mm-delay,mm-link,mm-loss} "$install_dir/"
fi
