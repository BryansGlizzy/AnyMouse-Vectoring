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
  #define NUM_ENDPOINTS         7
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
  #define GHOST0_INTERFACE      4
  #define GHOST0_ENDPOINT       6
  #define GHOST1_INTERFACE      5
  #define GHOST1_ENDPOINT       7
  #define ENDPOINT2_CONFIG      ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT3_CONFIG      ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT4_CONFIG      ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT5_CONFIG      ENDPOINT_RECEIVE_INTERRUPT + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT6_CONFIG      ENDPOINT_RECEIVE_INTERRUPT + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT7_CONFIG      ENDPOINT_RECEIVE_INTERRUPT + ENDPOINT_TRANSMIT_INTERRUPT

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
# AnyMouse Ghost Mode: expose base descriptors and enlarge usb_descriptor_buffer for cloned HID report descriptors
assert s2.count("uint8_t usb_descriptor_buffer[CONFIG_DESC_SIZE];") == 1
s2 = s2.replace("uint8_t usb_descriptor_buffer[CONFIG_DESC_SIZE];",
                "uint8_t usb_descriptor_buffer[CONFIG_DESC_SIZE < 1024 ? 1024 : CONFIG_DESC_SIZE];", 1)
pwr_old = """        0xC0,                                   // bmAttributes
        50,                                     // bMaxPower"""
pwr_new = """#ifdef USB_AMV_DONGLE
        0x80,                                   // bmAttributes (bus-powered)
        250,                                    // bMaxPower (500 mA USB 2.0 max)
#else
        0xC0,                                   // bmAttributes
        50,                                     // bMaxPower
#endif"""
assert s2.count(pwr_old) == 2, f"expected 2 bMaxPower blocks, found {s2.count(pwr_old)}"
s2 = s2.replace(pwr_old, pwr_new)
assert s2.count("#endif // NUM_ENDPOINTS") == 1
s2 = s2.replace("#endif // NUM_ENDPOINTS", """const uint8_t *usb_base_device_descriptor(void) { return device_descriptor; }
uint16_t usb_base_config_desc_len(void) { return CONFIG_DESC_SIZE; }
#endif // NUM_ENDPOINTS""", 1)
d.write_text(s2)

# AnyMouse Ghost Mode hooks in usb.c
u = core / "usb.c"
su = u.read_text()
hook_decls = """#ifdef GHOST0_INTERFACE
int usb_ghost_get_descriptor(uint16_t wValue, uint16_t wIndex, uint8_t high_speed, const uint8_t **data, uint32_t *len);
void usb_ghost_configure(void);
int usb_ghost_ep0_setup(uint16_t wRequestAndType, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint32_t seq);
void usb_ghost_ep0_set_complete(uint16_t wValue, uint16_t wIndex, uint16_t wLength);
static volatile uint32_t ghost_ep0_seq = 0;
#endif
static uint8_t reply_buffer[8];"""
assert su.count("static uint8_t reply_buffer[8];") == 1
su = su.replace("static uint8_t reply_buffer[8];", hook_decls, 1)

setup_hdr = """\tsetup.bothwords = setupdata;
\t#ifdef GHOST0_INTERFACE
\tghost_ep0_seq++;
\t#endif"""
assert su.count("\tsetup.bothwords = setupdata;") == 1
su = su.replace("\tsetup.bothwords = setupdata;", setup_hdr, 1)

rawhid_cfg = """\t\t#if defined(RAWHID_INTERFACE)
\t\tusb_rawhid_configure();
\t\t#endif
\t\t#if defined(GHOST0_INTERFACE)
\t\tusb_ghost_configure();
\t\t#endif"""
assert su.count("\t\t#if defined(RAWHID_INTERFACE)\n\t\tusb_rawhid_configure();\n\t\t#endif") == 1
su = su.replace("\t\t#if defined(RAWHID_INTERFACE)\n\t\tusb_rawhid_configure();\n\t\t#endif", rawhid_cfg, 1)

get_desc_hdr = """\t  case 0x0680: // GET_DESCRIPTOR
\t  case 0x0681:
\t\t#ifdef GHOST0_INTERFACE
\t\t{
\t\t\tconst uint8_t *gsrc = NULL;
\t\t\tuint32_t glen = 0;
\t\t\tif (usb_ghost_get_descriptor(setup.wValue, setup.wIndex, usb_high_speed, &gsrc, &glen)) {
\t\t\t\tif (glen > setup.wLength) glen = setup.wLength;
\t\t\t\tif (glen > 1024) glen = 1024;
\t\t\t\tmemcpy(usb_descriptor_buffer, gsrc, glen);
\t\t\t\tarm_dcache_flush_delete(usb_descriptor_buffer, glen);
\t\t\t\tendpoint0_transmit(usb_descriptor_buffer, glen, 0);
\t\t\t\treturn;
\t\t\t}
\t\t}
\t\t#endif"""
assert su.count("\t  case 0x0680: // GET_DESCRIPTOR\n\t  case 0x0681:") == 1
su = su.replace("\t  case 0x0680: // GET_DESCRIPTOR\n\t  case 0x0681:", get_desc_hdr, 1)

