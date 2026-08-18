#!/usr/bin/env bash
# Enforces Chinese/English documentation parity: every Markdown change under
# docs/ must also change its mirror under docs/en/ (and vice versa), so the two
# languages cannot silently drift apart. Build artifacts (docs/public,
# docs/.vitepress) and non-Markdown files are ignored.
set -euo pipefail

base_ref=${1:-}
if [[ -z "$base_ref" || "$base_ref" == "null" ]]; then
    base_ref="HEAD^"
fi

changed=()
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    changed+=("$line")
done < <(git diff --name-only "${base_ref}...HEAD" -- 'docs/*.md' 2>/dev/null || true)

if ((${#changed[@]} == 0)); then
    printf 'Documentation parity check passed (no Markdown changes).\n'
    exit 0
fi

failures=0
declare -a problems=()

is_excluded() {
    local path=$1
    case "$path" in
    docs/public/* | docs/.vitepress/*) return 0 ;;
    *) return 1 ;;
    esac
}

counterpart() {
    local path=$1
    if [[ "$path" == docs/en/* ]]; then
        printf 'docs/%s\n' "${path#docs/en/}"
    else
        printf 'docs/en/%s\n' "${path#docs/}"
    fi
}

for path in "${changed[@]}"; do
    [[ -z "$path" ]] && continue
    [[ "$path" == *.md ]] || continue
    is_excluded "$path" && continue
    mirror=$(counterpart "$path")
    synced=false
    for candidate in "${changed[@]}"; do
        if [[ "$candidate" == "$mirror" ]]; then
            synced=true
            break
        fi
    done
    if [[ "$synced" != true ]]; then
        if git cat-file -e "HEAD:$mirror" 2>/dev/null; then
            problems+=("$path changed but $mirror was not changed in the same commit")
        else
            problems+=("$path changed but $mirror does not exist")
        fi
        failures=$((failures + 1))
    fi
done

if ((failures > 0)); then
    printf 'Documentation drift detected: %d file(s) changed without their mirror.\n' "$failures" >&2
    printf '%s\n' "${problems[@]}" >&2
    printf 'Update docs/en/<path> (or docs/<path>) in the same commit to keep the languages in sync.\n' >&2
    exit 1
fi

printf 'Documentation parity check passed (%d Markdown change(s) considered).\n' "${#changed[@]}"
