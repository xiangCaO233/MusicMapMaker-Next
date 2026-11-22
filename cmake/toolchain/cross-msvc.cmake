# cross-msvc.cmake
#
# 本工具链在 Linux 主机上组合 clang-cl、lld-link、挂载的 MSVC 头库与 Windows SDK，产出使用 MSVC ABI 的
# x86_64 Windows 二进制。所有路径变量均保留 cache 覆盖点，供 CI 挂载与本地布局共用。 默认使用 LLVM 23。切换 LLVM
# 时，msvc/2026 预编译目录仍按 MSVC ABI 选择。

# CMAKE_SYSTEM_NAME 必须在 project() 前确定，驱动 CMake 进入交叉编译模式。
set(CMAKE_SYSTEM_NAME Windows)
# 当前预编译库只提供 x86_64 MSVC ABI，不允许由宿主架构隐式推断。
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 基础路径定义。默认面向 Debian CI 的 /mnt/cross/windows 布局， 本地机器可通过环境变量或 CMake cache 覆盖。
if(DEFINED ENV{WINDOWS_CROSS_ROOT} AND NOT "$ENV{WINDOWS_CROSS_ROOT}" STREQUAL
                                       "")
  # 环境变量来自 CI 挂载配置，先规范成 CMake 可移植路径。
  file(TO_CMAKE_PATH "$ENV{WINDOWS_CROSS_ROOT}" WINDOWS_CROSS_ROOT_DEFAULT)
else()
  # Debian CI 镜像约定把 Windows 工具链统一挂载到该目录。
  set(WINDOWS_CROSS_ROOT_DEFAULT "/mnt/cross/windows")
endif()
set(WINDOWS_CROSS_ROOT
    "${WINDOWS_CROSS_ROOT_DEFAULT}"
    CACHE PATH "Root directory containing mounted Windows toolchains.")

if(DEFINED ENV{MSVC_BASE} AND NOT "$ENV{MSVC_BASE}" STREQUAL "")
  # 独立覆盖 MSVC toolset，允许 SDK 与编译器来自不同挂载位置。
  file(TO_CMAKE_PATH "$ENV{MSVC_BASE}" MSVC_BASE_DEFAULT)
else()
  # 默认版本必须与 CI 获取脚本和 msvc/2026 预编译标签同步更新。
  set(MSVC_BASE_DEFAULT
      "${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.52.37025"
  )
endif()
# 已配置的构建树保留 cache 中选定的工具集，普通重配置不会切换 CRT 头库。
set(MSVC_BASE
    "${MSVC_BASE_DEFAULT}"
    CACHE PATH "MSVC toolset root directory.")

if(DEFINED ENV{WINSDK_BASE} AND NOT "$ENV{WINSDK_BASE}" STREQUAL "")
  # SDK 根目录不包含版本层，具体版本由 WINSDK_VER 继续选择。
  file(TO_CMAKE_PATH "$ENV{WINSDK_BASE}" WINSDK_BASE_DEFAULT)
else()
  # 默认路径匹配 Windows Build Tools 安装介质的标准目录结构。
  set(WINSDK_BASE_DEFAULT
      "${WINDOWS_CROSS_ROOT}/Program Files (x86)/Windows Kits/10")
endif()
# 保留 Windows Kits 安装根，头文件和导入库按该根下的同一版本解析。
set(WINSDK_BASE
    "${WINSDK_BASE_DEFAULT}"
    CACHE PATH "Windows SDK root directory.")

if(DEFINED ENV{WINSDK_VER} AND NOT "$ENV{WINSDK_VER}" STREQUAL "")
  # CI 可在不修改工具链文件的情况下切换已挂载的 SDK 版本。
  set(WINSDK_VER_DEFAULT "$ENV{WINSDK_VER}")
else()
  # 版本号必须同时存在于 Include 与 Lib 子目录中。
  set(WINSDK_VER_DEFAULT "10.0.26100.0")
endif()
# SDK 根与版本独立保存，同一安装根下并存多个版本时仍保持选择明确。
set(WINSDK_VER
    "${WINSDK_VER_DEFAULT}"
    CACHE STRING "Windows SDK version directory.")

