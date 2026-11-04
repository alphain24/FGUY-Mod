"""Test tool: how a first-person gun moves on the screen, frame by frame - from a burst of every frame (the test pipe's
"shots <n> <folder>": frame_000.bmp.., times.txt). The gun found by a colour (default Peter's drill's orange) in the
lower right; per frame its centre and size, the step from the frame before, and a strip of the frames.

  fpv_burst.py <folder> [--strip out.png] [--every k] [--color r,g,b] [--tol t]
"""
import argparse
import os

from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--strip')
    ap.add_argument('--every', type=int, default=1)
    ap.add_argument('--color', default='240,110,20')
    ap.add_argument('--tol', type=int, default=60)
    a = ap.parse_args()
    want = [int(c) for c in a.color.split(',')]
    times = [float(t) for t in open(os.path.join(a.folder, 'times.txt'))]
    frames = sorted(f for f in os.listdir(a.folder) if f.startswith('frame_'))
    rows, ims = [], []
    import numpy as np
    for i, name in enumerate(frames):
        im = Image.open(os.path.join(a.folder, name)).convert('RGB')
        w, h = im.size
        # (every pixel of the lower right two thirds, weighted by how near the colour it is - a soft edge: its centre
        # moves by fractions of a pixel, not in steps as pixels flip)
        arr = np.asarray(im, dtype=np.float32)[h // 3:, w // 3:]
        d = np.abs(arr - np.array(want, dtype=np.float32)).sum(axis=2)
        wt = np.clip(1 - d / a.tol, 0, 1)
        n = float(wt.sum())
        if n > 2:
            ys, xs = np.mgrid[h // 3:h, w // 3:w]
            c = (float((wt * xs).sum() / n), float((wt * ys).sum() / n))
        else:
            c = (None, None)
        rows.append((times[i] if i < len(times) else 0, c[0], c[1], int(n)))
        if a.strip and i % a.every == 0:
            ims.append((i, im))
    print('frame   ms     x      y    size   step')
    prev = None
    steps = []
    for i, (t, x, y, n) in enumerate(rows):
        step = ''
        if prev and x is not None and prev[1] is not None:
            d = ((x - prev[1]) ** 2 + (y - prev[2]) ** 2) ** 0.5
            steps.append(d)
            step = '%.3f' % d
        ms = (t - rows[0][0]) * 1000
        print('%4d %7.1f %6s %6s %6d %6s' % (i, ms, '%.3f' % x if x is not None else '-',
                                             '%.3f' % y if y is not None else '-', n, step))
        prev = (t, x, y)
    if steps:
        s = sorted(steps)
        print('steps (px, quarter size): median %.2f, p95 %.2f, max %.2f; frames without the gun: %d' % (
            s[len(s) // 2], s[int(len(s) * 0.95)], s[-1], sum(1 for r in rows if r[1] is None)))
    if a.strip and ims:
        W = ims[0][1].width
        cols = 6
        H = ims[0][1].height
        sheet = Image.new('RGB', (W * cols, H * ((len(ims) + cols - 1) // cols)))
        for k, (i, im) in enumerate(ims):
            ImageDraw.Draw(im).text((4, 4), '%d' % i, fill=(255, 255, 0))
            sheet.paste(im, ((k % cols) * W, (k // cols) * H))
        sheet.save(a.strip)
        print('strip ->', a.strip)


if __name__ == '__main__':
    main()
