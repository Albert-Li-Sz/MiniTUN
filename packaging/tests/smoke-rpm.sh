#!/bin/sh
set -eu

package_directory=${1:?usage: smoke-rpm.sh PACKAGE_DIRECTORY [ARCH]}
expected_arch=${2:-x86_64}
client_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-client-*.${expected_arch}.rpm")
server_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-server-*.${expected_arch}.rpm")
sdk_library_package=$(find "$package_directory" -maxdepth 1 -type f -name "libminitun-client1-*.${expected_arch}.rpm")
sdk_development_package=$(find "$package_directory" -maxdepth 1 -type f -name "libminitun-client-devel-*.${expected_arch}.rpm")
[ -n "$client_package" ]
[ -n "$server_package" ]
[ -n "$sdk_library_package" ]
[ -n "$sdk_development_package" ]

dnf install -y "$client_package" "$server_package" "$sdk_library_package" "$sdk_development_package"

minitun version
/usr/libexec/minitun/minitund --version
minitun-p2p --version
minitun-server --version
test -f /usr/lib/systemd/system/minitund.service
test -f /usr/lib/systemd/system/minitun-server.service
test -f /usr/lib/sysusers.d/minitun.conf
test -f /usr/lib/sysusers.d/minitun-server.conf
test ! -e /etc/minitun-server/server.crt
test ! -e /etc/minitun-server/server.key
test ! -e /etc/minitun-server/token
test -f /usr/include/minitun/client.h
test -f /usr/include/minitun/client.hpp
test -f /usr/include/minitun/remote_protocol.hpp
pkg-config --exists minitun-client
pkg-config --exists minitun-remote-protocol
getent passwd minitun >/dev/null
getent passwd minitun-server >/dev/null

install -d /var/lib/minitun /var/lib/minitun-server
touch /var/lib/minitun/upgrade-marker /var/lib/minitun-server/upgrade-marker
rpm -Uvh --replacepkgs "$client_package" "$server_package" "$sdk_library_package" "$sdk_development_package"
test -f /var/lib/minitun/upgrade-marker
test -f /var/lib/minitun-server/upgrade-marker

dnf remove -y minitun-client minitun-server libminitun-client-devel libminitun-client1
test ! -e /usr/bin/minitun
test ! -e /usr/bin/minitun-server
test -f /var/lib/minitun/upgrade-marker
test -f /var/lib/minitun-server/upgrade-marker

echo "RPM package smoke test passed"
