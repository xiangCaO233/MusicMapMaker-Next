# =========================================================================
# FreeType 配置开关
# =========================================================================

# 禁用非必要可选依赖，避免预编译静态库隐式依赖未纳入布局的系统库。
set(FT_DISABLE_ZLIB
    ON
    CACHE BOOL "" FORCE)
# 压缩字体的可选支持不进入发布依赖闭包，预编译链接不假设系统安装压缩库。
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
# 项目发布脚本负责快照公共头，安装规则不应额外产生一份来源不同的头。
set(SKIP_INSTALL_HEADERS
    ON
    CACHE BOOL "" FORCE)
# 正式产物由项目打包规则选择，不能直接拷贝上游默认安装目标。
set(SKIP_INSTALL_LIBRARIES
    ON
    CACHE BOOL "" FORCE)

# 如果是 Windows，构建静态库以避免 DLL 地狱
if(WIN32)
  # Windows 下同样跟随 PROJECT_LINKAGE，shared 预编译包必须产出真正的 FreeType DLL。
  if(PROJECT_LINKAGE STREQUAL "shared")
    # 动态 Windows 包配套生成 DLL 和导入库，消费者分别使用运行时与链接目录。
    set(_freetype_build_shared ON)
  else()
    # 静态包不依赖同目录另行部署 FreeType DLL。
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
# 导出同预编译脚本一致的名称，同时容许采用新版源码已经定义的别名。
if(NOT TARGET freetype::freetype)
  add_library(freetype::freetype ALIAS freetype)
endif()
