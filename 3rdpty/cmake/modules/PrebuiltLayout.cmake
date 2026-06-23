include_guard(GLOBAL)

# 本文件只负责解析预编译包目录和库文件路径，不创建任何具体目标。 每个库的目标 必须在自己的 Findxxx.cmake 中显式声明。

# 初始化主项目预编译库目录选择变量。头文件共享布局为： <root>/<包>/include 静态偏好布局为：
# <root>/<platform>/<包>/libs/<arch>/<toolchain>/<compiler-tag>/<config> 动态偏好布局为：
# <root>/<platform>/<包>/libs/<arch>/<toolchain>/<compiler-tag>/shared/<config>
# <root>/<platform>/<包>/bin/<arch>/<toolchain>/<compiler-tag>/shared/<config>
function(prebuilt_init default_root)
  if(NOT DEFINED PROJECT_LINKAGE OR PROJECT_LINKAGE STREQUAL "")
    set(PROJECT_LINKAGE
        "static"
        CACHE STRING "Third-party dependency linkage preference.")
  endif()
  string(TOLOWER "${PROJECT_LINKAGE}" PROJECT_LINKAGE)
  if(NOT PROJECT_LINKAGE MATCHES "^(static|shared)$")
    message(FATAL_ERROR "PROJECT_LINKAGE 只能是 static 或 shared。")
  endif()
  set(PROJECT_LINKAGE
      "${PROJECT_LINKAGE}"
      CACHE STRING "Third-party dependency linkage preference." FORCE)

  if(NOT DEFINED PROJECT_PREBUILT_ROOT OR PROJECT_PREBUILT_ROOT STREQUAL "")
    set(PROJECT_PREBUILT_ROOT
        "${default_root}"
        CACHE PATH "Root directory containing project prebuilt packages.")
  endif()

  # 平台目录名固定为仓库约定，避免直接暴露 CMake 系统名。
  if(NOT DEFINED PROJECT_PREBUILT_PLATFORM OR PROJECT_PREBUILT_PLATFORM
                                              STREQUAL "")
    if(WIN32)
      set(_platform "windows")
    elseif(APPLE)
      set(_platform "macos")
    else()
      set(_platform "linux")
    endif()
    set(PROJECT_PREBUILT_PLATFORM
        "${_platform}"
        CACHE STRING "Prebuilt platform directory name.")
  endif()

  # 目前预编译目录按指针宽度区分架构。
  if(NOT DEFINED PROJECT_PREBUILT_ARCH OR PROJECT_PREBUILT_ARCH STREQUAL "")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_arch "x86_64")
    else()
      set(_arch "x86")
    endif()
    set(PROJECT_PREBUILT_ARCH
        "${_arch}"
        CACHE STRING "Prebuilt architecture directory name.")
  endif()

  # 工具链目录用于区分 MSVC、MinGW 等二进制格式。
  if(NOT DEFINED PROJECT_PREBUILT_TOOLCHAIN OR PROJECT_PREBUILT_TOOLCHAIN
                                               STREQUAL "")
    if(MSVC)
      set(_toolchain "msvc")
    elseif(MINGW)
      set(_toolchain "mingw")
    else()
      string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _toolchain)
    endif()
    set(PROJECT_PREBUILT_TOOLCHAIN
        "${_toolchain}"
        CACHE STRING "Prebuilt toolchain directory name.")
  endif()

  # 编译器标签用于区分同一工具链下的 ABI 或运行库版本。
  if(NOT DEFINED PROJECT_PREBUILT_COMPILER_TAG OR PROJECT_PREBUILT_COMPILER_TAG
                                                  STREQUAL "")
    if(MSVC)
      set(_compiler_tag "2026")
    elseif(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set(_compiler_tag "clang64")
    elseif(MINGW)
      set(_compiler_tag "ucrt64")
    else()
      string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _compiler_tag)
    endif()
    set(PROJECT_PREBUILT_COMPILER_TAG
        "${_compiler_tag}"
        CACHE STRING "Prebuilt compiler/runtime directory name.")
  endif()

  set(_platform_root "${PROJECT_PREBUILT_ROOT}/${PROJECT_PREBUILT_PLATFORM}")
  if(NOT IS_DIRECTORY "${_platform_root}")
    message(
      FATAL_ERROR
        "SOURCES_BUILD=OFF 需要预编译库目录：${_platform_root}。请提供预编译库，或显式开启 SOURCES_BUILD。"
    )
  endif()
