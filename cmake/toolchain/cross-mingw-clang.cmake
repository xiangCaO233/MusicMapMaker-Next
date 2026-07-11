# Linux 到 Windows MinGW clang 交叉工具链。
#
# 编译和链接优先使用完整 llvm-mingw UCRT 工具链，也兼容 Linux 宿主上的 clang-22/clang++-22 + MinGW
# binutils。CLANG64 使用 UCRT C runtime 与 libc++，预编译库必须写入 mingw/clang64， 避免和 GCC
# UCRT64 的 mingw/ucrt64 布局混用。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TARGET_TRIPLE
    "x86_64-w64-windows-gnu"
    CACHE STRING "clang target triple for the MinGW Windows build.")
set(MINGW_TOOLCHAIN_PREFIX
    "x86_64-w64-mingw32"
    CACHE STRING "Prefix for Linux-hosted MinGW binutils.")

if(DEFINED ENV{LLVM_MINGW_ROOT} AND NOT "$ENV{LLVM_MINGW_ROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{LLVM_MINGW_ROOT}" LLVM_MINGW_ROOT_DEFAULT)
else()
  set(LLVM_MINGW_ROOT_DEFAULT "")
endif()
set(LLVM_MINGW_ROOT
    "${LLVM_MINGW_ROOT_DEFAULT}"
    CACHE PATH "Root directory of a complete llvm-mingw toolchain.")

set(_LLVM_MINGW_PROGRAM_PATHS "")
if(LLVM_MINGW_ROOT)
  list(APPEND _LLVM_MINGW_PROGRAM_PATHS "${LLVM_MINGW_ROOT}/bin")
endif()

function(find_mingw_clang_tool out_var)
  if(_LLVM_MINGW_PROGRAM_PATHS)
    find_program(
      ${out_var}
      NAMES ${ARGN}
      PATHS ${_LLVM_MINGW_PROGRAM_PATHS}
      NO_DEFAULT_PATH)
  endif()
  if(NOT ${out_var})
    find_program(${out_var} NAMES ${ARGN})
  endif()
  set(${out_var}
      "${${out_var}}"
      PARENT_SCOPE)
endfunction()

if(DEFINED ENV{MINGW_SYSROOT} AND NOT "$ENV{MINGW_SYSROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{MINGW_SYSROOT}" MINGW_SYSROOT_DEFAULT)
else()
  set(MINGW_SYSROOT_DEFAULT "")
  if(LLVM_MINGW_ROOT
     AND EXISTS "${LLVM_MINGW_ROOT}/${MINGW_TOOLCHAIN_PREFIX}/include"
     AND EXISTS "${LLVM_MINGW_ROOT}/${MINGW_TOOLCHAIN_PREFIX}/lib")
    set(MINGW_SYSROOT_DEFAULT "${LLVM_MINGW_ROOT}/${MINGW_TOOLCHAIN_PREFIX}")
  elseif(LLVM_MINGW_ROOT)
    set(MINGW_SYSROOT_DEFAULT "${LLVM_MINGW_ROOT}")
  endif()

  if(MINGW_SYSROOT_DEFAULT STREQUAL "")
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
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
  endif()
endif()

if(MINGW_SYSROOT_DEFAULT STREQUAL "")
  set(MINGW_SYSROOT_DEFAULT "/usr/x86_64-w64-mingw32")
endif()

set(MINGW_SYSROOT
    "${MINGW_SYSROOT_DEFAULT}"
    CACHE PATH "Root directory of the MinGW Windows sysroot.")
set(CMAKE_SYSROOT "${MINGW_SYSROOT}")

find_mingw_clang_tool(MMM_MINGW_CLANG_C ${MINGW_TOOLCHAIN_PREFIX}-clang
                      clang-22 clang)
find_mingw_clang_tool(MMM_MINGW_CLANG_CXX ${MINGW_TOOLCHAIN_PREFIX}-clang++
                      clang++-22 clang++)
find_mingw_clang_tool(MMM_MINGW_WINDRES ${MINGW_TOOLCHAIN_PREFIX}-windres
                      llvm-windres)
find_mingw_clang_tool(MMM_MINGW_AR ${MINGW_TOOLCHAIN_PREFIX}-ar llvm-ar)
find_mingw_clang_tool(MMM_MINGW_RANLIB ${MINGW_TOOLCHAIN_PREFIX}-ranlib
                      llvm-ranlib)
find_mingw_clang_tool(MMM_MINGW_STRIP ${MINGW_TOOLCHAIN_PREFIX}-strip
                      llvm-strip)
find_mingw_clang_tool(MMM_MINGW_OBJCOPY ${MINGW_TOOLCHAIN_PREFIX}-objcopy
                      llvm-objcopy)
find_mingw_clang_tool(MMM_MINGW_NM ${MINGW_TOOLCHAIN_PREFIX}-nm llvm-nm-22
                      llvm-nm-21 llvm-nm-20 llvm-nm)
