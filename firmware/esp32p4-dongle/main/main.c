#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "amv_ctrl.h"
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
    ESP_ERROR_CHECK(amv_usb_device_start());
    amv_vector_start();
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
        ESP_LOGI(TAG, "usb host: %d device(s) enumerated", amv_usb_host_device_count());
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
