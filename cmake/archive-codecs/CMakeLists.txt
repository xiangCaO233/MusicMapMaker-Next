# * 独立压缩依赖工程，避免上游通用选项污染主工程缓存。 仅由 SOURCES_BUILD 的外部项目调用，不是缺失预编译库时的回退。
cmake_minimum_required(VERSION 3.31)
# * 编码器由 C 实现，但 zstd 的配置探针也可能需要 C++ 编译器。
project(MmmArchiveCodecs LANGUAGES C CXX)
# * 依赖只使用随仓库固定的子模块版本，不读取系统压缩库。 安装前缀由调用方提供，禁止安装到用户系统目录。
get_filename_component(MMM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# * 静态对象也可能被上层共享库引用，因此所有配置都启用位置无关代码。
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# * 缓存使用 FORCE 清理复用构建树中的上次上游默认值。
set(BUILD_TESTING
    OFF
    CACHE BOOL "" FORCE)
# * 各依赖的配置局限在函数作用域，避免 BUILD_SHARED_LIBS 之外的同名变量泄漏。
function(buildArchiveZlib)
  # * 静态和共享只构建其中一种，避免安装同名导入库时互相覆盖。
  # * ZLIB 的专属开关不跟随通用 BUILD_SHARED_LIBS，必须显式同步。
  set(ZLIB_BUILD_SHARED
      ${BUILD_SHARED_LIBS}
      CACHE BOOL "" FORCE)
  # * 静态目标默认启用，在共享分支中显式禁用。
  set(ZLIB_BUILD_STATIC
      ON
      CACHE BOOL "" FORCE)
  # * 此分支只影响库类型，平台 CRT 由外层工具链统一选择。
  if(BUILD_SHARED_LIBS)
    set(ZLIB_BUILD_STATIC
        OFF
        CACHE BOOL "" FORCE)
  endif()
  # * 应用不调用命令行程序；安装步骤只为后续 libarchive 探针提供库。
  set(ZLIB_BUILD_TESTING
      OFF
      CACHE BOOL "" FORCE)
  # * 禁止直接引用 zlib 构建树，安装树才是 libarchive 的探针输入。
  set(ZLIB_INSTALL
      ON
      CACHE BOOL "" FORCE)
  # * 复用引擎固定的 zlib 源码，但使用独立构建目录和输出名。
  add_subdirectory(
    "${MMM_ROOT}/3rdpty/sources/IonCachyEngine/3rdpty/sources/zlib" zlib)
endfunction()
# * XZ 只提供 liblzma，7z 的 LZMA/LZMA2 与压缩 TAR 共用此库。
function(buildArchiveLzma)
  # * 禁止命令行、翻译与文档构建，避免交叉编译依赖宿主生成工具。
  foreach(_option XZ_TOOL_XZ XZ_TOOL_XZDEC XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO
                  XZ_NLS XZ_DOC)
    # * 使用缓存强制覆盖，确保增量构建不会重新启用曾开启的工具。
    set(${_option}
        OFF
        CACHE BOOL "" FORCE)
  endforeach()
  # * XZ 自身遵循 BUILD_SHARED_LIBS，不需要第二套库类型开关。
  add_subdirectory("${MMM_ROOT}/3rdpty/sources/xz" xz)
endfunction()
# * zstd 使用其官方 CMake 入口，只输出应用要分发的编码器库。
function(buildArchiveZstd)
  # * 不运行第三方测试，业务回归在主工程中通过实际导入入口执行。
  foreach(_option ZSTD_BUILD_PROGRAMS ZSTD_BUILD_TESTS)
    set(${_option}
        OFF
        CACHE BOOL "" FORCE)
  endforeach()
  # * 两种库类型必须与其他编码器同步，否则静态部署会遗漏 DLL。
  # * Zstd 的共享与静态目标可以并存，此处主动收窄到选中的部署类型。
  set(ZSTD_BUILD_SHARED
      ${BUILD_SHARED_LIBS}
      CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_STATIC
      ON
      CACHE BOOL "" FORCE)
  # * 此分支只影响库类型，平台 CRT 由外层工具链统一选择。
  if(BUILD_SHARED_LIBS)
    set(ZSTD_BUILD_STATIC
        OFF
        CACHE BOOL "" FORCE)
  endif()
  # * 上游入口位于 build/cmake，二进制目录独立命名为 zstd。
  add_subdirectory("${MMM_ROOT}/3rdpty/sources/zstd/build/cmake" zstd)
endfunction()
# * 先建立上游目标，再统一规范文件名；不改写任何上游源码。
buildarchivezlib()
buildarchivelzma()
buildarchivezstd()
# * bzip2 稳定版不带 CMake；按官方库源列表建立独立库，不构建命令行程序。 该包装只声明构建规则，不改动上游文件。
# * 路径只指向固定 gitlink，不在配置阶段下载或升级稳定版源码。
set(_bz2_root "${MMM_ROOT}/3rdpty/sources/bzip2")
# * 该列表来自稳定版的库对象集合，不包含 main、测试或恢复工具。
# * 保持原始 C 语言编译，不能用 C++ 编译导致公开符号名称变化。
add_library(
  bz2
  "${_bz2_root}/blocksort.c"
  "${_bz2_root}/huffman.c"
  "${_bz2_root}/crctable.c"
  "${_bz2_root}/randtable.c"
  "${_bz2_root}/compress.c"
  "${_bz2_root}/decompress.c"
  "${_bz2_root}/bzlib.c")
# * 使用稳定输出名，使 Windows 和 POSIX 的 Find 模块不依赖目标命名细节。
set_target_properties(bz2 PROPERTIES OUTPUT_NAME bz2 WINDOWS_EXPORT_ALL_SYMBOLS
                                                     ON)
# * 公开接口只有 bzlib.h，内部源文件依赖同目录私有头。
target_include_directories(bz2 PUBLIC "${_bz2_root}")
# * bzip2 Windows 接口使用宏区分导入；构建 DLL 时按官方约定导出。
# * 静态消费者不定义 BZ_IMPORT，避免将静态函数声明误编译为 DLL 导入。
if(WIN32 AND BUILD_SHARED_LIBS)
  target_compile_definitions(bz2 PRIVATE BZ_EXPORT)
endif()
# * 导入库、共享实体与 Windows DLL 分别进入标准安装子目录。
# * 下游 staging 依赖这些稳定目录，不从构建树猜测输出位置。
install(
  TARGETS bz2
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)
# * 不安装 bzip2 内部结构头，避免业务层耦合压缩器实现细节。
# * 公开声明未被项目改写，消费者使用上游原始 ABI。
install(FILES "${_bz2_root}/bzlib.h" DESTINATION include)

# * 固定文件名，消除上游平台前缀、Debug 后缀和目标别名差异。 预编译目录已隔离配置，不需要文件名再次携带 d 后缀。
foreach(_pair "zlibstatic|z" "zlib|z" "liblzma|lzma" "libzstd_static|zstd"
              "libzstd_shared|zstd" "bz2|bz2")
  # * 映射只作用于存在的目标，静态或共享未选中的目标不会被创建。
  string(REPLACE "|" ";" _parts "${_pair}")
  # * 分隔符仅用于本地映射，库名不携带路径，防止改变安装布局。
  list(GET _parts 0 _target)
  list(GET _parts 1 _output)
  # * 同一包的静态和共享候选只有一个存在，不创建无意义的占位目标。
  if(TARGET ${_target})
    # * ARCHIVE 控制 Windows 导入库，RUNTIME 控制 DLL，两者须使用同一基础名称。
    # * 同时覆盖上游显式设置的 LIBRARY/RUNTIME 名称，OUTPUT_NAME 本身不足以覆盖。
    set_target_properties(
      ${_target}
      PROPERTIES OUTPUT_NAME "${_output}"
                 ARCHIVE_OUTPUT_NAME "${_output}"
                 LIBRARY_OUTPUT_NAME "${_output}"
                 RUNTIME_OUTPUT_NAME "${_output}"
                 DEBUG_POSTFIX "")
  endif()
endforeach()
