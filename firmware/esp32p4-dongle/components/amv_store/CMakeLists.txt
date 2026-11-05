idf_component_register(SRCS "amv_store.c" INCLUDE_DIRS "include"
                       REQUIRES amv_common
                       PRIV_REQUIRES nvs_flash esp_rom)
