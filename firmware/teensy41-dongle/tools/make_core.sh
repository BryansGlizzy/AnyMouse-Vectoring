#!/bin/sh
# Builds ./fw: a copy of the PlatformIO Teensy framework whose Teensy 4 core has two patches:
#   1. USB_SERIAL_HID: mouse bInterval 2 -> 1, i.e. the mouse endpoint is polled every 125 us at
#      High-Speed (8 kHz) instead of every 250 us (4 kHz). Full-Speed hosts still see 1 ms.
#   2. usb_mouse_idle(): true when the previous mouse report has left the endpoint, so the bridge
#      can keep exactly one report in flight (lowest latency, no queue behind the host's polls).
# Everything else in ./fw is a symlink to the installed framework. Safe to re-run.
set -e
cd "$(dirname "$0")/.."
SRC="${PLATFORMIO_FRAMEWORK:-$HOME/.platformio/packages/framework-arduinoteensy}"
[ -d "$SRC/cores/teensy4" ] || { echo "framework not found: $SRC (run a pio build once)"; exit 1; }
rm -rf fw
mkdir -p fw/cores
for e in "$SRC"/*; do
  n=$(basename "$e"); [ "$n" = cores ] && continue
  ln -s "$e" "fw/$n"
done
for c in "$SRC"/cores/*; do
  n=$(basename "$c"); [ "$n" = teensy4 ] && continue
  ln -s "$c" "fw/cores/$n"
done
cp -r "$SRC/cores/teensy4" fw/cores/teensy4
python3 -I - <<'PY'
import re, pathlib
core = pathlib.Path("fw/cores/teensy4")

h = core / "usb_desc.h"
s = h.read_text()
a = s.index("#elif defined(USB_SERIAL_HID)")
b = s.index("#elif", a + 10)
blk = s[a:b]
new, n = re.subn(r"(#define MOUSE_INTERVAL\s+)2", r"\g<1>1", blk)
assert n == 1, "MOUSE_INTERVAL not found in USB_SERIAL_HID block"
h.write_text(s[:a] + new + s[b:])

c = core / "usb_mouse.c"
s = c.read_text()
marker = "// Set the mouse buttons."
assert marker in s
s = s.replace(marker, """// AnyMouse patch: nonzero when the last report handed to the endpoint has been collected by the host.
int usb_mouse_idle(void)
{
        if (!usb_configuration) return 0;
        uint32_t prev = tx_head ? tx_head - 1 : TX_NUM - 1;
        return !(usb_transfer_status(tx_transfer + prev) & 0x80);
}

""" + marker, 1)
c.write_text(s)

hh = core / "usb_mouse.h"
s = hh.read_text()
s = s.replace("int usb_mouse_move(int8_t x, int8_t y, int8_t wheel, int8_t horiz);",
              "int usb_mouse_move(int8_t x, int8_t y, int8_t wheel, int8_t horiz);\nint usb_mouse_idle(void);", 1)
assert "usb_mouse_idle" in s
hh.write_text(s)
print("patched teensy4 core")
PY
