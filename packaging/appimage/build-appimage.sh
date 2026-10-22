#!/usr/bin/env bash
# Builds AudioKnowledgeExplorer-<version>-x86_64.AppImage in ./dist with Docker (Ubuntu 22.04 inside, so it runs on 22.04+).
#   packaging/appimage/build-appimage.sh
# Optional: AKE_WHISPER_DIR=/path/in/container (mount it yourself) to use a local whisper.cpp instead of fetching v1.7.6.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$ROOT/dist" "$ROOT/.appimage-work"
docker build -t ake-appimage-builder:22.04 "$HERE"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/work -v "$ROOT":/src:ro -v "$ROOT/dist":/out -v "$ROOT/.appimage-work":/work \
    -e AKE_VERSION="${AKE_VERSION:-}" ake-appimage-builder:22.04 bash /src/packaging/appimage/inner-build.sh
