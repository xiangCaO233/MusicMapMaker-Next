cmake_minimum_required(VERSION 3.31)

if(CMAKE_BUILD_TYPE STREQUAL "RelWithDbgInfo")
  message(
    WARNING
      "CMAKE_BUILD_TYPE=RelWithDbgInfo is not a standard CMake configuration; "
      "using RelWithDebInfo instead.")
  set(CMAKE_BUILD_TYPE
      "RelWithDebInfo"
      CACHE STRING "Build type" FORCE)
endif()

if(DEFINED CMAKE_CONFIGURATION_TYPES)
  list(FIND CMAKE_CONFIGURATION_TYPES "RelWithDbgInfo"
       _MMM_RELWITHDBGINFO_INDEX)
  if(NOT _MMM_RELWITHDBGINFO_INDEX EQUAL -1)
    list(REMOVE_ITEM CMAKE_CONFIGURATION_TYPES "RelWithDbgInfo")
    list(APPEND CMAKE_CONFIGURATION_TYPES "RelWithDebInfo")
    list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES
        "${CMAKE_CONFIGURATION_TYPES}"
        CACHE STRING "Available build configurations" FORCE)
    message(
      WARNING
        "CMAKE_CONFIGURATION_TYPES contained non-standard RelWithDbgInfo; "
        "using RelWithDebInfo instead.")
  endif()
  unset(_MMM_RELWITHDBGINFO_INDEX)
endif()

set(APPVER "0.5.9")

if(APPLE)
  project(
    MusicMapMaker-Next
    VERSION ${APPVER}
    LANGUAGES C CXX OBJC OBJCXX)
else()
  project(
    MusicMapMaker-Next
    VERSION ${APPVER}
    LANGUAGES C CXX)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      "${CMAKE_BUILD_TYPE}"
      CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release
                                               RelWithDebInfo MinSizeRel)
endif()

set(PROJECT_LINKAGE
    "static"
    CACHE STRING "Third-party dependency linkage preference: static or shared.")
set_property(CACHE PROJECT_LINKAGE PROPERTY STRINGS static shared)
string(TOLOWER "${PROJECT_LINKAGE}" PROJECT_LINKAGE)
if(NOT PROJECT_LINKAGE MATCHES "^(static|shared)$")
  message(FATAL_ERROR "PROJECT_LINKAGE 只能是 static 或 shared。")
endif()
set(PROJECT_LINKAGE
    "${PROJECT_LINKAGE}"
    CACHE STRING "Third-party dependency linkage preference: static or shared."
          FORCE)

if(PROJECT_LINKAGE STREQUAL "shared")
  set(BUILD_SHARED_LIBS
      ON
      CACHE BOOL "Prefer shared libraries for third-party dependencies." FORCE)
else()
  set(BUILD_SHARED_LIBS
      OFF
      CACHE BOOL "Prefer shared libraries for third-party dependencies." FORCE)
endif()

# 动态链接偏好不应携带 GNU/Clang 常见静态运行库参数。
if(PROJECT_LINKAGE STREQUAL "shared" AND NOT MSVC)
  foreach(_project_link_flag_var IN
          ITEMS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
                CMAKE_MODULE_LINKER_FLAGS)
    if(DEFINED ${_project_link_flag_var})
      set(_project_link_flags "${${_project_link_flag_var}}")
      foreach(_project_static_runtime_flag IN ITEMS "-static-libstdc++"
                                                    "-static-libgcc" "-static")
        string(REPLACE "${_project_static_runtime_flag}" "" _project_link_flags
                       "${_project_link_flags}")
      endforeach()
      set(${_project_link_flag_var}
          "${_project_link_flags}"
          CACHE STRING "Linker flags without static runtime options." FORCE)
    endif()
  endforeach()
endif()

if(MSVC)
  if(PROJECT_LINKAGE STREQUAL "shared")
    # 动态链接偏好使用 /MD 或 /MDd，避免把 MSVCRT 静态并入本体。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    # 静态链接偏好使用 /MT 或 /MTd，保持依赖闭包尽量自包含。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain")

include("project-configure")

# 项目测试默认不参与常规构建，避免大量测试可执行文件显著增加链接时间；需要测试时显式开启。
option(BUILD_TESTING "Build and register MusicMapMaker tests." OFF)

# 本机开发构建默认把仓库翻译和内置皮肤同步到应用配置目录；CI 与打包脚本必须显式关闭。
option(MMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN
       "Sync translations and built-in skins into the local user config." ON)

if(BUILD_TESTING)
  enable_testing()
