#!/usr/bin/env bash
# Builds the MeCab JUMAN dictionary for the Windows zip (run in an MSYS2 UCRT64 shell with mingw-w64-ucrt-x86_64-mecab):
#   packaging/windows/build-dictionary.sh <out dir>
# The Ubuntu package mecab-jumandic-utf8 holds the dictionary as source (CSV); Debian compiles it when the package is installed,
# so the same is done here with mecab-dict-index. Output: sys.dic matrix.bin char.bin unk.dic dicrc, an empty mecabrc (MeCab on
# Windows would otherwise look for one in the registry) and the copyright notice (BSD-3-clause: it must travel with the files).
set -euo pipefail
mkdir -p "$1"; OUT=$(cd "$1" && pwd)
URL=https://archive.ubuntu.com/ubuntu/pool/universe/m/mecab-jumandic/mecab-jumandic-utf8_7.0-20130310-7_all.deb
SHA256=9fabacabaf88e9cd0a1013a47fa56dcef1e8ccd49c4da25262f1fa31d8d52e13
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
curl -fsSL -o "$W/d.deb" "$URL"
echo "$SHA256  $W/d.deb" | sha256sum -c -
(cd "$W" && ar x d.deb && ls -la && tar -xf data.tar.* && echo "extracted") || { echo "extract failed: $?"; ls -la "$W"; exit 1; }
SRC=$W/usr/share/mecab/dic/juman
MDI=$(command -v mecab-dict-index || ls /ucrt64/libexec/mecab/mecab-dict-index* /ucrt64/lib/mecab/mecab-dict-index* 2>/dev/null | head -1)
echo "mecab-dict-index: $MDI"
"$MDI" -d "$SRC" -o "$OUT" -f utf-8 -t utf-8
cp "$SRC/dicrc" "$OUT/"
: > "$OUT/mecabrc"
cp "$W/usr/share/doc/mecab-jumandic-utf8/copyright" "$OUT/COPYRIGHT-jumandic.txt"
ls -la "$OUT"
