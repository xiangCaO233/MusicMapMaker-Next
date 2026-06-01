enable_language(C)

add_library(3rd_miniz STATIC "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz.c")

include(GenerateExportHeader)
generate_export_header(
    3rd_miniz
    BASE_NAME miniz
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h")

target_include_directories(
    3rd_miniz
    SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/miniz"
        "${CMAKE_CURRENT_BINARY_DIR}")

set_target_properties(3rd_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
