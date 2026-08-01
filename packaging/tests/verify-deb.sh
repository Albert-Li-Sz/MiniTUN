#!/bin/sh
set -eu

package_directory=${1:?usage: verify-deb.sh PACKAGE_DIRECTORY}
package_count=$(find "$package_directory" -maxdepth 1 -type f -name '*.deb' | wc -l | tr -d ' ')
[ "$package_count" -eq 2 ]

client_package=$(find "$package_directory" -maxdepth 1 -type f -name 'minitun-client_*_amd64.deb')
server_package=$(find "$package_directory" -maxdepth 1 -type f -name 'minitun-server_*_amd64.deb')
[ -n "$client_package" ]
[ -n "$server_package" ]

dpkg-deb -I "$client_package"
dpkg-deb -c "$client_package"
dpkg-deb -I "$server_package"
dpkg-deb -c "$server_package"

[ "$(dpkg-deb -f "$client_package" Package)" = minitun-client ]
[ "$(dpkg-deb -f "$server_package" Package)" = minitun-server ]
[ "$(dpkg-deb -f "$client_package" Architecture)" = amd64 ]
[ "$(dpkg-deb -f "$server_package" Architecture)" = amd64 ]

client_contents=$(dpkg-deb --fsys-tarfile "$client_package" | tar -tf -)
server_contents=$(dpkg-deb --fsys-tarfile "$server_package" | tar -tf -)

printf '%s\n' "$client_contents" | grep -qx './usr/bin/minitun'
printf '%s\n' "$client_contents" | grep -qx './usr/libexec/minitun/minitund'
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