endfunction()

# 获取指定包的根目录。
function(prebuilt_package_root out_var package)
  set(_root "${PROJECT_PREBUILT_ROOT}/${PROJECT_PREBUILT_PLATFORM}/${package}")
  if(NOT IS_DIRECTORY "${_root}")
    message(FATAL_ERROR "缺少预编译包目录：${_root}")
  endif()
  set(${out_var}
      "${_root}"
      PARENT_SCOPE)
endfunction()

# 获取指定包的 include 目录。头文件默认跨平台共用，只有二进制按平台区分。
function(prebuilt_include_dir out_var package)
  set(_include_dir "${PROJECT_PREBUILT_ROOT}/${package}/include")
  if(NOT IS_DIRECTORY "${_include_dir}")
    message(FATAL_ERROR "缺少预编译头文件目录：${_include_dir}")
  endif()
  set(${out_var}
      "${_include_dir}"
      PARENT_SCOPE)
endfunction()

# 获取当前生成器需要的构建配置列表。
function(prebuilt_target_configs out_var)
  if(CMAKE_CONFIGURATION_TYPES)
    set(_configs ${CMAKE_CONFIGURATION_TYPES})
  elseif(CMAKE_BUILD_TYPE)
    set(_configs "${CMAKE_BUILD_TYPE}")
  else()
    set(_configs "Release")
  endif()
  set(${out_var}
      ${_configs}
      PARENT_SCOPE)
endfunction()

# 将 CMake 配置名映射到预编译目录候选名。
function(prebuilt_config_dir_candidates out_var config)
  string(TOLOWER "${config}" _config_lower)
  if(_config_lower STREQUAL "debug")
    set(_candidates "Debug" "debug")
  elseif(config STREQUAL "" OR _config_lower STREQUAL "noconfig")
    set(_candidates "RelWithDebInfo" "relwithdebinfo" "Release" "release")
  elseif(
    _config_lower STREQUAL "release"
    OR _config_lower STREQUAL "relwithdebinfo"
    OR _config_lower STREQUAL "minsizerel")
    set(_candidates "RelWithDebInfo" "relwithdebinfo" "Release" "release")
  else()
    set(_candidates "${config}" "${_config_lower}" "RelWithDebInfo"
                    "relwithdebinfo" "Release" "release")
  endif()
  list(REMOVE_DUPLICATES _candidates)
  set(${out_var}
      ${_candidates}
      PARENT_SCOPE)
endfunction()

# 获取指定包在指定配置下的库目录。
function(prebuilt_library_dir out_var package config)
  prebuilt_package_root(_package_root "${package}")
  set(_library_base_dir
      "${_package_root}/libs/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}/${PROJECT_PREBUILT_COMPILER_TAG}"
  )
  if(NOT IS_DIRECTORY "${_library_base_dir}" AND PROJECT_PREBUILT_COMPILER_TAG
                                                 STREQUAL "")
    set(_library_base_dir
        "${_package_root}/libs/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}"
    )
  endif()
  if(PROJECT_LINKAGE STREQUAL "shared")
    set(_library_base_dir "${_library_base_dir}/shared")
  endif()

  prebuilt_config_dir_candidates(_config_candidates "${config}")
  foreach(_config_dir_name IN LISTS _config_candidates)
    set(_library_dir "${_library_base_dir}/${_config_dir_name}")
    if(IS_DIRECTORY "${_library_dir}")
      set(${out_var}
          "${_library_dir}"
          PARENT_SCOPE)
      return()
    endif()
  endforeach()

  message(
    FATAL_ERROR
      "缺少 ${package} 的 ${config} 预编译库目录。期望布局：${_library_base_dir}/<config>")
endfunction()

# 获取指定包在指定配置下的运行时目录。仅动态链接偏好需要此目录。
function(prebuilt_runtime_dir out_var package config)
  prebuilt_package_root(_package_root "${package}")
  set(_runtime_base_dir
      "${_package_root}/bin/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}/${PROJECT_PREBUILT_COMPILER_TAG}/shared"
  )
  if(NOT IS_DIRECTORY "${_runtime_base_dir}" AND PROJECT_PREBUILT_COMPILER_TAG
                                                 STREQUAL "")
    set(_runtime_base_dir
        "${_package_root}/bin/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}/shared"
    )
  endif()

  prebuilt_config_dir_candidates(_config_candidates "${config}")
  foreach(_config_dir_name IN LISTS _config_candidates)
    set(_runtime_dir "${_runtime_base_dir}/${_config_dir_name}")
    if(IS_DIRECTORY "${_runtime_dir}")
      set(${out_var}
          "${_runtime_dir}"
          PARENT_SCOPE)
      return()
    endif()
  endforeach()

  message(
    FATAL_ERROR
      "缺少 ${package} 的 ${config} 动态运行时目录。期望布局：${_runtime_base_dir}/<config>")