else()
  # 清理旧配置遗留的 CTest 入口，避免关闭开关后继续读取已禁用的测试。
  file(REMOVE "${CMAKE_BINARY_DIR}/CTestTestfile.cmake")
endif()

set(MMM_TEST_RESOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    CACHE PATH "Shared test resource directory.")

if(APPLE AND CMAKE_GENERATOR MATCHES "Ninja")
  # macOS 测试链接会消费业务库中的 ThinLTO bitcode，限制并发以预留 Runner 网络保活所需资源。
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS macos_test_link_pool=1)
endif()

function(mmm_set_test_target MODULE_NAME TARGET_NAME)
  set(TEST_RUNTIME_DIR "${CMAKE_BINARY_DIR}/tests/${MODULE_NAME}")
  set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                  "${TEST_RUNTIME_DIR}")

  # 测试会调用正式配置保存入口；每个目标必须隔离配置与 ImGui 状态，禁止落入个人目录。
  set(TEST_CONFIG_ROOT
      "${CMAKE_BINARY_DIR}/test_output/config/${MODULE_NAME}/${TARGET_NAME}")
  target_sources(${TARGET_NAME}
                 PRIVATE "${CMAKE_SOURCE_DIR}/tests/TestConfigIsolation.cpp")
  target_compile_definitions(${TARGET_NAME}
                             PRIVATE MMM_TEST_CONFIG_ROOT="${TEST_CONFIG_ROOT}")
  # CTest 启动器覆盖继承环境中的配置根；直接执行测试时由公共启动代码提供同一路径。
  set_property(
    TARGET ${TARGET_NAME}
    PROPERTY TEST_LAUNCHER
             "${CMAKE_COMMAND};-E;env;MMM_CONFIG_ROOT=${TEST_CONFIG_ROOT}")

  if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # macOS 测试目标仍需从业务库的 ThinLTO bitcode 生成本机代码；先禁用跨模块优化。
    target_link_options(
      ${TARGET_NAME} PRIVATE
      "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fno-lto>")
    if(CMAKE_GENERATOR MATCHES "Ninja")
      # 串行化高负载测试链接，避免多个 ld 同时耗尽 CPU 与内存，导致 Runner 与 GitHub 失联。
      set_property(TARGET ${TARGET_NAME} PROPERTY JOB_POOL_LINK
                                                  macos_test_link_pool)
    endif()
  endif()

  foreach(CONFIG_NAME Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${CONFIG_NAME}" CONFIG_NAME_UPPER)
    set_target_properties(
      ${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CONFIG_NAME_UPPER}
                                "${TEST_RUNTIME_DIR}")
  endforeach()

  # shared 预编译依赖需要复制到测试可执行文件旁边，否则 Windows 动态加载无法从独立测试目录解析 DLL。
  if(COMMAND prebuilt_copy_runtime_files)
    prebuilt_copy_runtime_files(${TARGET_NAME})
  endif()
endfunction()

# 统一声明项目测试目标；关闭测试时保留显式目标但排除出默认构建，便于各模块继续集中配置依赖。
function(mmm_add_test_executable MODULE_NAME TARGET_NAME)
  if(BUILD_TESTING)
    add_executable(${TARGET_NAME} ${ARGN})
  else()
    add_executable(${TARGET_NAME} EXCLUDE_FROM_ALL ${ARGN})
  endif()
  mmm_set_test_target(${MODULE_NAME} ${TARGET_NAME})
endfunction()

# 第三方依赖默认使用预编译库；需要重建依赖时显式开启。
option(SOURCES_BUILD "Build Deps from sources." OFF)

# Windows 原生文件对话框默认会用最近访问目录覆盖 defaultPath； 另存为需要强制打开当前项目目录。
set(NFD_OVERRIDE_RECENT_WITH_DEFAULT
    ON
    CACHE BOOL "Use NFD defaultPath instead of Windows recent folders." FORCE)

find_package(Vulkan REQUIRED)

add_subdirectory("3rdpty")

# PROJECT_LINKAGE 只描述第三方依赖链接偏好；业务模块维持静态库组织，避免未指定类型的 add_library 被 shared 偏好改写。
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL
          "Build project modules as static libraries after third-party setup."
          FORCE)

# PGO 只作用于本项目业务模块，预编译库和用于生成预编译库的第三方源码都不插桩。
include("pgo")

add_subdirectory("Modules")

# macOS 需要把应用束、Vulkan 运行时和更新器组合为可分发的 DMG；Windows/Linux 继续直接发布单可执行文件。
if(APPLE)
  include("macos-package")
endif()
