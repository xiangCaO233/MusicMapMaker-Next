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

set(APPVER "0.4.6")

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

if(MSVC)
  # 设置msvc需要的 VCPKG_ROOT
  if(NOT DEFINED ENV{VCPKG_ROOT} OR "$ENV{VCPKG_ROOT}" STREQUAL "")
    message(
      FATAL_ERROR
        "VCPKG_ROOT environment variable is required when building with MSVC.")
  endif()
  file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT)
  # 强制 MSVC 使用静态运行时 (/MT)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain")

include("project-configure")
include("pgo")

enable_testing()

set(MMM_TEST_RESOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    CACHE PATH "Shared test resource directory.")

function(mmm_set_test_target MODULE_NAME TARGET_NAME)
  set(TEST_RUNTIME_DIR "${CMAKE_BINARY_DIR}/tests/${MODULE_NAME}")
  set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                  "${TEST_RUNTIME_DIR}")

  foreach(CONFIG_NAME Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${CONFIG_NAME}" CONFIG_NAME_UPPER)
    set_target_properties(
      ${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CONFIG_NAME_UPPER}
                                "${TEST_RUNTIME_DIR}")
  endforeach()
endfunction()

function(mmm_add_test_executable MODULE_NAME TARGET_NAME)
  add_executable(${TARGET_NAME} ${ARGN})
  mmm_set_test_target(${MODULE_NAME} ${TARGET_NAME})
endfunction()

# 从源码编译依赖 默认开启
option(SOURCES_BUILD "Build Deps from sources." ON)

# Windows 原生文件对话框默认会用最近访问目录覆盖 defaultPath； 另存为需要强制打开当前项目目录。
set(NFD_OVERRIDE_RECENT_WITH_DEFAULT
    ON
    CACHE BOOL "Use NFD defaultPath instead of Windows recent folders." FORCE)

find_package(Vulkan REQUIRED)

add_subdirectory("3rdpty")
add_subdirectory("Modules")
