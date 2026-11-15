#!/usr/bin/env bash
# Publishes what package.sh made (../pd-data/dist/release) as a release on the fork,
# which is where Update.ps1 in everyone's folder looks for a newer build.
#
# usage: tools/nettest/publish.sh [outdir]
#
# The release goes to the fork named below, never to upstream, and holds only the
# program: no ROM, no texture pack, no save. The commit must be pushed first (the
# release's tag points at it). It becomes the fork's "latest" release.

set -eu
REPO="JoshuaACurrier/perfect_dark"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PDDATA="$(cd "$ROOT/.." && pwd)/pd-data"
OUT="${1:-$PDDATA/dist}"
REL="$OUT/release"

[ -f "$REL/manifest.txt" ] || { echo "run package.sh first" >&2; exit 1; }
BUILD="$(awk '$1 == "build" {print $2}' "$REL/manifest.txt")"
HASH="$(awk '$1 == "commit" {print $2}' "$REL/manifest.txt")"
PROTOCOL="$(awk '$1 == "protocol" {print $2}' "$REL/manifest.txt")"
FULL="$(git -C "$ROOT" rev-parse "$HASH")"

# nothing that must stay private may be in there
if find "$REL" -iname '*.z64' -o -iname 'eeprom.bin' -o -iname 'pd.ini' -o -ipath '*texture*' | grep -q .; then
	echo "refusing: $REL holds a ROM, save, settings or textures" >&2
	exit 1
fi

if [ "$(git -C "$ROOT" rev-parse HEAD)" != "$FULL" ]; then
	echo "the packaged build ($HASH) is not HEAD: run package.sh again" >&2
	exit 1
fi
if ! git -C "$ROOT" branch -r --contains "$FULL" | grep -q "origin/"; then
	echo "commit $HASH is not on the fork yet: push it first" >&2
	exit 1
fi

NOTES="Perfect Dark Online build $BUILD ($HASH), network protocol $PROTOCOL.

First install: download PerfectDarkOnline.zip, unpack it, put your own ROM in data/, run Play.bat.
After that Play.bat fetches newer builds by itself.

$(awk '$1 == "note" { sub(/^note /, ""); print "- " $0 }' "$REL/manifest.txt")"

gh release create "online-$BUILD" -R "$REPO" --target "$FULL" --latest \
	--title "Perfect Dark Online build $BUILD" --notes "$NOTES" \
	"$REL/PerfectDarkOnline.zip" "$REL/manifest.txt" "$REL/pd.x86_64.exe" \
	"$REL/SDL2.dll" "$REL/libz-1.dll" "$REL/libgcc_s_seh-1.dll" "$REL/libwinpthread-1.dll" \
	"$REL/Update.ps1" "$REL/README.txt"

echo "published: https://github.com/$REPO/releases/tag/online-$BUILD"
