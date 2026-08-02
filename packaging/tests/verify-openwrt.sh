#!/bin/sh

set -eu

if [ "$#" -lt 4 ] || [ "$#" -gt 5 ]; then
	echo "usage: verify-openwrt.sh SDK_DIR PACKAGE_DIR VERSION ARCH [QEMU]" >&2
	exit 2
fi

sdk_dir=$1
package_dir=$2
expected_version=$3
expected_arch=$4
qemu_binary=${5:-}
apk_tool="$sdk_dir/staging_dir/host/bin/apk"

[ -x "$apk_tool" ] || {
	echo "OpenWrt SDK apk tool not found: $apk_tool" >&2
	exit 2
}
command -v jq >/dev/null 2>&1 || {
	echo "jq is required" >&2
	exit 2
}

find_one_package() {
	pattern=$1
	result=
	count=0
	for candidate in "$package_dir"/$pattern; do
		[ -f "$candidate" ] || continue
		result=$candidate
		count=$((count + 1))
	done
	[ "$count" -eq 1 ] || {
		echo "expected one $pattern package in $package_dir, found $count" >&2
		exit 1
	}
	printf '%s\n' "$result"
}

check_metadata() {
	package=$1
	name=$2
	metadata=$3

	"$apk_tool" --allow-untrusted verify "$package"
	"$apk_tool" adbdump --format json "$package" >"$metadata"
	jq -e --arg value "$name" '.info.name == $value' "$metadata" >/dev/null
	jq -e --arg value "${expected_version}-r1" \
		'.info.version == $value' "$metadata" >/dev/null
	jq -e --arg value "$expected_arch" '.info.arch == $value' \
		"$metadata" >/dev/null
}

check_mode() {
	path=$1
	expected_mode=$2
	actual_mode=$(stat -c '%a' "$path")
	[ "$actual_mode" = "$expected_mode" ] || {
		echo "$path has mode $actual_mode, expected $expected_mode" >&2
		exit 1
	}
}

client_package=$(find_one_package 'minitun-client-*.apk')
server_package=$(find_one_package 'minitun-server-*.apk')

temporary_dir=$(mktemp -d)
trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM
client_root="$temporary_dir/client"
server_root="$temporary_dir/server"
mkdir -p "$client_root" "$server_root"

check_metadata "$client_package" minitun-client "$temporary_dir/client.json"
check_metadata "$server_package" minitun-server "$temporary_dir/server.json"

"$apk_tool" --allow-untrusted extract --destination "$client_root" \
	"$client_package"
"$apk_tool" --allow-untrusted extract --destination "$server_root" \
	"$server_package"

for path in \
	"$client_root/usr/bin/minitun" \
	"$client_root/usr/libexec/minitun/minitund" \
	"$client_root/etc/init.d/minitun" \
	"$server_root/usr/bin/minitun-server" \
	"$server_root/etc/init.d/minitun-server"; do
	[ -x "$path" ] || {
		echo "missing executable package file: $path" >&2
		exit 1
	}
	check_mode "$path" 755
done

for path in \
	"$client_root/etc/config/minitun" \
	"$server_root/etc/config/minitun-server"; do
	[ -f "$path" ] || {
		echo "missing configuration file: $path" >&2
		exit 1
	}
	check_mode "$path" 600
done

grep -q "option enabled '0'" "$client_root/etc/config/minitun"
grep -q "option enabled '0'" "$server_root/etc/config/minitun-server"
if grep -q 'option allow_ports' "$server_root/etc/config/minitun-server"; then
	echo "the default OpenWrt configuration must not restrict tunnel ports" >&2
	exit 1
fi

for binary in \
	"$client_root/usr/bin/minitun" \
	"$client_root/usr/libexec/minitun/minitund" \
	"$server_root/usr/bin/minitun-server"; do
	file "$binary" | grep -q 'ELF'
done

if [ -n "$qemu_binary" ]; then
	command -v "$qemu_binary" >/dev/null 2>&1 || {
		echo "QEMU executable not found: $qemu_binary" >&2
		exit 2
	}
	runtime_root=
	for candidate in "$sdk_dir"/staging_dir/target-*/root-*; do
		[ -d "$candidate" ] || continue
		runtime_root=$candidate
		break
	done
	[ -n "$runtime_root" ] || {
		echo "OpenWrt target runtime root was not found" >&2
		exit 1
	}

	"$qemu_binary" -L "$runtime_root" \
		"$client_root/usr/bin/minitun" version | \
		grep -F "$expected_version"
	"$qemu_binary" -L "$runtime_root" \
		"$client_root/usr/libexec/minitun/minitund" --version | \
		grep -F "$expected_version"
	"$qemu_binary" -L "$runtime_root" \
		"$server_root/usr/bin/minitun-server" --version | \
		grep -F "$expected_version"
fi

printf 'verified OpenWrt %s packages for %s\n' \
	"$expected_version" "$expected_arch"
