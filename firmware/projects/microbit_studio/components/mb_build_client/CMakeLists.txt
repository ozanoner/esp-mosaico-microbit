idf_component_register(SRCS "mb_build_client.c"
    INCLUDE_DIRS "include"
    REQUIRES mb_flash mb_event
    PRIV_REQUIRES mb_voice esp_http_client esp_netif
    # The payload the gate builds, straight from the repository — no copy, so
    # the embedded source cannot drift from microbit/payloads/hello.ts. The
    # embedded symbol is _binary_hello_ts_start (mb_build_client.c).
    EMBED_TXTFILES "../../../../../microbit/payloads/hello.ts")
