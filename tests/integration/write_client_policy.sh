#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
socket_path=${2:?missing daemon socket}
config_path=${3:?missing client policy path}
psk_path=${4:?missing PSK path}
allowed_ports=${5:-1024-65535}
max_tunnels=${6:-128}
max_connections=${7:-10000}
max_idle_workers=${8:-32}

client_id=$(
    "$minitun_bin" --socket "$socket_path" daemon identity --json |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["client_id"])'
)

python3 - "$config_path" "$client_id" "$psk_path" "$allowed_ports" \
    "$max_tunnels" "$max_connections" "$max_idle_workers" <<'PY'
import json
import os
import sys

path, client_id, psk_path, allowed_ports = sys.argv[1:5]
document = {
    "format_version": 1,
    "clients": [{
        "client_id": client_id,
        "enabled": True,
        "psk_file": os.path.abspath(psk_path),
        "allowed_ports": [allowed_ports],
        "max_tunnels": int(sys.argv[5]),
        "max_connections": int(sys.argv[6]),
        "max_idle_workers": int(sys.argv[7]),
    }],
}
temporary = path + ".tmp"
with open(temporary, "w", encoding="utf-8") as stream:
    json.dump(document, stream, separators=(",", ":"), sort_keys=True)
    stream.write("\n")
os.chmod(temporary, 0o640)
os.replace(temporary, path)
PY

printf '%s\n' "$client_id"
