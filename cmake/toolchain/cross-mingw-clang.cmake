# Linux 到 Windows MinGW clang 交叉工具链。
#
# 编译和链接使用 Linux 宿主上的 clang/clang++ + lld，目标端 sysroot 默认使用
# CI 机器挂载的 MSYS2 clang64，以匹配 mingw/clang64 预编译库的 libc++ ABI。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TARGET_TRIPLE
    "x86_64-w64-windows-gnu"
    CACHE STRING "clang target triple for the MinGW Windows build.")
set(MINGW_TOOLCHAIN_PREFIX
    "x86_64-w64-mingw32"
    CACHE STRING "Prefix for Linux-hosted MinGW binutils.")

if(DEFINED ENV{MINGW_SYSROOT} AND NOT "$ENV{MINGW_SYSROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{MINGW_SYSROOT}" MINGW_SYSROOT_DEFAULT)
else()
  if(DEFINED ENV{WINDOWS_CROSS_ROOT} AND NOT "$ENV{WINDOWS_CROSS_ROOT}"
                                      STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{WINDOWS_CROSS_ROOT}" WINDOWS_CROSS_ROOT_DEFAULT)
  else()
    set(WINDOWS_CROSS_ROOT_DEFAULT "/mnt/cross/windows")
  endif()

  set(MSYS2_CLANG64_SYSROOT_DEFAULT
      "${WINDOWS_CROSS_ROOT_DEFAULT}/msys64/clang64")
  if(EXISTS "${MSYS2_CLANG64_SYSROOT_DEFAULT}/include"
     AND EXISTS "${MSYS2_CLANG64_SYSROOT_DEFAULT}/lib")
    set(MINGW_SYSROOT_DEFAULT "${MSYS2_CLANG64_SYSROOT_DEFAULT}")
  else()
    execute_process(
      COMMAND ${MINGW_TOOLCHAIN_PREFIX}-gcc -print-sysroot
      OUTPUT_VARIABLE MINGW_SYSROOT_DEFAULT
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
endif()

if(MINGW_SYSROOT_DEFAULT STREQUAL "")
  set(MINGW_SYSROOT_DEFAULT "/usr/x86_64-w64-mingw32")
endif()

set(MINGW_SYSROOT
    "${MINGW_SYSROOT_DEFAULT}"
    CACHE PATH "Root directory of the MinGW Windows sysroot.")
set(CMAKE_SYSROOT "${MINGW_SYSROOT}")

find_program(MMM_MINGW_CLANG_C NAMES clang-22 clang-21 clang-20 clang)
find_program(MMM_MINGW_CLANG_CXX NAMES clang++-22 clang++-21 clang++-20 clang++)
if(NOT MMM_MINGW_CLANG_C)
  message(FATAL_ERROR "找不到 clang，请安装 LLVM clang 20 或更新版本。")
endif()
if(NOT MMM_MINGW_CLANG_CXX)
  message(FATAL_ERROR "找不到 clang++，请安装 LLVM clang++ 20 或更新版本。")
endif()

set(CMAKE_C_COMPILER
    "${MMM_MINGW_CLANG_C}"
    CACHE FILEPATH "C compiler for MinGW clang cross builds.")
set(CMAKE_CXX_COMPILER
    "${MMM_MINGW_CLANG_CXX}"
    CACHE FILEPATH "C++ compiler for MinGW clang cross builds.")
set(CMAKE_C_COMPILER_TARGET "${MINGW_TARGET_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${MINGW_TARGET_TRIPLE}")

# MSYS2 clang64 使用 libc++/compiler-rt/libunwind。这里显式固定运行库，
# 避免 Linux 发行版 clang 默认回退到 libstdc++ 或 GCC runtime。
set(MINGW_CLANG_RUNTIME_FLAGS "-rtlib=compiler-rt -unwindlib=libunwind")
set(MINGW_CLANG_CXX_STDLIB_FLAGS "-stdlib=libc++")
set(CMAKE_C_FLAGS_INIT "")
set(CMAKE_CXX_FLAGS_INIT "${MINGW_CLANG_CXX_STDLIB_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld ${MINGW_CLANG_RUNTIME_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "-fuse-ld=lld ${MINGW_CLANG_RUNTIME_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT
    "-fuse-ld=lld ${MINGW_CLANG_RUNTIME_FLAGS}")

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

# 工具程序使用宿主 Linux 环境；库和包同时允许 sysroot 与仓库内显式
# 预编译库路径，避免 CMake 把 3rdpty/prebuilts 的绝对路径重映射到 sysroot。
set(CMAKE_FIND_ROOT_PATH "${MINGW_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# 本工具链使用 Linux clang + MSYS2 clang64 sysroot。当前 CI 先禁用项目级
# ThinLTO，避免与第三方预编译库和跨平台调试信息组合时产生额外变量。
set(MMM_DISABLE_CLANG_LTO
    ON
    CACHE BOOL "Disable LLVM ThinLTO for MinGW clang cross builds.")
set(MMM_MINGW_CLANG_USE_GCC_LINKER
    OFF
    CACHE BOOL "Use MinGW GCC driver for final links in clang cross builds.")

# 预编译库目录固定使用 Windows MinGW clang64 布局。
set(PROJECT_PREBUILT_PLATFORM
    "windows"
    CACHE STRING "Prebuilt platform directory name." FORCE)
set(PROJECT_PREBUILT_TOOLCHAIN
    "mingw"
    CACHE STRING "Prebuilt toolchain directory name." FORCE)
set(PROJECT_PREBUILT_COMPILER_TAG
    "clang64"
    CACHE STRING "Prebuilt compiler/runtime directory name." FORCE)
