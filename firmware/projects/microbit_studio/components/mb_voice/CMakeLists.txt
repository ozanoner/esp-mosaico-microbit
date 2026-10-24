idf_component_register(SRCS "mb_voice.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp-mosaico-bsp mb_event
    PRIV_REQUIRES esp_http_client esp_wifi esp_netif espressif__button espressif__esp_codec_dev)
