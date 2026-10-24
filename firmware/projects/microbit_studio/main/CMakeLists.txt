idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_display app_hello esp_iris esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris mb_ble_central mb_mem_probe
        mb_payload mb_flash nvs_flash log)
