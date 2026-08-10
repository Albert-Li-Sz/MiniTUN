#!/bin/sh
set -eu

package_directory=${1:?usage: smoke-deb.sh PACKAGE_DIRECTORY [ARCH]}
expected_arch=${2:-amd64}
client_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-client_*_${expected_arch}.deb")
server_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-server_*_${expected_arch}.deb")
sdk_library_package=$(find "$package_directory" -maxdepth 1 -type f -name "libminitun-client1_*_${expected_arch}.deb")
sdk_development_package=$(find "$package_directory" -maxdepth 1 -type f -name "libminitun-client-dev_*_${expected_arch}.deb")
[ -n "$client_package" ]
[ -n "$server_package" ]
[ -n "$sdk_library_package" ]
[ -n "$sdk_development_package" ]

# The container's /etc/resolv.conf is a bind mount; systemd's postinst cannot
# move it aside when it installs the systemd dependency. Detach it first so
# package installation works in clean CI containers.
if command -v mountpoint >/dev/null 2>&1 && mountpoint -q /etc/resolv.conf; then
    umount /etc/resolv.conf 2>/dev/null || true
fi
rm -f /etc/resolv.conf 2>/dev/null || true
printf 'nameserver 1.1.1.1\n' > /etc/resolv.conf 2>/dev/null || true

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y pkg-config \
	"$client_package" "$server_package" "$sdk_library_package" "$sdk_development_package"

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
test -f /usr/include/minitun/client.h
test -f /usr/include/minitun/client.hpp
pkg-config --exact-version="$(minitun version | awk 'NR == 1 {print $2}')" minitun-client
getent passwd minitun >/dev/null
getent passwd minitun-server >/dev/null

install -d /var/lib/minitun /var/lib/minitun-server
touch /var/lib/minitun/upgrade-marker /var/lib/minitun-server/upgrade-marker
dpkg -i "$client_package" "$server_package" "$sdk_library_package" "$sdk_development_package"
test -f /var/lib/minitun/upgrade-marker
test -f /var/lib/minitun-server/upgrade-marker

apt-get remove -y minitun-client minitun-server libminitun-client-dev libminitun-client1
test ! -e /usr/bin/minitun
test ! -e /usr/bin/minitun-server
test -f /var/lib/minitun/upgrade-marker
test -f /var/lib/minitun-server/upgrade-marker

dpkg --purge minitun-client minitun-server
test ! -e /var/lib/minitun
test ! -e /var/lib/minitun-server

echo "DEB package smoke test passed"
