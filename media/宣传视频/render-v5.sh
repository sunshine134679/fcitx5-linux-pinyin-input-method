#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
: "${FILM_PYTHON:=python3}"
: "${FFMPEG:=ffmpeg}"
: "${SKIA_CANVAS_THREADS:=2}"
export FFMPEG SKIA_CANVAS_THREADS
"$FILM_PYTHON" soundtrack-v5.py
for film_segment in opening learning ending; do
  node film-v5.cjs --segment "$film_segment"
done
"$FILM_PYTHON" edit-v5.py
node film-v5.cjs --poster
