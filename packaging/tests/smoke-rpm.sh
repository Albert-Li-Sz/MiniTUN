#!/bin/sh
set -eu

package_directory=${1:?usage: smoke-rpm.sh PACKAGE_DIRECTORY}
client_package=$(find "$package_directory" -maxdepth 1 -type f -name 'minitun-client-*-1.x86_64.rpm')
server_package=$(find "$package_directory" -maxdepth 1 -type f -name 'minitun-server-*-1.x86_64.rpm')
[ -n "$client_package" ]
[ -n "$server_package" ]

dnf install -y "$client_package" "$server_package"

minitun version
/usr/libexec/minitun/minitund --version
minitun-server --version
test -f /usr/lib/systemd/system/minitund.service
test -f /usr/lib/systemd/system/minitun-server.service
test -f /usr/lib/sysusers.d/minitun.conf
test -f /usr/lib/sysusers.d/minitun-server.conf
test ! -e /etc/minitun-server/server.crt
test ! -e /etc/minitun-server/server.key
test ! -e /etc/minitun-server/token
getent passwd minitun >/dev/null
getent passwd minitun-server >/dev/null

install -d /var/lib/minitun /var/lib/minitun-server
touch /var/lib/minitun/upgrade-marker /var/lib/minitun-server/upgrade-marker
rpm -Uvh --replacepkgs "$client_package" "$server_package"
test -f /var/lib/minitun/upgrade-marker
test -f /var/lib/minitun-server/upgrade-marker

dnf remove -y minitun-client minitun-server
test ! -e /usr/bin/minitun
test ! -e /usr/bin/minitun-server
test -f /var/lib/minitun/upgrade-marker
test -f /var/lib/minitun-server/upgrade-marker

echo "RPM package smoke test passed"
