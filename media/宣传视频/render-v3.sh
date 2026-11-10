#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
: "${FILM_PYTHON:=python3}"
: "${FFMPEG:=ffmpeg}"
export FFMPEG
if [ ! -f assets/music/digital-clouds-175.mp3 ]; then
  echo '缺少本地音乐素材。请按「音乐来源与使用说明.md」从 Mixkit 获取 Digital Clouds。' >&2
  exit 1
fi
"$FILM_PYTHON" soundtrack-v3.py
node film-v3.cjs "$@"
if [ "$#" -eq 0 ]; then
  "$FFMPEG" -y -hide_banner -loglevel warning \
    -i 'output/v3/ModernIME-个性化篇-4K-60fps.mp4' \
    -vf 'scale=1920:1080:flags=lanczos' -c:v libx264 -preset medium -crf 18 \
    -threads 3 -c:a copy -movflags +faststart 'output/v3/ModernIME-个性化篇-1080p-60fps.mp4'
  node film-v3.cjs --poster
fi
