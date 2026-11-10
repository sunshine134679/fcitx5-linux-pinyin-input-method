#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
: "${FILM_PYTHON:=python3}"
: "${FFMPEG:=ffmpeg}"
: "${SKIA_CANVAS_THREADS:=2}"
export FFMPEG SKIA_CANVAS_THREADS
"$FILM_PYTHON" soundtrack-v4.py
for film_segment in opening learning ending; do
  node film-v4.cjs --segment "$film_segment"
done
"$FILM_PYTHON" edit-v4.py
node film-v4.cjs --poster
