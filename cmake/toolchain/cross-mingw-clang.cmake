# Linux 到 Windows MinGW clang 交叉工具链。
#
# 编译和链接优先使用完整 llvm-mingw UCRT 工具链，也兼容 Linux 宿主上的 clang-22/clang++-22 + MinGW
# binutils。CLANG64 使用 UCRT C runtime 与 libc++，预编译库必须写入 mingw/clang64， 避免和 GCC
# UCRT64 的 mingw/ucrt64 布局混用。 搜索顺序优先完整 llvm-mingw 根目录，再退回宿主 PATH 中的版本化工具。
# sysroot 可来自 llvm-mingw、MSYS2 CLANG64 挂载或传统 MinGW 安装。
# 该文件只描述工具链，不在此处探测项目依赖或修改业务目标。 CMAKE_SYSTEM_NAME 必须在 project() 前确定，驱动 CMake
# 进入交叉编译模式。
set(CMAKE_SYSTEM_NAME Windows)
# 预编译目录仅支持 x86_64，因此目标处理器必须显式固定。
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# triple 控制 clang 生成 PE/COFF，prefix 控制配套 binutils 的程序名。
set(MINGW_TARGET_TRIPLE
    "x86_64-w64-windows-gnu"
    CACHE STRING "clang target triple for the MinGW Windows build.")
set(MINGW_TOOLCHAIN_PREFIX
    "x86_64-w64-mingw32"
    CACHE STRING "Prefix for Linux-hosted MinGW binutils.")

if(DEFINED ENV{LLVM_MINGW_ROOT} AND NOT "$ENV{LLVM_MINGW_ROOT}" STREQUAL "")
  # 环境变量适合 CI 临时挂载，转换后再进入缓存路径。
  file(TO_CMAKE_PATH "$ENV{LLVM_MINGW_ROOT}" LLVM_MINGW_ROOT_DEFAULT)
else()
  # 空默认值允许直接使用 PATH 中的 clang 与 MinGW 工具。
  set(LLVM_MINGW_ROOT_DEFAULT "")
endif()
set(LLVM_MINGW_ROOT
    "${LLVM_MINGW_ROOT_DEFAULT}"
    CACHE PATH "Root directory of a complete llvm-mingw toolchain.")

set(_LLVM_MINGW_PROGRAM_PATHS "")
if(LLVM_MINGW_ROOT)
  # 完整 llvm-mingw 发行包将所有宿主工具集中放在 bin 下。
  list(APPEND _LLVM_MINGW_PROGRAM_PATHS "${LLVM_MINGW_ROOT}/bin")
endif()

# 在可选 llvm-mingw 根与宿主 PATH 中查找工具，并把结果返回调用域。 out_var 为接收绝对路径的变量名，ARGN
# 按优先级列出候选程序名。
function(find_mingw_clang_tool out_var)
  if(_LLVM_MINGW_PROGRAM_PATHS)
    # 指定根时先禁止默认路径，避免意外混入另一套 LLVM 组件。
    find_program(
      ${out_var}
      NAMES ${ARGN}
      PATHS ${_LLVM_MINGW_PROGRAM_PATHS}
      NO_DEFAULT_PATH)
  endif()
  if(NOT ${out_var})
    # 完整工具链未提供候选程序时，才允许使用系统 PATH 回退。
    find_program(${out_var} NAMES ${ARGN})
  endif()
  set(${out_var}
      "${${out_var}}"
      PARENT_SCOPE)
endfunction()

