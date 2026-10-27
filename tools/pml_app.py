#!/usr/bin/env python3
"""pml_app.py - helpers for making apps that show up properly on PicoMultiLoader's Home screen.

  pml_app.py icon    PICTURE OUT.bmp [--keep-indices]  make a 32x32 icon.bmp from a PNG/BMP/...
  pml_app.py preview APP_DIR OUT.png                   the folder's icon in every built-in theme
  pml_app.py check   APP_DIR [APP_DIR...]              check a folder as the loader reads it

The format is described in docs/apps.md. `icon` and `preview` need Pillow (pip install pillow);
`check` needs only Python 3.
"""
import argparse
import os
import struct
import sys

INI_MAX = 512  # the loader reads only this much of app.ini
# key: longest value the loader keeps (common/pml/apps.c)
KEYS = {'name': 47, 'author': 31, 'version': 15, 'category': 15, 'icon': 31, 'binary': 63,
        'binary.rp2040': 63, 'binary.rp2350': 63, 'binary.esp32p4': 63, 'board': 39, 'desc': 95}
CATEGORIES = ('Emulators', 'Tools', 'Dev', 'Media')
BOARDS = {'rp2040': '.uf2', 'rp2350': '.uf2', 'esp32p4': '.bin'}
TILE_COLS, FOOTER_COLS = 12, 50  # characters shown under the icon, and on Home's bottom line

# Palette roles 1..12 of the built-in themes (common/pml/look.c); role 0 is see-through in icons.
ROLES = ('clear', 'PANEL', 'FIELD', 'LINE', 'DIM', 'FG', 'BRIGHT', 'ACCENT', 'ACCDK', 'OK', 'WARN', 'ERR', 'ACC2')
THEMES = {
    'Graphite': (0x0e131c, 0x172030, 0x212c40, 0x33415c, 0x7a88a3, 0xd5ddeb, 0xffffff, 0x4cc3ff, 0x1d5f86,
                 0x5fd98a, 0xffb347, 0xff6b6b, 0xb48cff),
    'Phosphor': (0x04110a, 0x0a1f12, 0x10301c, 0x1d4a2c, 0x3f8a55, 0x8cf0a5, 0xd8ffe2, 0x5cff8a, 0x1f7a3c,
                 0x5cff8a, 0xe6ff5c, 0xff7a5c, 0xa0ffd0),
    'Amber': (0x140c02, 0x24170a, 0x352310, 0x523718, 0xa0723a, 0xffc670, 0xfff0d0, 0xffa020, 0x8a5208,
              0xd0e060, 0xffd040, 0xff5a3a, 0xffd8a0),
    'Paper': (0xe9e6dc, 0xf6f4ee, 0xdcd8cb, 0xb9b3a2, 0x6f6a5d, 0x24221d, 0x000000, 0xc2410c, 0xf0c8a8,
              0x2f7d3a, 0xb8740a, 0xb42318, 0x5b4bb7),
    'Clockwork': (0x1b1d21, 0x26292e, 0x31353b, 0x474c55, 0x8b929e, 0xe3e6ea, 0xffffff, 0xff8a3d, 0x7a3b12,
                  0x7bd88f, 0xffc24d, 0xff6161, 0x7cc7ff),
}


def rgb(c):
    return c >> 16 & 255, c >> 8 & 255, c & 255


# ---- icons ------------------------------------------------------------------------------------
def write_icon(path, pixels, palette=THEMES['Graphite']):
    """pixels: 32 rows of 32 role numbers (0-15). The BMP palette is the Graphite theme, so the
    file looks right in an image viewer; the loader ignores it and uses the pixel values."""
    pal = b''.join(struct.pack('<BBBB', b, g, r, 0) for r, g, b in (rgb(palette[i % 13]) for i in range(16)))
    rows = b''.join(bytes(row[x] << 4 | row[x + 1] for x in range(0, 32, 2)) for row in reversed(pixels))
    off = 14 + 40 + len(pal)
    with open(path, 'wb') as f:
        f.write(b'BM' + struct.pack('<IHHI', off + len(rows), 0, 0, off))
        f.write(struct.pack('<IiiHHIIiiII', 40, 32, 32, 1, 4, 0, len(rows), 2835, 2835, 16, 0))
        f.write(pal + rows)


