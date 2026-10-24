idf_component_register(SRCS "mb_flash.c"
    INCLUDE_DIRS "include"
    # The image the device streams: only region 2, extracted from a mkc build by
    # scripts/extract_region2.py. The SoftDevice and DAL are already on the board.
    EMBED_FILES "fixture/mb_fixture.bin")
