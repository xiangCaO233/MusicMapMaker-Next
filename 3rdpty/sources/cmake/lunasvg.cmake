# 禁用构建示例
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lunasvg SYSTEM)

# 创建接口库
add_library(3rd_lunasvg INTERFACE)
target_link_libraries(3rd_lunasvg INTERFACE lunasvg::lunasvg)