stall_anchor = "\t}\n\tprintf(\"endpoint 0 stall\\n\");"
assert su.count(stall_anchor) == 1
ghost_ep0_cases = """#if defined(GHOST0_INTERFACE) && !defined(SEREMU_INTERFACE) && !defined(KEYBOARD_INTERFACE) && !defined(MULTITOUCH_INTERFACE)
\t  case 0x0A21: // HID SET_IDLE
\t  case 0x0B21: // HID SET_PROTOCOL
\t\tif (setup.wIndex == GHOST0_INTERFACE || setup.wIndex == GHOST1_INTERFACE) {
\t\t\tendpoint0_receive(NULL, 0, 0);
\t\t\treturn;
\t\t}
\t\tbreak;
\t  case 0x0921: // HID SET_REPORT
\t  case 0x01A1: // HID GET_REPORT
\t\tif (usb_ghost_ep0_setup(setup.wRequestAndType, setup.wValue, setup.wIndex, setup.wLength, ghost_ep0_seq)) {
\t\t\treturn;
\t\t}
\t\tbreak;
#endif
""" + stall_anchor
su = su.replace(stall_anchor, ghost_ep0_cases, 1)

complete_anchor = "\tsetup.bothwords = endpoint0_setupdata.bothwords;"
assert su.count(complete_anchor) == 1
su = su.replace(complete_anchor, complete_anchor + """
#ifdef GHOST0_INTERFACE
\tif (setup.wRequestAndType == 0x0921 && (setup.wIndex == GHOST0_INTERFACE || setup.wIndex == GHOST1_INTERFACE)) {
\t\tusb_ghost_ep0_set_complete(setup.wValue, setup.wIndex, setup.wLength);
\t}
#endif""", 1)

ep0_helpers = """
#ifdef GHOST0_INTERFACE
void usb_ep0_ghost_receive(void *data, uint32_t len, uint16_t wRequestAndType, uint16_t wValue, uint16_t wIndex, uint16_t wLength)
{
\tsetup_t s;
\ts.wRequestAndType = wRequestAndType;
\ts.wValue = wValue;
\ts.wIndex = wIndex;
\ts.wLength = wLength;
\tendpoint0_setupdata.bothwords = s.bothwords;
\tendpoint0_receive(data, len, 1);
}
void usb_ep0_ghost_transmit(const void *data, uint32_t len, uint32_t seq)
{
\tNVIC_DISABLE_IRQ(IRQ_USB1);
\tif (seq == ghost_ep0_seq && !(USB1_ENDPTSETUPSTAT & 1)) {
\t\tif (data == NULL && len == 0xFFFFFFFFu) {
\t\t\tUSB1_ENDPTCTRL0 = 0x00010001;
\t\t} else {
\t\t\tendpoint0_transmit(data, len, 0);
\t\t}
\t}
\tNVIC_ENABLE_IRQ(IRQ_USB1);
}
void usb_soft_disconnect(void)
{
\tNVIC_DISABLE_IRQ(IRQ_USB1);
\tUSB1_USBCMD &= ~USB_USBCMD_RS;
\tUSB1_DEVICEADDR = 0;
\twhile (USB1_ENDPTPRIME) ;
\tUSB1_ENDPTFLUSH = 0xFFFFFFFFu;
\twhile (USB1_ENDPTFLUSH) ;
\tUSB1_ENDPTSETUPSTAT = USB1_ENDPTSETUPSTAT;
\tUSB1_ENDPTCOMPLETE = USB1_ENDPTCOMPLETE;
\tusb_configuration = 0;
\tendpointN_notify_mask = 0;
\tendpoint0_notify_mask = 0;
\tNVIC_ENABLE_IRQ(IRQ_USB1);
}
void usb_soft_connect(void)
{
\tUSB1_USBCMD |= USB_USBCMD_RS;
}
#endif
#else // defined(NUM_ENDPOINTS)"""
assert su.count("#else // defined(NUM_ENDPOINTS)") == 1
su = su.replace("#else // defined(NUM_ENDPOINTS)", ep0_helpers, 1)
u.write_text(su)

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
