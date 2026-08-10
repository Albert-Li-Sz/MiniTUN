#!/bin/sh
set -eu

package_directory=${1:?usage: verify-rpm.sh PACKAGE_DIRECTORY [ARCH]}
expected_arch=${2:-x86_64}
package_count=$(find "$package_directory" -maxdepth 1 -type f -name '*.rpm' | wc -l | tr -d ' ')
[ "$package_count" -eq 4 ]

client_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-client-*.${expected_arch}.rpm")
server_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-server-*.${expected_arch}.rpm")
sdk_library_package=$(find "$package_directory" -maxdepth 1 -type f -name "libminitun-client1-*.${expected_arch}.rpm")
sdk_development_package=$(find "$package_directory" -maxdepth 1 -type f -name "libminitun-client-devel-*.${expected_arch}.rpm")
[ -n "$client_package" ]
[ -n "$server_package" ]
[ -n "$sdk_library_package" ]
[ -n "$sdk_development_package" ]

rpm -qip "$client_package"
rpm -qlp "$client_package"
rpm -qip "$server_package"
rpm -qlp "$server_package"
rpm -qip "$sdk_library_package"
rpm -qlp "$sdk_library_package"
rpm -qip "$sdk_development_package"
rpm -qlp "$sdk_development_package"

[ "$(rpm -qp --queryformat '%{NAME}' "$client_package")" = minitun-client ]
[ "$(rpm -qp --queryformat '%{NAME}' "$server_package")" = minitun-server ]
[ "$(rpm -qp --queryformat '%{NAME}' "$sdk_library_package")" = libminitun-client1 ]
[ "$(rpm -qp --queryformat '%{NAME}' "$sdk_development_package")" = libminitun-client-devel ]
[ "$(rpm -qp --queryformat '%{ARCH}' "$client_package")" = "$expected_arch" ]
[ "$(rpm -qp --queryformat '%{ARCH}' "$server_package")" = "$expected_arch" ]
[ "$(rpm -qp --queryformat '%{ARCH}' "$sdk_library_package")" = "$expected_arch" ]
[ "$(rpm -qp --queryformat '%{ARCH}' "$sdk_development_package")" = "$expected_arch" ]

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

rpm -qlp "$sdk_library_package" | grep -Eq '^/usr/lib(64|/[^/]+)?/libminitun-client\.so\.1(\.|$)'
if rpm -qlp "$sdk_library_package" | grep -Eq '/include/|libminitun-client\.so$'; then
    exit 1
fi
rpm -qlp "$sdk_development_package" | grep -qx '/usr/include/minitun/client.h'
rpm -qlp "$sdk_development_package" | grep -qx '/usr/include/minitun/client.hpp'
rpm -qlp "$sdk_development_package" | grep -Eq '^/usr/lib(64|/[^/]+)?/pkgconfig/minitun-client\.pc$'
rpm -qlp "$sdk_development_package" | grep -Eq '^/usr/lib(64|/[^/]+)?/cmake/MiniTun/MiniTunConfig\.cmake$'
rpm -qlp "$sdk_development_package" | grep -Eq '^/usr/lib(64|/[^/]+)?/libminitun-client\.so$'
rpm -qp --requires "$sdk_development_package" | grep -q 'libminitun-client1'

rpm -qp --scripts "$client_package" | grep -q systemd-sysusers
rpm -qp --scripts "$server_package" | grep -q systemd-sysusers
rpm -qp --scripts "$client_package" | grep -q daemon-reload
rpm -qp --scripts "$server_package" | grep -q daemon-reload

echo "RPM package verification passed"