# 直接使用工具链文件时，版本化入口将默认工具限制在 LLVM 23。 交叉构建脚本可通过 cache 传入另一套已完整解析的 LLVM；msvc/2026
# 仍表示目标 ABI。 资源编译器和归档器随编译器选择，以统一 Windows PE/COFF 工具版本。
find_program(MMM_CLANG_CL NAMES clang-cl-23)
# lld-link 负责最终 PE 链接。 链接器与归档器必须使用 MSVC 风格驱动，不能回退到 GNU ld/ar。
find_program(MMM_LLD_LINK NAMES lld-link-23)
find_program(MMM_LLVM_LIB NAMES llvm-lib-23)
# llvm-rc 编译 Windows 资源脚本。 资源和 manifest 工具负责生成 Windows PE 附属元数据。
find_program(MMM_LLVM_RC NAMES llvm-rc-23)
find_program(MMM_LLVM_MT NAMES llvm-mt-23)
# ranlib 为 COFF 归档补齐索引。 二进制维护工具用于静态归档索引、符号检查、strip 与调试信息处理。
find_program(MMM_LLVM_RANLIB NAMES llvm-ranlib-23)
find_program(MMM_LLVM_STRIP NAMES llvm-strip-23)
find_program(
  MMM_LLVM_NM
  NAMES llvm-nm-23 llvm-nm
  PATHS /opt/llvm-23/bin)
find_program(
  MMM_LLVM_OBJCOPY
  NAMES llvm-objcopy-23 llvm-objcopy
  PATHS /opt/llvm-23/bin)
if(NOT MMM_CLANG_CL)
  # 禁止选择宿主默认 clang，防止编译器主版本与预编译库标签漂移。
  message(FATAL_ERROR "找不到 clang-cl-23，请安装 LLVM 23 clang-cl。")
endif()
if(NOT MMM_LLD_LINK)
  # clang-cl 交叉模式无法调用 Linux 宿主的系统链接器生成 PE。
  message(FATAL_ERROR "找不到 lld-link-23，请安装 LLVM 23 lld。")
endif()
if(NOT MMM_LLVM_LIB)
  # MSVC ABI 静态库需要 COFF archive 工具，GNU ar 不是受支持回退。
  message(FATAL_ERROR "找不到 llvm-lib-23，请安装 LLVM 23 llvm-lib。")
endif()
if(NOT MMM_LLVM_RC)
  # 缺少资源编译器会使图标与版本资源无法进入最终可执行文件。
  message(FATAL_ERROR "找不到 llvm-rc-23，请安装 LLVM 23 resource compiler。")
endif()
if(NOT MMM_LLVM_MT)
  # manifest 工具缺失时无法稳定嵌入运行时权限与兼容性声明。
  message(FATAL_ERROR "找不到 llvm-mt-23，请安装 LLVM 23 manifest tool。")
endif()
if(NOT MMM_LLVM_RANLIB
   OR NOT MMM_LLVM_STRIP
   OR NOT MMM_LLVM_NM
   OR NOT MMM_LLVM_OBJCOPY)
  # 这些工具共同构成预编译库校验和发布流程，必须成套存在。
  message(FATAL_ERROR "找不到完整的 LLVM 23 归档与二进制检查工具。")
endif()

# 编译器 ABI 探针会重新加载工具链。将显式指定的 LLVM 和 SDK 路径传入子配置， 使探针与主构建使用同一套工具和目标头库。显式路径可在 PATH
# 缺少某个版本别名时使用， 因此探针不能依赖默认的版本化命令重新解析编译器。
list(
  APPEND
  CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  WINDOWS_CROSS_ROOT
  MSVC_BASE
  WINSDK_BASE
  WINSDK_VER
  MMM_CLANG_CL
  MMM_LLD_LINK
  MMM_LLVM_LIB
  MMM_LLVM_RC
  MMM_LLVM_MT
  MMM_LLVM_RANLIB
  MMM_LLVM_STRIP
  MMM_LLVM_NM
  MMM_LLVM_OBJCOPY)
# CMake 在一次配置内可能重复包含此文件，去重后每个变量只需传递一次。
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

# 强制写入缓存可覆盖 CMake 首次探测出的宿主编译器。
set(CMAKE_C_COMPILER
    "${MMM_CLANG_CL}"
    # C 探针也必须采用 MSVC frontend 语义。
    CACHE FILEPATH "MSVC-like LLVM C compiler." FORCE)
# C 与 C++ 都由 clang-cl 驱动，但分别写入 CMake 对应语言槽位。 CXX 槽位单独声明以启用 C++ 探针。
set(CMAKE_CXX_COMPILER
    "${MMM_CLANG_CL}"
    # C++ 探针负责确认 MSVC ABI 标准库可见。
    CACHE FILEPATH "MSVC-like LLVM C++ compiler." FORCE)
# 直接记录 lld-link 路径，供后面的自定义链接规则引用。
set(CMAKE_LINKER
    "${MMM_LLD_LINK}"
    # 链接器路径不交由 clang-cl 二次猜测。
    CACHE FILEPATH "MSVC-like LLVM linker." FORCE)
