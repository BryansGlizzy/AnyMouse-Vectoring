/*
 * M0 spike: can the ESP32-P4 Full-Speed OTG controller be moved onto the GPIO24/25
 * PHY (the "other" USB-C, normally USB Serial/JTAG) in software, and enumerate as
 * a HID mouse + vendor HID device?
 *
 * Console output stays on UART0 (the CH343 USB-C).
 */
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

/* Which internal FS/LS PHY the USB-OTG 1.1 controller uses: 0 = GPIO24/25, 1 = GPIO26/27 (default), -1 = leave alone */
#define SPIKE_FS_PHY_IDX 0

static const char *TAG = "m0";

static const uint8_t mouse_report_desc[] = { TUD_HID_REPORT_DESC_MOUSE() };

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

enum { ITF_MOUSE = 0, ITF_VENDOR, ITF_COUNT };

#define CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CFG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, 4, HID_ITF_PROTOCOL_MOUSE, sizeof(mouse_report_desc), 0x81, 8, 1),
    TUD_HID_INOUT_DESCRIPTOR(ITF_VENDOR, 5, HID_ITF_PROTOCOL_NONE, sizeof(vendor_report_desc), 0x02, 0x82, 64, 1),
};

static const tusb_desc_device_t device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,   /* Espressif; development only */
    .idProduct          = 0x4D56,   /* placeholder, must differ from 0x1001 (Serial/JTAG) */
    .bcdDevice          = 0x0001,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},             /* 0: English */
    "AnyMouse",                             /* 1 */
    "AnyMouse Vectoring Dongle (M0 spike)", /* 2 */
    "M0-0001",                              /* 3 */
    "AnyMouse Mouse",                       /* 4 */
    "AnyMouse Control",                     /* 5 */
};

/* ---- TinyUSB HID callbacks ---- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == ITF_MOUSE ? mouse_report_desc : vendor_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

/* Vendor channel: echo every OUT report back with byte0 = 0xE0 so the host can prove the round trip. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    ESP_EARLY_LOGI(TAG, "set_report inst=%u id=%u type=%u len=%u first=0x%02X",
                   instance, report_id, report_type, bufsize, bufsize ? buffer[0] : 0);
    if (instance == ITF_VENDOR && bufsize) {
        uint8_t reply[64] = {0};
        memcpy(reply, buffer, bufsize > 64 ? 64 : bufsize);
        reply[0] = 0xE0;
        tud_hid_n_report(ITF_VENDOR, 0, reply, sizeof(reply));
    }
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    static const char *names[] = {"ATTACHED", "DETACHED", "SUSPENDED", "RESUMED"};
    ESP_EARLY_LOGI(TAG, "USB event: %s", names[event->id]);
}

void app_main(void)
{
    ESP_LOGI(TAG, "M0 spike start, FS PHY idx = %d", SPIKE_FS_PHY_IDX);

#if SPIKE_FS_PHY_IDX >= 0
    /* Route USB-OTG 1.1 to the chosen internal FS/LS PHY (and Serial/JTAG to the other one). */
    usb_wrap_ll_phy_select(&USB_WRAP, SPIKE_FS_PHY_IDX);
    ESP_LOGI(TAG, "PHY select written: USB_WRAP -> FSLS PHY %d", SPIKE_FS_PHY_IDX);
#endif

    tinyusb_config_t cfg = TINYUSB_CONFIG_FULL_SPEED(usb_event_cb, NULL);
    cfg.descriptor.device = &device_desc;
    cfg.descriptor.string = string_desc;
    cfg.descriptor.string_count = sizeof(string_desc) / sizeof(string_desc[0]);
    cfg.descriptor.full_speed_config = fs_config_desc;
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "tinyusb_driver_install OK (port FS_0)");

    for (;;) {
        ESP_LOGI(TAG, "mounted=%d suspended=%d mouse_ready=%d vendor_ready=%d",
                 tud_mounted(), tud_suspended(), tud_hid_n_ready(ITF_MOUSE), tud_hid_n_ready(ITF_VENDOR));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
