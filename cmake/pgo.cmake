# ==============================================================================
# PGO (Profile-Guided Optimization) 自动化工作流 — Clang ThinLTO
#
# profile 数据源 (MMM_PGO_USE=ON 时以下三选一):
#
# A) 预合并文件   -DMMM_PGO_DATA=merged.profdata B) 本地目录
# -DMMM_PGO_SOURCE_DIR=/path/to/profiles  (自动合并 *.profraw) C) 远程 URL
# -DMMM_PGO_SOURCE_URL=https://server.com/pgo/  (自动下载+合并)
#
# 三种构建模式:
#
# 1) 纯插桩 (MMM_PGO_INSTRUMENT=ON): cmake -B build_pgo -G Ninja
# -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMMM_PGO_INSTRUMENT=ON
# -DMMM_PGO_UPLOAD_URL="https://your-server.com/pgo/upload"
#
# 2) 纯优化 (MMM_PGO_USE=ON + 数据源): cmake -B build_pgo -G Ninja
# -DCMAKE_BUILD_TYPE=Release -DMMM_PGO_USE=ON
# -DMMM_PGO_SOURCE_URL="https://your-server.com/pgo/" # 或: cmake -B build_pgo -G
# Ninja -DMMM_PGO_USE=ON -DMMM_PGO_SOURCE_DIR=C:/profiles # 或: cmake -B
# build_pgo -G Ninja -DMMM_PGO_USE=ON -DMMM_PGO_DATA=merged.profdata
#
# 工作流循环 (profile 在版本间累积): 周期 N  : 插桩版 (Build A) → 分发用户 → 服务器合并 profile[0..N] 周期
# N+1: 优化版 (Build B) → 使用 profile[0..N] 编译最终发行版 同时继续: 插桩版 (Build A) → 分发更多用户 →
# 扩大 profile 覆盖
#
# Build A 和 Build B 是两个独立构建目录，不能合并！ Clang 不允许 -fprofile-instr-use 和
# -fprofile-instr-generate 同时用。 每轮把 Build B 的 -DMMM_PGO_SOURCE_DIR/URL 指向服务器合并后的
# 最新 profile 数据即可实现持续优化。
#
# 配置阶段把三种输入统一解析为一个本地 .profdata 路径。 插桩与优化标志只添加到随后创建的项目业务目标，不作用于此前的依赖。
# 远程下载和本地合并均在构建目录生成中间文件，不修改采样源。 任何名义启用但缺少有效数据或 LLVM 工具的状态都应立即配置失败。
# ==============================================================================

set(MMM_PGO_IS_LLVM_COMPILER OFF)
# 当前 profile 格式和合并工具均来自 LLVM，因此只接受 Clang 系列编译器。
if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
  set(MMM_PGO_IS_LLVM_COMPILER ON)
endif()

set(MMM_PGO_IS_MSVC_LIKE_CLANG_CROSS OFF)
# 交叉 clang-cl 需要单独识别，因为其 profile 运行库解析不同于本机 clang-cl。
if(MMM_PGO_IS_LLVM_COMPILER
   AND CMAKE_CROSSCOMPILING
   AND ("${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "MSVC"
        OR "${CMAKE_CXX_SIMULATE_ID}" STREQUAL "MSVC"))
  set(MMM_PGO_IS_MSVC_LIKE_CLANG_CROSS ON)
endif()

option(MMM_PGO_USE "Build using PGO profile data" OFF)

# 开启优化模式后先验证编译器能力。 使用 profile 的优化构建与采集 profile 的插桩构建必须互斥。
if(MMM_PGO_USE)
  if(NOT MMM_PGO_IS_LLVM_COMPILER)
    # 非 LLVM 编译器无法消费本工作流生成的 profdata，配置阶段立即失败。
    message(
      FATAL_ERROR
        "PGO: the current profile workflow uses LLVM .profraw/.profdata files "
        "and requires Clang/LLVM. Configure with Clang or set MMM_PGO_USE=OFF.")
  endif()

  # PGO optimization and instrumentation cannot be used together. When
  # MMM_PGO_USE is enabled, we force disable instrumentation.
  set(MMM_PGO_INSTRUMENT
      OFF
      CACHE BOOL "Build with PGO instrumentation for profile collection" FORCE)
  message(
    STATUS
      "PGO: MMM_PGO_USE is enabled. Disabling PGO instrumentation (MMM_PGO_INSTRUMENT=OFF)."
  )
