cmake_minimum_required(VERSION 3.20)
project(mb_ble_central_host LANGUAGES C)

# Only the STATUS parser is host-testable: it has no BLE state and no IDF
# dependency beyond the accessor-free types in its own header.
add_executable(mb_status_test
    test_status.c
    ../mb_status_parse.c)
target_include_directories(mb_status_test PRIVATE ../include)
target_compile_options(mb_status_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME mb_status_test COMMAND mb_status_test)
