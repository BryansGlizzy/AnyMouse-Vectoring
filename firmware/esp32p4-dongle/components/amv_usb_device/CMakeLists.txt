idf_component_register(SRCS "amv_usb_device.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES amv_proto espressif__esp_tinyusb hal soc)