else()
  # LLVM 工具链默认采集 profile；交叉 clang-cl 在下方显式解析目标运行库。
  set(MMM_PGO_INSTRUMENT_DEFAULT ${MMM_PGO_IS_LLVM_COMPILER})
  option(MMM_PGO_INSTRUMENT
         "Build with PGO instrumentation for profile collection"
         ${MMM_PGO_INSTRUMENT_DEFAULT})
endif()

# 对缓存中遗留的无效组合再次防御，确保切换编译器后不会误用旧选项。
if(MMM_PGO_INSTRUMENT AND NOT MMM_PGO_IS_LLVM_COMPILER)
  message(
    WARNING
      "PGO: disabling instrumentation because ${CMAKE_CXX_COMPILER_ID} does not "
      "support this project's LLVM profile workflow.")
  set(MMM_PGO_INSTRUMENT
      OFF
      CACHE BOOL "Build with PGO instrumentation for profile collection" FORCE)
endif()

# 交叉链接直接调用 lld-link，不能依赖 clang-cl 驱动补齐 compiler-rt 搜索路径。 允许 CI 显式挂载与所选 LLVM
# 版本匹配的 Windows x86_64 profile 运行库。
set(MMM_PGO_PROFILE_RUNTIME
    ""
    CACHE FILEPATH "Windows compiler-rt profile library for cross clang-cl PGO")
# 公共缓存只保存目标 Windows 运行库。 未插桩或非交叉构建不会因该缓存为空而增加依赖要求， 宿主 Linux 的 compiler-rt
# 安装仍由原有编译器驱动处理。
if(MMM_PGO_INSTRUMENT AND MMM_PGO_IS_MSVC_LIKE_CLANG_CROSS)
  if("${MMM_PGO_PROFILE_RUNTIME}" STREQUAL "")
    # 资源目录来自实际编译器，避免 PATH 上另一 LLVM 版本的运行库混入。 Linux 安装通常只提供宿主库，查询成功不能代替目标运行库存在检查。
    # 查询失败统一交由后续缺库错误处理，用户仍可通过公共缓存提供独立挂载的运行库。
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
      OUTPUT_VARIABLE _mmm_pgo_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REGEX MATCH "^[0-9]+" _mmm_pgo_llvm_major
                 "${CMAKE_CXX_COMPILER_VERSION}")
    # 主版本取自 CMake 已识别的编译器，交叉脚本选择其他 LLVM 版本时同步改变搜索目录。 编译器资源目录保留完整安装信息，挂载默认路径则使用
    # LLVM 的主版本目录约定。 仅搜索 Windows 布局；宿主 Linux 的同名归档不适用于 MSVC ABI。
    set(_mmm_pgo_runtime_hints
        "${_mmm_pgo_resource_dir}/lib/windows"
        "${_mmm_pgo_resource_dir}/lib/x86_64-pc-windows-msvc")
    # 传统 Windows 子目录和按目标 triple 安装的子目录均可提供 MSVC ABI 库。 候选中不包含资源目录的 Linux
    # 子目录，不允许同名 ELF 归档替代 COFF 运行库。
    if(WINDOWS_CROSS_ROOT)
      # 挂载布局兼容独立 LLVM 安装及 Windows 默认 Program Files 目录。
      list(
        APPEND
        _mmm_pgo_runtime_hints
        "${WINDOWS_CROSS_ROOT}/LLVM/lib/clang/${_mmm_pgo_llvm_major}/lib/windows"
        "${WINDOWS_CROSS_ROOT}/Program Files/LLVM/lib/clang/${_mmm_pgo_llvm_major}/lib/windows"
      )
    endif()
    if(MSVC_BASE)
      # Visual Studio 的 LLVM 位于 VC/Tools/Llvm/x64，与 MSVC toolset 并列。 从 MSVC
      # 具体版本返回两层得到 Tools；SDK 的 UCRT 目录没有 LLVM profile 运行库。 路径仍从用户指定的 MSVC_BASE
      # 派生，避免把固定 Visual Studio 版本写入 PGO 模块。
      list(
        APPEND
        _mmm_pgo_runtime_hints
        "${MSVC_BASE}/../../Llvm/x64/lib/clang/${_mmm_pgo_llvm_major}/lib/windows"
      )
    endif()
    find_file(
      _mmm_pgo_runtime
      NAMES clang_rt.profile-x86_64.lib clang_rt.profile.lib
      HINTS ${_mmm_pgo_runtime_hints}
      NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE)
    # 禁止默认搜索和再次应用 sysroot，保持挂载绝对路径的含义不变。 临时搜索结果不缓存，失败后重新配置仍能发现随后安装或挂载的目标库。
    if(_mmm_pgo_runtime)
      # 自动发现也记录为显式绝对路径，后续链接不再猜测默认库名。
      set(MMM_PGO_PROFILE_RUNTIME
          "${_mmm_pgo_runtime}"
          CACHE FILEPATH
                "Windows compiler-rt profile library for cross clang-cl PGO"
                FORCE)
    endif()
  endif()
  # 配置失败代替静默关闭；调用方可以提供运行库或明确选择普通构建。 显式无效路径不能被自动发现覆盖，否则会隐藏 CI 参数或挂载错误。
  # 绝对路径让配置验证与直接链接引用同一文件，不受生成器工作目录影响。 扩展名拒绝宿主 .a 归档，实际架构及 LLVM 版本兼容性仍由运行库提供方保证。
  if(NOT IS_ABSOLUTE "${MMM_PGO_PROFILE_RUNTIME}"
     OR NOT EXISTS "${MMM_PGO_PROFILE_RUNTIME}"
     OR IS_DIRECTORY "${MMM_PGO_PROFILE_RUNTIME}"
     OR NOT MMM_PGO_PROFILE_RUNTIME MATCHES "\\.lib$")
    message(
      FATAL_ERROR
        "PGO: cross clang-cl instrumentation requires a Windows x86_64 compiler-rt profile .lib matching the selected LLVM version. "
        "Set -DMMM_PGO_PROFILE_RUNTIME=/absolute/path/to/clang_rt.profile-x86_64.lib, "
        "or disable instrumentation with --no-pgo-instrument in the CI build script (-DMMM_PGO_INSTRUMENT=OFF in CMake)."
    )
  endif()
  message(STATUS "PGO: Windows profile runtime = ${MMM_PGO_PROFILE_RUNTIME}")
  # 已缓存的运行库不会随编译器切换自动替换。 调整 LLVM 主版本时，应使用独立构建目录， 或通过 MMM_PGO_PROFILE_RUNTIME
  # 显式同步替换目标运行库，避免跨版本链接。
