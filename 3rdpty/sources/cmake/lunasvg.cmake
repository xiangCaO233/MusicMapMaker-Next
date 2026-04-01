# 禁用构建示例
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lunasvg SYSTEM)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/lunasvg/plutovg/source/plutovg-font.c"
	PROPERTIES COMPILE_FLAGS "/Od"
)
# 创建接口库
add_library(3rd_lunasvg INTERFACE)
target_link_libraries(3rd_lunasvg INTERFACE lunasvg::lunasvg)
