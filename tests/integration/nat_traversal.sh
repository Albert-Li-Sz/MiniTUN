#!/usr/bin/env bash
# End-to-end TCP simultaneous open across a real NAT topology built with
# network namespaces and iptables. Three client roles run in their own netns
# behind two independent endpoint-independent-mapping NATs (SNAT), with the
# public server in a fifth "public" namespace. The P2P connector and the daemon
# Worker must punch both NATs and select the direct path; the loopback unit
# tests cannot observe this crossing, which is why this gate exists.
#
# Topology:
#   mt-pub (server)  10.99.0.10/11
#   mt-nata (SNAT -> 10.99.0.2)  |  mt-natb (SNAT -> 10.99.0.3)
#   mt-a (daemon) 10.0.1.2/24     |  mt-b (connector) 10.0.2.2/24
#
# The test skips cleanly on non-Linux hosts and when privileged tooling is
# unavailable; on CI (Linux, passwordless sudo) it runs for real.
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}
p2p_bin=${4:?missing minitun-p2p binary}

if [[ "$(uname -s)" != "Linux" ]]; then
    echo 'NAT traversal test skipped (requires Linux netns/iptables)'
    exit 77
fi
for tool in ip iptables sysctl python3 openssl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "NAT traversal test skipped (missing ${tool})"
        exit 77
    fi
done

if [[ $EUID -ne 0 ]]; then
    if ! sudo -n true 2>/dev/null; then
        echo 'NAT traversal test skipped (requires root or passwordless sudo)'
        exit 77
    fi
    exec sudo "$0" "$@"
fi

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-nat.XXXXXX")
integration_dir=$(cd "$(dirname "$0")" && pwd -P)
ns_list=("pub" "nata" "natb" "a" "b")
daemon_pid=
server_pid=
echo_pid=
p2p_pid=

cleanup() {
    for process_id in "$p2p_pid" "$daemon_pid" "$server_pid" "$echo_pid"; do
        if [[ -n "$process_id" ]] && kill -0 "$process_id" 2>/dev/null; then
            kill -TERM "$process_id" 2>/dev/null || true
        fi
    done
    for ns in "${ns_list[@]}"; do
        for pid in $(ip netns pids "mt-$ns" 2>/dev/null); do
            kill -TERM "$pid" 2>/dev/null || true
        done
    done
    for ns in "${ns_list[@]}"; do
        ip netns del "mt-$ns" 2>/dev/null || true
    done
    rm -rf "$runtime_dir"
}
trap cleanup EXIT

for ns in "${ns_list[@]}"; do
    ip netns add "mt-$ns"
done

# Public backbone: server namespace to each NAT.
ip link add veth-puba type veth peer name veth-pub-a
ip link add veth-pubb type veth peer name veth-pub-b
# Private legs: each NAT to its client.
ip link add veth-a-nat type veth peer name veth-a
ip link add veth-b-nat type veth peer name veth-b

ip link set veth-puba netns mt-pub
ip link set veth-pub-a netns mt-nata
ip link set veth-pubb netns mt-pub
ip link set veth-pub-b netns mt-natb
ip link set veth-a-nat netns mt-nata
ip link set veth-a netns mt-a
ip link set veth-b-nat netns mt-natb
ip link set veth-b netns mt-b

ip netns exec mt-pub ip addr add 10.99.0.10/24 dev veth-puba
ip netns exec mt-pub ip addr add 10.99.0.11/24 dev veth-pubb
ip netns exec mt-pub ip link set veth-puba up
ip netns exec mt-pub ip link set veth-pubb up

ip netns exec mt-nata ip addr add 10.99.0.2/24 dev veth-pub-a
ip netns exec mt-nata ip addr add 10.0.1.1/24 dev veth-a-nat
ip netns exec mt-nata ip link set veth-pub-a up
ip netns exec mt-nata ip link set veth-a-nat up
ip netns exec mt-nata ip link set lo up
ip netns exec mt-nata sysctl -qw net.ipv4.ip_forward=1
ip netns exec mt-nata iptables -t nat -A POSTROUTING \
    -s 10.0.1.0/24 -o veth-pub-a -j SNAT --to-source 10.99.0.2

ip netns exec mt-natb ip addr add 10.99.0.3/24 dev veth-pub-b
ip netns exec mt-natb ip addr add 10.0.2.1/24 dev veth-b-nat
ip netns exec mt-natb ip link set veth-pub-b up
ip netns exec mt-natb ip link set veth-b-nat up
ip netns exec mt-natb ip link set lo up
ip netns exec mt-natb sysctl -qw net.ipv4.ip_forward=1
ip netns exec mt-natb iptables -t nat -A POSTROUTING \
    -s 10.0.2.0/24 -o veth-pub-b -j SNAT --to-source 10.99.0.3

ip netns exec mt-a ip addr add 10.0.1.2/24 dev veth-a
ip netns exec mt-a ip link set veth-a up
ip netns exec mt-a ip link set lo up
ip netns exec mt-a ip route add default via 10.0.1.1

ip netns exec mt-b ip addr add 10.0.2.2/24 dev veth-b
ip netns exec mt-b ip link set veth-b up
ip netns exec mt-b ip link set lo up
ip netns exec mt-b ip route add default via 10.0.2.1

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost,IP:10.99.0.10 \
    -keyout "$runtime_dir/server.key" \
    -out "$runtime_dir/server.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/server.key"
