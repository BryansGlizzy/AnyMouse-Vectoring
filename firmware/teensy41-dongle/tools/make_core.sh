#!/bin/sh
# Builds ./fw: a copy of the PlatformIO Teensy framework whose Teensy 4 core has four patches:
#   1. USB_AMV_DONGLE: CDC serial, the 8-byte mouse at bInterval 1, and a 64-byte vendor RawHID
#      interface (usage page 0xFF4D), with no keyboard, joystick or SEREMU.
#   2. USB_SERIAL_HID: mouse bInterval 2 -> 1, i.e. the mouse endpoint is polled every 125 us at
#      High-Speed (8 kHz) instead of every 250 us (4 kHz). Full-Speed hosts still see 1 ms.
#   3. usb_mouse_idle(): true when the previous mouse report has left the endpoint, so the bridge
#      can keep exactly one report in flight (lowest latency, no queue behind the host's polls).
#   4. usb_rawhid_send(): timeout 0 returns immediately when every TX buffer is still in flight.
#   5. USB_AMV_DONGLE mouse report: 16 buttons, 16-bit X/Y, 8-bit wheel and AC Pan, no report ID (8 bytes),
#      the same report the ESP32-P4 dongle sends.
#   6. usb_mouse_send_raw() / usb_mouse_set_tx_hook(): a non-blocking send that keeps one report in
#      flight, and a hook called from the USB interrupt when that report has been collected, so the
#      bridge sends from interrupts and never waits in loop().
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
marker = "#elif defined(USB_TOUCHSCREEN)"
assert s.count(marker) == 1, "USB_TOUCHSCREEN block not found"
s = s.replace(marker, """#elif defined(USB_AMV_DONGLE)
  #define VENDOR_ID             0x16C0
  #define PRODUCT_ID            0x0489
  #define DEVICE_CLASS          0xEF
  #define DEVICE_SUBCLASS       0x02
  #define DEVICE_PROTOCOL       0x01
  #define RAWHID_USAGE_PAGE     0xFF4D
  #define RAWHID_USAGE          0x0001
  #define MANUFACTURER_NAME     {'A','n','y','M','o','u','s','e'}
  #define MANUFACTURER_NAME_LEN 8
  #define PRODUCT_NAME          {'A','n','y','M','o','u','s','e',' ','P','r','o',' ','D','o','n','g','l','e'}
  #define PRODUCT_NAME_LEN      19
  #define EP0_SIZE              64
  #define NUM_ENDPOINTS         5
  #define NUM_INTERFACE         4
  #define CDC_IAD_DESCRIPTOR    1
  #define CDC_STATUS_INTERFACE  0
  #define CDC_DATA_INTERFACE    1
  #define CDC_ACM_ENDPOINT      2
  #define CDC_RX_ENDPOINT       3
  #define CDC_TX_ENDPOINT       3
  #define CDC_ACM_SIZE          16
  #define CDC_RX_SIZE_480       512
  #define CDC_TX_SIZE_480       512
  #define CDC_RX_SIZE_12        64
  #define CDC_TX_SIZE_12        64
  #define MOUSE_INTERFACE       2
  #define MOUSE_ENDPOINT        4
  #define MOUSE_SIZE            8
  #define MOUSE_INTERVAL        1
  #define RAWHID_INTERFACE      3
  #define RAWHID_TX_ENDPOINT    5
  #define RAWHID_TX_SIZE        64
  #define RAWHID_TX_INTERVAL    1
  #define RAWHID_RX_ENDPOINT    5
  #define RAWHID_RX_SIZE        64
  #define RAWHID_RX_INTERVAL    1
  #define ENDPOINT2_CONFIG      ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT3_CONFIG      ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT4_CONFIG      ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT5_CONFIG      ENDPOINT_RECEIVE_INTERRUPT + ENDPOINT_TRANSMIT_INTERRUPT

""" + marker, 1)
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

