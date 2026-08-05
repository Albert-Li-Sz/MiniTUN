#!/bin/sh
set -eu

package_directory=${1:?usage: verify-rpm.sh PACKAGE_DIRECTORY [ARCH]}
expected_arch=${2:-x86_64}
package_count=$(find "$package_directory" -maxdepth 1 -type f -name '*.rpm' | wc -l | tr -d ' ')
[ "$package_count" -eq 2 ]

client_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-client-*.${expected_arch}.rpm")
server_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-server-*.${expected_arch}.rpm")
[ -n "$client_package" ]
[ -n "$server_package" ]

rpm -qip "$client_package"
rpm -qlp "$client_package"
rpm -qip "$server_package"
rpm -qlp "$server_package"

[ "$(rpm -qp --queryformat '%{NAME}' "$client_package")" = minitun-client ]
[ "$(rpm -qp --queryformat '%{NAME}' "$server_package")" = minitun-server ]
[ "$(rpm -qp --queryformat '%{ARCH}' "$client_package")" = "$expected_arch" ]
[ "$(rpm -qp --queryformat '%{ARCH}' "$server_package")" = "$expected_arch" ]

rpm -qlp "$client_package" | grep -qx '/usr/bin/minitun'
rpm -qlp "$client_package" | grep -qx '/usr/libexec/minitun/minitund'
rpm -qlp "$client_package" | grep -qx '/usr/lib/systemd/system/minitund.service'
rpm -qlp "$client_package" | grep -qx '/usr/lib/sysusers.d/minitun.conf'
if rpm -qlp "$client_package" | grep -q 'minitun-server'; then
    exit 1
fi

rpm -qlp "$server_package" | grep -qx '/usr/bin/minitun-server'
rpm -qlp "$server_package" | grep -qx '/usr/lib/systemd/system/minitun-server.service'
rpm -qlp "$server_package" | grep -qx '/usr/lib/sysusers.d/minitun-server.conf'
rpm -qlp "$server_package" | grep -qx '/etc/minitun-server/README'
if rpm -qlp "$server_package" | grep -Eq '/etc/minitun-server/(server\.crt|server\.key|token)$'; then
    exit 1
fi

rpm -qp --scripts "$client_package" | grep -q systemd-sysusers
rpm -qp --scripts "$server_package" | grep -q systemd-sysusers
rpm -qp --scripts "$client_package" | grep -q daemon-reload
rpm -qp --scripts "$server_package" | grep -q daemon-reload

echo "RPM package verification passed"