token='nat-traversal-test-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

read -r control_port p2p_remote p2p_local echo_port < <(python3 - <<'PY'
import socket

ports = []
for _ in range(4):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    ports.append(probe.getsockname()[1])
    probe.close()
print(*ports)
PY
)

# Local TCP echo target inside the daemon's namespace.
ip netns exec mt-a python3 - "$echo_port" <<'PY' &
import signal
import socket
import sys
import threading

port = int(sys.argv[1])
stop = threading.Event()
signal.signal(signal.SIGTERM, lambda *_a: stop.set())

def echo(connection):
    try:
        while True:
            chunk = connection.recv(16384)
            if not chunk:
                break
            connection.sendall(chunk)
    except OSError:
        pass
    finally:
        connection.close()

listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", port))
listener.listen()
listener.settimeout(0.2)
while not stop.is_set():
    try:
        connection, _ = listener.accept()
    except TimeoutError:
        continue
    threading.Thread(target=echo, args=(connection,), daemon=True).start()
listener.close()
PY
echo_pid=$!

ip netns exec mt-a "$minitund_bin" --foreground \
    --socket "$runtime_dir/minitun.sock" \
    --database "$runtime_dir/state.db" \
    --credentials "$runtime_dir/credentials.db" \
    --insecure-skip-verify \
    --io-threads 4 \
    >>"$runtime_dir/minitund.log" 2>&1 &
daemon_pid=$!
for _ in $(seq 1 100); do
    [[ -S "$runtime_dir/minitun.sock" ]] && break
    kill -0 "$daemon_pid" 2>/dev/null
    sleep 0.05
done
[[ -S "$runtime_dir/minitun.sock" ]]

bash "$integration_dir/write_client_policy.sh" "$minitun_bin" "$runtime_dir/minitun.sock" \
    "$runtime_dir/clients.json" "$runtime_dir/token" >/dev/null

ip netns exec mt-pub "$server_bin" --foreground \
    --listen "10.99.0.10:$control_port" \
    --tls-cert "$runtime_dir/server.crt" \
    --tls-key "$runtime_dir/server.key" \
    --clients-config "$runtime_dir/clients.json" \
    --heartbeat-interval 1 \
    --heartbeat-timeout 3 \
    --io-threads 4 \
    >>"$runtime_dir/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 100); do
    if ip netns exec mt-a python3 - "$control_port" <<'PY' 2>/dev/null
import socket
import sys
probe = socket.create_connection(("10.99.0.10", int(sys.argv[1])), timeout=0.1)
probe.close()
PY
    then
        break
    fi
    kill -0 "$server_pid" 2>/dev/null
    sleep 0.05
done

"$minitun_bin" --socket "$runtime_dir/minitun.sock" server add "10.99.0.10:$control_port" \
    --name primary >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$runtime_dir/minitun.sock" server login primary --psk-stdin >/dev/null
"$minitun_bin" --socket "$runtime_dir/minitun.sock" tun add primary "$echo_port" "$p2p_remote" \
    --name p2p-echo --protocol p2p --remote-host 0.0.0.0 >/dev/null

wait_tunnel() {
    for _ in $(seq 1 160); do
        if "$minitun_bin" --socket "$runtime_dir/minitun.sock" tun inspect p2p-echo --json 2>/dev/null |
            python3 -c "import json,sys; raise SystemExit(json.load(sys.stdin)['actual_state'] != 'active')"
        then
            return
        fi
        sleep 0.1
    done
    sed -n '1,240p' "$runtime_dir/minitund.log" >&2
    sed -n '1,240p' "$runtime_dir/server.log" >&2
    return 1
}
wait_tunnel

: >"$runtime_dir/p2p.log"
ip netns exec mt-b "$p2p_bin" "10.99.0.10:$p2p_remote" --listen "127.0.0.1:$p2p_local" \
    --negotiation-timeout 8 --direct-timeout 5 \
    >>"$runtime_dir/p2p.log" 2>&1 &
p2p_pid=$!
for _ in $(seq 1 100); do
    if ip netns exec mt-b python3 - "$p2p_local" <<'PY' 2>/dev/null
import socket
import sys
probe = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.1)
probe.close()
PY
    then
        break
    fi
    kill -0 "$p2p_pid" 2>/dev/null
    sleep 0.05
done

ip netns exec mt-b python3 - "$p2p_local" <<'PY'
import socket
import sys

payload = b"nat-traversal-simultaneous-open" * 512
connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2)
connection.settimeout(10)
connection.sendall(payload)
received = bytearray()
while len(received) < len(payload):
    chunk = connection.recv(len(payload) - len(received))
    if not chunk:
        break
    received.extend(chunk)
if bytes(received) != payload:
    raise SystemExit("P2P payload did not round-trip through the NAT")
connection.close()
PY

for _ in $(seq 1 60); do
    grep -q "selected direct path" "$runtime_dir/p2p.log" && break
    sleep 0.1
done
grep -q "selected direct path" "$runtime_dir/p2p.log" || {
    sed -n '1,240p' "$runtime_dir/p2p.log" >&2
    echo 'NAT traversal did not select the direct path' >&2
    exit 1
}

printf 'NAT traversal e2e passed (direct path through dual EIM NAT)\n'