c = core / "usb_rawhid.c"
s = c.read_text()
old = "if (systick_millis_count - wait_begin_at > timeout) return 0;"
assert s.count(old) == 1, "RawHID send timeout check not found"
s = s.replace(old, "if (!timeout || systick_millis_count - wait_begin_at > timeout) return 0;", 1)
s += "\nint usb_rawhid_tx_idle(void) {\n#ifdef RAWHID_INTERFACE\n    for (int i = 0; i < TX_NUM; ++i) if (usb_transfer_status(tx_transfer + i) & 0x80) return 0;\n#endif\n    return 1;\n}\n"
c.write_text(s)

d = core / "usb_desc.c"
s2 = d.read_text()
anchor = "static uint8_t mouse_report_desc[] = {"
assert s2.count(anchor) == 1, "mouse_report_desc not found"
s2 = s2.replace(anchor, """#if defined(USB_AMV_DONGLE)
// AnyMouse: 16 buttons, 16-bit X/Y, 8-bit wheel and AC Pan, no report ID (as the ESP32-P4 dongle)
static uint8_t mouse_report_desc[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01, 0x81, 0x02,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
        0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
        0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,
        0xC0, 0xC0,
};
#else
""" + anchor, 1)
# close the #else after the original descriptor (ends with the first "};" after the anchor)
i = s2.index(anchor, s2.index("#else\n" + anchor)) 
j = s2.index("};", i) + 2
s2 = s2[:j] + "\n#endif // USB_AMV_DONGLE" + s2[j:]
d.write_text(s2)

c = core / "usb_mouse.c"
s3 = c.read_text()
old3 = "usb_config_tx(MOUSE_ENDPOINT, MOUSE_SIZE, 0, NULL);"
assert s3.count(old3) == 1
s3 = s3.replace(old3, "usb_config_tx(MOUSE_ENDPOINT, MOUSE_SIZE, 0, amv_mouse_tx_done);", 1)
old3 = "void usb_mouse_configure(void)"
s3 = s3.replace(old3, """// AnyMouse patch: transmit-complete hook (runs in the USB interrupt) and a non-blocking send that keeps
// exactly one report in flight. Safe to call from any interrupt or from loop().
static void (*amv_tx_hook)(void);
static void amv_mouse_tx_done(transfer_t *t) { (void)t; if (amv_tx_hook) amv_tx_hook(); }
void usb_mouse_set_tx_hook(void (*hook)(void)) { amv_tx_hook = hook; }
int usb_mouse_send_raw(const uint8_t *data, uint32_t len)
{
        uint32_t primask;
        __asm__ volatile("mrs %0, primask" : "=r" (primask));
        __disable_irq();
        int ret = -1;
        if (usb_configuration && len <= TX_BUFSIZE) {
                uint32_t prev = tx_head ? tx_head - 1 : TX_NUM - 1;
                transfer_t *xfer = tx_transfer + tx_head;
                if (!(usb_transfer_status(tx_transfer + prev) & 0x80) && !(usb_transfer_status(xfer) & 0x80)) {
                        uint8_t *buffer = txbuffer + tx_head * TX_BUFSIZE;
                        memcpy(buffer, data, len);
                        usb_prepare_transfer(xfer, buffer, len, 0);
                        arm_dcache_flush_delete(buffer, TX_BUFSIZE);
                        usb_transmit(MOUSE_ENDPOINT, xfer);
                        if (++tx_head >= TX_NUM) tx_head = 0;
                        ret = 0;
                }
        }
        if (!primask) __enable_irq();
        return ret;
}

""" + old3, 1)
c.write_text(s3)

hh = core / "usb_mouse.h"
s4 = hh.read_text()
s4 = s4.replace("int usb_mouse_idle(void);", "int usb_mouse_idle(void);\nint usb_mouse_send_raw(const uint8_t *data, uint32_t len);\nvoid usb_mouse_set_tx_hook(void (*hook)(void));", 1)
assert "usb_mouse_send_raw" in s4
hh.write_text(s4)
print("patched teensy4 core")
PY
