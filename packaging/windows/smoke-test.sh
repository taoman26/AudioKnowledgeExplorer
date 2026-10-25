#!/usr/bin/env bash
# Unpacks the zip into a clean folder and starts the programs from there, with PATH reduced to the Windows basics, so that a
# DLL missing from the zip (and found only in MSYS2) shows up: packaging/windows/smoke-test.sh <dist dir>
set -euo pipefail
DIST=$(cd "$1" && pwd)
T=$(mktemp -d)
cd "$T" && unzip -q "$DIST"/*.zip && cd AudioKnowledgeExplorer-*
export PATH=/c/Windows/System32:/c/Windows
export APPDATA="$T/appdata" LOCALAPPDATA="$T/localappdata"
./ake_cli.exe --help | head -3
./ake_cli.exe --db "$T/x.db" seed-demo
./ake_cli.exe --db "$T/x.db" list | head -3
QT_QPA_PLATFORM=offscreen ./AudioKnowledgeExplorer.exe --db "$T/x.db" --grab "$T/shot.png"
test -s "$T/shot.png" && echo "screenshot ok: $(stat -c %s "$T/shot.png") bytes"
