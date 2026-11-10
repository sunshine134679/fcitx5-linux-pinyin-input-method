#!/usr/bin/env python3
"""Original, deterministic electronic score and synced UI foley. No sampled music."""
import pathlib
import wave
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent
SR = 48000
DURATION = 40
rng = np.random.default_rng(20261110)
mix = np.zeros((SR * DURATION, 2), dtype=np.float64)

def place(signal, start, gain=1.0, pan=0.0):
    offset = int(start * SR)
    if offset >= len(mix):
        return
    signal = signal[:len(mix) - offset] * gain
    mix[offset:offset + len(signal), 0] += signal * np.sqrt((1-pan)/2)
    mix[offset:offset + len(signal), 1] += signal * np.sqrt((1+pan)/2)

def frequency(note):
    return 440 * 2 ** ((note-69)/12)

def bell(note, length=1.5, soft=False):
    t = np.arange(int(length * SR))/SR
    f = frequency(note)
    phase = 2*np.pi*f*t
    sound = np.sin(phase + .5*np.sin(phase*2)*np.exp(-t*6))
    sound += .22*np.sin(phase*2)*np.exp(-t*4)
    sound += .08*np.sin(phase*3)*np.exp(-t*7)
    attack = 1-np.exp(-t*(95 if soft else 240))
    return sound * attack * np.exp(-t*(2.0 if soft else 3.5))

def chord(notes, start, duration=2.8, gain=.04):
    t = np.arange(int(duration*SR))/SR
    env = np.minimum(t/.28,1)*np.minimum((duration-t)/.7,1)
    for i,note in enumerate(notes):
        f=frequency(note)
        signal=(np.sin(2*np.pi*f*t)+.35*np.sin(2*np.pi*(f*1.002)*t)+.16*np.sin(2*np.pi*f*2*t))/1.51
        place(signal*env,start,gain,(i/(len(notes)-1)-.5)*1.2)

chords = [[57,61,64,68,71],[54,57,61,64,68],[50,54,57,61,64],[52,56,59,64,66]]
# Slow, warm harmony; tiny attack variation prevents a mechanical grid.
for bar in range(19):
    chord(chords[bar%4],bar*2,duration=2.9,gain=.034 if bar<3 else .045)
    if bar>=3:
        place(bell(chords[bar%4][0]-12,1.8,True),bar*2,.095,-.04)

# A sparse mallet motif, opening into a bright stereo arpeggio at the theme sequence.
motif=[0,2,4,2,1,3,4,2]
for step in range(74):
    start=step*.5+1
    notes=chords[int(start//2)%4]
    if start<6 and step%2:continue
    if 28<start<34 and step%2:continue
    gain=.105 if 12<=start<22 else .073
    note=notes[motif[step%8]]+12
    sound=bell(note,1.8,True)
    pan=np.sin(step*.8)*.42
    place(sound,start,gain,pan)
    place(sound,start+.375,gain*.18,-pan)
    place(sound,start+.75,gain*.07,pan)

def kick(start, gain=.17):
    t=np.arange(int(.36*SR))/SR
    freq=48+77*np.exp(-t*28)
    phase=2*np.pi*np.cumsum(freq)/SR
    place(np.sin(phase)*(1-np.exp(-t*900))*np.exp(-t*13),start,gain)

def hat(start,gain=.018,pan=.2):
    t=np.arange(int(.08*SR))/SR
    noise=rng.standard_normal(len(t))
    noise=np.diff(noise,prepend=0)*.4
    place(noise*np.exp(-t*74),start,gain,pan)

for beat in range(12,72):
    start=beat*.5
    if beat%2==0:kick(start,.12 if start>=28 else .18)
    hat(start+.25,.013 if beat%2 else .019,(-1 if beat%2 else 1)*.35)
    if beat%4==2:
        t=np.arange(int(.13*SR))/SR
        noise=rng.standard_normal(len(t))
        soft=np.convolve(noise,np.ones(5)/5,mode='same')
        place(soft*np.exp(-t*35),start,.038,-.12)

# Gentle keyed clicks and brief colored-noise swishes line up with the visual beats.
for i,start in enumerate([.35,.55,.75,.95,1.15,1.8,2.7]):
    place(bell(78+i%3,.13),start,.035,(i-3)*.12)

def swish(start,length=.55,gain=.036,pan=0):
    t=np.arange(int(length*SR))/SR
    noise=rng.standard_normal(len(t))
    noise=np.convolve(noise,np.ones(24)/24,mode='same')
    env=np.sin(np.pi*t/length)**2
    place(noise*env,start,gain,pan)

for i,start in enumerate([5.4,11.35,13.3,14.6,15.9,17.2,18.5,19.8,21.4,24.3,27.4,33.4]):
    swish(start,.65,.075,(-1 if i%2 else 1)*.3)

for i,note in enumerate([69,73,76,80]):
    place(bell(note,3.5,True),35.05+i*.13,.095,(-.3+i*.2))

# A little cross-channel ambience, with a clean tail and no clipping.
dry=mix.copy()
for delay,gain in [(.071,.12),(.137,.07),(.281,.045)]:
    n=int(delay*SR)
    mix[n:] += dry[:-n,::-1]*gain
t=np.arange(len(mix))/SR
envelope=np.minimum(t/.5,1)*np.clip((40-t)/2.4,0,1)
mix *= envelope[:,None]
mix *= .86/max(1e-9,float(np.max(np.abs(mix))))
out=ROOT/'output'/'soundtrack.wav'
out.parent.mkdir(exist_ok=True)
with wave.open(str(out),'wb') as file:
    file.setnchannels(2);file.setsampwidth(2);file.setframerate(SR)
    file.writeframes((mix*32767).astype('<i2').tobytes())
print(f'{out.name}: {DURATION}s, stereo {SR} Hz, peak {20*np.log10(np.max(np.abs(mix))):.2f} dBFS')