# sysroot 环境变量具有最高优先级，并与工具程序来源相互独立。
if(DEFINED ENV{MINGW_SYSROOT} AND NOT "$ENV{MINGW_SYSROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{MINGW_SYSROOT}" MINGW_SYSROOT_DEFAULT)
else()
  set(MINGW_SYSROOT_DEFAULT "")
  # llvm-mingw 常见布局在根目录下再按 target prefix 分层。
  if(LLVM_MINGW_ROOT
     AND EXISTS "${LLVM_MINGW_ROOT}/${MINGW_TOOLCHAIN_PREFIX}/include"
     AND EXISTS "${LLVM_MINGW_ROOT}/${MINGW_TOOLCHAIN_PREFIX}/lib")
    set(MINGW_SYSROOT_DEFAULT "${LLVM_MINGW_ROOT}/${MINGW_TOOLCHAIN_PREFIX}")
  elseif(LLVM_MINGW_ROOT)
    # 某些发行包直接把 include 与 lib 放在根目录，保留该兼容路径。
    set(MINGW_SYSROOT_DEFAULT "${LLVM_MINGW_ROOT}")
  endif()

  if(MINGW_SYSROOT_DEFAULT STREQUAL "")
    # 未提供 llvm-mingw 时，尝试 CI 挂载的 MSYS2 CLANG64 根。
    if(DEFINED ENV{WINDOWS_CROSS_ROOT} AND NOT "$ENV{WINDOWS_CROSS_ROOT}"
                                           STREQUAL "")
      file(TO_CMAKE_PATH "$ENV{WINDOWS_CROSS_ROOT}" WINDOWS_CROSS_ROOT_DEFAULT)
    else()
      # 默认值与 Windows 交叉构建容器的挂载约定一致。
      set(WINDOWS_CROSS_ROOT_DEFAULT "/mnt/cross/windows")
    endif()

    set(MSYS2_CLANG64_SYSROOT_DEFAULT
        "${WINDOWS_CROSS_ROOT_DEFAULT}/msys64/clang64")
    if(EXISTS "${MSYS2_CLANG64_SYSROOT_DEFAULT}/include"
       AND EXISTS "${MSYS2_CLANG64_SYSROOT_DEFAULT}/lib")
      set(MINGW_SYSROOT_DEFAULT "${MSYS2_CLANG64_SYSROOT_DEFAULT}")
    else()
      # 最后询问传统 MinGW GCC，以复用其 Windows SDK/sysroot 布局。
      execute_process(
        COMMAND ${MINGW_TOOLCHAIN_PREFIX}-gcc -print-sysroot
        OUTPUT_VARIABLE MINGW_SYSROOT_DEFAULT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
  endif()
endif()

if(MINGW_SYSROOT_DEFAULT STREQUAL "")
  # 无工具可查询时保留 Debian MinGW 的标准目录作为可诊断默认值。
  set(MINGW_SYSROOT_DEFAULT "/usr/x86_64-w64-mingw32")
endif()

set(MINGW_SYSROOT
    "${MINGW_SYSROOT_DEFAULT}"
    # cache 允许调用脚本覆盖自动探测结果。
    CACHE PATH "Root directory of the MinGW Windows sysroot.")
# CMAKE_SYSROOT 让 clang 与 CMake 查找逻辑共享同一个目标文件系统根。
set(CMAKE_SYSROOT "${MINGW_SYSROOT}")

# 工具程序与 sysroot 可以来自不同安装根。 sysroot 负责目标头与库，编译器程序本身仍从 Linux 主机执行。 C/C++ 编译器优先选择带
# target prefix 的 llvm-mingw 包装器。
find_mingw_clang_tool(MMM_MINGW_CLANG_C ${MINGW_TOOLCHAIN_PREFIX}-clang
                      clang-22 clang)
# C++ 候选优先使用同 prefix 包装器，避免与 C 编译器来自不同发行包。
find_mingw_clang_tool(MMM_MINGW_CLANG_CXX ${MINGW_TOOLCHAIN_PREFIX}-clang++
                      clang++-22 clang++)
# 资源、归档与二进制工具允许使用 MinGW 或等价 LLVM 名称。 windres 负责把图标、manifest 等资源编译为 COFF 对象。
find_mingw_clang_tool(MMM_MINGW_WINDRES ${MINGW_TOOLCHAIN_PREFIX}-windres
                      llvm-windres)
# ar 与 ranlib 共同维护静态库归档及其符号索引。
find_mingw_clang_tool(MMM_MINGW_AR ${MINGW_TOOLCHAIN_PREFIX}-ar llvm-ar)
find_mingw_clang_tool(MMM_MINGW_RANLIB ${MINGW_TOOLCHAIN_PREFIX}-ranlib
                      llvm-ranlib)
# strip 与 objcopy 用于发布裁剪和调试信息检查，必须识别 PE/COFF。
find_mingw_clang_tool(MMM_MINGW_STRIP ${MINGW_TOOLCHAIN_PREFIX}-strip
                      llvm-strip)
find_mingw_clang_tool(MMM_MINGW_OBJCOPY ${MINGW_TOOLCHAIN_PREFIX}-objcopy
                      llvm-objcopy)
# nm 允许多个 LLVM 主版本名称，以兼容 CI 镜像的安装命名方式。
find_mingw_clang_tool(MMM_MINGW_NM ${MINGW_TOOLCHAIN_PREFIX}-nm llvm-nm-22
                      llvm-nm-21 llvm-nm-20 llvm-nm)
if(NOT MMM_MINGW_CLANG_C)
  # 编译器缺失时给出可操作的根目录覆盖入口。
  message(FATAL_ERROR "找不到 clang，请安装 LLVM clang 22 或提供 LLVM_MINGW_ROOT。")
endif()
if(NOT MMM_MINGW_CLANG_CXX)
  # C++ 驱动单独校验，避免误用 clang C 驱动链接 libc++ 目标。
  message(FATAL_ERROR "找不到 clang++，请安装 LLVM clang++ 22 或提供 LLVM_MINGW_ROOT。")
endif()
if(NOT MMM_MINGW_WINDRES)
  # Windows 图标和版本资源要求 PE 资源编译器。
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-windres。")
endif()
if(NOT MMM_MINGW_AR)
  # 静态预编译产物必须使用可处理 COFF 成员的归档器。
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-ar。")
endif()
if(NOT MMM_MINGW_RANLIB)
  # 归档索引用于保证后续链接器稳定发现库内符号。
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-ranlib。")
endif()
if(NOT MMM_MINGW_STRIP)
  # 发布构建的后处理宏依赖该工具，禁止静默跳过工具链完整性检查。
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-strip。")
endif()
if(NOT MMM_MINGW_OBJCOPY)
  # 调试信息和二进制检查流程需要兼容 COFF 的 objcopy。
  message(FATAL_ERROR "找不到 ${MINGW_TOOLCHAIN_PREFIX}-objcopy。")
endif()
if(NOT MMM_MINGW_NM)
  # 符号校验用于检查预编译库 ABI 与调试信息完整性。
  message(FATAL_ERROR "找不到 llvm-nm 或 ${MINGW_TOOLCHAIN_PREFIX}-nm。")
endif()

# 编译器路径进入缓存后，子项目与 try-compile 将复用同一套程序。
set(CMAKE_C_COMPILER
    "${MMM_MINGW_CLANG_C}"
    # 编译器绝对路径传递给所有子构建探针。
    CACHE FILEPATH "C compiler for MinGW clang cross builds.")
# C++ 编译器单独设置，确保 CMake 正确识别链接语言和标准库。
set(CMAKE_CXX_COMPILER
    "${MMM_MINGW_CLANG_CXX}"
    CACHE FILEPATH "C++ compiler for MinGW clang cross builds.")
# target 属性比全局手拼 --target 更容易被 CMake 的编译器检查继承。
set(CMAKE_C_COMPILER_TARGET "${MINGW_TARGET_TRIPLE}")
# 两种语言必须使用同一 target triple，避免归档中混入不同对象格式。
set(CMAKE_CXX_COMPILER_TARGET "${MINGW_TARGET_TRIPLE}")

# MSYS2 CLANG64 使用 libc++/compiler-rt/libunwind。这里显式固定运行库，避免宿主 clang 回退到 GCC 的
# libstdc++/libgcc，导致 clang64 产物和 gcc-ucrt64 产物混用。
set(MINGW_CLANG_RUNTIME_FLAGS "-rtlib=compiler-rt -unwindlib=libunwind")
# C++ 标准库固定 libc++，与 CLANG64 预编译包的 ABI 保持一致。
set(MINGW_CLANG_CXX_STDLIB_FLAGS "-stdlib=libc++")
# 初始 flags 只在编译器首次配置时注入，尊重后续用户追加选项。
set(CMAKE_C_FLAGS_INIT "")
set(CMAKE_CXX_FLAGS_INIT "${MINGW_CLANG_CXX_STDLIB_FLAGS}")
# 三类链接目标都固定 lld、compiler-rt 与 libunwind，不能只配置可执行文件。
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld ${MINGW_CLANG_RUNTIME_FLAGS}")
# shared 与 module 目标不能遗漏相同运行库，否则插件链接结果会产生 ABI 差异。
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld ${MINGW_CLANG_RUNTIME_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld ${MINGW_CLANG_RUNTIME_FLAGS}")

# 以下工具变量必须使用前面解析出的绝对路径，防止子构建再次搜索 PATH。
set(CMAKE_RC_COMPILER
    "${MMM_MINGW_WINDRES}"
    CACHE FILEPATH "Windows resource compiler for MinGW cross builds.")
# 静态归档器来自目标工具链，不能使用 Linux 宿主 ar 处理 COFF。
set(CMAKE_AR
    "${MMM_MINGW_AR}"
    CACHE FILEPATH "Archive tool for MinGW cross builds.")
# ranlib 与 ar 配套，防止不同实现生成不兼容的索引成员。
set(CMAKE_RANLIB
    "${MMM_MINGW_RANLIB}"
    CACHE FILEPATH "Archive index tool for MinGW cross builds.")
# strip 仅处理目标二进制，路径必须指向支持 PE/COFF 的实现。
set(CMAKE_STRIP
    "${MMM_MINGW_STRIP}"
    CACHE FILEPATH "Strip tool for MinGW cross builds.")
# objcopy 为调试辅助保留，必须与产物格式兼容。
set(CMAKE_OBJCOPY
    "${MMM_MINGW_OBJCOPY}"
    CACHE FILEPATH "Objcopy tool for MinGW cross builds.")
# nm 用于预编译包符号验证，不参与实际链接但属于工具链契约。
set(CMAKE_NM
    "${MMM_MINGW_NM}"
    CACHE FILEPATH "Symbol table tool for MinGW cross builds.")

# try-compile 只生成静态库，避免配置阶段依赖尚未完整建立的运行库链接。
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 查找根只描述目标侧头文件和库。 工具程序使用宿主 Linux 环境；库和包同时允许 sysroot 与仓库内显式 预编译库路径，避免 CMake 把
# 3rdpty/prebuilts 的绝对路径重映射到 sysroot。
set(CMAKE_FIND_ROOT_PATH "${MINGW_SYSROOT}")
# 构建时执行的工具属于 Linux 宿主，绝不能从 Windows sysroot 查找。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# 依赖库、头和包配置允许显式绝对预编译路径绕过 sysroot 重映射。
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
# 头文件允许从 sysroot 和显式仓库路径查找，但不会搜索宿主默认头目录。
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
# package 配置遵循相同策略，使预编译 Config.cmake 不被错误重映射。
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# 本工具链使用 Linux clang-22 + MSYS2 CLANG64 sysroot。当前 CI 先禁用项目级
# ThinLTO，避免与第三方预编译库和跨平台调试信息组合时产生额外变量。
set(MMM_DISABLE_CLANG_LTO
    ON
    CACHE BOOL "Disable LLVM ThinLTO for MinGW clang cross builds.")
# 保留诊断开关但默认关闭 GCC driver 回退，正式产物坚持 clang64 ABI。
set(MMM_MINGW_CLANG_USE_GCC_LINKER
    OFF
    CACHE BOOL "Use MinGW GCC driver for final links in clang cross builds.")

# 预编译库目录固定使用 Windows MinGW CLANG64 布局。CLANG64 的 C runtime 是 UCRT， 但 C++ runtime
# 是 libc++，不能和 GCC UCRT64 的 mingw/ucrt64 目录混用。
set(PROJECT_PREBUILT_PLATFORM
    "windows"
    CACHE STRING "Prebuilt platform directory name." FORCE)
# 工具链目录层固定为 mingw，具体运行库差异由 compiler tag 表达。
set(PROJECT_PREBUILT_TOOLCHAIN
    "mingw"
    CACHE STRING "Prebuilt toolchain directory name." FORCE)
# compiler tag 同时编码 libc++ 与 UCRT 组合，是目录选择的 ABI 边界。
set(PROJECT_PREBUILT_COMPILER_TAG
    "clang64"
    CACHE STRING "Prebuilt compiler/runtime directory name." FORCE)
