#!/usr/bin/env python3
"""Conform the existing 4K camera footage and assemble the new 60-second edit."""
import argparse
import os
import pathlib
import subprocess

ROOT=pathlib.Path(__file__).resolve().parent
OUT=ROOT/'output/v4'
FF=os.environ.get('FFMPEG','ffmpeg')
SOURCE=ROOT/'output/v2/ModernIME-重制版-4K-60fps.mp4'
parser=argparse.ArgumentParser()
parser.add_argument('--width',type=int,default=3840)
parser.add_argument('--native-only',action='store_true')
parser.add_argument('--assemble-only',action='store_true')
args=parser.parse_args()
width=args.width
height=width*9//16
OUT.mkdir(parents=True,exist_ok=True)

def run(command):
    subprocess.run(command,check=True)

def conform(name,start,end,frames):
    destination=OUT/f'{name}-{height}p.mp4'
    ratio=(frames/60)/(end-start)
    print(f'整理原始 4K 运镜：{name}',flush=True)
    run([FF,'-y','-hide_banner','-loglevel','warning','-ss',str(start),'-t',str(end-start),'-i',str(SOURCE),
         '-map','0:v:0','-an','-map_metadata','-1','-map_chapters','-1',
         '-vf',f'setpts=(PTS-STARTPTS)*{ratio},fps=60,scale={width}:{height}:flags=lanczos,tpad=stop_mode=clone:stop_duration=0.05',
         '-frames:v',str(frames),'-c:v','libx264','-preset','fast','-crf','16','-threads','3','-pix_fmt','yuv420p',
         '-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709','-video_track_timescale','15360',
         '-movflags','+faststart',str(destination)])

if not args.assemble_only:
    if not SOURCE.is_file():raise SystemExit('缺少第二版 4K 运镜素材。请先运行 render-v2.sh，保留 output/v2 成片。')
    # Cut before the old wipe reveals the next (now unrelated) scene.
    # Keep the entry/theme cuts on the score's bar boundaries. Only shorten the
    # tail of the appearance shot to avoid the previous film's outgoing wipe.
    conform('native-entry',60/7,108/7,412)
    conform('native-themes',108/7,156/7,411)
    conform('native-appearance',156/7,30.5,515)
    native_manifest=OUT/f'native-concat-{height}p.txt'
    native_manifest.write_text(''.join(f"file '{name}-{height}p.mp4'\n" for name in ['native-entry','native-themes','native-appearance']))
    run([FF,'-y','-hide_banner','-loglevel','warning','-f','concat','-safe','0','-i',str(native_manifest),
         '-c:v','copy','-an','-movflags','+faststart',str(OUT/f'native-{height}p.mp4')])
    conform('schemes',216/7,33.92,205)

if not args.native_only:
    clips=[OUT/f'{name}-{height}p.mp4' for name in ['opening','native','learning','schemes','ending']]
    for clip in clips:
        if not clip.is_file():raise SystemExit(f'缺少片段：{clip.name}')
    manifest=OUT/f'concat-{height}p.txt'
    manifest.write_text(''.join("file '"+clip.name+"'\n" for clip in clips))
    quality='4K' if width==3840 else f'{height}p'
    destination=OUT/f'ModernIME-节奏重剪版-{quality}-60fps.mp4'
    print('合成 60 秒成片与同步声音',flush=True)
    run([FF,'-y','-hide_banner','-loglevel','warning','-f','concat','-safe','0','-i',str(manifest),
         '-i',str(OUT/'soundtrack.wav'),'-i',str(ROOT/'chapters-v4.ffmeta'),
         '-map','0:v:0','-map','1:a:0','-map_metadata','2','-map_chapters','2','-c:v','copy',
         '-c:a','aac','-b:a','320k','-ar','48000','-t','60','-movflags','+faststart',str(destination)])
    if width==3840:
        print('生成 1080p 60 帧分享版',flush=True)
        run([FF,'-y','-hide_banner','-loglevel','warning','-i',str(destination),
             '-vf','scale=1920:1080:flags=lanczos','-c:v','libx264','-preset','fast','-crf','18','-threads','3',
             '-c:a','copy','-movflags','+faststart',str(OUT/'ModernIME-节奏重剪版-1080p-60fps.mp4')])
    print('完成：'+str(destination),flush=True)
