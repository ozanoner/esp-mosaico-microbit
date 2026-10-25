set(BENCH_FILES "")
if(CONFIG_MB_VOICE_BENCH_WAV)
    list(APPEND BENCH_FILES "bench/command.wav")
endif()

idf_component_register(SRCS "mb_voice.c" EMBED_FILES ${BENCH_FILES}
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp-mosaico-bsp mb_event
    PRIV_REQUIRES esp_http_client esp_wifi esp_netif espressif__button espressif__esp_codec_dev)
