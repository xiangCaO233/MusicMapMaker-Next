# Linux 到 Windows MinGW clang 交叉工具链。
#
# 编译使用 clang/clang++ + x86_64-w64-windows-gnu target，最终链接交给
# x86_64-w64-mingw32-gcc/g++，以兼容只提供静态 GCC runtime 的 MinGW sysroot。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TARGET_TRIPLE
    "x86_64-w64-windows-gnu"
    CACHE STRING "clang target triple for the MinGW Windows build.")
set(MINGW_TOOLCHAIN_PREFIX
    "x86_64-w64-mingw32"
    CACHE STRING "Prefix for MinGW binutils and GCC linker driver.")

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
    clang
    CACHE FILEPATH "C compiler for MinGW clang cross builds.")
set(CMAKE_CXX_COMPILER
    clang++
    CACHE FILEPATH "C++ compiler for MinGW clang cross builds.")
set(CMAKE_C_COMPILER_TARGET "${MINGW_TARGET_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${MINGW_TARGET_TRIPLE}")

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

# CMake 的编译器探测会尝试链接测试程序；这里提前使用项目专用链接规则，
# 避免 clang driver 在不完整 compiler-rt/libgcc_s 环境下误判编译器不可用。
set(CMAKE_USER_MAKE_RULES_OVERRIDE
    "${CMAKE_CURRENT_LIST_DIR}/cross-mingw-clang-rules.cmake")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 工具程序使用宿主 Linux 环境；库和包同时允许 sysroot 与仓库内显式
# 预编译库路径，避免 CMake 把 3rdpty/prebuilts 的绝对路径重映射到 sysroot。
set(CMAKE_FIND_ROOT_PATH "${MINGW_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# 本工具链链接阶段由 MinGW GCC driver 负责，不能启用 LLVM ThinLTO。
set(MMM_DISABLE_CLANG_LTO
    ON
    CACHE BOOL "Disable LLVM ThinLTO for clang compile plus GCC link builds.")
set(MMM_MINGW_CLANG_USE_GCC_LINKER
    ON
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
