idf_component_register(SRCS "amv_proto.c" INCLUDE_DIRS "include"
                       REQUIRES amv_common amv_ctrl amv_usb_host
                       PRIV_REQUIRES espressif__esp_tinyusb esp_timer esp_system esp_hw_support freertos)
