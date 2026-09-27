# Linux 到 Windows MinGW GCC 交叉工具链。
#
# 远端 CI 的 Debian MinGW GCC 工具链默认使用 GCC 14 UCRT64 运行库；默认匹配 mingw/ucrt64
# 预编译库目录。需要切换其它运行时布局时，通过 -DPROJECT_PREBUILT_COMPILER_TAG=<tag> 或脚本参数覆盖。 工具链文件在
# project() 之前加载，只负责锁定目标平台、编译工具和查找根。 所有程序均解析为绝对路径，保证 try-compile 与
# ExternalProject 不发生漂移。 运行库变体编码在预编译 compiler tag 中，不能仅凭 GCC 版本推断。 明确进入 Windows
# 交叉模式，避免 CMake 使用 Linux 宿主平台规则。
set(CMAKE_SYSTEM_NAME Windows)
# 当前 MinGW 预编译产物只覆盖 x86_64。
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# prefix 同时决定 GCC 驱动与 binutils 的程序名，默认指向 UCRT 变体。
set(MINGW_TOOLCHAIN_PREFIX
    "x86_64-w64-mingw32ucrt"
    CACHE STRING "Prefix for MinGW GCC and binutils.")

if(DEFINED ENV{MINGW_SYSROOT} AND NOT "$ENV{MINGW_SYSROOT}" STREQUAL "")
  # CI 可显式挂载 sysroot，路径先转换为 CMake 统一格式。
  file(TO_CMAKE_PATH "$ENV{MINGW_SYSROOT}" MINGW_SYSROOT_DEFAULT)
