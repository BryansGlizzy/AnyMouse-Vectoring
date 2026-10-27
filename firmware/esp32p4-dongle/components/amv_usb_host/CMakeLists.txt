idf_component_register(SRCS "amv_usb_host.c" "drv_razer.c" "drv_finalmouse.c" "drv_gearhub.c" "drv_logitech.c"
                       INCLUDE_DIRS "include"
                       REQUIRES amv_hid_parse amv_common amv_catalog
                       PRIV_REQUIRES usb amv_vector amv_ctrl amv_store freertos esp_driver_gpio esp_timer soc)
