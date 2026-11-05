#!/usr/bin/env python3 -I
"""Talk to the Teensy bridge's USB serial console.
   console.py SECONDS [command ...]   sends each command line, then prints output for SECONDS.
   e.g.  console.py 6 i          console.py 8 "r 8000" q        console.py 7 "t 5"
"""
import sys, time, glob
import serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 3
cmds = sys.argv[2:]
port = (glob.glob('/dev/ttyACM*') or ['/dev/ttyACM0'])[0]
s = serial.Serial(port, 115200, timeout=0.05)
s.dtr = True
time.sleep(0.3)
for c in cmds:
    s.write((c + "\n").encode())
    time.sleep(0.2)
end = time.time() + secs
buf = b''
while time.time() < end:
    d = s.read(4096)
    if d:
        sys.stdout.write(d.decode(errors='replace')); sys.stdout.flush()
