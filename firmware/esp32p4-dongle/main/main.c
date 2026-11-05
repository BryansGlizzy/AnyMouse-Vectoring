#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "amv_ctrl.h"
#include "amv_js.h"
#include "amv_proto.h"
#include "amv_usb_device.h"
#include "amv_usb_host.h"
#include "amv_vector.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "AnyMouse dongle fw %d.%d.%d", AMV_FW_MAJOR, AMV_FW_MINOR, AMV_FW_PATCH);
    ESP_ERROR_CHECK(amv_ctrl_init());
    amv_proto_init();
#ifndef AMV_HOST_ONLY   /* bring-up switch: -DAMV_HOST_ONLY leaves the PC-side device off to test the host alone */
    ESP_ERROR_CHECK(amv_usb_device_start());
#else
    ESP_LOGW(TAG, "AMV_HOST_ONLY: PC-side USB device NOT started");
#endif
    amv_vector_start();
#if CONFIG_AMV_OPENMOUSE_JS
    if (amv_js_start() != ESP_OK) ESP_LOGE(TAG, "OpenMouse engine not started; mice stay generic");
#endif
    ESP_ERROR_CHECK(amv_usb_host_start());

    for (;;) {
        amv_state_t st;
        amv_ctrl_get_state(&st);
        ESP_LOGI(TAG, "gen=%u slot=%u bypass=%d w=[%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f]",
                 (unsigned)st.gen, st.active_slot, st.bypass,
                 st.w[0], st.w[1], st.w[2], st.w[3], st.w[4], st.w[5], st.w[6], st.w[7]);
        amv_vector_stats_t vs;
        amv_vector_get_stats(&vs);
        ESP_LOGI(TAG, "in=%u frames=%u raw=(%lld,%lld) out=(%lld,%lld) btn_overflow=%u", (unsigned)vs.reports_in,
                 (unsigned)vs.frames_out, (long long)vs.raw_dx, (long long)vs.raw_dy, (long long)vs.out_dx,
                 (long long)vs.out_dy, (unsigned)vs.button_overflow);
#if CONFIG_AMV_OPENMOUSE_JS
        amv_js_stats_t js;
        amv_js_get_stats(&js);
        if (js.ready) {
            ESP_LOGI(TAG, "openmouse: %d session(s), heap %u KB (peak %u KB), stack free %u B; internal RAM free %u KB, PSRAM free %u KB",
                     js.sessions, (unsigned)(js.heap_used / 1024), (unsigned)(js.heap_peak / 1024), (unsigned)js.stack_free,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        }
#endif
        char port[128];
        amv_usb_host_port_status(port, sizeof port);
        ESP_LOGI(TAG, "usb host: %d device(s) enumerated; %s", amv_usb_host_device_count(), port);
        amv_usb_host_mouse_t mice[8];
        int n = amv_usb_host_mice(mice, 8);
        for (int i = 0; i < n; i++) {
            ESP_LOGI(TAG, "mouse %d: %04x:%04x \"%s\" if%u mps=%u interval=%u reports=%u errors=%u", i, mice[i].vid,
                     mice[i].pid, mice[i].name, mice[i].iface, mice[i].mps, mice[i].interval,
                     (unsigned)mice[i].reports, (unsigned)mice[i].errors);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
