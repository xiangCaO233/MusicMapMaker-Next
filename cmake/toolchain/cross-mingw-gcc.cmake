# Linux 到 Windows MinGW GCC 交叉工具链。
#
# 远端 CI 的 Debian MinGW GCC 工具链默认使用 GCC 14 UCRT64 运行库；默认匹配 mingw/ucrt64
# 预编译库目录。需要切换其它运行时布局时，通过 -DPROJECT_PREBUILT_COMPILER_TAG=<tag> 或脚本参数覆盖。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TOOLCHAIN_PREFIX
    "x86_64-w64-mingw32ucrt"
    CACHE STRING "Prefix for MinGW GCC and binutils.")

if(DEFINED ENV{MINGW_SYSROOT} AND NOT "$ENV{MINGW_SYSROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{MINGW_SYSROOT}" MINGW_SYSROOT_DEFAULT)
else()
  execute_process(
    COMMAND ${MINGW_TOOLCHAIN_PREFIX}-gcc -print-sysroot
    OUTPUT_VARIABLE MINGW_SYSROOT_DEFAULT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

if(MINGW_SYSROOT_DEFAULT STREQUAL "")
  if(EXISTS "/usr/${MINGW_TOOLCHAIN_PREFIX}/include"
     AND EXISTS "/usr/${MINGW_TOOLCHAIN_PREFIX}/lib")
    set(MINGW_SYSROOT_DEFAULT "/usr/${MINGW_TOOLCHAIN_PREFIX}")
  else()
    set(MINGW_SYSROOT_DEFAULT "/usr/x86_64-w64-mingw32ucrt")
  endif()
endif()

# CMake 外部项目会把相对的 ar/ranlib 解析到子构建目录下；工具链入口统一解析为绝对路径。 这样 try-compile 与
# ExternalProject 子工程都会使用同一套 MinGW GCC/binutils。
find_program(MINGW_GCC_C_COMPILER NAMES ${MINGW_TOOLCHAIN_PREFIX}-gcc REQUIRED)
find_program(MINGW_GCC_CXX_COMPILER NAMES ${MINGW_TOOLCHAIN_PREFIX}-g++
                                          REQUIRED)
find_program(MINGW_GCC_RC_COMPILER NAMES ${MINGW_TOOLCHAIN_PREFIX}-windres
                                         REQUIRED)
find_program(MINGW_GCC_AR NAMES ${MINGW_TOOLCHAIN_PREFIX}-ar REQUIRED)
find_program(MINGW_GCC_RANLIB NAMES ${MINGW_TOOLCHAIN_PREFIX}-ranlib REQUIRED)
find_program(MINGW_GCC_NM NAMES ${MINGW_TOOLCHAIN_PREFIX}-nm REQUIRED)
find_program(MINGW_GCC_STRIP NAMES ${MINGW_TOOLCHAIN_PREFIX}-strip REQUIRED)
find_program(MINGW_GCC_OBJCOPY NAMES ${MINGW_TOOLCHAIN_PREFIX}-objcopy REQUIRED)

set(MINGW_SYSROOT
    "${MINGW_SYSROOT_DEFAULT}"
    CACHE PATH "Root directory of the MinGW Windows sysroot.")
set(CMAKE_SYSROOT "${MINGW_SYSROOT}")

set(CMAKE_C_COMPILER
    ${MINGW_GCC_C_COMPILER}
    CACHE FILEPATH "C compiler for MinGW GCC cross builds.")
set(CMAKE_CXX_COMPILER
    ${MINGW_GCC_CXX_COMPILER}
    CACHE FILEPATH "C++ compiler for MinGW GCC cross builds.")
set(CMAKE_RC_COMPILER
    ${MINGW_GCC_RC_COMPILER}
    CACHE FILEPATH "Windows resource compiler for MinGW cross builds.")
set(CMAKE_AR
    ${MINGW_GCC_AR}
    CACHE FILEPATH "Archive tool for MinGW cross builds.")
set(CMAKE_RANLIB
    ${MINGW_GCC_RANLIB}
    CACHE FILEPATH "Archive index tool for MinGW cross builds.")
set(CMAKE_NM
    ${MINGW_GCC_NM}
    CACHE FILEPATH "Symbol table tool for MinGW cross builds.")
set(CMAKE_STRIP
    ${MINGW_GCC_STRIP}
    CACHE FILEPATH "Strip tool for MinGW cross builds.")
set(CMAKE_OBJCOPY
    ${MINGW_GCC_OBJCOPY}
    CACHE FILEPATH "Objcopy tool for MinGW cross builds.")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 工具程序使用宿主 Linux 环境；库和包同时允许 sysroot 与仓库内显式预编译库路径。 这样可避免 CMake 把 3rdpty/prebuilts
# 的绝对路径重映射到 sysroot。
set(CMAKE_FIND_ROOT_PATH "${MINGW_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# 预编译库目录默认使用远端 CI 产出的 GCC 14 UCRT64 布局。
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
  set(_mingw_gcc_prebuilt_tag "ucrt64")
endif()

set(PROJECT_PREBUILT_COMPILER_TAG
    "${_mingw_gcc_prebuilt_tag}"
    CACHE STRING "Prebuilt compiler/runtime directory name." FORCE)