endif()

# --- 数据源 (三选一) ---
set(MMM_PGO_DATA
    ""
    # 单文件来源可以是本地路径或 HTTP URL。
    CACHE FILEPATH "Path to pre-merged .profdata file")
# 目录源接受多个 profraw，由合并脚本在配置阶段生成单一结果。
set(MMM_PGO_PROFILE_DIR
    ""
    # 原始采样目录不会被合并脚本修改。
    CACHE STRING "Directory containing .profraw files to auto-merge")
set(MMM_PGO_SOURCE_URL
    ""
    CACHE STRING
          "URL to download profiles from (autoindex dir or .profdata file)")

set(MMM_PGO_DEFAULT_UPLOAD_URL
    "https://mmm.xiang233.top/api/performance/upload")
# 上传地址写入插桩程序生成头，空值按项目默认服务处理。
set(MMM_PGO_UPLOAD_URL
    "${MMM_PGO_DEFAULT_UPLOAD_URL}"
    CACHE STRING "URL for uploading collected .profraw profiles")
if("${MMM_PGO_UPLOAD_URL}" STREQUAL "")
  # 空缓存值不表示禁用上传。 空值与未传入变量保持相同行为，统一回退到项目默认上传地址。
  set(MMM_PGO_UPLOAD_URL
      "${MMM_PGO_DEFAULT_UPLOAD_URL}"
      CACHE STRING "URL for uploading collected .profraw profiles" FORCE)
endif()

