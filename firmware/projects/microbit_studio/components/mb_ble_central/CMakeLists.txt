idf_component_register(SRCS "mb_ble_central.c" "mb_status_parse.c"
    INCLUDE_DIRS "include"
    REQUIRES bt
    PRIV_REQUIRES mb_mem_probe)
