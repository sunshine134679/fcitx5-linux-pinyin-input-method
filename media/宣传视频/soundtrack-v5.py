#!/usr/bin/env python3
"""Mix the licensed recording with very restrained typing/UI foley for this video."""
import json
import os
import pathlib
import subprocess
import wave
import numpy as np

root = pathlib.Path(__file__).resolve().parent
out = root / 'output/v5'
out.mkdir(parents=True, exist_ok=True)
timing = json.loads((root / 'timing-v5.json').read_text())
ffmpeg = os.environ.get('FFMPEG', 'ffmpeg')
music = root / 'assets/music/digital-clouds-175.mp3'
raw = subprocess.check_output([ffmpeg, '-v', 'error', '-ss', str(timing['musicStart']), '-i', str(music), '-t', str(timing['duration']), '-f', 'f32le', '-ac', '2', '-ar', '48000', '-'])
score = np.frombuffer(raw, dtype='<f4').reshape(-1, 2).astype(np.float64)
score *= .56
rate = 48000
rng = np.random.default_rng(11010)

def add_click(at, space=False, pan=0, gain=1):
    duration = .065 if space else .035
    t = np.arange(int(rate*duration))/rate
    noise = rng.standard_normal(len(t))
    # A short, damped mechanical tap, not a pitched musical note.
    noise = np.convolve(noise, np.ones(7)/7, mode='same')
    signal = noise*np.exp(-t*(78 if space else 130))
    signal += .12*np.sin(2*np.pi*(380 if space else 760)*t)*np.exp(-t*110)
    signal *= np.minimum(t/.001, 1)*(.145 if space else .115)*gain
    # Keep a short, gentle key transient audible below the music.
    duck_start=max(0,int((at-.015)*rate));duck_end=min(len(score),int((at+.11)*rate))
    duck_t=np.arange(duck_end-duck_start)/rate
    score[duck_start:duck_end] *= (1-.10*np.exp(-((duck_t-.02)/.045)**2))[:,None]
    start = int(at*rate)
    length = min(len(signal), len(score)-start)
    if length<=0: return
    score[start:start+length, 0] += signal[:length]*(1-pan)
    score[start:start+length, 1] += signal[:length]*(1+pan)

for group in timing['typing']:
    start = timing['starts'][group['scene']]
    for i, letter in enumerate(group['pinyin']):
        add_click(start+group['start']+i*group['interval'], pan=(i%3-1)*.1)
    add_click(start+group['commit'], True)
    if 'afterAt' in group: add_click(start+group['afterAt'])
for event in timing['extraSounds']:
    add_click(event['at'],event['key']=='SPACE',gain=.55 if event['key']=='CLICK' else 1)
t = np.arange(len(score))/rate
score *= (np.minimum(t/.08,1)*np.clip((timing['duration']-t)/1.5,0,1))[:,None]
peak = float(np.abs(score).max())
if peak>.88: score *= .88/peak
with wave.open(str(out/'soundtrack.wav'), 'wb') as stream:
    stream.setnchannels(2);stream.setsampwidth(2);stream.setframerate(rate)
    stream.writeframes((score*32767).astype('<i2').tobytes())
print('Licensed music + typing foley:', timing['duration'], 's, 48 kHz stereo, peak',round(20*np.log10(np.abs(score).max()),2),'dBFS')