# llvm-lib 生成 MSVC ABI 使用的 COFF 静态库，而非 GNU archive。 CMAKE_AR 必须指向 COFF 归档器。
set(CMAKE_AR
    "${MMM_LLVM_LIB}"
    # 静态库必须使用 COFF library manager。
    CACHE FILEPATH "MSVC-like LLVM static library manager." FORCE)
# 资源编译器把 .rc 输入转换为链接器可消费的 COFF 资源对象。
set(CMAKE_RC_COMPILER
    "${MMM_LLVM_RC}"
    # 资源对象必须匹配 x86_64 PE 目标。
    CACHE FILEPATH "LLVM resource compiler." FORCE)
# manifest 工具由 CMake 的 Windows 平台规则调用以嵌入声明。 manifest 工具仅由 Windows 平台规则调用。
set(CMAKE_MT
    "${MMM_LLVM_MT}"
    # manifest 在链接后嵌入目标 PE 文件。
    CACHE FILEPATH "LLVM manifest tool." FORCE)
# ranlib、nm、strip 与 objcopy 均锁定 LLVM 版本，服务预编译检查流程。
set(CMAKE_RANLIB
    "${MMM_LLVM_RANLIB}"
    # 归档索引器与 llvm-lib 版本保持一致。
    CACHE FILEPATH "LLVM archive indexer." FORCE)
# nm 只读取符号表，用于 CI 验证导出和对象架构。
set(CMAKE_NM
    "${MMM_LLVM_NM}"
    # 符号检查不能调用仅识别 ELF 的宿主工具。
    CACHE FILEPATH "LLVM symbol inspector." FORCE)
# strip 由发布目标后处理宏调用，Debug 静态库不会经过该步骤。 strip 保留为发布目标的后处理工具。
set(CMAKE_STRIP
    "${MMM_LLVM_STRIP}"
    CACHE FILEPATH "LLVM strip tool." FORCE)
# objcopy 为需要拆分或检查调试段的辅助流程提供统一入口。 所有工具路径在配置缓存中强制固定。
set(CMAKE_OBJCOPY
    "${MMM_LLVM_OBJCOPY}"
    CACHE FILEPATH "LLVM object copy tool." FORCE)

# 告诉 clang-cl 目标平台 显式 triple 防止 clang-cl 根据 Linux 宿主选择 ELF 目标。
set(MSVC_TARGET_TRIPLE x86_64-pc-windows-msvc)
# /EHsc: 开启异常支持 -fms-compatibility-version=19: 模拟 MSVC 2015+ -fms-compatibility:
# 开启更多 MSVC 兼容特性 -D__FMA__: 解决 Clang builtin 与 MSVC <complex> 的冲突
set(FLAGS
    "--target=${MSVC_TARGET_TRIPLE} -Xclang -fms-compatibility-version=19.41 -fms-compatibility /EHsc -D__FMA__ -D_CRT_DECLARE_NON_CONSTEXPR_FMA_INTRINSICS"
)

# 统一 Windows 版本定义 (Windows 10) _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000
# (NTDDI_WIN10) 三个宏保持同一最低系统版本，避免 SDK 头暴露不一致的 API 集。
set(WIN_VER_FLAGS
    "-D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000000 -DWINVER=0x0A00")

# 禁用 C++20 Modules 扫描 (clang-cl 在交叉编译下支持不佳) 依赖工程也读取 ALSOFT_ENABLE_MODULES，因此同步关闭
# OpenAL Soft 模块。
set(CMAKE_CXX_SCAN_FOR_MODULES
    OFF
    CACHE BOOL "" FORCE)
# OpenAL Soft 子构建也禁用模块，避免交叉 clang-cl 探针失败。
set(ALSOFT_ENABLE_MODULES
    OFF
    CACHE BOOL "" FORCE)

# clang-cl 的 Linux 交叉编译不会生成 cl.exe 的编译器 PDB，因此将完整 CodeView 调试信息保留在 COFF
# 对象中，随静态归档一起分发。
if(POLICY CMP0141)
  # 新策略允许通过抽象变量选择 CodeView 位置，而非手拼 /Z7。
  cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
    "Embedded"
    CACHE STRING "Embed CodeView debug information in clang-cl prebuilts."
          FORCE)

# --- 核心：配置头文件搜索路径 (-imsvc 模拟 MSVC 的包含逻辑) ---
# 额外包含 lowercase 代理目录以解决 Linux 大小写敏感问题 代理路径位于项目根，内容由 lowercase 生成脚本维护。 独立依赖工程的
# CMAKE_SOURCE_DIR 不再是仓库根，代理路径必须锚定工具链位置。 否则 ExternalProject 重新加载工具链时会丢失 SDK
# 大小写别名。
get_filename_component(MMM_CROSS_PROXY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.."
                       ABSOLUTE)