if(NOT MMM_MINGW_CLANG_C)
  message(FATAL_ERROR "找不到 clang，请安装 LLVM clang 22 或提供 LLVM_MINGW_ROOT。")
endif()
if(NOT MMM_MINGW_CLANG_CXX)
  message(FATAL_ERROR "找不到 clang++，请安装 LLVM clang++ 22 或提供 LLVM_MINGW_ROOT。")
endif()
if(NOT MMM_MINGW_WINDRES)
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-windres。")
endif()
if(NOT MMM_MINGW_AR)
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-ar。")
endif()
if(NOT MMM_MINGW_RANLIB)
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-ranlib。")
endif()
if(NOT MMM_MINGW_STRIP)
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-strip。")
endif()
if(NOT MMM_MINGW_OBJCOPY)
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-objcopy。")
endif()
if(NOT MMM_MINGW_NM)
  message(FATAL_ERROR "找不到 llvm-nm 或 ${MINGW_TOOLCHAIN_PREFIX}-nm。")
endif()

set(CMAKE_C_COMPILER
    "${MMM_MINGW_CLANG_C}"
    CACHE FILEPATH "C compiler for MinGW clang cross builds.")
set(CMAKE_CXX_COMPILER
    "${MMM_MINGW_CLANG_CXX}"
    CACHE FILEPATH "C++ compiler for MinGW clang cross builds.")
set(CMAKE_C_COMPILER_TARGET "${MINGW_TARGET_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${MINGW_TARGET_TRIPLE}")

# MSYS2 CLANG64 使用 libc++/compiler-rt/libunwind。这里显式固定运行库，避免宿主 clang 回退到 GCC 的
# libstdc++/libgcc，导致 clang64 产物和 gcc-ucrt64 产物混用。
set(MINGW_CLANG_RUNTIME_FLAGS "-rtlib=compiler-rt -unwindlib=libunwind")
set(MINGW_CLANG_CXX_STDLIB_FLAGS "-stdlib=libc++")
set(CMAKE_C_FLAGS_INIT "")
set(CMAKE_CXX_FLAGS_INIT "${MINGW_CLANG_CXX_STDLIB_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld ${MINGW_CLANG_RUNTIME_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld ${MINGW_CLANG_RUNTIME_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld ${MINGW_CLANG_RUNTIME_FLAGS}")

set(CMAKE_RC_COMPILER
    "${MMM_MINGW_WINDRES}"
    CACHE FILEPATH "Windows resource compiler for MinGW cross builds.")
set(CMAKE_AR
    "${MMM_MINGW_AR}"
    CACHE FILEPATH "Archive tool for MinGW cross builds.")
set(CMAKE_RANLIB
    "${MMM_MINGW_RANLIB}"
    CACHE FILEPATH "Archive index tool for MinGW cross builds.")
set(CMAKE_STRIP
    "${MMM_MINGW_STRIP}"
    CACHE FILEPATH "Strip tool for MinGW cross builds.")
set(CMAKE_OBJCOPY
    "${MMM_MINGW_OBJCOPY}"
    CACHE FILEPATH "Objcopy tool for MinGW cross builds.")
set(CMAKE_NM
    "${MMM_MINGW_NM}"
    CACHE FILEPATH "Symbol table tool for MinGW cross builds.")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 工具程序使用宿主 Linux 环境；库和包同时允许 sysroot 与仓库内显式 预编译库路径，避免 CMake 把 3rdpty/prebuilts
# 的绝对路径重映射到 sysroot。
set(CMAKE_FIND_ROOT_PATH "${MINGW_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# 本工具链使用 Linux clang-22 + MSYS2 CLANG64 sysroot。当前 CI 先禁用项目级
# ThinLTO，避免与第三方预编译库和跨平台调试信息组合时产生额外变量。
set(MMM_DISABLE_CLANG_LTO
    ON
    CACHE BOOL "Disable LLVM ThinLTO for MinGW clang cross builds.")
set(MMM_MINGW_CLANG_USE_GCC_LINKER
    OFF
    CACHE BOOL "Use MinGW GCC driver for final links in clang cross builds.")

# 预编译库目录固定使用 Windows MinGW CLANG64 布局。CLANG64 的 C runtime 是 UCRT， 但 C++ runtime
# 是 libc++，不能和 GCC UCRT64 的 mingw/ucrt64 目录混用。
set(PROJECT_PREBUILT_PLATFORM
    "windows"
    CACHE STRING "Prebuilt platform directory name." FORCE)
set(PROJECT_PREBUILT_TOOLCHAIN
    "mingw"
    CACHE STRING "Prebuilt toolchain directory name." FORCE)
set(PROJECT_PREBUILT_COMPILER_TAG
    "clang64"
    CACHE STRING "Prebuilt compiler/runtime directory name." FORCE)
