# 禁用构建示例
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# 交叉编译时禁用 plutovg 对 m 的自动检测，改用 lib_proxy 中的 m.lib
if(CMAKE_CROSSCOMPILING)
    set(MATH_LIBRARY "m" CACHE FILEPATH "Forced math library" FORCE)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lunasvg SYSTEM)

# 创建接口库
add_library(3rd_lunasvg INTERFACE)
target_link_libraries(3rd_lunasvg INTERFACE lunasvg::lunasvg)
