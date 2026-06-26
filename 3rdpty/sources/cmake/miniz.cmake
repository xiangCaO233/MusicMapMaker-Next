enable_language(C)

# miniz 是项目自维护包装目标，shared 偏好下生成 DLL 而不是静态归档。
set(MINIZ_LIBRARY_TYPE STATIC)
if(PROJECT_LINKAGE STREQUAL "shared")
  set(MINIZ_LIBRARY_TYPE SHARED)
endif()

add_library(
  3rd_miniz ${MINIZ_LIBRARY_TYPE}
  "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz_tdef.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz_tinfl.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz_zip.c")

include(GenerateExportHeader)
generate_export_header(3rd_miniz BASE_NAME miniz EXPORT_FILE_NAME
                       "${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h")

target_include_directories(
  3rd_miniz SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/miniz"
                          "${CMAKE_CURRENT_BINARY_DIR}")

set_target_properties(3rd_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
  set_target_properties(3rd_miniz PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
