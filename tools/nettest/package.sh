#!/usr/bin/env bash
# Packages the current build into a zip a friend can unpack and run:
# the exe, the MinGW DLLs it needs, and an empty data/ folder for their own ROM.
# The ROM is never included.
#
# usage: tools/nettest/package.sh [outdir]   (default: ../pd-data/dist)

set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$(cd "$ROOT/.." && pwd)/pd-data/dist}"
HASH="$(git -C "$ROOT" rev-parse --short HEAD)"
NAME="PerfectDarkOnline-$HASH"
STAGE="$OUT/$NAME"
MINGW=/c/msys64/mingw64/bin

# reconfigure so the version hash baked into the exe matches HEAD, then build
/c/msys64/usr/bin/bash.exe -lc "export MSYSTEM=MINGW64 PATH=/mingw64/bin:\$PATH; cd '$ROOT' && cmake -G'Unix Makefiles' -Bbuild . > /dev/null && cmake --build build -j12 -- -O > /dev/null"

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
EOF

rm -f "$OUT/$NAME.zip"
powershell -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$STAGE")\\*' -DestinationPath '$(cygpath -w "$OUT/$NAME.zip")'"
echo "$OUT/$NAME.zip"