# =============================================================================
# 解析 profile 数据源 → 统一为 MMM_PGO_DATA (在需要时自动下载/合并)
# =============================================================================
if(MMM_PGO_USE)
  # 彻底的调试信息：列出所有可能影响 PGO 的变量
  message(STATUS "PGO: --- Configuration Debug Start ---")
  # 四个输入值完整回显，便于区分缓存恢复与调用方参数错误。
  message(STATUS "  MMM_PGO_USE         = '${MMM_PGO_USE}'")
  message(STATUS "  MMM_PGO_DATA        = '${MMM_PGO_DATA}'")
  message(STATUS "  MMM_PGO_PROFILE_DIR = '${MMM_PGO_PROFILE_DIR}'")
  message(STATUS "  MMM_PGO_SOURCE_URL  = '${MMM_PGO_SOURCE_URL}'")

  # 尝试从缓存中强制同步，防止父级局部变量遮蔽命令行缓存值。
  if("${MMM_PGO_PROFILE_DIR}" STREQUAL "" AND NOT "$CACHE{MMM_PGO_PROFILE_DIR}"
                                              STREQUAL "")
    set(MMM_PGO_PROFILE_DIR "$CACHE{MMM_PGO_PROFILE_DIR}")
    message(
      STATUS
        "  (Recovered MMM_PGO_PROFILE_DIR from cache: '${MMM_PGO_PROFILE_DIR}')"
    )
  endif()

  # 优先查找与当前 clang 同目录的 llvm-profdata，避免跨版本合并不兼容。
  get_filename_component(_clang_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(
    LLVM_PROFDATA
    NAMES llvm-profdata
    PATHS "${_clang_dir}"
    NO_DEFAULT_PATH)
  if(NOT LLVM_PROFDATA)
    # 编译器目录未提供工具时才允许从 PATH 搜索系统安装版本。
    find_program(LLVM_PROFDATA llvm-profdata)
  endif()

  if(NOT "${MMM_PGO_DATA}" STREQUAL "")
    # A) 直接指定 .profdata 文件
    if(MMM_PGO_DATA MATCHES "^https?://")
      # URL 数据先下载到构建目录，编译器始终消费稳定的本地文件。
      set(_downloaded "${CMAKE_BINARY_DIR}/pgo_downloaded.profdata")
      message(STATUS "PGO: Downloading profile from ${MMM_PGO_DATA}")
      file(
        DOWNLOAD "${MMM_PGO_DATA}" "${_downloaded}"
        STATUS _dl_status
        TIMEOUT 60)
      list(GET _dl_status 0 _dl_code)
      # CMake DOWNLOAD 用状态列表返回协议码与错误文本，需要分别解包。
      if(NOT _dl_code EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        message(FATAL_ERROR "PGO: Download failed (${_dl_code}): ${_dl_msg}")
      endif()
      set(MMM_PGO_DATA "${_downloaded}")
    endif()

    # 无论来源是本地还是下载结果，进入编译选项前都验证文件存在。
    if(NOT EXISTS "${MMM_PGO_DATA}")
      message(FATAL_ERROR "PGO: Profile data not found: ${MMM_PGO_DATA}")
    endif()

  elseif(NOT "${MMM_PGO_SOURCE_URL}" STREQUAL ""
         OR NOT "${MMM_PGO_PROFILE_DIR}" STREQUAL "")
    # B/C) 自动合并: 调 pgo_merge.py 合并结果固定写入构建目录，避免修改用户提供的原始 profile。
    set(_merged "${CMAKE_BINARY_DIR}/pgo_merged.profdata")
    set(_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/pgo_merge.py")

    if(NOT EXISTS "${_script}")
      # 脚本缺失代表源码包不完整，不能静默退回非 PGO 构建。
      message(FATAL_ERROR "PGO: Merge script not found: ${_script}")
    endif()

    # Python 仅在需要自动下载或合并时成为配置依赖。
    find_package(
      Python3
      COMPONENTS Interpreter
      REQUIRED)
    set(_py_cmd ${Python3_EXECUTABLE})

    # 远程 URL 优先于本地目录，保证同时误传两个来源时选择确定。

    if(NOT "${MMM_PGO_SOURCE_URL}" STREQUAL "")
      # 远程源由脚本负责发现 profraw、下载并调用 llvm-profdata 合并。
      message(
        STATUS "PGO: Fetching & merging profiles from ${MMM_PGO_SOURCE_URL}")
      execute_process(
        COMMAND
          ${_py_cmd} "${_script}" --source-url "${MMM_PGO_SOURCE_URL}" --output
          "${_merged}" --profdata "${LLVM_PROFDATA}" --min-files 1
        RESULT_VARIABLE _ret
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    elseif(NOT "${MMM_PGO_PROFILE_DIR}" STREQUAL "")
      # 本地目录模式直接扫描用户指定位置，不复制或删除原始采样文件。
      message(STATUS "PGO: Merging profiles from ${MMM_PGO_PROFILE_DIR}")
      execute_process(
        COMMAND
          ${_py_cmd} "${_script}" --input-dir "${MMM_PGO_PROFILE_DIR}" --output
          "${_merged}" --profdata "${LLVM_PROFDATA}" --min-files 1
        RESULT_VARIABLE _ret
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    endif()

    message(STATUS "${_out}")
    # 只有脚本成功且结果确实存在时，才把路径提升为统一数据源。
    if(_ret EQUAL 0 AND EXISTS "${_merged}")
      set(MMM_PGO_DATA "${_merged}")
      message(STATUS "PGO: Auto-merge OK → ${_merged}")
    else()
      if(NOT _ret EQUAL 0)
        # 保留脚本 stderr 便于定位下载、格式或 llvm-profdata 失败。
        message("${_err}")
      endif()
      message(FATAL_ERROR "PGO: Auto-merge failed or no profiles found")
    endif()

  else()
    # 优化模式没有数据源时必须失败，避免发布一个名义 PGO、实际未优化的包。
    message(
      FATAL_ERROR
        "PGO: Profile data source missing!\n"
        "  Please set one of:\n"
        "  -DMMM_PGO_DATA=path/to/merged.profdata\n"
        "  -DMMM_PGO_PROFILE_DIR=path/to/raw_profiles_dir\n"
        "  -DMMM_PGO_SOURCE_URL=https://...\n")
  endif()
endif()

# =========================================================================
# Optimized 模式 — 使用收集的 profile 数据优化编译
# =========================================================================
if(MMM_PGO_USE)
  if(NOT "${MMM_PGO_DATA}" STREQUAL "")
    # 同一 profdata 同时应用于 C 与 C++，保持跨语言调用图权重一致。
    message(STATUS "PGO: Using profile data = ${MMM_PGO_DATA}")
    add_compile_options(
      "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-fprofile-instr-use=${MMM_PGO_DATA}>"
      "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fprofile-instr-use=${MMM_PGO_DATA}>"
    )
  endif()
endif()

# =========================================================================
# Instrumentation 模式 — 编译插桩版本，采集 profile
# =========================================================================
if(MMM_PGO_INSTRUMENT)
  # 插桩标志必须同时进入编译和链接阶段。
  message(STATUS "PGO: Instrumentation build enabled")
  # 默认丢弃普通进程产生的 profile，应用内显式写出流程负责选择最终路径。
  if(WIN32)
    # Windows 空设备名不能写成 POSIX 的 /dev/null。
    set(DEFAULT_PGO_PATH "NUL")
  else()
    set(DEFAULT_PGO_PATH "/dev/null")
  endif()
  add_compile_options(
    "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-fprofile-instr-generate=${DEFAULT_PGO_PATH}>"
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fprofile-instr-generate=${DEFAULT_PGO_PATH}>"
  )
  if(MMM_PGO_IS_MSVC_LIKE_CLANG_CROSS)
    # 禁止对象写入猜测的默认库名，直接链接已验证的 Windows 运行库路径。 默认库名可能随 LLVM 安装布局变化，不能依靠固定
    # clang_rt.profile.lib 别名。 关闭的仅是 compiler-rt 隐式指令，MSVC CRT 的 libcmt
    # 等默认库指令仍由编译器保留。
    add_compile_options(
      "$<$<COMPILE_LANG_AND_ID:C,Clang>:-fno-rtlib-defaultlib>"
      "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-fno-rtlib-defaultlib>")
    link_libraries("${MMM_PGO_PROFILE_RUNTIME}")
    # 目录级链接依赖只进入此后创建的业务目标，与插桩编译选项使用相同作用范围。 lld-link 直接接收库文件路径，不接收只由 Clang
    # 驱动解释的插桩参数。
  else()
    # 其他工具链由 Clang 驱动在链接阶段解析 profile 运行库。
    add_link_options(
      "$<$<LINK_LANG_AND_ID:C,Clang,AppleClang>:-fprofile-instr-generate=${DEFAULT_PGO_PATH}>"
      "$<$<LINK_LANG_AND_ID:CXX,Clang,AppleClang>:-fprofile-instr-generate=${DEFAULT_PGO_PATH}>"
    )
  endif()
  add_compile_definitions(MMM_PGO_INSTRUMENT=1)

  # 上传地址只在插桩构建生成，普通与优化构建不会暴露该编译常量。
  message(STATUS "PGO: Upload URL = ${MMM_PGO_UPLOAD_URL}")
  # PGO_UPLOAD_URL 使用模板期望的短变量名，与缓存接口保持隔离。
  set(PGO_UPLOAD_URL "${MMM_PGO_UPLOAD_URL}")
  # configure_file 只替换上传常量，生成头位于统一 generated include 目录。
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pgo_upload_url.h.in"
                 "${CMAKE_BINARY_DIR}/generated/pgo_upload_url.h" @ONLY)
endif()
