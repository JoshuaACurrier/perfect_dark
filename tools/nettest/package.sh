#!/usr/bin/env bash
# Packages the current build for playing and for sharing.
#
# usage: tools/nettest/package.sh [--textures] [-n "what changed"]... [outdir]
#        (outdir defaults to ../pd-data/dist)
#
# It makes, in outdir:
#
#   PerfectDarkOnline/        the folder to play from. Program files are replaced in
#                             place; a ROM, save, settings and logs already in it stay.
#   PerfectDarkOnline.zip     the same program files, for someone's first install
#                             (no ROM, save or settings in it).
#   release/                  what publish.sh uploads: the zip, every program file on
#                             its own, and manifest.txt (build number, commit, and each
#                             file's SHA-256 and size). Update.ps1 in the folder reads
#                             the published manifest and fetches only what changed.
#
# The build number is the number of commits on HEAD, so it only goes up.
# -n adds a line the updater shows people ("note ..." in the manifest); repeatable.
#
# --textures also makes texture-packs.zip: a smaller copy of the HD texture pack in
# ../pd-data/texture-packs (see tools/texpack/packshrink.c). It only needs making
# again when the pack changes. It is unpacked to <outdir>/texture-packs too, where
# the game finds it. Not for public upload: the pack's textures belong to their
# authors and the game's owners. The ROM is never included in anything.

set -eu
TEXTURES=0
NOTES=()
while [ $# -gt 0 ]; do
	case "$1" in
		--textures) TEXTURES=1; shift ;;
		-n) NOTES+=("$2"); shift 2 ;;
		*) break ;;
	esac
done

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PDDATA="$(cd "$ROOT/.." && pwd)/pd-data"
OUT="${1:-$PDDATA/dist}"
HASH="$(git -C "$ROOT" rev-parse --short HEAD)"
BUILD="$(git -C "$ROOT" rev-list --count HEAD)"
PROTOCOL="$(grep -E '^#define NET_PROTOCOL_VER' "$ROOT/port/include/net/net.h" | awk '{print $3}')"
NAME="PerfectDarkOnline"
PLAY="$OUT/$NAME"
REL="$OUT/release"
MINGW=/c/msys64/mingw64/bin

# reconfigure so the version hash baked into the exe matches HEAD, then build
/c/msys64/usr/bin/bash.exe -lc "export MSYSTEM=MINGW64 PATH=/mingw64/bin:\$PATH; cd '$ROOT' && cmake -G'Unix Makefiles' -Bbuild . > /dev/null && cmake --build build -j12 -- -O > /dev/null"

# the program files, each also a release asset the updater can fetch on its own
rm -rf "$REL"
mkdir -p "$REL"
cp "$ROOT/build/pd.x86_64.exe" "$REL/"
# The shipped exe has its debug information stripped (about a third of the size to
# download). The full one is kept in ../pd-data/symbols: a crash in pd.log gives
# addresses, and  addr2line -f -i -e symbols/pd-<commit>.exe 0x140000000+<offset>
# turns them into function names (stripping does not move any code).
mkdir -p "$PDDATA/symbols"
cp "$ROOT/build/pd.x86_64.exe" "$PDDATA/symbols/pd-$HASH.exe"
"$MINGW/strip.exe" "$REL/pd.x86_64.exe"
for dll in SDL2.dll libz-1.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
	cp "$MINGW/$dll" "$REL/"
done
cp "$ROOT/tools/nettest/Update.ps1" "$REL/"

cat > "$REL/README.txt" <<EOF
Perfect Dark Online

1. Put your ROM in data/ (see data/PUT-YOUR-ROM-HERE.txt).
2. Run Play.bat. It first checks for a newer build and downloads what changed
   (a few seconds; your ROM, save and settings are never touched), then starts
   the game. The game writes pd.log next to it: if something goes wrong, send
   Josh pd.log (and pd.log.prev, the run before).
