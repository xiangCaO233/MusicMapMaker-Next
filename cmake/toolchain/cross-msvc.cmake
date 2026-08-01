# cross-msvc.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 基础路径定义。默认面向 Debian CI 的 /mnt/cross/windows 布局， 本地机器可通过环境变量或 CMake cache 覆盖。
if(DEFINED ENV{WINDOWS_CROSS_ROOT} AND NOT "$ENV{WINDOWS_CROSS_ROOT}" STREQUAL
                                       "")
  file(TO_CMAKE_PATH "$ENV{WINDOWS_CROSS_ROOT}" WINDOWS_CROSS_ROOT_DEFAULT)
else()
  set(WINDOWS_CROSS_ROOT_DEFAULT "/mnt/cross/windows")
endif()
set(WINDOWS_CROSS_ROOT
    "${WINDOWS_CROSS_ROOT_DEFAULT}"
    CACHE PATH "Root directory containing mounted Windows toolchains.")

if(DEFINED ENV{MSVC_BASE} AND NOT "$ENV{MSVC_BASE}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{MSVC_BASE}" MSVC_BASE_DEFAULT)
else()
  set(MSVC_BASE_DEFAULT
      "${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.51.36231"
  )
endif()
set(MSVC_BASE
    "${MSVC_BASE_DEFAULT}"
    CACHE PATH "MSVC toolset root directory.")

if(DEFINED ENV{WINSDK_BASE} AND NOT "$ENV{WINSDK_BASE}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{WINSDK_BASE}" WINSDK_BASE_DEFAULT)
else()
  set(WINSDK_BASE_DEFAULT
      "${WINDOWS_CROSS_ROOT}/Program Files (x86)/Windows Kits/10")
endif()
set(WINSDK_BASE
    "${WINSDK_BASE_DEFAULT}"
    CACHE PATH "Windows SDK root directory.")

if(DEFINED ENV{WINSDK_VER} AND NOT "$ENV{WINSDK_VER}" STREQUAL "")
  set(WINSDK_VER_DEFAULT "$ENV{WINSDK_VER}")
else()
  set(WINSDK_VER_DEFAULT "10.0.26100.0")
endif()
set(WINSDK_VER
    "${WINSDK_VER_DEFAULT}"
    CACHE STRING "Windows SDK version directory.")

# 指定 LLVM 22 工具。MSVC 预编译库以 2026 标签发布，必须固定 clang-cl/lld-link/llvm-lib 的主版本，避免 CI
# 机器上旧版 LLVM 被误选。
find_program(MMM_CLANG_CL NAMES clang-cl-22)
find_program(MMM_LLD_LINK NAMES lld-link-22)
find_program(MMM_LLVM_LIB NAMES llvm-lib-22)
find_program(MMM_LLVM_RC NAMES llvm-rc-22)
find_program(MMM_LLVM_MT NAMES llvm-mt-22)
find_program(MMM_LLVM_RANLIB NAMES llvm-ranlib-22)
find_program(MMM_LLVM_STRIP NAMES llvm-strip-22)
find_program(
  MMM_LLVM_NM
  NAMES llvm-nm-22 llvm-nm
  PATHS /opt/llvm-22/bin)
find_program(
  MMM_LLVM_OBJCOPY
  NAMES llvm-objcopy-22 llvm-objcopy
  PATHS /opt/llvm-22/bin)
if(NOT MMM_CLANG_CL)
  message(FATAL_ERROR "找不到 clang-cl-22，请安装 LLVM 22 clang-cl。")
endif()
if(NOT MMM_LLD_LINK)
  message(FATAL_ERROR "找不到 lld-link-22，请安装 LLVM 22 lld。")
endif()
if(NOT MMM_LLVM_LIB)
  message(FATAL_ERROR "找不到 llvm-lib-22，请安装 LLVM 22 llvm-lib。")
endif()
if(NOT MMM_LLVM_RC)
  message(FATAL_ERROR "找不到 llvm-rc-22，请安装 LLVM 22 resource compiler。")
endif()
if(NOT MMM_LLVM_MT)
  message(FATAL_ERROR "找不到 llvm-mt-22，请安装 LLVM 22 manifest tool。")
endif()
if(NOT MMM_LLVM_RANLIB
   OR NOT MMM_LLVM_STRIP
   OR NOT MMM_LLVM_NM
   OR NOT MMM_LLVM_OBJCOPY)
  message(FATAL_ERROR "找不到完整的 LLVM 22 归档与二进制检查工具。")
endif()
set(CMAKE_C_COMPILER
    "${MMM_CLANG_CL}"
    CACHE FILEPATH "MSVC-like LLVM C compiler." FORCE)
set(CMAKE_CXX_COMPILER
    "${MMM_CLANG_CL}"
    CACHE FILEPATH "MSVC-like LLVM C++ compiler." FORCE)
set(CMAKE_LINKER
    "${MMM_LLD_LINK}"
    CACHE FILEPATH "MSVC-like LLVM linker." FORCE)
set(CMAKE_AR
    "${MMM_LLVM_LIB}"
    CACHE FILEPATH "MSVC-like LLVM static library manager." FORCE)
