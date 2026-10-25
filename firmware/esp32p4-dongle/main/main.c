#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "amv_ctrl.h"
#include "amv_proto.h"
#include "amv_usb_device.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "AnyMouse dongle fw %d.%d.%d", AMV_FW_MAJOR, AMV_FW_MINOR, AMV_FW_PATCH);
    ESP_ERROR_CHECK(amv_ctrl_init());
    amv_proto_init();
    ESP_ERROR_CHECK(amv_usb_device_start());

    for (;;) {
        amv_state_t st;
        amv_ctrl_get_state(&st);
        ESP_LOGI(TAG, "gen=%u slot=%u bypass=%d w=[%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f]",
                 (unsigned)st.gen, st.active_slot, st.bypass,
                 st.w[0], st.w[1], st.w[2], st.w[3], st.w[4], st.w[5], st.w[6], st.w[7]);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
