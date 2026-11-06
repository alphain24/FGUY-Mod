"""Test tool: a burst of every frame (fpv_burst.py's folders) as an animated GIF at its real speed, several side by side
with labels - to see how a motion looks, not only measure it. Frames are taken every <step> and doubled in size.

  fpv_clip.py <out.gif> <label>=<folder> [<label>=<folder> ...] [--step 25] [--crop lower]
"""
import argparse
import os

from PIL import Image, ImageDraw


def frames(folder, step_ms, crop):
    times = [float(t) for t in open(os.path.join(folder, 'times.txt'))]
    names = sorted(f for f in os.listdir(folder) if f.startswith('frame_'))
    out, next_t = [], times[0]
    for t, n in zip(times, names):
        if t >= next_t:
            im = Image.open(os.path.join(folder, n)).convert('RGB')
            if crop == 'lower':
                w, h = im.size
                im = im.crop((w // 3, h // 4, w, h))
            out.append(im.resize((im.width * 2, im.height * 2), Image.BICUBIC))
            next_t += step_ms / 1000
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('sets', nargs='+')
    ap.add_argument('--step', type=int, default=25)
    ap.add_argument('--crop', default='')
    a = ap.parse_args()
    sets = [(s.split('=', 1)[0], frames(s.split('=', 1)[1], a.step, a.crop)) for s in a.sets]
    n = min(len(f) for _, f in sets)
    w, h = sets[0][1][0].size
    sheet = []
    for i in range(n):
        im = Image.new('RGB', (w * len(sets), h))
        for k, (label, f) in enumerate(sets):
            im.paste(f[i], (k * w, 0))
            ImageDraw.Draw(im).text((k * w + 6, 4), label, fill=(255, 255, 0))
        sheet.append(im.quantize(colors=128, dither=Image.NONE))
    sheet[0].save(a.out, save_all=True, append_images=sheet[1:], duration=a.step, loop=0, optimize=True)
    print('%d frames, %d ms each -> %s (%.1f MB)' % (n, a.step, a.out, os.path.getsize(a.out) / 1e6))


if __name__ == '__main__':
    main()
