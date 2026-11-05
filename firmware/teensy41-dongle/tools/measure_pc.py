#!/usr/bin/env python3 -I
"""Measure what the PC actually receives from the Teensy bridge's mouse (the 'PC side' of the 8 kHz check).

   measure_pc.py [SECONDS] [--vid 16c0] [--pid 0487]

Reads the kernel input node of the bridge's mouse (no grab, it does not steal the pointer) and reports, per
second: events (SYN frames) per second, the spacing between frames from the kernel's own timestamps, and the
sum of the motion. Over the whole run it prints percentiles and a histogram of the spacing.
Needs read access to /dev/input/eventN (root:input 0660, or a uaccess ACL); otherwise it says so.
"""
import os, sys, struct, time, glob, select, collections, argparse

ap = argparse.ArgumentParser()
ap.add_argument('seconds', nargs='?', type=float, default=10)
ap.add_argument('--vid', default='16c0')
ap.add_argument('--pid', default='0487')
ap.add_argument('--event', help='explicit /dev/input/eventN')
a = ap.parse_args()

def find_node():
    # /proc/bus/input/devices blocks: pick the one for this VID:PID that has a 'mouse' handler (REL axes)
    txt = open('/proc/bus/input/devices').read().split('\n\n')
    for blk in txt:
        if f'vendor={a.vid} product={a.pid}' in blk.lower() and 'mouse' in blk.split('H: Handlers=')[-1].split('\n')[0]:
            for tok in blk.split('H: Handlers=')[-1].split('\n')[0].split():
                if tok.startswith('event'):
                    return '/dev/input/' + tok
    return None

node = a.event or find_node()
if not node:
    sys.exit(f'no mouse input node for {a.vid}:{a.pid} (is the Teensy bridge running?)')
try:
    fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
except PermissionError:
    sys.exit(f'cannot read {node}: permission denied. Run:  sudo setfacl -m u:{os.environ.get("USER","you")}:r {node}')

# Ask for CLOCK_MONOTONIC stamps (EVIOCSCLOCKID); fine if it fails, we only use differences.
try:
    import fcntl
    fcntl.ioctl(fd, 0x400445a0, struct.pack('i', time.CLOCK_MONOTONIC))
except Exception:
    pass

EV_SYN, EV_REL = 0, 2
REL_X, REL_Y, REL_WHEEL = 0, 1, 8
FMT = 'llHHi'
SZ = struct.calcsize(FMT)

print(f'reading {node} for {a.seconds:.0f} s ... move the mouse (or run the self-test)')
t_end = time.time() + a.seconds
last_t = None
sec_start = time.time()
frames = 0; sx = sy = 0
all_dt = []
sec_frames = []
buf = b''
dts = []
total_frames = 0; tx = ty = 0
while time.time() < t_end:
    r, _, _ = select.select([fd], [], [], 0.05)
    if r:
        try:
            buf += os.read(fd, 24 * 4096)
        except BlockingIOError:
            pass
        n = len(buf) // SZ
        for i in range(n):
            sec, usec, typ, code, val = struct.unpack_from(FMT, buf, i * SZ)
            if typ == EV_REL:
                if code == REL_X: sx += val
                elif code == REL_Y: sy += val
            elif typ == EV_SYN and code == 0:
                t = sec * 1_000_000 + usec
                if last_t is not None:
                    dts.append(t - last_t)
                last_t = t
                frames += 1
        buf = buf[n * SZ:]
    now = time.time()
    if now - sec_start >= 1.0:
        w = now - sec_start
        print(f'  {frames / w:7.0f} frames/s   motion x={sx:+d} y={sy:+d}')
        sec_frames.append(frames / w)
        total_frames += frames; tx += sx; ty += sy
        all_dt.extend(dts); dts = []
        frames = 0; sx = sy = 0
        sec_start = now

if all_dt:
    all_dt.sort()
    def pct(p): return all_dt[min(len(all_dt) - 1, int(len(all_dt) * p))]
    edges = [(0, 60), (60, 100), (100, 150), (150, 250), (250, 500), (500, 1000), (1000, 2000), (2000, 10**9)]
    names = ['<60us', '60-100', '100-150', '150-250', '250-500', '0.5-1ms', '1-2ms', '>2ms']
    print(f'\nframes {total_frames}   net motion x={tx:+d} y={ty:+d}')
    print(f'frames/s: best {max(sec_frames):.0f}  median {sorted(sec_frames)[len(sec_frames)//2]:.0f}')
    print(f'spacing us: p1 {pct(.01)}  p50 {pct(.5)}  p99 {pct(.99)}  max {all_dt[-1]}   (8 kHz = 125 us)')
    print('histogram: ' + '  '.join(f'{nm}:{sum(1 for d in all_dt if lo <= d < hi)}' for nm, (lo, hi) in zip(names, edges)))
else:
    print('\nno motion frames seen')
