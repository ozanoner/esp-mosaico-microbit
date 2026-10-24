idf_component_register(SRCS "mb_ble_central.c"
    INCLUDE_DIRS "include"
    REQUIRES bt
    PRIV_REQUIRES mb_flash mb_mem_probe)
