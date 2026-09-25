#!/usr/bin/env bash
# Build an update manifest + content-addressed blobs for differential updates.
# Usage: packaging/make-update-manifest.sh <install-dir> <platform> <version> <out-dir>
# Example: packaging/make-update-manifest.sh build/seerr-portable windows-x64 0.1.0+abc build/update-windows
set -euo pipefail

ROOT_DIR="${1:?install / portable directory}"
PLATFORM="${2:?platform id e.g. windows-x64}"
VER="${3:?version string}"
OUT="${4:?output directory}"
REPO="${SEERR_UPDATE_REPO:-hdmain/tezeusz}"
CHANNEL="${SEERR_UPDATE_CHANNEL:-continuous}"
BASE_URL="${SEERR_UPDATE_BASE_URL:-https://github.com/${REPO}/releases/download/${CHANNEL}/}"

if [[ ! -d "$ROOT_DIR" ]]; then
  echo "missing dir: $ROOT_DIR" >&2
  exit 1
fi

sha_file() {
  local f="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$f" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$f" | awk '{print $1}'
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$f" | awk '{print $NF}'
  else
    echo "need sha256sum, shasum, or openssl" >&2
    exit 1
  fi
}

rm -rf "$OUT"
mkdir -p "$OUT/blobs"

MANIFEST="$OUT/update-manifest-${PLATFORM}.json"
TMP_FILES="$(mktemp)"
trap 'rm -f "$TMP_FILES"' EXIT

# Collect relative paths (posix, forward slashes). Skip helper/docs.
(
  cd "$ROOT_DIR"
  find . -type f \
    ! -name 'README-PORTABLE.txt' \
    ! -name 'check-deps.bat' \
    ! -name '.seerr-write-test' \
    ! -name '*.tmp' \
    | sed 's|^\./||' | sort
) > "$TMP_FILES"

FILE_COUNT=0
BLOB_COUNT=0
declare -A SEEN_BLOB=()

echo "{" > "$MANIFEST"
echo "  \"version\": \"${VER}\"," >> "$MANIFEST"
echo "  \"platform\": \"${PLATFORM}\"," >> "$MANIFEST"
echo "  \"channel\": \"${CHANNEL}\"," >> "$MANIFEST"
echo "  \"baseUrl\": \"${BASE_URL}\"," >> "$MANIFEST"
echo "  \"files\": [" >> "$MANIFEST"

first=1
while IFS= read -r rel || [[ -n "$rel" ]]; do
  [[ -z "$rel" ]] && continue
  src="$ROOT_DIR/$rel"
  [[ -f "$src" ]] || continue
  # Skip VLC locale noise? Keep plugins — needed for playback.
  sha="$(sha_file "$src")"
  size="$(wc -c < "$src" | tr -d ' ')"
  # JSON-escape path
  path_json="${rel//\\/\\\\}"
  path_json="${path_json//\"/\\\"}"

  if [[ -z "${SEEN_BLOB[$sha]+x}" ]]; then
    SEEN_BLOB[$sha]=1
    cp -f "$src" "$OUT/blobs/b_${sha}"
    BLOB_COUNT=$((BLOB_COUNT + 1))
  fi

  if [[ $first -eq 1 ]]; then
    first=0
  else
    echo "," >> "$MANIFEST"
  fi
  printf '    {"path": "%s", "sha256": "%s", "size": %s}' "$path_json" "$sha" "$size" >> "$MANIFEST"
  FILE_COUNT=$((FILE_COUNT + 1))
done < "$TMP_FILES"

echo "" >> "$MANIFEST"
echo "  ]" >> "$MANIFEST"
echo "}" >> "$MANIFEST"

# List of blob names for CI upload filtering
ls -1 "$OUT/blobs" > "$OUT/blob-names.txt" || true

echo "Update manifest ready: $MANIFEST"
echo "  files: $FILE_COUNT"
echo "  unique blobs: $BLOB_COUNT"
echo "  platform: $PLATFORM"
echo "  version: $VER"
