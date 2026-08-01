#!/bin/sh
set -e

if command -v systemd-sysusers >/dev/null 2>&1; then
    for definition in \
        /usr/lib/sysusers.d/minitun.conf \
        /usr/lib/sysusers.d/minitun-server.conf
    do
        if [ -f "$definition" ]; then
            systemd-sysusers "$definition"
        fi
    done
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

exit 0
