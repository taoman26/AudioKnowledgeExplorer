#!/usr/bin/env bash
# Unpacks the zip into a clean folder and starts the programs from there, with PATH reduced to the Windows basics, so that a
# DLL missing from the zip (and found only in MSYS2) shows up: packaging/windows/smoke-test.sh <dist dir>
set -euo pipefail
DIST=$(cd "$1" && pwd)
T=$(mktemp -d)
cd "$T" && unzip -q "$DIST"/*.zip && cd AudioKnowledgeExplorer-*
export PATH=/usr/bin:/c/Windows/System32:/c/Windows   # no /ucrt64/bin: only what is in the zip
export APPDATA="$T/appdata" LOCALAPPDATA="$T/localappdata"
echo "DLLs the programs and plugins cannot find with this PATH (must be none):"
find . \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 ldd 2>/dev/null | grep "not found" | sort -u || true
./ake_cli.exe --help | head -3
./ake_cli.exe --db "$T/x.db" seed-demo
./ake_cli.exe --db "$T/x.db" list | head -3
# a window application has no console: its messages go to a file; a hang must not stall the job
timeout 40 ./AudioKnowledgeExplorer.exe --db "$T/x.db" --grab "$T/shot.png" > "$T/app.log" 2>&1 || { echo "app exit code: $?"; cat "$T/app.log"; exit 1; }
cat "$T/app.log"
test -s "$T/shot.png" && echo "screenshot ok: $(stat -c %s "$T/shot.png") bytes"
