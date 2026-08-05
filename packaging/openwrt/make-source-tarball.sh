#!/bin/sh
set -eu

# Generate the deterministic MiniTun source tarball consumed by the OpenWrt
# feed. `git archive` writes a pax_global_header containing the commit id,
# which would make the SHA-256 depend on the commit hash; this script strips
# that entry and gzip-compresses without a timestamp (gzip -n), so the output
# is a pure function of the archived tree and the commit date. The OpenWrt
# feed Makefile is excluded so PKG_HASH is not self-referential.

usage() {
	cat >&2 <<'EOF'
usage: make-source-tarball.sh SOURCE_DIR VERSION OUTPUT
EOF
}

[ "$#" -eq 3 ] || {
	usage
	exit 2
}

source_dir=$(cd "$1" && pwd -P)
version=$2
output=$3
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

git -C "$source_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
	echo "SOURCE_DIR must be a git work tree" >&2
	exit 2
}

tmp_tar=$(mktemp)
trap 'rm -f "$tmp_tar"' EXIT HUP INT TERM

git -C "$source_dir" archive \
	--format=tar \
	--prefix="minitun-$version/" \
	-o "$tmp_tar" \
	HEAD -- . ':(exclude)packaging/openwrt/Makefile'

"$script_dir/strip-tar-pax.py" "$tmp_tar" "${tmp_tar}.stripped"
gzip -n -c "${tmp_tar}.stripped" >"$output"
