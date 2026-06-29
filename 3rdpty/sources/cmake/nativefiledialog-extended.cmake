add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/nativefiledialog-extended SYSTEM)
# nfd 作为 shared 预编译包时使用自动导出，保持业务侧 target 名称不变。
if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32 AND TARGET nfd)
  set_target_properties(nfd PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

add_library(3rd_nfd INTERFACE)
target_link_libraries(3rd_nfd INTERFACE nfd::nfd)
