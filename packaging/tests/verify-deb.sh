#!/bin/sh
set -eu

package_directory=${1:?usage: verify-deb.sh PACKAGE_DIRECTORY [ARCH]}
expected_arch=${2:-amd64}
package_count=$(find "$package_directory" -maxdepth 1 -type f -name '*.deb' | wc -l | tr -d ' ')
[ "$package_count" -eq 4 ]

client_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-client_*_${expected_arch}.deb")
server_package=$(find "$package_directory" -maxdepth 1 -type f -name "minitun-server_*_${expected_arch}.deb")
sdk_library_package=$(find "$package_directory" -maxdepth 1 -type f -name "libminitun-client1_*_${expected_arch}.deb")
sdk_development_package=$(find "$package_directory" -maxdepth 1 -type f -name "libminitun-client-dev_*_${expected_arch}.deb")
[ -n "$client_package" ]
[ -n "$server_package" ]
[ -n "$sdk_library_package" ]
[ -n "$sdk_development_package" ]

dpkg-deb -I "$client_package"
dpkg-deb -c "$client_package"
dpkg-deb -I "$server_package"
dpkg-deb -c "$server_package"
dpkg-deb -I "$sdk_library_package"
dpkg-deb -c "$sdk_library_package"
dpkg-deb -I "$sdk_development_package"
dpkg-deb -c "$sdk_development_package"

[ "$(dpkg-deb -f "$client_package" Package)" = minitun-client ]
[ "$(dpkg-deb -f "$server_package" Package)" = minitun-server ]
[ "$(dpkg-deb -f "$sdk_library_package" Package)" = libminitun-client1 ]
[ "$(dpkg-deb -f "$sdk_development_package" Package)" = libminitun-client-dev ]
[ "$(dpkg-deb -f "$client_package" Architecture)" = "$expected_arch" ]
[ "$(dpkg-deb -f "$server_package" Architecture)" = "$expected_arch" ]
[ "$(dpkg-deb -f "$sdk_library_package" Architecture)" = "$expected_arch" ]
[ "$(dpkg-deb -f "$sdk_development_package" Architecture)" = "$expected_arch" ]

client_contents=$(dpkg-deb --fsys-tarfile "$client_package" | tar -tf -)
server_contents=$(dpkg-deb --fsys-tarfile "$server_package" | tar -tf -)
sdk_library_contents=$(dpkg-deb --fsys-tarfile "$sdk_library_package" | tar -tf -)
sdk_development_contents=$(dpkg-deb --fsys-tarfile "$sdk_development_package" | tar -tf -)

printf '%s\n' "$client_contents" | grep -qx './usr/bin/minitun'
printf '%s\n' "$client_contents" | grep -qx './usr/bin/minitun-gui'
printf '%s\n' "$client_contents" | grep -qx './usr/bin/minitun-p2p'
printf '%s\n' "$client_contents" | grep -qx './usr/libexec/minitun/minitund'
printf '%s\n' "$client_contents" | grep -qx './usr/share/minitun/gui/index.html'
printf '%s\n' "$client_contents" | grep -qx './usr/share/minitun/gui/logo.svg'
printf '%s\n' "$client_contents" | grep -qx './usr/lib/systemd/system/minitund.service'
printf '%s\n' "$client_contents" | grep -qx './usr/lib/sysusers.d/minitun.conf'
if printf '%s\n' "$client_contents" | grep -q 'minitun-server'; then
    exit 1
fi

printf '%s\n' "$server_contents" | grep -qx './usr/bin/minitun-server'
printf '%s\n' "$server_contents" | grep -qx './usr/lib/systemd/system/minitun-server.service'
printf '%s\n' "$server_contents" | grep -qx './usr/lib/sysusers.d/minitun-server.conf'
printf '%s\n' "$server_contents" | grep -qx './etc/minitun-server/README'
if printf '%s\n' "$server_contents" | grep -Eq '/etc/minitun-server/(server\.crt|server\.key|token)$'; then
    exit 1
fi

printf '%s\n' "$sdk_library_contents" | grep -Eq '^\./usr/lib(/[^/]+)?/libminitun-client\.so\.1(\.|$)'
printf '%s\n' "$sdk_library_contents" | grep -Eq '^\./usr/lib(/[^/]+)?/libminitun-remote-protocol\.so\.1(\.|$)'
if printf '%s\n' "$sdk_library_contents" | grep -Eq '/include/|libminitun-client\.so$'; then
    exit 1
fi
printf '%s\n' "$sdk_development_contents" | grep -qx './usr/include/minitun/client.h'
printf '%s\n' "$sdk_development_contents" | grep -qx './usr/include/minitun/client.hpp'
printf '%s\n' "$sdk_development_contents" | grep -qx './usr/include/minitun/remote_protocol.hpp'
printf '%s\n' "$sdk_development_contents" | grep -Eq '^\./usr/lib(/[^/]+)?/pkgconfig/minitun-client\.pc$'
printf '%s\n' "$sdk_development_contents" | grep -Eq '^\./usr/lib(/[^/]+)?/pkgconfig/minitun-remote-protocol\.pc$'
printf '%s\n' "$sdk_development_contents" | grep -Eq '^\./usr/lib(/[^/]+)?/cmake/MiniTun/MiniTunConfig\.cmake$'
printf '%s\n' "$sdk_development_contents" | grep -Eq '^\./usr/lib(/[^/]+)?/libminitun-client\.so$'
printf '%s\n' "$sdk_development_contents" | grep -Eq '^\./usr/lib(/[^/]+)?/libminitun-remote-protocol\.so$'
dpkg-deb -f "$sdk_development_package" Depends | grep -q 'libminitun-client1'

control_directory=$(mktemp -d)
trap 'rm -rf -- "$control_directory"' EXIT HUP INT TERM
dpkg-deb -e "$client_package" "$control_directory/client"
dpkg-deb -e "$server_package" "$control_directory/server"
grep -q systemd-sysusers "$control_directory/client/postinst"
grep -q systemd-sysusers "$control_directory/server/postinst"
grep -q 'daemon-reload' "$control_directory/client/postrm"
grep -q 'daemon-reload' "$control_directory/server/postrm"
grep -q '/var/lib/minitun' "$control_directory/client/postrm"
grep -q '/var/lib/minitun-server' "$control_directory/server/postrm"

echo "DEB package verification passed"
