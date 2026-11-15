#!/usr/bin/env bash
# Packages the current build into a zip a friend can unpack and run:
# the exe, the MinGW DLLs it needs, and an empty data/ folder for their own ROM.
# The ROM is never included.
#
# usage: tools/nettest/package.sh [--textures] [outdir]   (default: ../pd-data/dist)
#
# --textures also makes texture-packs.zip: a smaller copy of the HD texture pack in
# ../pd-data/texture-packs (see tools/texpack/packshrink.c). It only needs making
# again when the pack changes. It is unpacked to <outdir>/texture-packs too, where
# every build in <outdir> finds it. Not for public upload: the pack's textures
# belong to their authors and the game's owners.

set -eu
TEXTURES=0
if [ "${1:-}" = "--textures" ]; then
	TEXTURES=1
	shift
fi
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PDDATA="$(cd "$ROOT/.." && pwd)/pd-data"
OUT="${1:-$PDDATA/dist}"
HASH="$(git -C "$ROOT" rev-parse --short HEAD)"
NAME="PerfectDarkOnline-$HASH"
STAGE="$OUT/$NAME"
MINGW=/c/msys64/mingw64/bin

# reconfigure so the version hash baked into the exe matches HEAD, then build
/c/msys64/usr/bin/bash.exe -lc "export MSYSTEM=MINGW64 PATH=/mingw64/bin:\$PATH; cd '$ROOT' && cmake -G'Unix Makefiles' -Bbuild . > /dev/null && cmake --build build -j12 -- -O > /dev/null"

# (a texture-packs link someone made in an old copy must not take its target with it)
if [ -e "$STAGE/texture-packs" ]; then
	cmd //c rmdir "$(cygpath -w "$STAGE/texture-packs")" 2> /dev/null || true
fi
if [ -e "$STAGE/texture-packs" ]; then
	echo "remove $STAGE/texture-packs by hand first" >&2
	exit 1
fi
rm -rf "$STAGE"
mkdir -p "$STAGE/data"
cp "$ROOT/build/pd.x86_64.exe" "$STAGE/"
for dll in SDL2.dll libz-1.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
	cp "$MINGW/$dll" "$STAGE/"
done

cat > "$STAGE/data/PUT-YOUR-ROM-HERE.txt" <<EOF
Put your own Perfect Dark ROM in this folder, named exactly:

    pd.ntsc-final.z64

It must be the US version, revision 1.1 ("NTSC Final"). Both players need the
same build of this game ($HASH) and the same ROM version.
EOF

printf '@echo off\r\ncd /d "%%~dp0"\r\nstart "" pd.x86_64.exe --log %%*\r\n' > "$STAGE/Play.bat"

cat > "$STAGE/README.txt" <<EOF
Perfect Dark Online test build $HASH

1. Put your ROM in data/ (see data/PUT-YOUR-ROM-HERE.txt).
2. Run Play.bat (it starts pd.x86_64.exe and writes pd.log next to it).
   If something goes wrong, send Josh pd.log (and pd.log.prev, the run before).
3. Pick or create an agent, then choose Network Game from the menu.
   Host Game to host, Join Game to join (type the host's address).
Alt+Enter toggles fullscreen. ~ opens the console (chat), F9 shows network stats.

Optional HD textures: get texture-packs.zip from Josh, put it next to this
folder, right-click it and choose Extract All. You should end up with a folder
called texture-packs next to this folder (inside it works too):

    PerfectDarkOnline-$HASH/pd.x86_64.exe
    texture-packs/PD Forever Plus HD v0.10/0003.jpg

Next to this folder is best: the next build finds it there too. To check it
worked: Options > Extended > Video > HD Texture Pack is ticked and not greyed out.
The switches for Ambient Occlusion and Bloom are in the same menu.
(The original pack, "PD Forever Plus HD", is at
https://github.com/retro-foundry/Perfect-Dark-Plus-HD-Textures/releases)
EOF

rm -f "$OUT/$NAME.zip"
powershell -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$STAGE")\\*' -DestinationPath '$(cygpath -w "$OUT/$NAME.zip")'"
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
