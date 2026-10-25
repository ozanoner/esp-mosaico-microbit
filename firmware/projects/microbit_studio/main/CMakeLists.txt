idf_component_register(SRCS "main.c" "debug_rpc.c"
    PRIV_REQUIRES app_display app_studio esp_iris esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris mb_ble_central mb_build_client
        mb_event mb_mem_probe mb_payload mb_flash mb_voice nvs_flash log)
