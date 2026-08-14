#!/usr/bin/env bash
set -euo pipefail

source_sha=${1:?usage: download_release_evidence.sh SOURCE_SHA KIND DESTINATION}
kind=${2:?usage: download_release_evidence.sh SOURCE_SHA KIND DESTINATION}
destination=${3:?usage: download_release_evidence.sh SOURCE_SHA KIND DESTINATION}

if [[ ! "$source_sha" =~ ^[0-9a-f]{40}$ ]]; then
    printf 'source SHA must be a lowercase 40-character object ID\n' >&2
    exit 2
fi
: "${GH_REPO:?GH_REPO must identify the owner/repository}"
: "${GH_TOKEN:?GH_TOKEN is required to read workflow evidence}"

case "$kind" in
    performance)
        prefix="performance-release-gate-$source_sha-"
        expected_file=gate-summary.json
        ;;
    full-24h)
        prefix="full-24h-release-gate-$source_sha-"
        expected_file=full-24h.json
        ;;
    mixed-7d)
        prefix="mixed-7d-release-gate-$source_sha-"
        expected_file=mixed-7d.json
        ;;
    *)
        printf 'unknown evidence kind: %s\n' "$kind" >&2
        exit 2
        ;;
esac

[[ ! -e "$destination" ]] || {
    printf 'evidence destination already exists: %s\n' "$destination" >&2
    exit 2
}

destination_parent=$(dirname -- "$destination")
destination_base=$(basename -- "$destination")
install -d -m 0755 "$destination_parent"
active_temporary=""
discard_temporary() {
    if [[ -n "$active_temporary" && -d "$active_temporary" ]]; then
        rm -rf -- "$active_temporary"
    fi
    active_temporary=""
}
trap discard_temporary EXIT

mapfile -t run_ids < <(
    gh run list \
        --repo "$GH_REPO" \
        --workflow performance.yml \
        --commit "$source_sha" \
        --event workflow_dispatch \
        --status success \
        --limit 100 \
        --json databaseId,headSha,createdAt \
        --jq "sort_by(.createdAt) | reverse | .[] | select(.headSha == \"$source_sha\") | .databaseId"
)

for run_id in "${run_ids[@]}"; do
    artifacts=$(gh api "repos/$GH_REPO/actions/runs/$run_id/artifacts?per_page=100")
    artifact_name=$(jq -r --arg prefix "$prefix" '
        [.artifacts[] | select(.expired == false and (.name | startswith($prefix)))]
        | sort_by(.created_at) | reverse | .[0].name // empty
    ' <<<"$artifacts")
    [[ -n "$artifact_name" ]] || continue

    active_temporary=$(mktemp -d \
        "$destination_parent/.${destination_base}.download.XXXXXX")
    if ! gh run download "$run_id" --repo "$GH_REPO" \
            --name "$artifact_name" --dir "$active_temporary"; then
        discard_temporary
        continue
    fi
    if [[ ! -f "$active_temporary/$expected_file" ]]; then
        discard_temporary
        continue
    fi
    artifact_digest=$(jq -r --arg name "$artifact_name" '
        .artifacts[] | select(.name == $name) | .digest // ""
    ' <<<"$artifacts" | head -n 1)
    python3 - "$active_temporary/download-receipt.json" "$kind" "$source_sha" \
        "$run_id" "$artifact_name" "$artifact_digest" <<'PY'
import json
import os
import sys

destination = sys.argv[1]
document = {
    "evidence_format": 1,
    "kind": sys.argv[2],
    "source_commit": sys.argv[3],
    "workflow_run_id": int(sys.argv[4]),
    "artifact_name": sys.argv[5],
    "artifact_digest": sys.argv[6],
}
temporary = destination + ".tmp"
with open(temporary, "w", encoding="utf-8") as stream:
    json.dump(document, stream, indent=2, sort_keys=True)
    stream.write("\n")
os.replace(temporary, destination)
PY
    mv -- "$active_temporary" "$destination"
    active_temporary=""
    printf 'downloaded %s evidence from workflow run %s\n' "$kind" "$run_id"
    exit 0
done

printf 'no unexpired successful %s evidence exists for commit %s\n' \
    "$kind" "$source_sha" >&2
exit 1
