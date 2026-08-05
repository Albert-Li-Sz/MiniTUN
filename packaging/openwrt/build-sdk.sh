#!/bin/sh

set -eu

usage() {
	cat >&2 <<'EOF'
usage: build-sdk.sh SDK_DIR SOURCE_DIR [PACKAGE_VERSION]

Build MiniTun client and server packages with an extracted OpenWrt SDK
(24.10 opkg or 25.12 apk). The package is compiled from a deterministic
source tarball (git archive) that is generated from SOURCE_DIR and placed in
the SDK download directory; the OpenWrt Makefile's PKG_VERSION and PKG_HASH
are patched to match. Set MINITUN_OPENWRT_JOBS to control parallelism
(default: 2). Set MINITUN_OPENWRT_SKIP_FEED_UPDATE=1 only when the SDK feed
indexes are already current and their source checkouts are available.
EOF
}

[ "$#" -ge 2 ] && [ "$#" -le 3 ] || {
	usage
	exit 2
}

sdk_dir=$(cd "$1" && pwd -P)
source_dir=$(cd "$2" && pwd -P)
package_version=${3:-}
jobs=${MINITUN_OPENWRT_JOBS:-2}
skip_feed_update=${MINITUN_OPENWRT_SKIP_FEED_UPDATE:-0}

# Portable in-place sed (BSD sed -i and GNU sed -i differ).
sed_replace() {
	file=$1
	shift
	sed "$@" "$file" >"$file.tmp"
	mv "$file.tmp" "$file"
}

[ -x "$sdk_dir/scripts/feeds" ] || {
	echo "invalid OpenWrt SDK directory: $sdk_dir" >&2
	exit 2
}
[ -f "$source_dir/CMakeLists.txt" ] || {
	echo "invalid MiniTun source directory: $source_dir" >&2
	exit 2
}
case "$jobs" in
	''|*[!0-9]*|0)
		echo "MINITUN_OPENWRT_JOBS must be a positive integer" >&2
		exit 2
		;;
esac
case "$skip_feed_update" in
	0|1) ;;
	*)
		echo "MINITUN_OPENWRT_SKIP_FEED_UPDATE must be 0 or 1" >&2
		exit 2
		;;
esac

if [ -n "$package_version" ]; then
	printf '%s\n' "$package_version" | \
		grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+|_pre[0-9]+~[0-9a-f]{12})?$' || {
		echo "invalid package version: $package_version" >&2
		exit 2
	}
	project_version=$(awk '$1 == "VERSION" { print $2; exit }' \
		"$source_dir/CMakeLists.txt")
	base_version=${package_version%%-rc.*}
	base_version=${base_version%%_pre*}
	[ "$base_version" = "$project_version" ] || {
		echo "package version $package_version does not match project version $project_version" >&2
		exit 2
	}
else
	package_version=$(awk -F':=' '$1 == "PKG_VERSION" { print $2; exit }' \
		"$source_dir/packaging/openwrt/Makefile")
fi

cd "$sdk_dir"

if [ "$skip_feed_update" -eq 0 ]; then
	./scripts/feeds update base packages
else
	[ -f feeds/base.index ] && [ -f feeds/packages.index ] || {
		echo "OpenWrt feed indexes are unavailable" >&2
		exit 2
	}
fi
./scripts/feeds install -p base libopenssl ca-bundle zlib
./scripts/feeds install -p packages libsqlite3

[ ! -e package/minitun ] && [ ! -L package/minitun ] || {
	echo "package/minitun already exists; use a clean SDK directory" >&2
	exit 2
}
mkdir -p package/minitun
cp -a "$source_dir/packaging/openwrt/." package/minitun/

# Generate the deterministic source tarball consumed by OpenWrt's PKG_SOURCE
# machinery. `git archive --format=tar.gz` is reproducible, so the hash below
# matches the tarball attached to the matching GitHub Release. Non-git source
# directories fall back to a plain tar of the build inputs.
install -d dl
source_tarball="dl/minitun-$package_version.tar.gz"
if git -C "$source_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	git -C "$source_dir" archive \
		--format=tar.gz \
		--prefix="minitun-$package_version/" \
		-o "$sdk_dir/$source_tarball" \
		HEAD
else
	staging_dir=$(mktemp -d)
	trap 'rm -rf "$staging_dir"' EXIT HUP INT TERM
	install -d "$staging_dir/minitun-$package_version"
	cp -a \
		"$source_dir/CMakeLists.txt" \
		"$source_dir/LICENSE" \
		"$source_dir/README.md" \
		"$source_dir/apps" \
		"$source_dir/cmake" \
		"$source_dir/include" \
		"$source_dir/packaging" \
		"$source_dir/src" \
		"$staging_dir/minitun-$package_version/"
	tar -czf "$sdk_dir/$source_tarball" \
		-C "$staging_dir" "minitun-$package_version"
fi
source_hash=$(sha256sum "$source_tarball" | awk '{ print $1 }')

sed_replace package/minitun/Makefile \
	-e "s/^PKG_VERSION:=.*/PKG_VERSION:=$package_version/"
sed_replace package/minitun/Makefile \
	-e "s/^PKG_HASH:=.*/PKG_HASH:=$source_hash/"

touch .config
sed_replace .config \
	-e '/^CONFIG_PACKAGE_/d' \
	-e '/^# CONFIG_PACKAGE_.* is not set$/d' \
	-e '/^CONFIG_ALL=/d' \
	-e '/^CONFIG_ALL_KMODS=/d' \
	-e '/^CONFIG_ALL_NONSHARED=/d' \
	-e '/^# CONFIG_ALL is not set$/d' \
	-e '/^# CONFIG_ALL_KMODS is not set$/d' \
	-e '/^# CONFIG_ALL_NONSHARED is not set$/d' \
	.config
{
	printf '%s\n' '# CONFIG_ALL is not set'
	printf '%s\n' '# CONFIG_ALL_KMODS is not set'
	printf '%s\n' '# CONFIG_ALL_NONSHARED is not set'
	printf '%s\n' 'CONFIG_PACKAGE_minitun-client=m'
	printf '%s\n' 'CONFIG_PACKAGE_minitun-server=m'
} >>.config

make defconfig
if ! make -j"$jobs" package/minitun/compile; then
	echo "OpenWrt parallel build failed; retrying once with -j1 V=sc" >&2
	make -j1 V=sc package/minitun/compile
fi

# Generate the repository index (Packages/Packages.gz for 24.10 opkg,
# packages.adb for 25.12 apk). The release pipeline signs these indexes.
make package/index

printf 'built MiniTun %s from %s\n' "$package_version" "$source_tarball"