endfunction()

# 获取指定包在指定配置下的 PDB 符号目录。符号文件不参与链接，缺失时返回空值。
function(prebuilt_symbol_dir out_var package config)
  prebuilt_package_root(_package_root "${package}")
  set(_symbol_base_dir
      "${_package_root}/symbols/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}/${PROJECT_PREBUILT_COMPILER_TAG}"
  )
  if(NOT IS_DIRECTORY "${_symbol_base_dir}" AND PROJECT_PREBUILT_COMPILER_TAG
                                                STREQUAL "")
    set(_symbol_base_dir
        "${_package_root}/symbols/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}"
    )
  endif()
  if(PROJECT_LINKAGE STREQUAL "shared")
    set(_symbol_base_dir "${_symbol_base_dir}/shared")
  endif()

  prebuilt_config_dir_candidates(_config_candidates "${config}")
  foreach(_config_dir_name IN LISTS _config_candidates)
    set(_symbol_dir "${_symbol_base_dir}/${_config_dir_name}")
    if(IS_DIRECTORY "${_symbol_dir}")
      set(${out_var}
          "${_symbol_dir}"
          PARENT_SCOPE)
      return()
    endif()
  endforeach()

  set(${out_var}
      ""
      PARENT_SCOPE)
endfunction()

# 查找指定包在当前配置下已经提供的 PDB 符号文件。
function(prebuilt_find_symbol_files out_var package config)
  prebuilt_symbol_dir(_symbol_dir "${package}" "${config}")
  if(_symbol_dir STREQUAL "")
    set(${out_var}
        ""
        PARENT_SCOPE)
    return()
  endif()

  file(GLOB _symbol_files CONFIGURE_DEPENDS "${_symbol_dir}/*.pdb")
  set(${out_var}
      ${_symbol_files}
      PARENT_SCOPE)
endfunction()

# 在指定包和配置目录中查找库文件。
function(prebuilt_find_library out_var package config)
  prebuilt_library_dir(_library_dir "${package}" "${config}")
  find_library(
    _prebuilt_library
    NAMES ${ARGN}
    PATHS "${_library_dir}"
    NO_DEFAULT_PATH NO_CACHE)
  if(NOT _prebuilt_library)
    message(
      FATAL_ERROR
        "缺少 ${package} 的 ${config} 预编译库。搜索目录：${_library_dir}；候选名：${ARGN}")
  endif()
  set(${out_var}
      "${_prebuilt_library}"
      PARENT_SCOPE)

  if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
    prebuilt_runtime_dir(_runtime_dir "${package}" "${config}")
    set(_runtime_names "")
    foreach(_library_name IN LISTS ARGN)
      list(APPEND _runtime_names
           "${_library_name}${CMAKE_SHARED_LIBRARY_SUFFIX}")
    endforeach()
    find_file(
      _prebuilt_runtime
      NAMES ${_runtime_names}
      PATHS "${_runtime_dir}"
      NO_DEFAULT_PATH NO_CACHE)
    if(NOT _prebuilt_runtime)
      message(
        FATAL_ERROR
          "缺少 ${package} 的 ${config} 动态运行时文件。搜索目录：${_runtime_dir}；候选名：${_runtime_names}"
      )
    endif()
    set(${out_var}_RUNTIME
        "${_prebuilt_runtime}"
        PARENT_SCOPE)
  endif()

  if(MSVC)
    string(TOLOWER "${config}" _prebuilt_config_lower)
    if(_prebuilt_config_lower STREQUAL "debug" OR _prebuilt_config_lower
                                                  STREQUAL "relwithdebinfo")
      prebuilt_find_symbol_files(_prebuilt_symbol_files "${package}"
                                 "${config}")
      if(_prebuilt_symbol_files)
        set(${out_var}_PDBS
            ${_prebuilt_symbol_files}
            PARENT_SCOPE)
      endif()
    endif()
  endif()
endfunction()
