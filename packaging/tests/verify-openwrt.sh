#!/bin/sh

set -eu

if [ "$#" -lt 4 ] || [ "$#" -gt 6 ]; then
	echo "usage: verify-openwrt.sh SDK_DIR PACKAGE_DIR VERSION ARCH [QEMU] [KIND]" >&2
	exit 2
fi

sdk_dir=$1
package_dir=$2
expected_version=$3
expected_arch=$4
qemu_binary=${5:-}
package_kind=${6:-}
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

command -v jq >/dev/null 2>&1 || {
	echo "jq is required" >&2
	exit 2
}

if [ -z "$package_kind" ]; then
	if find "$package_dir" -maxdepth 1 -type f -name '*.apk' | grep -q .; then
		package_kind=apk
	elif find "$package_dir" -maxdepth 1 -type f -name '*.ipk' | grep -q .; then
		package_kind=ipk
	else
		echo "no .apk or .ipk packages found in $package_dir" >&2
		exit 2
	fi
fi

apk_tool="$sdk_dir/staging_dir/host/bin/apk"
case "$package_kind" in
	apk)
		[ -x "$apk_tool" ] || {
			echo "OpenWrt SDK apk tool not found: $apk_tool" >&2
			exit 2
		}
		;;
	ipk) ;;
	*)
		echo "package kind must be apk or ipk, got: $package_kind" >&2
		exit 2
		;;
esac

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

control_field() {
	package=$1
	field=$2
	tar -xOzf "$package" ./control.tar.gz |
		tar -xOzf - ./control |
		awk -v field="$field" \
			'$1 == field ":" { $1=""; sub(/^ /, ""); print; exit }'
}

check_metadata() {
	package=$1
	name=$2
	metadata=$3

	case "$package_kind" in
		apk)
			"$apk_tool" --allow-untrusted verify "$package"
			"$apk_tool" adbdump --format json "$package" >"$metadata"
			jq -e --arg value "$name" '.info.name == $value' "$metadata" >/dev/null
			jq -e --arg value "${expected_version}-r1" \
				'.info.version == $value' "$metadata" >/dev/null
			jq -e --arg value "$expected_arch" '.info.arch == $value' \
				"$metadata" >/dev/null
			;;
		ipk)
			[ "$(control_field "$package" Package)" = "$name" ] || {
				echo "$package has an unexpected Package field" >&2
				exit 1
			}
			[ "$(control_field "$package" Version)" = "${expected_version}-r1" ] || {
				echo "$package has an unexpected Version field" >&2
				exit 1
			}
			[ "$(control_field "$package" Architecture)" = "$expected_arch" ] || {
				echo "$package has an unexpected Architecture field" >&2
				exit 1
			}
			;;
	esac
}

extract_package() {
	package=$1
	destination=$2
	case "$package_kind" in
		apk)
			"$apk_tool" --allow-untrusted extract --destination "$destination" \
				"$package"
			;;
		ipk)
			mkdir -p "$destination"
			tar -xOzf "$package" ./data.tar.gz |
				tar -xzf - -C "$destination"
			;;
	esac
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

case "$package_kind" in
	apk)
		client_package=$(find_one_package 'minitun-client-*.apk')
		server_package=$(find_one_package 'minitun-server-*.apk')
		;;
	ipk)
		client_package=$(find_one_package 'minitun-client_*.ipk')
		server_package=$(find_one_package 'minitun-server_*.ipk')
		;;
esac

temporary_dir=$(mktemp -d)
trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM
client_root="$temporary_dir/client"
server_root="$temporary_dir/server"
mkdir -p "$client_root" "$server_root"

check_metadata "$client_package" minitun-client "$temporary_dir/client.json"
check_metadata "$server_package" minitun-server "$temporary_dir/server.json"

extract_package "$client_package" "$client_root"
extract_package "$server_package" "$server_root"

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
	"$server_root/etc/config/minitun-server" \
	"$server_root/etc/capabilities/minitun-server.json"; do
	[ -f "$path" ] || {
		echo "missing configuration file: $path" >&2
		exit 1
	}
	check_mode "$path" 600
done

jq -e '
	[.bounding, .effective, .ambient, .permitted, .inheritable]
	| all(. == ["CAP_NET_BIND_SERVICE"])
' "$server_root/etc/capabilities/minitun-server.json" >/dev/null
grep -q 'procd_set_param capabilities /etc/capabilities/minitun-server.json' \
	"$server_root/etc/init.d/minitun-server"
grep -q 'procd_set_param no_new_privs 1' \
	"$server_root/etc/init.d/minitun-server"

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

	if [ "$expected_arch" = aarch64_generic ]; then
		"$script_dir/openwrt-aarch64-e2e.sh" \
			"$qemu_binary" \
			"$runtime_root" \
			"$client_root/usr/bin/minitun" \
			"$client_root/usr/libexec/minitun/minitund" \
			"$server_root/usr/bin/minitun-server"
	fi
fi

printf 'verified OpenWrt %s packages for %s\n' \
	"$expected_version" "$expected_arch"
