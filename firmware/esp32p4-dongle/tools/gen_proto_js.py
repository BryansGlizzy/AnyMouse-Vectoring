#!/usr/bin/env python3
"""Generate web/proto.js from common/amv_dongle_proto.h so the web app's constants cannot drift from the firmware.

Run from anywhere:  python3 -I firmware/esp32p4-dongle/tools/gen_proto_js.py          (writes web/proto.js)
Check only (CI):    python3 -I firmware/esp32p4-dongle/tools/gen_proto_js.py --check  (exit 1 if web/proto.js is stale)
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
HEADER = os.path.join(ROOT, "common", "amv_dongle_proto.h")
OUT = os.path.join(ROOT, "web", "proto.js")

GROUPS = [("CMD", "AMV_CMD_"), ("ST", "AMV_ST_"), ("SRC", "AMV_SRC_")]


def parse():
    vals = {}
    for m in re.finditer(r"^#define\s+(AMV_\w+)\s+(0x[0-9A-Fa-f]+|\d+(?:\.\d+)?)", open(HEADER).read(), re.M):
        v = m.group(2)
        vals[m.group(1)] = int(v, 16) if v.startswith("0x") else (float(v) if "." in v else int(v))
    return vals


def render(vals):
    out = ["// GENERATED from common/amv_dongle_proto.h by firmware/esp32p4-dongle/tools/gen_proto_js.py. Do not edit.",
           "export const P = {"]
    taken = set()
    for name, prefix in GROUPS:
        items = [(k[len(prefix):], v) for k, v in vals.items() if k.startswith(prefix)]
        taken.update(k for k in vals if k.startswith(prefix))
        out.append(f"  {name}: {{")
        out += [f"    {k}: {v}," for k, v in items]
        out.append("  },")
    for k, v in vals.items():
        if k not in taken:
            out.append(f"  {k[len('AMV_'):]}: {v},")
    out.append("};")
    out.append("")
    inv = {}
    for name, prefix in GROUPS:
        inv[name] = {v: k[len(prefix):] for k, v in vals.items() if k.startswith(prefix)}
    for name in inv:
        out.append(f"export const {name}_NAMES = {{ " + ", ".join(f"{v}: '{k}'" for v, k in inv[name].items()) + " };")
    out.append("")
    return "\n".join(out)


if __name__ == "__main__":
    text = render(parse())
    if "--check" in sys.argv:
        cur = open(OUT).read() if os.path.exists(OUT) else ""
        sys.exit(0 if cur == text else 1)
    open(OUT, "w").write(text)
    print("wrote", os.path.relpath(OUT, ROOT))
