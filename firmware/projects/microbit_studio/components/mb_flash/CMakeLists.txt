idf_component_register(SRCS "mb_flash.c"
    INCLUDE_DIRS "include")
# The compiled-in region-2 fixture is gone: since gate G4 the image comes from
# POST /v1/build?region2=true, so no image bytes live in the application. The
# generated mb_fixture.h / mb_fixture.bin stay in the tree as the reference the
# host test and scripts/extract_region2.py --check compare against.