3. Pick or create an agent, then choose Network Game from the menu.
   Host Game to host, Join Game to join (type the host's address).

Keep this folder: you only need to download it once. Everyone in a game needs
the same build, which is what the check in Play.bat is for. version.txt says
which build you have. To stop the checks, put an empty file called noupdate.txt
in this folder.

Alt+Enter toggles fullscreen. T types a chat line, hold Tab for the scoreboard,
~ opens the console, F9 shows network stats.

Optional HD textures: get texture-packs.zip from Josh, put it next to this
folder, right-click it and choose Extract All. You should end up with a folder
called texture-packs next to this folder (inside it works too):

    PerfectDarkOnline/pd.x86_64.exe
    texture-packs/PD Forever Plus HD v0.10/0003.jpg

To check it worked: Options > Extended > Video > HD Texture Pack is ticked and
not greyed out. The switches for Ambient Occlusion and Bloom are in the same menu.
(The original pack, "PD Forever Plus HD", is at
https://github.com/retro-foundry/Perfect-Dark-Plus-HD-Textures/releases)
EOF

# manifest: what the updater compares against
{
	echo "build $BUILD"
	echo "commit $HASH"
	echo "protocol $PROTOCOL"
	for n in "${NOTES[@]+"${NOTES[@]}"}"; do
		echo "note $n"
	done
	for f in pd.x86_64.exe SDL2.dll libz-1.dll libgcc_s_seh-1.dll libwinpthread-1.dll Update.ps1 README.txt; do
		echo "file $f $(sha256sum "$REL/$f" | cut -d' ' -f1) $(stat -c %s "$REL/$f")"
	done
} > "$REL/manifest.txt"

# the folder for a first install: program files plus the things the updater never replaces
STAGE="$OUT/.stage/$NAME"
rm -rf "$OUT/.stage"
mkdir -p "$STAGE/data"
for f in pd.x86_64.exe SDL2.dll libz-1.dll libgcc_s_seh-1.dll libwinpthread-1.dll Update.ps1 README.txt; do
	cp "$REL/$f" "$STAGE/"
done
printf 'build %s\r\ncommit %s\r\n' "$BUILD" "$HASH" > "$STAGE/version.txt"

cat > "$STAGE/data/PUT-YOUR-ROM-HERE.txt" <<EOF
Put your own Perfect Dark ROM in this folder, named exactly:

    pd.ntsc-final.z64

It must be the US version, revision 1.1 ("NTSC Final"). Everyone in a game needs
the same ROM version.
EOF

# (Play.bat is never replaced by the updater: it is the script that is running it)
printf '@echo off\r\ncd /d "%%~dp0"\r\nif not exist noupdate.txt powershell -NoProfile -ExecutionPolicy Bypass -File Update.ps1\r\nstart "" pd.x86_64.exe --log %%*\r\n' > "$STAGE/Play.bat"

rm -f "$REL/$NAME.zip" "$OUT/$NAME.zip"
powershell -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$STAGE")' -DestinationPath '$(cygpath -w "$REL/$NAME.zip")'"
cp "$REL/$NAME.zip" "$OUT/$NAME.zip"

# the folder to play from: replace the program files, leave everything else alone
mkdir -p "$PLAY/data"
for f in pd.x86_64.exe SDL2.dll libz-1.dll libgcc_s_seh-1.dll libwinpthread-1.dll Update.ps1 README.txt version.txt Play.bat; do
	cp "$STAGE/$f" "$PLAY/"
done
[ -e "$PLAY/data/pd.ntsc-final.z64" ] || cp "$STAGE/data/PUT-YOUR-ROM-HERE.txt" "$PLAY/data/"
rm -rf "$OUT/.stage"

echo "build $BUILD ($HASH), protocol $PROTOCOL"
echo "$PLAY"
echo "$OUT/$NAME.zip"

if [ "$TEXTURES" = 1 ]; then
	SRC="$(find "$PDDATA/texture-packs" -mindepth 1 -maxdepth 1 -type d | sort | head -1)"
	if [ -z "$SRC" ]; then
		echo "no pack in $PDDATA/texture-packs" >&2
		exit 1
	fi
	TOOL="$ROOT/build/packshrink.exe"
	PATH="$MINGW:$PATH" gcc -O2 -o "$TOOL" "$ROOT/tools/texpack/packshrink.c" \
		-I"$ROOT/port/include/external" -I"$ROOT/tools/texpack" -lm
	rm -rf "$OUT/texture-packs"
	mkdir -p "$OUT/texture-packs"
	PATH="$MINGW:$PATH" "$TOOL" "$SRC" "$OUT/texture-packs/$(basename "$SRC")" 95
	rm -f "$OUT/texture-packs.zip"
	# (the images are compressed already: store them, don't squeeze them again)
	powershell -NoProfile -Command "Compress-Archive -CompressionLevel NoCompression -Path '$(cygpath -w "$OUT/texture-packs")\\*' -DestinationPath '$(cygpath -w "$OUT/texture-packs.zip")'"
	echo "$OUT/texture-packs.zip"
fi
