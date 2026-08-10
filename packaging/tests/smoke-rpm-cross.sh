#!/bin/sh
set -eu

# Cross-architecture RPM smoke test. Runs inside a foreign-architecture Ubuntu
# container (docker --platform). Fedora-built RPMs are installed with
# --nodeps after installing the matching shared libraries by soname, which
# validates the payload layout and that the binaries execute on the target
# architecture. Full dnf dependency resolution stays covered by the native
# Fedora smoke test.

package_directory=${1:?usage: smoke-rpm-cross.sh PACKAGE_DIRECTORY [ARCH]}
expected_arch=${2:-}
if [ -z "$expected_arch" ]; then
	echo "ARCH is required for smoke-rpm-cross.sh" >&2
	exit 2
fi

client_package=$(find "$package_directory" -maxdepth 1 -type f \
	-name "minitun-client-*.${expected_arch}.rpm")
server_package=$(find "$package_directory" -maxdepth 1 -type f \
	-name "minitun-server-*.${expected_arch}.rpm")
sdk_library_package=$(find "$package_directory" -maxdepth 1 -type f \
	-name "libminitun-client1-*.${expected_arch}.rpm")
sdk_development_package=$(find "$package_directory" -maxdepth 1 -type f \
	-name "libminitun-client-devel-*.${expected_arch}.rpm")
[ -n "$client_package" ]
[ -n "$server_package" ]
[ -n "$sdk_library_package" ]
[ -n "$sdk_development_package" ]

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
	libgcc-s1 \
	libsqlite3-0 \
	libssl3t64 \
	libstdc++6 \
	pkg-config \
	rpm \
	systemd

case "$expected_arch" in
	armv7hl)
		apt-get install -y --no-install-recommends libatomic1
		;;
esac

rpm -i --nodeps "$client_package" "$server_package" "$sdk_library_package" "$sdk_development_package"

minitun version
/usr/libexec/minitun/minitund --version
minitun-server --version
test -f /usr/lib/systemd/system/minitund.service
test -f /usr/lib/systemd/system/minitun-server.service
test -f /usr/lib/sysusers.d/minitun.conf
test -f /usr/lib/sysusers.d/minitun-server.conf
test -f /usr/include/minitun/client.h
test -f /usr/include/minitun/client.hpp
pkg-config --exists minitun-client
getent passwd minitun >/dev/null
getent passwd minitun-server >/dev/null

rpm -e minitun-client minitun-server libminitun-client-devel libminitun-client1
test ! -e /usr/bin/minitun
test ! -e /usr/bin/minitun-server

echo "RPM cross-architecture smoke test passed ($expected_arch)"
