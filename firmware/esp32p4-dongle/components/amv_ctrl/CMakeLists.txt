idf_component_register(SRCS "amv_ctrl.c" INCLUDE_DIRS "include"
                       REQUIRES amv_common
                       PRIV_REQUIRES amv_store esp_timer freertos)
