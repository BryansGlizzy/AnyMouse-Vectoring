idf_component_register(SRCS "amv_usb_host.c" INCLUDE_DIRS "include"
                       REQUIRES amv_hid_parse
                       PRIV_REQUIRES usb amv_vector amv_ctrl freertos esp_driver_gpio esp_timer soc)
