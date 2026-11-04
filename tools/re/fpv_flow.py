"""Test tool: how smoothly the view moves - from a burst of every frame (fpv_burst.py's folders): each frame's shift of
the world against the frame before (phase correlation of the screen's upper part, where the arms aren't), and how
evenly it goes (a frame that doesn't move, then one that moves twice as far: the view stood still a frame).

  fpv_flow.py <folder> [--rows a,b]
"""
import argparse
import os

import numpy as np
from PIL import Image


def shift(a, b):
    f = np.fft.fft2(a) * np.conj(np.fft.fft2(b))
    r = np.fft.ifft2(f / (np.abs(f) + 1e-9)).real
    y, x = np.unravel_index(np.argmax(r), r.shape)
    # sub-pixel: the peak's neighbours
    def sub(c, n, get):
        l, m, h = get((c - 1) % n), get(c), get((c + 1) % n)
        d = l - 2 * m + h
        return c + (0.5 * (l - h) / d if d else 0)
    sy = sub(y, r.shape[0], lambda i: r[i, x])
    sx = sub(x, r.shape[1], lambda i: r[y, i])
    if sy > r.shape[0] / 2: sy -= r.shape[0]
    if sx > r.shape[1] / 2: sx -= r.shape[1]
    return sx, sy


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--rows', default='0.05,0.45')
    a = ap.parse_args()
    lo, hi = (float(v) for v in a.rows.split(','))
    times = [float(t) for t in open(os.path.join(a.folder, 'times.txt'))]
    names = sorted(f for f in os.listdir(a.folder) if f.startswith('frame_'))
    ims = []
    for n in names:
        im = np.asarray(Image.open(os.path.join(a.folder, n)).convert('L'), dtype=np.float32)
        h = im.shape[0]
        ims.append(im[int(h * lo):int(h * hi), :] * np.hanning(im.shape[1])[None, :])
    moves = []
    for i in range(1, len(ims)):
        sx, sy = shift(ims[i], ims[i - 1])
        moves.append(((sx * sx + sy * sy) ** 0.5, (times[i] - times[i - 1]) * 1000, sx, sy))
    sp = [m[0] / m[1] for m in moves if m[1] > 0]
    mean = sum(sp) / len(sp)
    print('frame  ms    shift px (x, y)    px/ms')
    for i, (d, ms, sx, sy) in enumerate(moves):
        flag = '  <- still' if d < 0.15 * mean * ms else ('  <- jump' if d > 1.8 * mean * ms else '')
        print('%4d %5.1f %6.2f (%6.2f %6.2f) %6.3f%s' % (i + 1, ms, d, sx, sy, d / ms if ms else 0, flag))
    dev = sorted(abs(s - mean) / mean for s in sp)
    print('speed on the screen: mean %.3f px/ms; frame to frame off it: median %.0f%%, p95 %.0f%%; still frames %d, '
          'jumps %d of %d' % (mean, 100 * dev[len(dev) // 2], 100 * dev[int(len(dev) * 0.95)],
                              sum(1 for m in moves if m[0] < 0.15 * mean * m[1]),
                              sum(1 for m in moves if m[0] > 1.8 * mean * m[1]), len(moves)))


if __name__ == '__main__':
    main()
