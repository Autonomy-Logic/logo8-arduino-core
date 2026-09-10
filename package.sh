#!/usr/bin/env bash
# Package this core for the Arduino board manager and update the index to match.
#
# The index and the archive have to agree on version, filename, size and
# SHA-256, and arduino-cli refuses the download if they don't. Doing that by
# hand is how the published index ended up pointing at a file that did not
# exist, so it lives here instead.
#
#   ./package.sh 0.2.0
#
# Then create the release the index now points at:
#   gh release create v0.2.0 autonomylogic-tm4c-<ver>.tar.bz2 --title ... --notes ...
# and commit the updated index.
set -euo pipefail
VER="${1:?usage: package.sh <version>   e.g. ./package.sh 0.2.0}"
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

ARCHIVE="autonomylogic-tm4c-${VER}.tar.bz2"
ROOT="tm4c-${VER}"          # arduino-cli requires a single top-level directory
REPO="Autonomy-Logic/logo8-arduino-core"
URL="https://github.com/${REPO}/releases/download/v${VER}/${ARCHIVE}"

# What the platform actually consists of. Everything else in the repo (README,
# LICENSE, the index itself, this script) is repository furniture, not core.
CONTENT=(boards.txt platform.txt programmers.txt cores libraries system tools variants)

echo "==> staging ${ROOT}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/$ROOT"
for c in "${CONTENT[@]}"; do cp -R "$c" "$tmp/$ROOT/"; done
# macOS resource forks and Finder droppings must not reach the archive.
export COPYFILE_DISABLE=1
find "$tmp" \( -name '._*' -o -name '.DS_Store' \) -delete

echo "==> building ${ARCHIVE}"
rm -f "$ARCHIVE"
tar -C "$tmp" -cjf "$ARCHIVE" "$ROOT"

SIZE=$(stat -f%z "$ARCHIVE" 2>/dev/null || stat -c%s "$ARCHIVE")
SHA=$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)
echo "    size     = $SIZE"
echo "    sha256   = $SHA"
echo "    url      = $URL"

echo "==> updating package_autonomylogic_tm4c_index.json"
python3 - "$VER" "$ARCHIVE" "$URL" "$SHA" "$SIZE" <<'PY'
import json, sys
ver, archive, url, sha, size = sys.argv[1:6]
p = "package_autonomylogic_tm4c_index.json"
with open(p) as f: idx = json.load(f)
pkg = next(x for x in idx["packages"] if x["name"] == "autonomylogic")
plat = pkg["platforms"][0]
plat["version"]         = ver
plat["url"]             = url
plat["archiveFileName"] = archive
plat["checksum"]        = "SHA-256:" + sha
plat["size"]            = str(size)
with open(p, "w") as f:
    json.dump(idx, f, indent=2)
    f.write("\n")
print("    index updated to", ver)
PY

echo
echo "==> next:"
echo "    gh release create v${VER} ${ARCHIVE} -R ${REPO} --title 'v${VER}' --notes '...'"
echo "    git add package_autonomylogic_tm4c_index.json && git commit && git push"
