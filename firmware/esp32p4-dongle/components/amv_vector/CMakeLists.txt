idf_component_register(SRCS "amv_vector.c" INCLUDE_DIRS "include"
                       REQUIRES amv_common amv_ctrl amv_usb_device
                       PRIV_REQUIRES freertos esp_timer)