def read_icon(path):
    """The 32x32 role numbers, or raise ValueError saying why the loader would reject the file."""
    with open(path, 'rb') as f:
        b = f.read()
    if len(b) < 54 or b[:2] != b'BM':
        raise ValueError('not a BMP file')
    w, h, bpp, comp = struct.unpack_from('<i', b, 18)[0], struct.unpack_from('<i', b, 22)[0], \
        struct.unpack_from('<H', b, 28)[0], struct.unpack_from('<I', b, 30)[0]
    if w != 32 or abs(h) != 32:
        raise ValueError('is %dx%d, must be 32x32' % (w, abs(h)))
    if bpp != 4:
        raise ValueError('is %d bits per pixel, must be 4 (16 colours)' % bpp)
    if comp != 0:
        raise ValueError('is compressed (RLE), must be uncompressed')
    data = b[struct.unpack_from('<I', b, 10)[0]:][:512]
    if len(data) < 512:
        raise ValueError('pixel data is cut short')
    rows = [[data[r * 16 + c // 2] >> (0 if c & 1 else 4) & 15 for c in range(32)] for r in range(32)]
    return rows[::-1] if h > 0 else rows


def cmd_icon(args):
    from PIL import Image
    im = Image.open(args.picture)
    if args.keep_indices:
        if im.mode != 'P':
            sys.exit('--keep-indices needs an indexed (palette) image')
        if im.size != (32, 32):
            sys.exit('--keep-indices needs a 32x32 image, this one is %dx%d' % im.size)
        px = [[im.getpixel((x, y)) for x in range(32)] for y in range(32)]
        if any(v > 15 for row in px for v in row):
            sys.exit('the image uses palette entries above 15')
    else:
        im = im.convert('RGBA')
        if im.size != (32, 32):
            im.thumbnail((32, 32), Image.NEAREST if max(im.size) <= 64 else Image.LANCZOS)
            canvas = Image.new('RGBA', (32, 32), (0, 0, 0, 0))
            canvas.paste(im, ((32 - im.width) // 2, (32 - im.height) // 2))
            im = canvas
        pal = [rgb(c) for c in THEMES['Graphite'][1:]]  # roles 1..12
        px = []
        for y in range(32):
            row = []
            for x in range(32):
                r, g, b, a = im.getpixel((x, y))
                if a < 128:
                    row.append(0)
                else:
                    row.append(1 + min(range(12), key=lambda i: (pal[i][0] - r) ** 2 * 3 + (pal[i][1] - g) ** 2 * 4 +
                                                            (pal[i][2] - b) ** 2 * 2))
            px.append(row)
    write_icon(args.out, px)
    used = sorted({v for row in px for v in row})
    print('%s: 32x32, roles used: %s' % (args.out, ', '.join('%d %s' % (v, ROLES[v] if v < 13 else 'cycled')
                                                            for v in used)))


def cmd_preview(args):
    from PIL import Image
    icon = read_icon(os.path.join(args.app_dir, ini_values(args.app_dir)[0].get('icon') or 'icon.bmp'))
    scale, pad = 4, 8
    cell = 32 * scale + 2 * pad
    out = Image.new('RGB', (cell * len(THEMES), cell))
    for t, pal in enumerate(THEMES.values()):
        out.paste(rgb(pal[0]), (t * cell, 0, (t + 1) * cell, cell))
        for y, row in enumerate(icon):
            for x, v in enumerate(row):
                if v:
                    c = rgb(pal[v] if v < 13 else pal[7])
                    x0, y0 = t * cell + pad + x * scale, pad + y * scale
                    out.paste(c, (x0, y0, x0 + scale, y0 + scale))
    out.save(args.out)
    print('%s: %s' % (args.out, ', '.join(THEMES)))


# ---- app.ini ----------------------------------------------------------------------------------
def ini_values(app_dir):
    """(values, raw bytes, problems) as the loader sees them: first match of each key wins."""
    path = os.path.join(app_dir, 'app.ini')
    if not os.path.isfile(path):
        return {}, b'', []
    raw = open(path, 'rb').read()
    text = raw[:INI_MAX].decode('latin-1')
    values, problems = {}, []
    for n, line in enumerate(text.split('\n'), 1):
        line = line.strip(' \t\r')
        if not line or line[0] in ';#[':
            continue
        if '=' not in line:
            problems.append('line %d: no "=": %r' % (n, line))
            continue
        key, val = (s.strip(' \t') for s in line.split('=', 1))
        for i, ch in enumerate(val):  # ";" starts a comment at the start or after a blank
            if ch == ';' and (i == 0 or val[i - 1] in ' \t'):
                val = val[:i].rstrip(' \t')
                break
        key = key.lower()
        if key not in values:
            values[key] = val
    return values, raw, problems


def check(app_dir):
    errors, warnings = [], []
    name = os.path.basename(os.path.normpath(app_dir))
    v, raw, problems = ini_values(app_dir)
    warnings += problems
    files = sorted(os.listdir(app_dir))
    if not raw:
        if not any(f.lower().endswith(tuple(BOARDS.values())) for f in files):
            return ['not an app: no app.ini and no .uf2 or .bin, so Home does not list it'], []
        warnings.append('no app.ini: Home shows the folder name and a generic icon')
    if len(raw) > INI_MAX:
        lost = [k for k in ini_values_all(raw) if k not in v]
        errors.append('app.ini is %d bytes; only the first %d are read%s' %
                      (len(raw), INI_MAX, (': lost ' + ', '.join(lost)) if lost else ''))
    if raw.startswith(b'\xef\xbb\xbf'):
        errors.append('app.ini starts with a UTF-8 BOM: the first key is not found. Save it without one')
    if any(c > 126 for c in raw[:INI_MAX]):
        warnings.append('app.ini has non-ASCII characters; the screen font is ASCII only')
    for key, val in v.items():
        if key not in KEYS:
            warnings.append('unknown key "%s" (ignored by the loader)' % key)
        elif len(val) > KEYS[key]:
            warnings.append('%s is %d characters; the loader keeps %d: "%s"' % (key, len(val), KEYS[key],
                                                                               val[:KEYS[key]]))
    title = v.get('name') or name
    if len(title) > TILE_COLS:
        warnings.append('name "%s" is cut to %d characters under the icon' % (title, TILE_COLS))
    footer = (v.get('version', '') + '  ' if v.get('version') else '') + v.get('desc', '')
    if len(footer) > FOOTER_COLS:
        warnings.append('version + desc is %d characters; Home shows %d: "%s"' % (len(footer), FOOTER_COLS,
                                                                                  footer[:FOOTER_COLS]))
    if 'category' in v and v['category'].lower() not in [c.lower() for c in CATEGORIES]:
        warnings.append('category "%s" is not one of %s: the app is only under All' % (v['category'],
                                                                                     ', '.join(CATEGORIES)))
    boards = list(BOARDS)
    if v.get('board'):
        b = v['board'].lower()
        if 'any' not in b:
            boards = [k for k in BOARDS if k in b or (k == 'esp32p4' and 'p4' in b)]
            if not boards:
                errors.append('board "%s" matches no board (%s, any): the app is hidden everywhere' %
                              (v['board'], ', '.join(BOARDS)))
    for board in boards:
        ext = BOARDS[board]
        bin_ = v.get('binary.' + board) or v.get('binary', '')
        if bin_ and (not bin_.lower().endswith(ext) or '/' in bin_ or '\\' in bin_):
            bin_ = ''  # the loader ignores it and looks for the folder's only app file
        if bin_:
            if bin_ not in files and bin_.lower() not in [f.lower() for f in files]:
                errors.append('%s: binary "%s" is not in the folder' % (board, bin_))
            else:
                print('  %-8s runs %s' % (board, bin_))
            continue
        apps = [f for f in files if f.lower().endswith(ext) and not f.startswith('.')]
        if len(apps) == 1:
            print('  %-8s runs %s (the only %s)' % (board, apps[0], ext))
        elif len(apps) > 1:
            warnings.append('%s: %d %s files and no binary%s key: Enter opens the folder instead' %
                            (board, len(apps), ext, '.' + board if v.get('binary') else ''))
        elif v.get('board'):  # it says it runs here
            warnings.append('%s: no %s file in the folder' % (board, ext))
    icon = v.get('icon') or 'icon.bmp'
    if '/' in icon or '\\' in icon:
        errors.append('icon "%s": must be a file in the app folder, without a path' % icon)
    elif os.path.isfile(os.path.join(app_dir, icon)):
        try:
            px = read_icon(os.path.join(app_dir, icon))
            if not any(any(row) for row in px):
                warnings.append('%s is all role 0 (see-through): nothing is drawn' % icon)
        except ValueError as e:
            errors.append('%s %s: the generic icon is shown instead' % (icon, e))
    elif 'icon' in v or raw:
        warnings.append('no %s: the generic icon is shown' % icon)
    return errors, warnings


def ini_values_all(raw):
    keys = []
    for line in raw.decode('latin-1').split('\n'):
        line = line.strip()
        if '=' in line and line[0] not in ';#[':
            keys.append(line.split('=', 1)[0].strip().lower())
    return keys


def cmd_check(args):
    bad = 0
    for d in args.app_dir:
        print(d)
        if not os.path.isdir(d):
            print('  ERROR not a folder')
            bad += 1
            continue
        errors, warnings = check(d)
        for e in errors:
            print('  ERROR', e)
        for w in warnings:
            print('  warn ', w)
        if not errors and not warnings:
            print('  ok')
        bad += bool(errors)
    sys.exit(1 if bad else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('icon', help='make a 32x32 role-indexed icon.bmp from a picture')
    s.add_argument('picture')
    s.add_argument('out')
    s.add_argument('--keep-indices', action='store_true',
                   help='the picture is already a 32x32 indexed image drawn with role numbers')
    s.set_defaults(fn=cmd_icon)
    s = sub.add_parser('preview', help="render a folder's icon in every built-in theme")
    s.add_argument('app_dir')
    s.add_argument('out')
    s.set_defaults(fn=cmd_preview)
    s = sub.add_parser('check', help='check app folders as the loader reads them')
    s.add_argument('app_dir', nargs='+')
    s.set_defaults(fn=cmd_check)
    args = p.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
