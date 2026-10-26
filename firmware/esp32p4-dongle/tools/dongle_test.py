#!/usr/bin/env python3
"""
Bidirectional test of the AnyMouse dongle's vendor-HID protocol. No mouse needed.

Talks to the dongle over its 0xFF4D vendor interface and checks:
  host -> device : SET_WEIGHTS (valid / invalid / stale), bypass, profile slots, factory reset
  device -> host : EVENT_STATE (host-, device- and load-originated changes)
  numerics       : TEST_SCALE (the real firmware scaling) against an independent reference
  persistence    : auto-save and slot restore across a reboot

Needs `pip install hidapi`. Protocol constants are parsed from common/amv_dongle_proto.h so
this script cannot drift from the firmware. Run:  python3 -I tools/dongle_test.py
"""
import math
import os
import re
import random
import statistics
import struct
import sys
import time

import hid

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.normpath(os.path.join(HERE, "..", "..", "..", "common", "amv_dongle_proto.h"))

# ---- constants straight from the C header ----
C = {}
for m in re.finditer(r"^#define\s+(AMV_\w+)\s+(0x[0-9A-Fa-f]+|\d+(?:\.\d+)?)", open(HEADER).read(), re.M):
    v = m.group(2)
    C[m.group(1)] = int(v, 16) if v.startswith("0x") else (float(v) if "." in v else int(v))

VID, PID = C["AMV_USB_VID"], C["AMV_USB_PID"]
HDR = struct.Struct("<8B")          # magic0, magic1, ver, cmd, seq, status, len, rsvd
STATE = struct.Struct("<I8fBB")     # gen, w[8], bypass, active_slot (38 bytes)
EVENT = struct.Struct("<I8fBBB")    # state + source (39 bytes)


