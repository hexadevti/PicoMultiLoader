# composite.py SKIN RAWDIR OUTDIR name=out ... - put 320x320 shots into the PicoCalc picture's screen.
# The picture is scaled so the glass is 320 px tall, so the LCD pixels stay 1:1 and sharp.
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import read_png, write_png

skin, rawdir, outdir, pairs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
GX0, GY0, GX1, GY1 = 151, 146, 793, 776   # screen glass in picocalc.png
CROP_H = 1062                             # keep the screen and the top key rows
S = 320 / (GY1 - GY0)


def weights(n_src, n_dst):
    inv = n_src / n_dst
    out = []
    for d in range(n_dst):
        a, b = d * inv, (d + 1) * inv
        ws = []
        i = int(a)
        while i < b and i < n_src:
            w = min(b, i + 1) - max(a, i)
            if w > 1e-9:
                ws.append((i, w / inv))
            i += 1
        out.append(ws)
    return out


w, h, px = read_png(skin)
h = CROP_H
dw, dh = round(w * S), round(h * S)
cw, rw = weights(w, dw), weights(h, dh)
tmp = [None] * h                       # horizontal pass
for y in range(h):
    row = px[y * w * 4:(y + 1) * w * 4]
    line = []
    for ws in cw:
        acc = [0.0, 0.0, 0.0, 0.0]
        for i, wt in ws:
            for c in range(4):
                acc[c] += row[i * 4 + c] * wt
        line.append(acc)
    tmp[y] = line
bg = bytearray(dw * dh * 4)              # vertical pass
for y, ws in enumerate(rw):
    for x in range(dw):
        acc = [0.0, 0.0, 0.0, 0.0]
        for j, wt in ws:
            t = tmp[j][x]
            for c in range(4):
                acc[c] += t[c] * wt
        o = (y * dw + x) * 4
        bg[o:o + 4] = bytes(min(255, int(v + 0.5)) for v in acc)

gx0, gy0, gx1, gy1 = round(GX0 * S), round(GY0 * S), round(GX1 * S), round(GY1 * S)
ox, oy = (gx0 + gx1 - 320) // 2, (gy0 + gy1 - 320) // 2
bgcol = bytes((18, 20, 28, 255))  # the loader's C_BG
for pair in pairs:
    name, _, out = pair.partition('=')
    img = bytearray(bg)
    for y in range(gy0, gy1):
        for x in range(gx0, gx1):
            img[(y * dw + x) * 4:(y * dw + x) * 4 + 4] = bgcol
    _, _, shot = read_png(os.path.join(rawdir, name + '.png'))
    for y in range(320):
        o = ((oy + y) * dw + ox) * 4
        img[o:o + 1280] = shot[y * 1280:(y + 1) * 1280]
    write_png(os.path.join(outdir, (out or name) + '.png'), dw, dh, img)
    print(out or name, dw, dh)
