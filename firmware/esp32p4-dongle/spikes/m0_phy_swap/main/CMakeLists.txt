idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES espressif__esp_tinyusb hal soc esp_driver_gpio)
