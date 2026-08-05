#!/bin/sh
set -eu

# Assemble and sign the MiniTun OpenWrt binary repository published on GitHub
# Pages. Input directories mirror the official OpenWrt layout:
#   <artifacts>/openwrt/<version>/<target>/<subtarget>/packages/
# 24.10 (opkg) indexes are signed as Packages.sig; 25.12 (apk v3) indexes as
# packages.adb.sig. Both signatures use the same usign Ed25519 key.

usage() {
	cat >&2 <<'EOF'
usage: build-repo.sh ARTIFACTS_DIR OUTPUT_DIR SIGN_KEY SIGN_PUB [USIGN]
EOF
}

[ "$#" -ge 4 ] && [ "$#" -le 5 ] || {
	usage
	exit 2
}

artifacts_dir=$(cd "$1" && pwd -P)
mkdir -p "$2"
output_dir=$(cd "$2" && pwd -P)
sign_key=$(cd "$(dirname "$3")" && pwd -P)/$(basename "$3")
sign_pub=$(cd "$(dirname "$4")" && pwd -P)/$(basename "$4")
usign=${5:-usign}
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

[ -f "$sign_key" ] && [ -f "$sign_pub" ] || {
	echo "signing key and public key must exist" >&2
	exit 2
}
command -v "$usign" >/dev/null 2>&1 || {
	echo "usign binary not found: $usign" >&2
	exit 2
}

key_id=$("$usign" -F -p "$sign_pub")
[ -n "$key_id" ] || {
	echo "unable to determine usign key id" >&2
	exit 2
}

indexes_signed=0
for packages_dir in \
	"$artifacts_dir"/openwrt/*/*/*/packages; do
	[ -d "$packages_dir" ] || continue
	relative=${packages_dir#"$artifacts_dir"/}
	version=$(printf '%s\n' "$relative" | awk -F/ '{ print $2 }')
	destination="$output_dir/$relative"
	install -d "$destination"
	cp -a "$packages_dir"/. "$destination"/

	case "$version" in
		24.10.*)
			[ -f "$destination/Packages" ] || {
				echo "missing opkg index: $relative/Packages" >&2
				exit 1
			}
			if [ ! -f "$destination/Packages.gz" ]; then
				gzip -c "$destination/Packages" >"$destination/Packages.gz"
			fi
			"$usign" -S -m "$destination/Packages" \
				-s "$sign_key" -x "$destination/Packages.sig"
			"$usign" -V -m "$destination/Packages" \
				-p "$sign_pub" >/dev/null || {
				echo "opkg index signature verification failed: $relative" >&2
				exit 1
			}
			install -m 0644 "$sign_pub" "$destination/minitun-build.pub"
			install -m 0644 "$sign_pub" "$destination/$key_id"
			;;
		25.12.*)
			[ -f "$destination/packages.adb" ] || {
				echo "missing apk index: $relative/packages.adb" >&2
				exit 1
			}
			"$usign" -S -m "$destination/packages.adb" \
				-s "$sign_key" -x "$destination/packages.adb.sig"
			"$usign" -V -m "$destination/packages.adb" \
				-p "$sign_pub" >/dev/null || {
				echo "apk index signature verification failed: $relative" >&2
				exit 1
			}
			install -m 0644 "$sign_pub" "$destination/minitun-build.pub"
			"$script_dir/usign-to-pem.py" \
				"$sign_pub" "$destination/minitun-build.pem"
			;;
		*)
			echo "unsupported OpenWrt version in repository path: $relative" >&2
			exit 1
			;;
	esac
	indexes_signed=$((indexes_signed + 1))
done

[ "$indexes_signed" -gt 0 ] || {
	echo "no OpenWrt package directories found under $artifacts_dir/openwrt" >&2
	exit 1
}

cat >"$output_dir/README.txt" <<EOF
MiniTun signed OpenWrt repository

Repository key id: $key_id
Public keys: minitun-build.pub (opkg), minitun-build.pem (apk)

24.10 (opkg) - install the key then add the feed, for example:
  wget -O /etc/opkg/keys/$key_id https://<host>/openwrt/24.10.8/<target>/<subtarget>/packages/$key_id
  echo 'src/gz minitun https://<host>/openwrt/24.10.8/<target>/<subtarget>/packages' >> /etc/opkg/customfeeds.conf
  opkg update && opkg install minitun-server

25.12 (apk) - install the key then add the repository, for example:
  wget -O /etc/apk/keys/minitun-build.pem https://<host>/openwrt/25.12.5/<target>/<subtarget>/packages/minitun-build.pem
  apk add --repository https://<host>/openwrt/25.12.5/<target>/<subtarget>/packages minitun-server
EOF

printf 'signed %d OpenWrt package indexes into %s (key id %s)\n' \
	"$indexes_signed" "$output_dir" "$key_id"
