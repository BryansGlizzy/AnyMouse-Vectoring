#!/usr/bin/env python3
"""Record real request/reply frames from the dongle into web/tests/fixtures/frames.json.

The web app's protocol client and its JavaScript demo device are tested against these
recordings (web/tests/protocol.test.mjs), so they are checked against the real firmware, not
against assumptions. Needs the dongle connected and `pip install hidapi`. Leaves the dongle at
factory defaults.

Run:  python3 -I firmware/esp32p4-dongle/tools/capture_fixtures.py
"""
import json
import os
import re
import struct
import time

import hid

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
C = {}
for m in re.finditer(r"^#define\s+(AMV_\w+)\s+(0x[0-9A-Fa-f]+|\d+(?:\.\d+)?)", open(os.path.join(ROOT, "common", "amv_dongle_proto.h")).read(), re.M):
    v = m.group(2)
    C[m.group(1)] = int(v, 16) if v.startswith("0x") else (float(v) if "." in v else int(v))

h = None
for d in hid.enumerate(C["AMV_USB_VID"], C["AMV_USB_PID"]):
    if d["usage_page"] == C["AMV_VENDOR_USAGE_PAGE"]:
        h = hid.device(); h.open_path(d["path"]); h.set_nonblocking(True)
if h is None:
    raise SystemExit("dongle vendor interface not found")
while h.read(64):
    pass

seq = 0
steps = []


def frame(cmd, payload):
    global seq
    seq += 1
    f = bytes([C["AMV_MAGIC0"], C["AMV_MAGIC1"], C["AMV_PROTO_VERSION"], cmd, seq, 0, len(payload), 0]) + payload
    return f.ljust(64, b"\0")


def xchg(name, cmd, payload=b"", volatile=False):
    req = frame(cmd, payload)
    h.write(b"\x00" + req)
    reply, events = None, []
    end = time.time() + 1.0
    while time.time() < end:
        r = h.read(64)
        if not r:
            time.sleep(0.001)
            continue
        r = bytes(r)
        if r[3] == C["AMV_CMD_EVENT_STATE"]:
            events.append(r.hex())
        elif r[4] == req[4]:
            reply = r.hex()
            end = time.time() + 0.04     # collect events that follow the reply
    assert reply, f"no reply for {name}"
    steps.append(dict(name=name, request=req.hex(), reply=reply, events=events, volatile=volatile))
    return bytes.fromhex(reply)


def state(reply):
    v = struct.unpack("<I8fBB", reply[8:8 + 38])
    return dict(gen=v[0], w=list(v[1:9]), bypass=bool(v[9]), slot=v[10])


def slots_mask():
    r = xchg("probe_slots", C["AMV_CMD_GET_SLOTS"])
    steps.pop()
    return r[8]


pre = state(xchg("pre_state", C["AMV_CMD_GET_WEIGHTS"]))
steps.pop()
pre["filled_slots"] = [i for i in range(4) if slots_mask() >> i & 1]

xchg("factory_reset", C["AMV_CMD_FACTORY_RESET"])
g = state(xchg("get_weights", C["AMV_CMD_GET_WEIGHTS"]))["gen"]
xchg("get_slots_empty", C["AMV_CMD_GET_SLOTS"])
W = [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5]      # exactly representable in float32
t_first_set = time.time()
xchg("set_weights_ok", C["AMV_CMD_SET_WEIGHTS"], struct.pack("<I8f", g, *W))
xchg("set_weights_stale", C["AMV_CMD_SET_WEIGHTS"], struct.pack("<I8f", g, *W))
bad = list(W); bad[3] = 6.0
xchg("set_weights_bad_value", C["AMV_CMD_SET_WEIGHTS"], struct.pack("<I8f", g + 1, *bad))
xchg("set_weights_bad_len", C["AMV_CMD_SET_WEIGHTS"], b"\x01\x02\x03")
xchg("bypass_on", C["AMV_CMD_SET_BYPASS"], b"\x01")
xchg("scale_bypassed", C["AMV_CMD_TEST_SCALE"], struct.pack("<hhB", 100, 0, 1))
xchg("bypass_off", C["AMV_CMD_SET_BYPASS"], b"\x00")
for i, (dx, dy) in enumerate([(100, 0), (0, -100), (37, -23), (1, 1), (1, 1), (-9, 14), (3, 0)]):
    xchg(f"scale_{i}", C["AMV_CMD_TEST_SCALE"], struct.pack("<hhB", dx, dy, 1 if i == 0 else 0))
xchg("save_slot_2", C["AMV_CMD_SAVE_PROFILE"], bytes([2]))
xchg("get_slots_one", C["AMV_CMD_GET_SLOTS"])
xchg("load_empty_slot", C["AMV_CMD_LOAD_PROFILE"], bytes([3]))
xchg("set_weights_again", C["AMV_CMD_SET_WEIGHTS"], struct.pack("<I8f", g + 99, *W))   # stale on purpose
xchg("load_slot_2", C["AMV_CMD_LOAD_PROFILE"], bytes([2]))
xchg("bad_slot", C["AMV_CMD_SAVE_PROFILE"], bytes([9]))
xchg("bad_command", 0x7E)
# The firmware auto-saves the active slot 2 s after the last weight change; the recording must finish first.
assert time.time() - t_first_set < 1.8, "recording too slow: the auto-save would pollute the slot mask"
xchg("info", C["AMV_CMD_GET_INFO"], volatile=True)
xchg("stats", C["AMV_CMD_GET_STATS"], volatile=True)
xchg("factory_reset_end", C["AMV_CMD_FACTORY_RESET"])

out = os.path.join(ROOT, "web", "tests", "fixtures", "frames.json")
os.makedirs(os.path.dirname(out), exist_ok=True)
json.dump(dict(pre=pre, steps=steps), open(out, "w"), indent=1)
print(f"recorded {len(steps)} exchanges ->", os.path.relpath(out, ROOT))
