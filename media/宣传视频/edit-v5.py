#!/usr/bin/env python3
"""复用已确认的原生界面运镜，组装第五版 3909 帧成片。"""
import os
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent
OUT = ROOT / 'output/v5'
PREVIOUS = ROOT / 'output/v4'
FF = os.environ.get('FFMPEG', 'ffmpeg')
OUT.mkdir(parents=True, exist_ok=True)

def run(args):
    subprocess.run([FF, '-y', '-hide_banner', '-loglevel', 'warning', *args], check=True)

for name in ['native', 'learning', 'schemes']:
    if not (PREVIOUS / f'{name}-2160p.mp4').is_file():
        raise SystemExit('缺少第四版界面素材，请先执行 render-v4.sh。')

# 从准确帧边界提取个人词典、隐私及快捷输入；保留原有镜头速度。
tail = OUT / 'features-2160p.mp4'
run(['-ss', str(514 / 60), '-i', str(PREVIOUS / 'learning-2160p.mp4'),
     '-map', '0:v:0', '-an', '-map_metadata', '-1', '-map_chapters', '-1',
     '-frames:v', '823', '-c:v', 'libx264', '-preset', 'fast', '-crf', '16',
     '-threads', '3', '-pix_fmt', 'yuv420p', '-colorspace', 'bt709',
     '-color_primaries', 'bt709', '-color_trc', 'bt709',
     '-video_track_timescale', '15360', '-movflags', '+faststart', str(tail)])

clips = [OUT / 'opening-2160p.mp4', PREVIOUS / 'native-2160p.mp4',
         OUT / 'learning-2160p.mp4', tail, PREVIOUS / 'schemes-2160p.mp4',
         OUT / 'ending-2160p.mp4']
for clip in clips:
    if not clip.is_file():
        raise SystemExit(f'缺少片段：{clip}')
manifest = OUT / 'concat-2160p.txt'
manifest.write_text(''.join("file '" + str(clip).replace("'", "'\\''") + "'\n" for clip in clips))
destination = OUT / 'ModernIME-灵动版-4K-60fps.mp4'
print('合成 65.15 秒成片及同步声音', flush=True)
run(['-f', 'concat', '-safe', '0', '-i', str(manifest), '-i', str(OUT / 'soundtrack.wav'),
     '-i', str(ROOT / 'chapters-v5.ffmeta'), '-map', '0:v:0', '-map', '1:a:0',
     '-map_metadata', '2', '-map_chapters', '2', '-c:v', 'copy', '-c:a', 'aac',
     '-b:a', '320k', '-ar', '48000', '-t', str(3909 / 60), '-movflags', '+faststart', str(destination)])
print('生成 1080p 60 帧分享版', flush=True)
run(['-i', str(destination), '-vf', 'scale=1920:1080:flags=lanczos',
     '-c:v', 'libx264', '-preset', 'fast', '-crf', '18', '-threads', '3',
     '-c:a', 'copy', '-movflags', '+faststart', str(OUT / 'ModernIME-灵动版-1080p-60fps.mp4')])
print('完成：' + str(destination), flush=True)
