set(CLAY_INCLUDE_ALL_EXAMPLES OFF CACHE BOOL "" FORCE)

add_library(3rd_clay INTERFACE)
target_include_directories(3rd_clay INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/clay)
