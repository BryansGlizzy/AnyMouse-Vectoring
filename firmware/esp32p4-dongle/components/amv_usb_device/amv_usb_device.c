#include "amv_usb_device.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "soc/usb_wrap_struct.h"
#include "hal/usb_wrap_ll.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "amv_proto.h"
#include "amv_dongle_proto.h"

/* Internal FS/LS PHY used by the USB-OTG 1.1 controller: 0 = GPIO24/25 (the "other" USB-C). */
#define FS_PHY_IDX 0

static const char *TAG = "amv_usb";

/* Report-protocol mouse: 16 buttons, 16-bit X/Y, 8-bit wheel, 8-bit AC Pan (8 bytes, no report ID).
 * In boot protocol (BIOS) the host gets the standard 3-byte report instead. */
static const uint8_t mouse_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
};

static const uint8_t vendor_report_desc[] = {
    0x06, 0x4D, 0xFF,       /* Usage Page (0xFF4D, vendor) */
    0x09, 0x01,             /* Usage (1) */
    0xA1, 0x01,             /* Collection (Application) */
    0x09, 0x02,             /*   Usage (2) input */
    0x15, 0x00,             /*   Logical Min 0 */
    0x26, 0xFF, 0x00,       /*   Logical Max 255 */
    0x75, 0x08,             /*   Report Size 8 */
    0x95, 0x40,             /*   Report Count 64 */
    0x81, 0x02,             /*   Input (Data,Var,Abs) */
    0x09, 0x03,             /*   Usage (3) output */
    0x91, 0x02,             /*   Output (Data,Var,Abs) */
    0xC0,
};

static const uint8_t ghost_fallback_desc[] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,
    0x09, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02,
    0x09, 0x03, 0x91, 0x02, 0xC0,
};

enum { ITF_MOUSE = 0, ITF_VENDOR, ITF_GHOST0, ITF_GHOST1, ITF_MAX };

#define BASE_CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static uint8_t fs_config_desc[TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + 3 * TUD_HID_INOUT_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, BASE_CFG_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, 4, HID_ITF_PROTOCOL_MOUSE, sizeof(mouse_report_desc), 0x81, 8, 1),
    TUD_HID_INOUT_DESCRIPTOR(ITF_VENDOR, 5, HID_ITF_PROTOCOL_NONE, sizeof(vendor_report_desc), 0x02, 0x82, 64, 1),
};

static tusb_desc_device_t device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = AMV_USB_VID,   /* Espressif; development only (replace before sharing builds) */
    .idProduct          = AMV_USB_PID,   /* placeholder, must differ from 0x1001 (Serial/JTAG) */
    .bcdDevice          = 0x0002,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

static char s_mfg_str[AMV_GHOST_STR_MAX]  = "AnyMouse";
static char s_prod_str[AMV_GHOST_STR_MAX] = "AnyMouse Dongle";

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},     /* 0: English */
    s_mfg_str,                      /* 1 */
    s_prod_str,                     /* 2 */
    "DEV-0001",                     /* 3 */
    "AnyMouse Mouse",               /* 4 */
    "AnyMouse Control",             /* 5 */
    "Mouse Vendor",                 /* 6 */
    "Mouse Vendor 2",               /* 7 */
};

static uint8_t  s_ghost_n_if = 0;
static uint8_t  s_ghost_desc[AMV_GHOST_MAX_IFS][AMV_GHOST_DESC_MAX];
static uint16_t s_ghost_desc_len[AMV_GHOST_MAX_IFS];
static amv_ghost_ops_t s_ghost_ops;

