# shots.py PORT OUTDIR step... - reset the loader, run serial steps, save raw 320x320 screenshots.
# Steps: k:<name> (key), t:<text>, w:<seconds>, s:<shot name>
import sys, time, threading, serial, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import write_png

KEYS = {'up': b'\x1b[A', 'down': b'\x1b[B', 'right': b'\x1b[C', 'left': b'\x1b[D', 'enter': b'\r',
        'esc': b'\x1b', 'bs': b'\x7f', 'f1': b'\x1bOP', 'f4': b'\x1b[14~', 'f5': b'\x1b[15~',
        'home': b'\x1b[H', 'end': b'\x1b[F', 'pgdn': b'\x1b[6~', 'pgup': b'\x1b[5~', 'tab': b'\t'}

port, outdir, steps = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(outdir, exist_ok=True)
s = serial.Serial(port, 115200, timeout=0.05)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
lines, lock = [], threading.Lock()
stop = False


def reader():
    buf = b''
    while not stop:
        buf += s.read(65536)
        *done, buf = buf.split(b'\n')
        with lock:
            for l in done:
                t = l.decode('ascii', 'ignore').rstrip('\r')
                lines.append(t)
                if not t.startswith('FB ') and t.strip():
                    print('   |', t[:150])
threading.Thread(target=reader, daemon=True).start()


def shot(name):
    for attempt in range(3):
        with lock:
            lines.clear()
        s.write(b'\x10')
        t0 = time.time()
        while time.time() - t0 < 120:
            with lock:
                if 'FB end' in lines:
                    break
            time.sleep(0.2)
        rows = {}
        with lock:
            for l in lines:
                if l.startswith('FB ') and l != 'FB end':
                    try:
                        _, y, data = l.split(' ', 2)
                        px = []
                        for tok in data.strip(',').split(','):
                            c, _, n = tok.partition('*')
                            px += [int(c, 16)] * (int(n) if n else 1)
                        if len(px) == 320:
                            rows[int(y)] = px
                    except ValueError:
                        pass
        if len(rows) == 320:
            rgba = bytearray()
            for y in range(320):
                for c in rows[y]:
                    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
                    rgba += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255))
            write_png(os.path.join(outdir, name + '.png'), 320, 320, rgba)
            print('>>> shot', name, '%.1fs' % (time.time() - t0))
            return
        print('>>> shot', name, 'incomplete (%d rows), retrying' % len(rows))


time.sleep(5)
for st in steps:
    op, _, arg = st.partition(':')
    if op == 'k':
        s.write(KEYS[arg]); time.sleep(0.4)
    elif op == 't':
        s.write(arg.encode()); time.sleep(0.4)
    elif op == 'w':
        time.sleep(float(arg))
    elif op == 's':
        shot(arg)
stop = True; time.sleep(0.2)