class Dongle:
    def __init__(self):
        self.h = None
        self.seq = 0
        self.events = []
        self.open()

    def open(self, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            for d in hid.enumerate(VID, PID):
                if d["usage_page"] == C["AMV_VENDOR_USAGE_PAGE"]:
                    self.h = hid.device()
                    self.h.open_path(d["path"])
                    self.h.set_nonblocking(True)
                    self.drain()
                    return
            time.sleep(0.25)
        raise RuntimeError("dongle vendor interface not found")

    def drain(self):
        while self.h.read(64):
            pass
        self.events.clear()

    def close(self):
        if self.h:
            self.h.close()
            self.h = None

    def _frame(self, cmd, payload=b""):
        self.seq = (self.seq + 1) & 0xFF or 1
        f = HDR.pack(C["AMV_MAGIC0"], C["AMV_MAGIC1"], C["AMV_PROTO_VERSION"], cmd, self.seq, 0, len(payload), 0)
        return (f + payload).ljust(C["AMV_FRAME_SIZE"], b"\0")

    def _poll(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            r = self.h.read(64)
            if r:
                return bytes(r)
            time.sleep(0.0005)
        return None

    def request(self, cmd, payload=b"", timeout=1.0, raw=None):
        frame = raw if raw is not None else self._frame(cmd, payload)
        seq = frame[4]
        self.h.write(b"\x00" + frame)
        end = time.time() + timeout
        while time.time() < end:
            r = self._poll(end - time.time())
            if r is None:
                break
            m0, m1, ver, rcmd, rseq, status, ln, _ = HDR.unpack_from(r)
            body = r[C["AMV_HEADER_SIZE"]:C["AMV_HEADER_SIZE"] + ln]
            if rcmd == C["AMV_CMD_EVENT_STATE"]:
                self.events.append(EVENT.unpack(body))
                continue
            if rseq == seq:
                return status, body
        raise TimeoutError(f"no reply to cmd 0x{cmd:02X}")

    def wait_event(self, pred, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            for e in self.events:
                if pred(e):
                    self.events.remove(e)
                    return e
            r = self._poll(0.05)
            if r:
                m = HDR.unpack_from(r)
                if m[3] == C["AMV_CMD_EVENT_STATE"]:
                    self.events.append(EVENT.unpack(r[8:8 + m[6]]))
        return None

    # ---- typed helpers ----
    def info(self):
        s, b = self.request(C["AMV_CMD_GET_INFO"])
        assert s == 0
        v = struct.unpack("<BBBBHBB6sI", b[:18])
        return dict(proto=v[0], fw=v[1:4], chip_rev=v[4], slots=v[5], has_mouse=v[6], mac=v[7].hex(":"), uptime_ms=v[8])

    def state(self, status_body=None):
        if status_body is None:
            s, b = self.request(C["AMV_CMD_GET_WEIGHTS"])
            assert s == 0, s
        else:
            b = status_body
        v = STATE.unpack(b[:STATE.size])
        return dict(gen=v[0], w=list(v[1:9]), bypass=v[9], slot=v[10])

    def set_weights(self, w, base_gen=None):
        if base_gen is None:
            base_gen = self.state()["gen"]
        s, b = self.request(C["AMV_CMD_SET_WEIGHTS"], struct.pack("<I8f", base_gen, *w))
        return s, (self.state(b) if len(b) >= STATE.size else None)

    def slots_mask(self):
        s, b = self.request(C["AMV_CMD_GET_SLOTS"])
        assert s == 0 and len(b) >= 1
        return b[0]

    def stats(self):
        s, b = self.request(C["AMV_CMD_GET_STATS"])
        return dict(zip(["sets_ok", "stale", "bad_value", "saves", "loads", "events_sent", "events_dropped", "test_scales"],
                        struct.unpack("<8I", b[:32])))

    def test_scale(self, dx, dy, reset=False):
        s, b = self.request(C["AMV_CMD_TEST_SCALE"], struct.pack("<hhB", dx, dy, C["AMV_TEST_RESET_REM"] if reset else 0))
        assert s == 0, s
        ox, oy, rx, ry, octant = struct.unpack("<iiffB", b[:17])
        return ox, oy, rx, ry, octant


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def ref_scale(w, rem, dx, dy):
    """Independent float64 implementation of the README section 4 algorithm."""
    if dx == 0 and dy == 0:
        return 0, 0, 0
    th = math.atan2(-dy, dx)
    if th < 0:
        th += 2 * math.pi
    pos = th * 4 / math.pi
    k0 = int(pos) & 7
    k1 = (k0 + 1) & 7
    t = pos - int(pos)
    sm = (1 - math.cos(t * math.pi)) / 2
    scale = w[k0] * (1 - sm) + w[k1] * sm
    ex, ey = dx * scale + rem[0], dy * scale + rem[1]
    ix, iy = int(ex), int(ey)
    rem[0], rem[1] = ex - ix, ey - iy
    return ix, iy, int(pos + 0.5) & 7


# ---------------- tests ----------------
results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""))
    return bool(cond)


def main():
    d = Dongle()
    rnd = random.Random(1234)
    NEUTRAL = [1.0] * 8

    print("== info ==")
    i = d.info()
    print("  ", i)
    check("proto version matches header", i["proto"] == C["AMV_PROTO_VERSION"])
    check("MAC is the board's efuse MAC", i["mac"] == "30:ed:a0:ea:6c:ab", i["mac"])
    check("chip rev reported", i["chip_rev"] > 0, f"{i['chip_rev']}")

    print("== HELLO handshake (identity is proven by the protocol, not by VID:PID) ==")
    nonce = bytes(rnd.randrange(256) for _ in range(C["AMV_HELLO_NONCE_LEN"]))
    s, b = d.request(C["AMV_CMD_HELLO"], nonce)
    check("HELLO ok", s == 0, f"status {s}")
    ident, rn, pv, fa, fb, fc, caps, hmac = struct.unpack("<I8sBBBBH6s", b[:24])
    check("identity word is 'AMVD'", ident == C["AMV_HELLO_IDENT"] and b[:4] == b"AMVD", f"{ident:#x}")
    check("nonce echoed", rn == nonce)
    check("proto version and fw agree with GET_INFO", pv == i["proto"] and (fa, fb, fc) == tuple(i["fw"]))
    check("vectored-mouse capability set", bool(caps & C["AMV_CAP_VECTORED_MOUSE"]), f"caps {caps:#x}")
    check("HELLO MAC matches GET_INFO", hmac.hex(":") == i["mac"])
    nonce2 = bytes(rnd.randrange(256) for _ in range(C["AMV_HELLO_NONCE_LEN"]))
    s, b2 = d.request(C["AMV_CMD_HELLO"], nonce2)
    check("a fresh nonce is echoed fresh", s == 0 and b2[4:12] == nonce2 != nonce)
    s, _ = d.request(C["AMV_CMD_HELLO"], b"\x01\x02")
    check("short HELLO rejected", s == C["AMV_ST_BAD_LEN"], f"status {s}")

    print("== framing errors ==")
    bad = bytearray(d._frame(C["AMV_CMD_GET_WEIGHTS"]))
    bad[0] = 0x00
    s, _ = d.request(0, raw=bytes(bad))
    check("bad magic rejected", s == C["AMV_ST_BAD_MAGIC"], f"status {s}")
    s, _ = d.request(0x7E)
    check("unknown command rejected", s == C["AMV_ST_BAD_CMD"], f"status {s}")
    s, _ = d.request(C["AMV_CMD_SET_WEIGHTS"], b"\x01\x02\x03")
    check("short SET_WEIGHTS rejected", s == C["AMV_ST_BAD_LEN"], f"status {s}")

    print("== factory reset -> neutral ==")
    s, b = d.request(C["AMV_CMD_FACTORY_RESET"])
    st = d.state(b)
    check("factory reset ok", s == 0)
    check("weights neutral after reset", st["w"] == NEUTRAL and st["slot"] == 0 and not st["bypass"])
    s, _ = d.request(C["AMV_CMD_LOAD_PROFILE"], bytes([3]))
    check("empty slot reports EMPTY", s == C["AMV_ST_EMPTY"], f"status {s}")
    check("no slots filled after factory reset", d.slots_mask() == 0, f"mask {d.slots_mask():#x}")
    d.drain()

    print("== host -> device: SET_WEIGHTS ==")
    w1 = [f32(rnd.uniform(0.5, 1.5)) for _ in range(8)]
    g0 = d.state()["gen"]
    s, st = d.set_weights(w1, g0)
    check("set accepted", s == 0)
    check("gen incremented", st and st["gen"] == g0 + 1, f"{g0} -> {st and st['gen']}")
    check("readback equals sent (float32 exact)", d.state()["w"] == w1)
    ev = d.wait_event(lambda e: e[0] == g0 + 1)
    check("event emitted for host change", ev is not None and ev[-1] == C["AMV_SRC_HOST"] and list(ev[1:9]) == w1)

    print("== invalid values ==")
    gen_before = d.state()["gen"]
    for label, val in [("zero", 0.0), ("too big", 6.0), ("negative", -1.0), ("NaN", float("nan")), ("inf", float("inf"))]:
        w = list(w1)
        w[3] = val
        s, _ = d.set_weights(w, gen_before)
        check(f"reject {label}", s == C["AMV_ST_BAD_VALUE"], f"status {s}")
    check("state untouched by rejects", d.state()["w"] == w1 and d.state()["gen"] == gen_before)

    print("== stale generation ==")
    s, st = d.set_weights(NEUTRAL, gen_before - 1)
    check("stale rejected", s == C["AMV_ST_STALE"], f"status {s}")
    check("stale reply carries current state", st and st["gen"] == gen_before and st["w"] == w1)
    s, st = d.set_weights(w1, C["AMV_GEN_ANY"])
    check("GEN_ANY applies regardless", s == 0)

    print("== device -> host: self-originated change ==")
    d.drain()
    cur = d.state()
    s, _ = d.request(C["AMV_CMD_TEST_SELF_CHANGE"], struct.pack("<BfH", 5, 2.5, 300))
    check("self-change scheduled", s == 0)
    ev = d.wait_event(lambda e: e[-1] == C["AMV_SRC_DEVICE"], timeout=2.0)
    check("device-originated event received", ev is not None)
    if ev:
        check("event carries the new weight", abs(ev[1 + 5] - 2.5) < 1e-6 and ev[0] > cur["gen"], f"gen {ev[0]}, w5={ev[6]:.3f}")
        check("host readback matches event", d.state()["w"] == list(ev[1:9]))

    print("== bypass ==")
    d.set_weights([0.5] * 8, C["AMV_GEN_ANY"])
    ox, oy, *_ = d.test_scale(100, 0, reset=True)
    check("scaled output at weight 0.5", ox == 50 and oy == 0, f"{ox},{oy}")
    s, b = d.request(C["AMV_CMD_SET_BYPASS"], b"\x01")
    st = d.state(b)
    check("bypass on reported", st["bypass"] == 1 and st["w"] == [0.5] * 8, "raw weights preserved")
    ox, oy, *_ = d.test_scale(100, 0, reset=True)
    check("bypass gives identity (weight 1.0)", ox == 100 and oy == 0, f"{ox},{oy}")
    d.request(C["AMV_CMD_SET_BYPASS"], b"\x00")
    ox, *_ = d.test_scale(100, 0, reset=True)
    check("bypass off restores scaling", ox == 50)

    print("== scaling numerics vs independent reference ==")
    w = [f32(rnd.uniform(0.4, 1.8)) for _ in range(8)]
    d.set_weights(w, C["AMV_GEN_ANY"])
    rem = [0.0, 0.0]
    exact = n = 0
    dev_sum = [0, 0]
    ref_sum = [0, 0]
    octant_ok = True
    max_cum_err = 0
    first = True
    steps = [(rnd.randint(-40, 40), rnd.randint(-40, 40)) for _ in range(250)]
    steps += [(1, 0), (0, 1), (-1, 0), (0, -1), (1, 1), (-1, 1), (1, -1), (-1, -1)] * 4   # tiny moves exercise the remainder
    for dx, dy in steps:
        ox, oy, _, _, oc = d.test_scale(dx, dy, reset=first)
        first = False
        rx, ry, roc = ref_scale(w, rem, dx, dy)
        n += 1
        exact += (ox == rx and oy == ry)
        dev_sum[0] += ox; dev_sum[1] += oy
        ref_sum[0] += rx; ref_sum[1] += ry
        max_cum_err = max(max_cum_err, abs(dev_sum[0] - ref_sum[0]), abs(dev_sum[1] - ref_sum[1]))
        if (dx or dy) and oc != roc:
            octant_ok = False
    check("per-step output matches reference", exact / n > 0.98, f"{exact}/{n} exact")
    check("cumulative drift vs reference <= 1 count", max_cum_err <= 1, f"max {max_cum_err}")
    check("octant tag matches reference", octant_ok)

    print("== profile slots ==")
    wa = [f32(rnd.uniform(0.6, 1.4)) for _ in range(8)]
    wb = [f32(rnd.uniform(0.6, 1.4)) for _ in range(8)]
    d.set_weights(wa, C["AMV_GEN_ANY"])
    s, b = d.request(C["AMV_CMD_SAVE_PROFILE"], bytes([2]))
    check("save slot 2", s == 0 and d.state(b)["slot"] == 2)
    check("slot mask shows slot 2 only", d.slots_mask() == 0b0100, f"mask {d.slots_mask():#x}")
    d.set_weights(wb, C["AMV_GEN_ANY"])
    d.drain()
    s, b = d.request(C["AMV_CMD_LOAD_PROFILE"], bytes([2]))
    check("load slot 2 restores saved weights", s == 0 and d.state(b)["w"] == wa)
    ev = d.wait_event(lambda e: e[-1] == C["AMV_SRC_LOAD"])
    check("load emits event", ev is not None and list(ev[1:9]) == wa)
    s, _ = d.request(C["AMV_CMD_SAVE_PROFILE"], bytes([9]))
    check("out-of-range slot rejected", s == C["AMV_ST_BAD_SLOT"], f"status {s}")
    s, b = d.request(C["AMV_CMD_SAVE_PROFILE"], bytes([9]))
    cur = d.state()
    check("refusal carries the real current state (no stale stack bytes)", d.state(b) == cur)
    s, _ = d.request(C["AMV_CMD_LOAD_PROFILE"], bytes([9]))
    check("out-of-range load rejected", s == C["AMV_ST_BAD_SLOT"], f"status {s}")

    print("== latency of a control round trip ==")
    lat = []
    for _ in range(100):
        t = time.perf_counter()
        d.request(C["AMV_CMD_GET_WEIGHTS"])
        lat.append((time.perf_counter() - t) * 1000)
    print(f"   median {statistics.median(lat):.2f} ms, p95 {sorted(lat)[94]:.2f} ms, max {max(lat):.2f} ms")
    check("round trips complete (100/100)", len(lat) == 100)

    print("== persistence: auto-save, then reboot ==")
    wp = [f32(rnd.uniform(0.7, 1.3)) for _ in range(8)]
    d.request(C["AMV_CMD_LOAD_PROFILE"], bytes([1]))   # slot 1 is empty: expect EMPTY, no change
    d.request(C["AMV_CMD_SAVE_PROFILE"], bytes([1]))   # make slot 1 the active slot
    d.set_weights(wp, C["AMV_GEN_ANY"])
    gen_pre = d.state()["gen"]
    time.sleep(2.6)                                    # debounce is 2 s
    d.request(C["AMV_CMD_REBOOT"])
    d.close()
    time.sleep(1.0)
    d = Dongle()
    st = d.state()
    check("weights survive reboot (auto-save)", st["w"] == wp)
    check("active slot survives reboot", st["slot"] == 1, f"slot {st['slot']}")
    check("bypass is not persisted", st["bypass"] == 0)
    check("slot mask survives reboot (slots 1 and 2)", d.slots_mask() == 0b0110, f"mask {d.slots_mask():#x}")
    print(f"   gen before reboot {gen_pre}, after {st['gen']}")

    print("== cleanup ==")
    d.request(C["AMV_CMD_FACTORY_RESET"])
    check("left at factory defaults", d.state()["w"] == NEUTRAL and d.slots_mask() == 0)
    stats = d.stats()
    print("   stats:", stats)
    check("no events dropped", stats["events_dropped"] == 0)
    d.close()

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    if failed:
        print("FAILED:", *failed, sep="\n  - ")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
