# =========================================================================
# FreeType 配置开关
# =========================================================================

# 禁用非必要可选依赖，避免预编译静态库隐式依赖未纳入布局的系统库。
set(FT_DISABLE_ZLIB
    ON
    CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2
    ON
    CACHE BOOL "" FORCE)
if(APPLE)
  # macOS 预编译布局未携带 libpng，必须关闭 PNG 以保证静态库可独立链接。
  set(FT_DISABLE_PNG
      ON
      CACHE BOOL "" FORCE)
else()
  set(FT_DISABLE_PNG
      OFF
      CACHE BOOL "" FORCE)
endif()
set(FT_DISABLE_HARFBUZZ
    ON
    CACHE BOOL "" FORCE) # 这是一个循环依赖大坑，建议禁用
set(FT_DISABLE_BROTLI
    ON
    CACHE BOOL "" FORCE)
set(FT_DISABLE_HVF
    ON
    CACHE BOOL "" FORCE) # HVF 仅在新 SDK 中提供，不适合最低版本预编译库。

# 禁止生成安装规则 (防止 ninja install 时把 freetype 塞进系统目录)
set(SKIP_INSTALL_ALL
    ON
    CACHE BOOL "" FORCE)
set(SKIP_INSTALL_HEADERS
    ON
    CACHE BOOL "" FORCE)
set(SKIP_INSTALL_LIBRARIES
    ON
    CACHE BOOL "" FORCE)

# 如果是 Windows，构建静态库以避免 DLL 地狱
if(WIN32)
  # Windows 下同样跟随 PROJECT_LINKAGE，shared 预编译包必须产出真正的 FreeType DLL。
  if(PROJECT_LINKAGE STREQUAL "shared")
    set(_freetype_build_shared ON)
  else()
    set(_freetype_build_shared OFF)
  endif()
  set(BUILD_SHARED_LIBS
      ${_freetype_build_shared}
      CACHE BOOL "" FORCE)
endif()

# =========================================================================
# 引入官方源码
# =========================================================================
# 假设源码在 ../sources/freetype
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/freetype SYSTEM)

# =========================================================================
# 别名设置
# =========================================================================
if(NOT TARGET freetype::freetype)
  add_library(freetype::freetype ALIAS freetype)
endif()
