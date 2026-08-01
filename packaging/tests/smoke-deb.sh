#!/bin/sh
set -eu

package_directory=${1:?usage: smoke-deb.sh PACKAGE_DIRECTORY}
client_package=$(find "$package_directory" -maxdepth 1 -type f -name 'minitun-client_*_amd64.deb')
server_package=$(find "$package_directory" -maxdepth 1 -type f -name 'minitun-server_*_amd64.deb')
[ -n "$client_package" ]
[ -n "$server_package" ]

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y "$client_package" "$server_package"

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
dpkg -i "$client_package" "$server_package"
test -f /var/lib/minitun/upgrade-marker
test -f /var/lib/minitun-server/upgrade-marker

apt-get remove -y minitun-client minitun-server
test ! -e /usr/bin/minitun
test ! -e /usr/bin/minitun-server
test -f /var/lib/minitun/upgrade-marker
test -f /var/lib/minitun-server/upgrade-marker

dpkg --purge minitun-client minitun-server
test ! -e /var/lib/minitun
test ! -e /var/lib/minitun-server

echo "DEB package smoke test passed"
