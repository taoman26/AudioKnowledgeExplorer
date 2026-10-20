#!/usr/bin/env bash
# Generates dummy audio (sine tones, no real recordings) in all supported formats.
# usage: tools/make_fixtures.sh <outdir> [seconds]
set -euo pipefail
out=${1:?usage: make_fixtures.sh <outdir> [seconds]}; sec=${2:-2}
mkdir -p "$out/meetings/2026-09" "$out/podcasts"
gen() { # <file> <freq> [extra ffmpeg args]
  f=$1; hz=$2; shift 2
  ffmpeg -v error -y -f lavfi -i "sine=frequency=$hz:duration=$sec" "$@" "$f"
}
gen "$out/meetings/2026-09/meeting.m4a"  440 -c:a aac
gen "$out/meetings/standup.mp3"          520 -c:a libmp3lame
gen "$out/podcasts/ep87.ogg"             600 -c:a libvorbis
gen "$out/podcasts/ep88.flac"            680 -c:a flac
gen "$out/voice_memo.wav"                760 -c:a pcm_s16le
# a meeting recorded as video: picture + sound (only the sound is used)
ffmpeg -v error -y -f lavfi -i "sine=frequency=840:duration=$sec" -f lavfi -i "color=c=blue:s=160x120:d=$sec" -c:a aac -c:v libx264 -pix_fmt yuv420p -shortest "$out/meetings/screen.mp4"
ffmpeg -v error -y -f lavfi -i "sine=frequency=920:duration=$sec" -f lavfi -i "color=c=red:s=160x120:d=$sec" -c:a libopus -c:v libvpx -shortest "$out/meetings/call.webm"
echo "notes" > "$out/readme.txt"           # must be ignored