static void build_config_desc(const amv_ghost_cfg_t *cfg)
{
    uint8_t n_if = (cfg && cfg->n_if) ? (cfg->n_if > AMV_GHOST_MAX_IFS ? AMV_GHOST_MAX_IFS : cfg->n_if) : 0;
    uint16_t total = (uint16_t)(BASE_CFG_LEN + n_if * TUD_HID_INOUT_DESC_LEN);
    uint8_t base[] = {
        TUD_CONFIG_DESCRIPTOR(1, 2 + n_if, 0, total, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
        TUD_HID_DESCRIPTOR(ITF_MOUSE, 4, HID_ITF_PROTOCOL_MOUSE, sizeof(mouse_report_desc), 0x81, 8, 1),
        TUD_HID_INOUT_DESCRIPTOR(ITF_VENDOR, 5, HID_ITF_PROTOCOL_NONE, sizeof(vendor_report_desc), 0x02, 0x82, 64, 1),
    };
    memcpy(fs_config_desc, base, sizeof base);
    uint16_t off = sizeof base;
    for (uint8_t s = 0; s < n_if; s++) {
        uint8_t ep_out = (uint8_t)(0x03 + s);
        uint8_t ep_in  = (uint8_t)(0x83 + s);
        uint16_t mps = cfg->ifs[s].mps_in > cfg->ifs[s].mps_out ? cfg->ifs[s].mps_in : cfg->ifs[s].mps_out;
        if (mps == 0 || mps > 64) mps = 64;
        uint8_t iv = cfg->ifs[s].interval_in ? cfg->ifs[s].interval_in : 1;
        uint8_t g[] = {
            TUD_HID_INOUT_DESCRIPTOR(ITF_GHOST0 + s, 6 + s, HID_ITF_PROTOCOL_NONE,
                                     s_ghost_desc_len[s], ep_out, ep_in, mps, iv),
        };
        memcpy(fs_config_desc + off, g, sizeof g);
        off += sizeof g;
    }
    s_ghost_n_if = n_if;
}

void amv_usb_device_set_ghost_ops(const amv_ghost_ops_t *ops)
{
    if (ops) s_ghost_ops = *ops;
    else memset(&s_ghost_ops, 0, sizeof s_ghost_ops);
}

void amv_usb_device_set_ghost(const amv_ghost_cfg_t *cfg)
{
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(60));
    if (cfg && cfg->n_if > 0) {
        device_desc.idVendor = cfg->vid;
        device_desc.idProduct = cfg->pid;
        snprintf(s_mfg_str, sizeof s_mfg_str, "%s", cfg->manufacturer[0] ? cfg->manufacturer : "USB");
        snprintf(s_prod_str, sizeof s_prod_str, "%s", cfg->product[0] ? cfg->product : "USB Mouse");
        uint8_t n = cfg->n_if > AMV_GHOST_MAX_IFS ? AMV_GHOST_MAX_IFS : cfg->n_if;
        for (uint8_t s = 0; s < n; s++) {
            uint16_t dlen = cfg->ifs[s].desc_len;
            if (dlen > AMV_GHOST_DESC_MAX) dlen = AMV_GHOST_DESC_MAX;
            memcpy(s_ghost_desc[s], cfg->ifs[s].desc, dlen);
            s_ghost_desc_len[s] = dlen;
        }
        build_config_desc(cfg);
    } else {
        device_desc.idVendor = AMV_USB_VID;
        device_desc.idProduct = AMV_USB_PID;
        snprintf(s_mfg_str, sizeof s_mfg_str, "AnyMouse");
        snprintf(s_prod_str, sizeof s_prod_str, "AnyMouse Dongle");
        build_config_desc(NULL);
    }
    tud_connect();
}