set(PROXY_INCLUDE "${MMM_CROSS_PROXY_ROOT}/include_proxy")

# include_proxy 提供 Windows SDK 大小写别名，其余目录保持 cl.exe 搜索顺序。 MSVC 与 ATL/MFC 头位于
# toolset 根，UCRT、Win32 和 WinRT 头位于 SDK 根。
set(MSVC_INCLUDE
    "-imsvc \"${PROXY_INCLUDE}\""
    "-imsvc \"${MSVC_BASE}/include\""
    "-imsvc \"${MSVC_BASE}/atlmfc/include\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/shared\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/um\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/winrt\"")

# --- 核心：配置库文件搜索路径 (/libpath) ---
# 顺序从 MSVC、ATL/MFC 到 SDK，最后才允许仓库代理库补足别名。 lib_proxy 只解决大小写或命名兼容，不应抢先覆盖真实工具链库。
set(MSVC_LIB_PATHS
    "/libpath:\"${MSVC_BASE}/lib/x64\""
    "/libpath:\"${MSVC_BASE}/atlmfc/lib/x64\""
    "/libpath:\"${WINSDK_BASE}/Lib/${WINSDK_VER}/ucrt/x64\""
    "/libpath:\"${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64\""
    "/libpath:\"${MMM_CROSS_PROXY_ROOT}/lib_proxy\"")

string(REPLACE ";" " " MSVC_INCLUDE_STR "${MSVC_INCLUDE}")
# CMake 初始 flags 接受单字符串，因此把列表边界转换为空格。
string(REPLACE ";" " " MSVC_LIB_STR "${MSVC_LIB_PATHS}")

# 将这些参数传给编译器和链接器 FORCE 确保已有构建缓存不会残留宿主编译器自动探测出的 flags。
set(CMAKE_C_FLAGS
    "${FLAGS} ${WIN_VER_FLAGS} ${MSVC_INCLUDE_STR}"
    CACHE STRING "" FORCE)
# 两种语言共享 target、兼容级别、Windows 版本和系统头搜索配置。
set(CMAKE_CXX_FLAGS
    "${FLAGS} ${WIN_VER_FLAGS} ${MSVC_INCLUDE_STR}"
    CACHE STRING "" FORCE)
# Debug 使用嵌入式 CodeView 并关闭优化，静态库无需旁路编译器 PDB。
set(CMAKE_C_FLAGS_DEBUG
    "/Z7 /Ob0 /Od /RTC1"
    CACHE STRING "" FORCE)
# C++ Debug 标志必须与 C 保持一致，避免混合运行库和优化级别。
set(CMAKE_CXX_FLAGS_DEBUG
    "/Z7 /Ob0 /Od /RTC1"
    CACHE STRING "" FORCE)
# RelWithDebInfo 保留 CodeView，同时启用与发布构建相近的优化级别。
set(CMAKE_C_FLAGS_RELWITHDEBINFO
    "/Z7 /O2 /Ob1 /DNDEBUG"
    CACHE STRING "" FORCE)
# C++ RelWithDebInfo 同样将 CodeView 嵌入对象，供静态归档分发。
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO
    "/Z7 /O2 /Ob1 /DNDEBUG"
    CACHE STRING "" FORCE)
# 可执行文件与动态库共享同一组 Windows SDK 和 MSVC 库目录。
set(CMAKE_EXE_LINKER_FLAGS
    "${MSVC_LIB_STR}"
    CACHE STRING "" FORCE)
# 动态库链接也需要同一 SDK 路径，否则导入库解析会与可执行文件不同。
set(CMAKE_SHARED_LINKER_FLAGS
    "${MSVC_LIB_STR}"
    CACHE STRING "" FORCE)

# 配置 RC 编译器路径 llvm-rc 不继承 clang-cl 的 -imsvc 参数，需要单独传递资源头目录。 资源编译只需要代理、MSVC 基础头和
# SDK 的 ucrt/shared/um 子集。
set(CMAKE_RC_FLAGS
    "-I\"${PROXY_INCLUDE}\" -I\"${MSVC_BASE}/include\" -I\"${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt\" -I\"${WINSDK_BASE}/Include/${WINSDK_VER}/shared\" -I\"${WINSDK_BASE}/Include/${WINSDK_VER}/um\""
    CACHE STRING "" FORCE)

# 修复 clang-cl 找不到本地链接器的问题 覆盖规则直接调用已解析的 lld-link，绕过 clang-cl 对宿主路径的推断。
set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_LINKER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
)
# C++ 规则额外保留 CMAKE_CXX_LINK_FLAGS，供配置与用户覆盖继续生效。
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_LINKER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
)
