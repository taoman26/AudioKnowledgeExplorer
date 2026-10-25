#!/usr/bin/env bash
# Packs the portable Windows build (run in an MSYS2 UCRT64 shell): packaging/windows/make-zip.sh <build dir> <out dir>
# The zip holds AudioKnowledgeExplorer.exe, ake_cli.exe, the Qt plugins and every DLL they need (windeployqt + ldd).
set -euo pipefail
BUILD=$(cd "$1" && pwd); mkdir -p "$2"; OUT=$(cd "$2" && pwd)
VERSION=$(git describe --tags --always 2>/dev/null | sed 's/^v//') ; VERSION=${VERSION:-dev}
NAME=AudioKnowledgeExplorer-$VERSION-windows-x64
STAGE=$OUT/$NAME
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp "$BUILD/AudioKnowledgeExplorer.exe" "$BUILD/ake_cli.exe" "$STAGE/"
cp LICENSE "$STAGE/"
windeployqt6 --no-translations --no-system-d3d-compiler --no-opengl-sw "$STAGE/AudioKnowledgeExplorer.exe"
# the other SQL drivers (MySQL, PostgreSQL, Firebird, ODBC) need server libraries nobody has here: only SQLite is used
find "$STAGE/sqldrivers" -name '*.dll' ! -name 'qsqlite.dll' -delete
# FFmpeg, SQLite, the C++ runtime ...: whatever the programs and the plugins still need from /ucrt64/bin
for i in 1 2 3; do                          # a copied DLL may need more DLLs
    find "$STAGE" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 ldd 2>/dev/null \
        | awk '/\/ucrt64\/bin\// {print $3}' | sort -u | while read -r dll; do
            [ -f "$STAGE/$(basename "$dll")" ] || cp "$dll" "$STAGE/"
        done
done
# MeCab dictionary (built by build-dictionary.sh): the program looks for <exe dir>/mecab/jumandic
if [ -n "${AKE_MECAB_DICT:-}" ]; then
    mkdir -p "$STAGE/mecab/jumandic" && cp "$AKE_MECAB_DICT"/* "$STAGE/mecab/jumandic/"
    cat > "$STAGE/THIRD-PARTY.txt" <<TXT
MeCab (libmecab.dll): Taku Kudo / NTT, BSD / LGPL / GPL (any one of them); https://taku910.github.io/mecab/
JUMAN dictionary for MeCab (mecab/jumandic): University of Tokyo, Taku Kudo; BSD-3-clause, see mecab/jumandic/COPYRIGHT-jumandic.txt
Qt 6 (LGPL v3, the DLLs next to the program can be replaced), FFmpeg (GPL / LGPL build of MSYS2), whisper.cpp (MIT).
TXT
fi
cat > "$STAGE/README.txt" <<TXT
AudioKnowledgeExplorer $VERSION (Windows, portable)
Unpack the folder anywhere and start AudioKnowledgeExplorer.exe. Nothing is installed.
The whisper model is downloaded on the first start (the settings dialog lets you choose another one).
Your library and models are kept in %APPDATA%\\AudioKnowledgeExplorer.
TXT
(cd "$OUT" && rm -f "$NAME.zip" && powershell.exe -NoProfile -Command "Compress-Archive -Path '$NAME' -DestinationPath '$NAME.zip'" 2>/dev/null || zip -qr "$NAME.zip" "$NAME")
ls -la "$OUT"
