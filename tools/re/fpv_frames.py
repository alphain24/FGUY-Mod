"""Test tool: frames of a test copy's view at set times after test-pipe commands, put together on one contact sheet.
Each step is "<pipe command>@<ms>,<ms>,..." (the frames taken that long after the command; "-" for no command), run in
order; the frames are labelled with the step's name.

  fpv_frames.py <pid> <out.png> "<name>=<command>@<ms>,..." ...
  e.g. fpv_frames.py 1234 peter.png "idle=-@0" "fire=hold 0 100 rt=255@150,600" "zoom=hold 0 2000 lt=255@1200"
Needs the copy's test pipe ([Debug] TestInput=1) and Pillow.
"""
import os
import sys
import tempfile
import time

from PIL import Image, ImageDraw


def send(pid, line):
    for _ in range(200):   # (the pipe takes one connection at a time: busy for a moment after each)
        try:
            with open(r'\\.\pipe\FGUYMod_%d' % pid, 'w') as p:
                p.write(line + '\n')
            return
        except OSError:
            time.sleep(0.005)


def main():
    pid, out = int(sys.argv[1]), sys.argv[2]
    tmp = tempfile.mkdtemp(prefix='fpv_frames_')
    shots = []
    for step in sys.argv[3:]:
        name, rest = step.split('=', 1)
        cmd, times = rest.rsplit('@', 1)
        t0 = time.time()
        if cmd != '-':
            send(pid, cmd)
        for ms in (int(t) for t in times.split(',')):
            while (time.time() - t0) * 1000 < ms:
                time.sleep(0.002)
            path = os.path.join(tmp, '%s_%d.bmp' % (name, ms))
            send(pid, 'shot ' + path)
            shots.append(('%s %d ms' % (name, ms), path))
        time.sleep(0.6)
    time.sleep(1)
    W, cols, ims = 640, 3, []
    for label, path in shots:
        if not os.path.exists(path):
            continue
        im = Image.open(path).convert('RGB')
        im = im.resize((W, im.height * W // im.width))
        ImageDraw.Draw(im).text((8, 8), label, fill=(255, 255, 0))
        ims.append(im)
    if not ims:
        sys.exit('no frames (is the game drawing?)')
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new('RGB', (W * min(cols, len(ims)), ims[0].height * rows))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % cols) * W, (i // cols) * ims[0].height))
    sheet.save(out)
    print('%d frames -> %s' % (len(ims), out))


if __name__ == '__main__':
    main()