bool amv_usb_device_ghost_send(uint8_t slot, const uint8_t *data, uint16_t len)
{
    if (slot >= s_ghost_n_if || !tud_mounted()) return false;
    uint8_t inst = (uint8_t)(ITF_GHOST0 + slot);
    if (!tud_hid_n_ready(inst)) return false;
    return tud_hid_n_report(inst, 0, data, len);
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    if (instance == ITF_MOUSE) return mouse_report_desc;
    if (instance == ITF_VENDOR) return vendor_report_desc;
    uint8_t slot = (uint8_t)(instance - ITF_GHOST0);
    if (slot < s_ghost_n_if && s_ghost_desc_len[slot] > 0) return s_ghost_desc[slot];
    return ghost_fallback_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    if ((instance == ITF_GHOST0 || instance == ITF_GHOST1) && s_ghost_ops.get_report) {
        uint8_t slot = (uint8_t)(instance - ITF_GHOST0);
        uint8_t raw[128];
        uint16_t want = (uint16_t)(reqlen + (report_id ? 1 : 0));
        if (want > sizeof raw) want = sizeof raw;
        int n = s_ghost_ops.get_report(slot, (uint8_t)report_type, report_id, raw, want);
        if (n <= 0) return 0;
        int off = (report_id && n > 0 && raw[0] == report_id) ? 1 : 0;
        int copy = n - off;
        if (copy < 0) copy = 0;
        if (copy > reqlen) copy = reqlen;
        memcpy(buffer, raw + off, (size_t)copy);
        return (uint16_t)copy;
    }
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    if (instance == ITF_VENDOR) {
        amv_proto_on_rx(buffer, bufsize);
        return;
    }
    if ((instance == ITF_GHOST0 || instance == ITF_GHOST1) && s_ghost_ops.set_report) {
        uint8_t slot = (uint8_t)(instance - ITF_GHOST0);
        s_ghost_ops.set_report(slot, (uint8_t)report_type, report_id, buffer, bufsize);
    }
}

static void (*s_mouse_done_cb)(void);

void amv_usb_device_set_mouse_done_cb(void (*cb)(void)) { s_mouse_done_cb = cb; }

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)report; (void)len;
    if (instance == ITF_MOUSE && s_mouse_done_cb) s_mouse_done_cb();
}

bool amv_usb_device_mouse_ready(void)
{
    return tud_mounted() && tud_hid_n_ready(ITF_MOUSE);
}

int amv_usb_device_mouse_max_delta(void)
{
    return tud_hid_n_get_protocol(ITF_MOUSE) == HID_PROTOCOL_BOOT ? 127 : 32767;
}

bool amv_usb_device_mouse_send(uint16_t buttons, int32_t dx, int32_t dy, int32_t wheel, int32_t pan)
{
    if (tud_hid_n_get_protocol(ITF_MOUSE) == HID_PROTOCOL_BOOT) {
        uint8_t r[3] = { (uint8_t)(buttons & 0x07), (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy };
        return tud_hid_n_report(ITF_MOUSE, 0, r, sizeof r);
    }
    uint8_t r[8] = { (uint8_t)buttons, (uint8_t)(buttons >> 8),
                     (uint8_t)dx, (uint8_t)(dx >> 8), (uint8_t)dy, (uint8_t)(dy >> 8),
                     (uint8_t)(int8_t)wheel, (uint8_t)(int8_t)pan };
    return tud_hid_n_report(ITF_MOUSE, 0, r, sizeof r);
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    static const char *names[] = {"ATTACHED", "DETACHED", "SUSPENDED", "RESUMED"};
    ESP_EARLY_LOGI(TAG, "USB event: %s", names[event->id]);
}

esp_err_t amv_usb_device_start(void)
{
    usb_wrap_ll_phy_select(&USB_WRAP, FS_PHY_IDX);

    tinyusb_config_t cfg = TINYUSB_CONFIG_FULL_SPEED(usb_event_cb, NULL);
    cfg.descriptor.device = &device_desc;
    cfg.descriptor.string = string_desc;
    cfg.descriptor.string_count = sizeof(string_desc) / sizeof(string_desc[0]);
    cfg.descriptor.full_speed_config = fs_config_desc;
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err == ESP_OK) ESP_LOGI(TAG, "USB device up on FS PHY %d", FS_PHY_IDX);
    return err;
}
