#!/usr/bin/env bash
# Publish differential update blobs + manifest to the continuous GitHub release.
# Uploads only blobs that are not already present as release assets.
# Usage: packaging/publish-update-assets.sh <update-out-dir> <platform>
# Requires: gh, GH_TOKEN
set -euo pipefail

OUT="${1:?update out dir from make-update-manifest.sh}"
PLATFORM="${2:?platform}"
CHANNEL="${SEERR_UPDATE_CHANNEL:-continuous}"
MANIFEST="$OUT/update-manifest-${PLATFORM}.json"
BLOBS="$OUT/blobs"

if [[ ! -f "$MANIFEST" ]]; then
  echo "missing manifest: $MANIFEST" >&2
  exit 1
fi
if [[ ! -d "$BLOBS" ]]; then
  echo "missing blobs dir: $BLOBS" >&2
  exit 1
fi

if ! gh release view "$CHANNEL" >/dev/null 2>&1; then
  gh release create "$CHANNEL" --title "Continuous build" --notes "Rolling update channel" --latest=false
fi

EXISTING="$(mktemp)"
NEWLIST="$(mktemp)"
trap 'rm -f "$EXISTING" "$NEWLIST"' EXIT
gh release view "$CHANNEL" --json assets -q '.assets[].name' > "$EXISTING" || true

echo "Scanning blobs for $PLATFORM…"
UPLOADED=0
SKIPPED=0
shopt -s nullglob
for blob in "$BLOBS"/b_*; do
  name="$(basename "$blob")"
  if grep -Fxq "$name" "$EXISTING"; then
    SKIPPED=$((SKIPPED + 1))
    continue
  fi
  echo "$blob" >> "$NEWLIST"
  UPLOADED=$((UPLOADED + 1))
done
shopt -u nullglob

if [[ -s "$NEWLIST" ]]; then
  echo "Uploading $UPLOADED new blobs (batch)…"
  # gh accepts many files per invocation — chunk to stay under arg limits
  BATCH=40
  mapfile -t ALL < "$NEWLIST"
  for ((i = 0; i < ${#ALL[@]}; i += BATCH)); do
    chunk=("${ALL[@]:i:BATCH}")
    gh release upload "$CHANNEL" "${chunk[@]}" --clobber
    echo "  uploaded batch $((i / BATCH + 1))"
  done
else
  echo "No new blobs to upload (skipped=$SKIPPED)"
fi

gh release upload "$CHANNEL" "$MANIFEST" --clobber
echo "Published $MANIFEST (blobs uploaded=$UPLOADED skipped=$SKIPPED)"
