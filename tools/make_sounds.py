"""Builds the loading-screen sounds: dist/FGUYMod/sounds/opening.wav and strings_01.wav, strings_02.wav, ...

Input (not in the repo - Family Guy show audio, supplied by you): sounds/
  "Family Guy (Opening Sound Effect #1).wav"  -> opening.wav (story level 1, the first multiplayer match):
                                                silence trimmed, a smooth fade in and a long fade out
  "Family Guy - Music Strings.wav"            -> one file per music string (every other loading screen)
The strings file holds several cues with short quiet gaps between them; each cue is cut from just before
it starts to the quietest moment of the gap after it (keeping its reverb tail), with short fades.

Usage: make_sounds.py [<input folder>] [<output folder>]
"""
import os
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'sounds')
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'dist', 'FGUYMod', 'sounds')
OPENING = 'Family Guy (Opening Sound Effect #1).wav'
STRINGS = 'Family Guy - Music Strings.wav'

WIN = 0.02          # seconds per loudness step
LOUD_DB = -40       # louder than this = part of a cue
MIN_GAP = 0.35      # a quiet stretch at least this long separates two cues
LEAD = 0.03         # keep this much before a cue starts
FADE = 0.03
OPENING_FADE_IN = 0.6
OPENING_FADE_OUT = 1.8


def read(path):
    w = wave.open(path)
    rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
    assert width == 2, 'expects 16-bit audio'
    a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, ch)
    return a, rate


def write(path, a, rate):
    w = wave.open(path, 'wb')
    w.setnchannels(a.shape[1])
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(a.astype(np.int16).tobytes())
    w.close()


def cues(a, rate):
    win = int(rate * WIN)
    k = len(a) // win
    peak = np.abs(a[:k * win].astype(np.float32) / 32768).max(1).reshape(k, win).max(1)
    db = 20 * np.log10(peak + 1e-9)
    spans, start, last = [], None, 0
    for i, v in enumerate(db):
        if v > LOUD_DB:
            if start is None:
                start = i
            last = i
        elif start is not None and (i - last) * WIN > MIN_GAP:
            spans.append([start, last + 1])
            start = None
    if start is not None:
        spans.append([start, k])
    # end each cue at the quietest step of the gap after it (the file's end for the last one)
    for n, s in enumerate(spans):
        nxt = spans[n + 1][0] if n + 1 < len(spans) else k
        if nxt > s[1]:
            s[1] = s[1] + int(np.argmin(db[s[1]:nxt]))
    return [(max(0, s[0] * win - int(LEAD * rate)), min(len(a), s[1] * win)) for s in spans]


def fade(a, rate):
    a = a.astype(np.float32)
    n = min(len(a) // 4, int(FADE * rate))
    ramp = np.linspace(0, 1, n)[:, None]
    a[:n] *= ramp
    a[-n:] *= ramp[::-1]
    return a


def ease(n):
    """0 -> 1 along a smooth S curve (no click at either end)."""
    return (0.5 - 0.5 * np.cos(np.linspace(0, np.pi, n)))[:, None]


def opening(a, rate):
    found = cues(a, rate)
    s, e = found[0][0], found[-1][1]           # from the first sound to the end of the last (silence trimmed)
    out = a[s:e].astype(np.float32)
    n_in, n_out = min(len(out) // 3, int(OPENING_FADE_IN * rate)), min(len(out) // 2, int(OPENING_FADE_OUT * rate))
    out[:n_in] *= ease(n_in)
    out[-n_out:] *= ease(n_out)[::-1]
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    a, rate = read(os.path.join(SRC, OPENING))
    write(os.path.join(OUT, 'opening.wav'), opening(a, rate), rate)
    a, rate = read(os.path.join(SRC, STRINGS))
    found = cues(a, rate)
    for i, (s, e) in enumerate(found, 1):
        write(os.path.join(OUT, f'strings_{i:02d}.wav'), fade(a[s:e], rate), rate)
        print(f'strings_{i:02d}.wav  {s / rate:6.2f}s - {e / rate:6.2f}s  ({(e - s) / rate:.2f}s)')
    print(f'opening.wav + {len(found)} strings -> {os.path.normpath(OUT)}')


if __name__ == '__main__':
    main()
