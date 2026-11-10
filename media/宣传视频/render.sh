#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
: "${FILM_PYTHON:=python3}"
: "${FFMPEG:=ffmpeg}"
export FFMPEG
"$FILM_PYTHON" soundtrack.py
node film.cjs "$@"
if [ "$#" -eq 0 ]; then
  "$FFMPEG" -y -hide_banner -loglevel warning \
    -i 'output/ModernIME-宣传片-4K-60fps.mp4' \
    -vf 'scale=1920:1080:flags=lanczos' -c:v libx264 -preset medium -crf 18 \
    -threads 3 -c:a copy -movflags +faststart 'output/ModernIME-宣传片-1080p-60fps.mp4'
  node film.cjs --poster
fi