else()
  # 未覆盖时询问选定 GCC 驱动，避免硬编码发行版路径。
  execute_process(
    COMMAND ${MINGW_TOOLCHAIN_PREFIX}-gcc -print-sysroot
    OUTPUT_VARIABLE MINGW_SYSROOT_DEFAULT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

if(MINGW_SYSROOT_DEFAULT STREQUAL "")
  # 某些 GCC 包返回空 sysroot，此时按常见 Debian 布局验证目录。
  if(EXISTS "/usr/${MINGW_TOOLCHAIN_PREFIX}/include"
     AND EXISTS "/usr/${MINGW_TOOLCHAIN_PREFIX}/lib")
    set(MINGW_SYSROOT_DEFAULT "/usr/${MINGW_TOOLCHAIN_PREFIX}")
  else()
    # 最终默认值保持 UCRT64 语义，即使目录缺失也能提供明确诊断路径。
    set(MINGW_SYSROOT_DEFAULT "/usr/x86_64-w64-mingw32ucrt")
  endif()
endif()

# CMake 外部项目会把相对的 ar/ranlib 解析到子构建目录下；工具链入口统一解析为绝对路径。 这样 try-compile 与
# ExternalProject 子工程都会使用同一套 MinGW GCC/binutils。 REQUIRED
# 让工具链不完整时在配置阶段失败，而不是晚到链接阶段才暴露。
find_program(MINGW_GCC_C_COMPILER NAMES ${MINGW_TOOLCHAIN_PREFIX}-gcc REQUIRED)
find_program(MINGW_GCC_CXX_COMPILER NAMES ${MINGW_TOOLCHAIN_PREFIX}-g++
                                          REQUIRED)
find_program(MINGW_GCC_RC_COMPILER NAMES ${MINGW_TOOLCHAIN_PREFIX}-windres
                                         REQUIRED)
# 归档、符号、strip 和 objcopy 工具必须与所选 target prefix 配套。
find_program(MINGW_GCC_AR NAMES ${MINGW_TOOLCHAIN_PREFIX}-ar REQUIRED)
find_program(MINGW_GCC_RANLIB NAMES ${MINGW_TOOLCHAIN_PREFIX}-ranlib REQUIRED)
find_program(MINGW_GCC_NM NAMES ${MINGW_TOOLCHAIN_PREFIX}-nm REQUIRED)
find_program(MINGW_GCC_STRIP NAMES ${MINGW_TOOLCHAIN_PREFIX}-strip REQUIRED)
find_program(MINGW_GCC_OBJCOPY NAMES ${MINGW_TOOLCHAIN_PREFIX}-objcopy REQUIRED)

set(MINGW_SYSROOT
    "${MINGW_SYSROOT_DEFAULT}"
    # cache 覆盖供 CI 固定目标文件系统版本。
    CACHE PATH "Root directory of the MinGW Windows sysroot.")
set(CMAKE_SYSROOT "${MINGW_SYSROOT}")

# 语言驱动与 binutils 必须来自同一 prefix。 编译器与资源工具使用绝对路径，防止子构建相对路径解析错误。
set(CMAKE_C_COMPILER
    ${MINGW_GCC_C_COMPILER}
    # CMake 子探针复用已解析的绝对驱动路径。
    CACHE FILEPATH "C compiler for MinGW GCC cross builds.")
# C++ 驱动负责自动链接对应版本的 libstdc++ 与异常运行库。
set(CMAKE_CXX_COMPILER
    ${MINGW_GCC_CXX_COMPILER}
    CACHE FILEPATH "C++ compiler for MinGW GCC cross builds.")
# windres 必须使用相同 target prefix，防止资源对象架构不匹配。
set(CMAKE_RC_COMPILER
    ${MINGW_GCC_RC_COMPILER}
    CACHE FILEPATH "Windows resource compiler for MinGW cross builds.")
set(CMAKE_AR
    ${MINGW_GCC_AR}
    CACHE FILEPATH "Archive tool for MinGW cross builds.")
# ranlib 与 ar 来自同套 binutils，保证静态库索引格式一致。
set(CMAKE_RANLIB
    ${MINGW_GCC_RANLIB}
    CACHE FILEPATH "Archive index tool for MinGW cross builds.")
set(CMAKE_NM
    ${MINGW_GCC_NM}
    CACHE FILEPATH "Symbol table tool for MinGW cross builds.")
# strip 与 objcopy 分别服务发布裁剪和调试信息处理。 两者都必须来自 UCRT prefix 对应的 binutils 套件。
set(CMAKE_STRIP
    ${MINGW_GCC_STRIP}
    CACHE FILEPATH "Strip tool for MinGW cross builds.")
set(CMAKE_OBJCOPY
    ${MINGW_GCC_OBJCOPY}
    CACHE FILEPATH "Objcopy tool for MinGW cross builds.")

# 配置探针只编译静态库，不要求 Windows 运行库在此阶段已经可链接。
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# sysroot 限定目标文件的默认搜索范围。 工具程序使用宿主 Linux 环境；库和包同时允许 sysroot 与仓库内显式预编译库路径。 这样可避免
# CMake 把 3rdpty/prebuilts 的绝对路径重映射到 sysroot。
set(CMAKE_FIND_ROOT_PATH "${MINGW_SYSROOT}")
# 构建工具必须来自 Linux 宿主，不能从 Windows sysroot 选择不可执行程序。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# 库、头和包允许 sysroot 与仓库预编译绝对路径共同参与查找。
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
# include 与 package 查找同样允许仓库内绝对预编译路径。
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# 预编译库目录默认使用远端 CI 产出的 GCC 14 UCRT64 布局。
set(PROJECT_PREBUILT_PLATFORM
    "windows"
    CACHE STRING "Prebuilt platform directory name." FORCE)
# 目录的 toolchain 层固定为 mingw，运行库变体由下一层标签决定。
set(PROJECT_PREBUILT_TOOLCHAIN
    "mingw"
    CACHE STRING "Prebuilt toolchain directory name." FORCE)

if(DEFINED ENV{MINGW_GCC_PREBUILT_COMPILER_TAG}
   AND NOT "$ENV{MINGW_GCC_PREBUILT_COMPILER_TAG}" STREQUAL "")
  # 环境覆盖用于选择 CI 已发布的特定 GCC/运行库目录标签。
  set(_mingw_gcc_prebuilt_tag "$ENV{MINGW_GCC_PREBUILT_COMPILER_TAG}")
else()
  # 默认 ucrt64 与本工具链 prefix 的 C runtime 保持一致。
  set(_mingw_gcc_prebuilt_tag "ucrt64")
endif()

# 最终标签强制写入缓存，确保所有 Find 模块选择相同 ABI 目录。
set(PROJECT_PREBUILT_COMPILER_TAG
    "${_mingw_gcc_prebuilt_tag}"
    CACHE STRING "Prebuilt compiler/runtime directory name." FORCE)