set(CMAKE_RC_COMPILER
    "${MMM_LLVM_RC}"
    CACHE FILEPATH "LLVM resource compiler." FORCE)
set(CMAKE_MT
    "${MMM_LLVM_MT}"
    CACHE FILEPATH "LLVM manifest tool." FORCE)
set(CMAKE_RANLIB
    "${MMM_LLVM_RANLIB}"
    CACHE FILEPATH "LLVM archive indexer." FORCE)
set(CMAKE_NM
    "${MMM_LLVM_NM}"
    CACHE FILEPATH "LLVM symbol inspector." FORCE)
set(CMAKE_STRIP
    "${MMM_LLVM_STRIP}"
    CACHE FILEPATH "LLVM strip tool." FORCE)
set(CMAKE_OBJCOPY
    "${MMM_LLVM_OBJCOPY}"
    CACHE FILEPATH "LLVM object copy tool." FORCE)

# 告诉 clang-cl 目标平台
set(MSVC_TARGET_TRIPLE x86_64-pc-windows-msvc)
# /EHsc: 开启异常支持 -fms-compatibility-version=19: 模拟 MSVC 2015+ -fms-compatibility:
# 开启更多 MSVC 兼容特性 -D__FMA__: 解决 Clang builtin 与 MSVC <complex> 的冲突
set(FLAGS
    "--target=${MSVC_TARGET_TRIPLE} -Xclang -fms-compatibility-version=19.41 -fms-compatibility /EHsc -D__FMA__ -D_CRT_DECLARE_NON_CONSTEXPR_FMA_INTRINSICS"
)

# 统一 Windows 版本定义 (Windows 10) _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000
# (NTDDI_WIN10)
set(WIN_VER_FLAGS
    "-D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000000 -DWINVER=0x0A00")

# 禁用 C++20 Modules 扫描 (clang-cl 在交叉编译下支持不佳)
set(CMAKE_CXX_SCAN_FOR_MODULES
    OFF
    CACHE BOOL "" FORCE)
set(ALSOFT_ENABLE_MODULES
    OFF
    CACHE BOOL "" FORCE)

# clang-cl 的 Linux 交叉编译不会生成 cl.exe 的编译器 PDB，因此将完整 CodeView 调试信息保留在 COFF
# 对象中，随静态归档一起分发。
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
    "Embedded"
    CACHE STRING "Embed CodeView debug information in clang-cl prebuilts."
          FORCE)

# --- 核心：配置头文件搜索路径 (-imsvc 模拟 MSVC 的包含逻辑) ---
# 额外包含 lowercase 代理目录以解决 Linux 大小写敏感问题
set(PROXY_INCLUDE "${CMAKE_SOURCE_DIR}/include_proxy")

set(MSVC_INCLUDE
    "-imsvc \"${PROXY_INCLUDE}\""
    "-imsvc \"${MSVC_BASE}/include\""
    "-imsvc \"${MSVC_BASE}/atlmfc/include\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/shared\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/um\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/winrt\"")

# --- 核心：配置库文件搜索路径 (/libpath) ---
set(MSVC_LIB_PATHS
    "/libpath:\"${MSVC_BASE}/lib/x64\""
    "/libpath:\"${MSVC_BASE}/atlmfc/lib/x64\""
    "/libpath:\"${WINSDK_BASE}/Lib/${WINSDK_VER}/ucrt/x64\""
    "/libpath:\"${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64\""
    "/libpath:\"${CMAKE_SOURCE_DIR}/lib_proxy\"")

string(REPLACE ";" " " MSVC_INCLUDE_STR "${MSVC_INCLUDE}")
string(REPLACE ";" " " MSVC_LIB_STR "${MSVC_LIB_PATHS}")

# 将这些参数传给编译器和链接器
set(CMAKE_C_FLAGS
    "${FLAGS} ${WIN_VER_FLAGS} ${MSVC_INCLUDE_STR}"
    CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS
    "${FLAGS} ${WIN_VER_FLAGS} ${MSVC_INCLUDE_STR}"
    CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_DEBUG
    "/Z7 /Ob0 /Od /RTC1"
    CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG
    "/Z7 /Ob0 /Od /RTC1"
    CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO
    "/Z7 /O2 /Ob1 /DNDEBUG"
    CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO
    "/Z7 /O2 /Ob1 /DNDEBUG"
    CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS
    "${MSVC_LIB_STR}"
    CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS
    "${MSVC_LIB_STR}"
    CACHE STRING "" FORCE)

# 配置 RC 编译器路径
set(CMAKE_RC_FLAGS
    "-I\"${PROXY_INCLUDE}\" -I\"${MSVC_BASE}/include\" -I\"${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt\" -I\"${WINSDK_BASE}/Include/${WINSDK_VER}/shared\" -I\"${WINSDK_BASE}/Include/${WINSDK_VER}/um\""
    CACHE STRING "" FORCE)

# 修复 clang-cl 找不到本地链接器的问题
set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_LINKER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
)
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_LINKER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
)

set(VCPKG_ROOT
    "${WINDOWS_CROSS_ROOT}/vcpkg"
    CACHE PATH "vcpkg root for Windows cross builds." FORCE)
list(APPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed/x64-windows-static")
