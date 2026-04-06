add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/nativefiledialog-extended SYSTEM)

add_library(3rd_nfd INTERFACE)
target_link_libraries(3rd_nfd INTERFACE nfd::nfd)

