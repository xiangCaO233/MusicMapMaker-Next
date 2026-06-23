# Linux 到 Windows MinGW GCC 交叉工具链。
#
# 当前仓库已有的 MinGW 预编译库目录为 mingw/clang64。若以后补齐 GCC/ucrt64
# 预编译包，可通过 -DPROJECT_PREBUILT_COMPILER_TAG=ucrt64 或脚本参数切换。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TOOLCHAIN_PREFIX
    "x86_64-w64-mingw32"
    CACHE STRING "Prefix for MinGW GCC and binutils.")

if(DEFINED ENV{MINGW_SYSROOT} AND NOT "$ENV{MINGW_SYSROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{MINGW_SYSROOT}" MINGW_SYSROOT_DEFAULT)
else()
  execute_process(
    COMMAND ${MINGW_TOOLCHAIN_PREFIX}-gcc -print-sysroot
    OUTPUT_VARIABLE MINGW_SYSROOT_DEFAULT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()

if(MINGW_SYSROOT_DEFAULT STREQUAL "")
  set(MINGW_SYSROOT_DEFAULT "/usr/lib/mingw64-toolchain")
endif()

set(MINGW_SYSROOT
    "${MINGW_SYSROOT_DEFAULT}"
    CACHE PATH "Root directory of the MinGW Windows sysroot.")
set(CMAKE_SYSROOT "${MINGW_SYSROOT}")

set(CMAKE_C_COMPILER
    ${MINGW_TOOLCHAIN_PREFIX}-gcc
    CACHE FILEPATH "C compiler for MinGW GCC cross builds.")
set(CMAKE_CXX_COMPILER
    ${MINGW_TOOLCHAIN_PREFIX}-g++
    CACHE FILEPATH "C++ compiler for MinGW GCC cross builds.")
set(CMAKE_RC_COMPILER
    ${MINGW_TOOLCHAIN_PREFIX}-windres
    CACHE FILEPATH "Windows resource compiler for MinGW cross builds.")
set(CMAKE_AR
    ${MINGW_TOOLCHAIN_PREFIX}-ar
    CACHE FILEPATH "Archive tool for MinGW cross builds.")
set(CMAKE_RANLIB
    ${MINGW_TOOLCHAIN_PREFIX}-ranlib
    CACHE FILEPATH "Archive index tool for MinGW cross builds.")
set(CMAKE_STRIP
    ${MINGW_TOOLCHAIN_PREFIX}-strip
    CACHE FILEPATH "Strip tool for MinGW cross builds.")
set(CMAKE_OBJCOPY
    ${MINGW_TOOLCHAIN_PREFIX}-objcopy
    CACHE FILEPATH "Objcopy tool for MinGW cross builds.")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 工具程序使用宿主 Linux 环境；库和包同时允许 sysroot 与仓库内显式
# 预编译库路径，避免 CMake 把 3rdpty/prebuilts 的绝对路径重映射到 sysroot。
set(CMAKE_FIND_ROOT_PATH "${MINGW_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# 预编译库目录默认复用当前仓库已经提供的 Windows MinGW clang64 布局。
set(PROJECT_PREBUILT_PLATFORM
    "windows"
    CACHE STRING "Prebuilt platform directory name." FORCE)
set(PROJECT_PREBUILT_TOOLCHAIN
    "mingw"
    CACHE STRING "Prebuilt toolchain directory name." FORCE)

if(DEFINED ENV{MINGW_GCC_PREBUILT_COMPILER_TAG}
   AND NOT "$ENV{MINGW_GCC_PREBUILT_COMPILER_TAG}" STREQUAL "")
  set(_mingw_gcc_prebuilt_tag "$ENV{MINGW_GCC_PREBUILT_COMPILER_TAG}")
else()
  set(_mingw_gcc_prebuilt_tag "clang64")
endif()

set(PROJECT_PREBUILT_COMPILER_TAG
    "${_mingw_gcc_prebuilt_tag}"
    CACHE STRING "Prebuilt compiler/runtime directory name." FORCE)
