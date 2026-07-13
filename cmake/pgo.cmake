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
# ==============================================================================

set(MMM_PGO_IS_LLVM_COMPILER OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
  set(MMM_PGO_IS_LLVM_COMPILER ON)
endif()

set(MMM_PGO_IS_MSVC_LIKE_CLANG_CROSS OFF)
if(MMM_PGO_IS_LLVM_COMPILER
   AND CMAKE_CROSSCOMPILING
   AND ("${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "MSVC"
        OR "${CMAKE_CXX_SIMULATE_ID}" STREQUAL "MSVC"))
  set(MMM_PGO_IS_MSVC_LIKE_CLANG_CROSS ON)
endif()

option(MMM_PGO_USE "Build using PGO profile data" OFF)

if(MMM_PGO_USE)
  if(NOT MMM_PGO_IS_LLVM_COMPILER)
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
  # PGO 插桩默认只在 LLVM GNU-like 工具链启用；clang-cl 交叉 MSVC 链接阶段依赖的 compiler-rt profile
  # 库不稳定，必须显式禁用。
  set(MMM_PGO_INSTRUMENT_DEFAULT ${MMM_PGO_IS_LLVM_COMPILER})
  if(MMM_PGO_IS_MSVC_LIKE_CLANG_CROSS)
    set(MMM_PGO_INSTRUMENT_DEFAULT OFF)
  endif()
  option(MMM_PGO_INSTRUMENT
         "Build with PGO instrumentation for profile collection"
         ${MMM_PGO_INSTRUMENT_DEFAULT})
endif()

if(MMM_PGO_INSTRUMENT AND NOT MMM_PGO_IS_LLVM_COMPILER)
  message(
    WARNING
      "PGO: disabling instrumentation because ${CMAKE_CXX_COMPILER_ID} does not "
      "support this project's LLVM profile workflow.")
  set(MMM_PGO_INSTRUMENT
      OFF
      CACHE BOOL "Build with PGO instrumentation for profile collection" FORCE)
endif()

if(MMM_PGO_INSTRUMENT AND MMM_PGO_IS_MSVC_LIKE_CLANG_CROSS)
  message(
    WARNING
      "PGO: disabling instrumentation for cross clang-cl/MSVC-like builds "
      "because lld-link may not find clang_rt.profile.lib.")
  set(MMM_PGO_INSTRUMENT
      OFF
      CACHE BOOL "Build with PGO instrumentation for profile collection" FORCE)
endif()

# --- 数据源 (三选一) ---
set(MMM_PGO_DATA
    ""
    CACHE FILEPATH "Path to pre-merged .profdata file")
set(MMM_PGO_PROFILE_DIR
    ""
    CACHE STRING "Directory containing .profraw files to auto-merge")
set(MMM_PGO_SOURCE_URL
    ""
    CACHE STRING
          "URL to download profiles from (autoindex dir or .profdata file)")

set(MMM_PGO_DEFAULT_UPLOAD_URL
    "https://mmm.xiang233.top/api/performance/upload")
set(MMM_PGO_UPLOAD_URL
    "${MMM_PGO_DEFAULT_UPLOAD_URL}"
    CACHE STRING "URL for uploading collected .profraw profiles")
if("${MMM_PGO_UPLOAD_URL}" STREQUAL "")
  # 空值与未传入变量保持相同行为，统一回退到项目默认上传地址。
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
  message(STATUS "  MMM_PGO_USE         = '${MMM_PGO_USE}'")
  message(STATUS "  MMM_PGO_DATA        = '${MMM_PGO_DATA}'")
  message(STATUS "  MMM_PGO_PROFILE_DIR = '${MMM_PGO_PROFILE_DIR}'")
  message(STATUS "  MMM_PGO_SOURCE_URL  = '${MMM_PGO_SOURCE_URL}'")

  # 尝试从缓存中强制同步 (防止某些环境下本地变量覆盖缓存)
  if("${MMM_PGO_PROFILE_DIR}" STREQUAL "" AND NOT "$CACHE{MMM_PGO_PROFILE_DIR}"
                                              STREQUAL "")
    set(MMM_PGO_PROFILE_DIR "$CACHE{MMM_PGO_PROFILE_DIR}")
    message(
      STATUS
        "  (Recovered MMM_PGO_PROFILE_DIR from cache: '${MMM_PGO_PROFILE_DIR}')"
    )
  endif()

  # 查找 llvm-profdata (与 clang 同目录)
  get_filename_component(_clang_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(
    LLVM_PROFDATA
    NAMES llvm-profdata
    PATHS "${_clang_dir}"
    NO_DEFAULT_PATH)
  if(NOT LLVM_PROFDATA)
    find_program(LLVM_PROFDATA llvm-profdata)
  endif()

  if(NOT "${MMM_PGO_DATA}" STREQUAL "")
    # A) 直接指定 .profdata 文件
    if(MMM_PGO_DATA MATCHES "^https?://")
      set(_downloaded "${CMAKE_BINARY_DIR}/pgo_downloaded.profdata")
      message(STATUS "PGO: Downloading profile from ${MMM_PGO_DATA}")
      file(
        DOWNLOAD "${MMM_PGO_DATA}" "${_downloaded}"
        STATUS _dl_status
        TIMEOUT 60)
      list(GET _dl_status 0 _dl_code)
      if(NOT _dl_code EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        message(FATAL_ERROR "PGO: Download failed (${_dl_code}): ${_dl_msg}")
      endif()
      set(MMM_PGO_DATA "${_downloaded}")
    endif()

    if(NOT EXISTS "${MMM_PGO_DATA}")
      message(FATAL_ERROR "PGO: Profile data not found: ${MMM_PGO_DATA}")
    endif()

  elseif(NOT "${MMM_PGO_SOURCE_URL}" STREQUAL ""
         OR NOT "${MMM_PGO_PROFILE_DIR}" STREQUAL "")
    # B/C) 自动合并: 调 pgo_merge.py
    set(_merged "${CMAKE_BINARY_DIR}/pgo_merged.profdata")
    set(_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/pgo_merge.py")

    if(NOT EXISTS "${_script}")
      message(FATAL_ERROR "PGO: Merge script not found: ${_script}")
    endif()

    # 查找 python
    find_package(
      Python3
      COMPONENTS Interpreter
      REQUIRED)
    set(_py_cmd ${Python3_EXECUTABLE})

    if(NOT "${MMM_PGO_SOURCE_URL}" STREQUAL "")
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
    if(_ret EQUAL 0 AND EXISTS "${_merged}")
      set(MMM_PGO_DATA "${_merged}")
      message(STATUS "PGO: Auto-merge OK → ${_merged}")
    else()
      if(NOT _ret EQUAL 0)
        message("${_err}")
      endif()
      message(FATAL_ERROR "PGO: Auto-merge failed or no profiles found")
    endif()

  else()
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
  message(STATUS "PGO: Instrumentation build enabled")
  if(WIN32)
    set(DEFAULT_PGO_PATH "NUL")
  else()
    set(DEFAULT_PGO_PATH "/dev/null")
  endif()
  add_compile_options(
    "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-fprofile-instr-generate=${DEFAULT_PGO_PATH}>"
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fprofile-instr-generate=${DEFAULT_PGO_PATH}>"
  )
  add_link_options(
    "$<$<LINK_LANG_AND_ID:C,Clang,AppleClang>:-fprofile-instr-generate=${DEFAULT_PGO_PATH}>"
    "$<$<LINK_LANG_AND_ID:CXX,Clang,AppleClang>:-fprofile-instr-generate=${DEFAULT_PGO_PATH}>"
  )
  add_compile_definitions(MMM_PGO_INSTRUMENT=1)

  message(STATUS "PGO: Upload URL = ${MMM_PGO_UPLOAD_URL}")
  set(PGO_UPLOAD_URL "${MMM_PGO_UPLOAD_URL}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pgo_upload_url.h.in"
                 "${CMAKE_BINARY_DIR}/generated/pgo_upload_url.h" @ONLY)
endif()
