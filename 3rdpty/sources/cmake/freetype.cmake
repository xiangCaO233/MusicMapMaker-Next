# =========================================================================
# FreeType 配置开关
# =========================================================================

# 禁用这一堆可选依赖，确保极速编译且不依赖系统库
# 如果你以后需要读取 WOFF (Web字体) 或 PNG (彩色Emoji)，可以把对应的改为 OFF
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE) # 这是一个循环依赖大坑，建议禁用
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)

# 禁止生成安装规则 (防止 ninja install 时把 freetype 塞进系统目录)
set(SKIP_INSTALL_ALL    ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_HEADERS ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_LIBRARIES ON CACHE BOOL "" FORCE)

# 如果是 Windows，构建静态库以避免 DLL 地狱
if(WIN32)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()

# =========================================================================
# 引入官方源码
# =========================================================================
# 假设源码在 ../sources/freetype
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/freetype SYSTEM)

# =========================================================================
# 别名设置
# =========================================================================
if(NOT TARGET freetype::freetype)
    add_library(freetype::freetype ALIAS freetype)
endif()
