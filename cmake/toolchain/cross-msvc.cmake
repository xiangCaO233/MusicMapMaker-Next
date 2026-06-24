# cross-msvc.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 基础路径定义。默认面向 Debian CI 的 /mnt/cross/windows 布局，
# 本地机器可通过环境变量或 CMake cache 覆盖。
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
    CACHE PATH "MSVC toolset root directory."
)

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

# 指定编译器。Debian CI 的系统 clang 可能落后于新版 MSVC STL，
# 因此优先使用 apt.llvm.org 安装的版本化 LLVM 工具。
find_program(MMM_CLANG_CL NAMES clang-cl-22 clang-cl-21 clang-cl-20 clang-cl)
find_program(MMM_LLD_LINK NAMES lld-link-22 lld-link-21 lld-link-20 lld-link)
find_program(MMM_LLVM_RC NAMES llvm-rc-22 llvm-rc-21 llvm-rc-20 llvm-rc llvm-rc-19)
find_program(MMM_LLVM_MT NAMES llvm-mt-22 llvm-mt-21 llvm-mt-20 llvm-mt llvm-mt-19)
if(NOT MMM_CLANG_CL)
  message(FATAL_ERROR "找不到 clang-cl，请安装 LLVM clang-cl 20 或更新版本。")
endif()
if(NOT MMM_LLD_LINK)
  message(FATAL_ERROR "找不到 lld-link，请安装 LLVM lld 20 或更新版本。")
endif()
if(NOT MMM_LLVM_RC)
  message(FATAL_ERROR "找不到 llvm-rc，请安装 LLVM resource compiler。")
endif()
if(NOT MMM_LLVM_MT)
  message(FATAL_ERROR "找不到 llvm-mt，请安装 LLVM manifest tool。")
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
set(CMAKE_RC_COMPILER
    "${MMM_LLVM_RC}"
    CACHE FILEPATH "LLVM resource compiler." FORCE)
set(CMAKE_MT
    "${MMM_LLVM_MT}"
    CACHE FILEPATH "LLVM manifest tool." FORCE)

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
    "/libpath:\"${CMAKE_SOURCE_DIR}/lib_proxy\""
)

string(REPLACE ";" " " MSVC_INCLUDE_STR "${MSVC_INCLUDE}")
string(REPLACE ";" " " MSVC_LIB_STR "${MSVC_LIB_PATHS}")

# 将这些参数传给编译器和链接器
set(CMAKE_C_FLAGS
    "${FLAGS} ${WIN_VER_FLAGS} ${MSVC_INCLUDE_STR}"
    CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS
    "${FLAGS} ${WIN_VER_FLAGS} ${MSVC_INCLUDE_STR}"
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
