cmake_minimum_required(VERSION 3.31)

# 根入口负责确定版本、依赖链接策略、测试基础设施和顶层子目录顺序。 平台与编译器优化细节委托给 project-configure，PGO 逻辑委托给
# pgo。 第三方 target 必须早于 Modules 建立，业务模块不得根据依赖来源分支。 所有可供用户覆盖的构建选择都使用
# cache，并限制为明确的合法取值。 本文件不直接列举业务源文件，模块边界由各自 CMakeLists 维护。

# 应用版本集中定义于根工程，供生成头、安装包和发布脚本共同消费。
set(APPVER "0.5.9")

# 平台语言集合必须先于首次编译器探测确定。 Apple 平台的窗口与应用束实现包含 Objective-C 和 Objective-C++ 源码。
if(APPLE)
  project(
    MusicMapMaker-Next
    VERSION ${APPVER}
    LANGUAGES C CXX OBJC OBJCXX)
else()
  # 其他平台仅启用业务源码实际使用的 C 与 C++ 编译器。
  project(
    MusicMapMaker-Next
    VERSION ${APPVER}
    LANGUAGES C CXX)
endif()

# 单配置生成器通过缓存项选择配置；多配置生成器由构建命令决定配置。
if(NOT CMAKE_CONFIGURATION_TYPES)
  # 缓存字符串保留命令行传入的单配置类型。
  set(CMAKE_BUILD_TYPE
      "${CMAKE_BUILD_TYPE}"
      CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release
                                               RelWithDebInfo MinSizeRel)
endif()

# PROJECT_LINKAGE 只表达第三方依赖偏好，不改变业务模块的静态组织方式。
set(PROJECT_LINKAGE
    "static"
    # 默认值面向自包含发布包。
    CACHE STRING "Third-party dependency linkage preference: static or shared.")
set_property(CACHE PROJECT_LINKAGE PROPERTY STRINGS static shared)
# 用户输入统一转为小写后再验证。
string(TOLOWER "${PROJECT_LINKAGE}" PROJECT_LINKAGE)
# 尽早拒绝未知值，避免查找依赖时静态与动态目录混用。
if(NOT PROJECT_LINKAGE MATCHES "^(static|shared)$")
  message(FATAL_ERROR "PROJECT_LINKAGE 只能是 static 或 shared。")
endif()
# 规范化后的值写回缓存，使后续子目录看到一致的小写值。
set(PROJECT_LINKAGE
    "${PROJECT_LINKAGE}"
    CACHE STRING "Third-party dependency linkage preference: static or shared."
          FORCE)

# 依赖源码构建遵循同一链接偏好，预编译查找脚本也读取该状态。
if(PROJECT_LINKAGE STREQUAL "shared")
  # shared 偏好传递给随后配置的第三方子工程。
  set(BUILD_SHARED_LIBS
      ON
      # 该值只在配置第三方依赖期间生效。
      CACHE BOOL "Prefer shared libraries for third-party dependencies." FORCE)
else()
  # 静态是发行构建默认值，确保第三方依赖闭包尽量自包含。
  set(BUILD_SHARED_LIBS
      OFF
      CACHE BOOL "Prefer shared libraries for third-party dependencies." FORCE)
endif()

# 共享依赖不能混入宿主静态运行库。 动态链接偏好不应携带 GNU/Clang 常见静态运行库参数。
if(PROJECT_LINKAGE STREQUAL "shared" AND NOT MSVC)
  # 三类链接器标志都可能由工具链文件预置，必须逐项清除静态运行库参数。
  foreach(_project_link_flag_var IN
          ITEMS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
                CMAKE_MODULE_LINKER_FLAGS)
    if(DEFINED ${_project_link_flag_var})
      # 在局部副本上替换，完成后再强制同步对应缓存变量。
      set(_project_link_flags "${${_project_link_flag_var}}")
      foreach(_project_static_runtime_flag IN ITEMS "-static-libstdc++"
                                                    "-static-libgcc" "-static")
        string(REPLACE "${_project_static_runtime_flag}" "" _project_link_flags
                       "${_project_link_flags}")
      endforeach()
      # 保留变量中与静态运行库无关的其余用户选项。
      set(${_project_link_flag_var}
          "${_project_link_flags}"
          CACHE STRING "Linker flags without static runtime options." FORCE)
    endif()
  endforeach()
endif()

# MSVC 运行库必须与依赖链接偏好一致，避免跨 CRT 边界释放对象。
if(MSVC)
  if(PROJECT_LINKAGE STREQUAL "shared")
    # 动态链接偏好使用 /MD 或 /MDd，避免把 MSVCRT 静态并入本体。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    # 静态链接偏好使用 /MT 或 /MTd，保持依赖闭包尽量自包含。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

# 自维护模块、依赖查找器和交叉工具链分别加入模块搜索路径。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# 自定义 Find 模块覆盖同名官方入口时仍会显式透传。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain")

include("project-configure")

# 测试开关必须在添加业务模块前确定。 项目测试默认不参与常规构建，避免大量测试可执行文件显著增加链接时间；需要测试时显式开启。
option(BUILD_TESTING "Build and register MusicMapMaker tests." OFF)

# 本机开发构建默认把仓库翻译和内置皮肤同步到应用配置目录；CI 与打包脚本必须显式关闭。
option(MMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN
       "Sync translations and built-in skins into the local user config." ON)

if(BUILD_TESTING)
  # 测试注册仅在显式开启时写入当前构建树。
  enable_testing()
else()
  # 清理旧配置遗留的 CTest 入口，避免关闭开关后继续读取已禁用的测试。
  file(REMOVE "${CMAKE_BINARY_DIR}/CTestTestfile.cmake")
endif()

# 公共夹具目录允许外部构建显式覆盖。
set(MMM_TEST_RESOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    # 调用方可用缓存路径指向外部只读夹具集。
    CACHE PATH "Shared test resource directory.")
# 测试资源根只读，测试生成内容必须写入构建树的 test_output。

if(APPLE AND CMAKE_GENERATOR MATCHES "Ninja")
  # macOS 测试链接会消费业务库中的 ThinLTO bitcode，限制并发以预留 Runner 网络保活所需资源。
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS macos_test_link_pool=1)
endif()

# 配置单个测试目标的输出目录、配置隔离和平台链接约束。 MODULE_NAME 用作测试输出分组，TARGET_NAME 必须是已经创建的目标。
function(mmm_set_test_target MODULE_NAME TARGET_NAME)
  # helper 只接受已经存在的目标，不负责声明源文件或注册测试名称。 所有配置共用模块级目录，便于 CTest 和手动诊断定位可执行文件。
  set(TEST_RUNTIME_DIR "${CMAKE_BINARY_DIR}/tests/${MODULE_NAME}")
  set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                  "${TEST_RUNTIME_DIR}")

  # 测试会调用正式配置保存入口；每个目标必须隔离配置与 ImGui 状态，禁止落入个人目录。
  set(TEST_CONFIG_ROOT
      "${CMAKE_BINARY_DIR}/test_output/config/${MODULE_NAME}/${TARGET_NAME}")
  # 每个目标独占目录，避免并行 CTest 共享用户配置或 ImGui 布局。
  target_sources(${TARGET_NAME}
                 PRIVATE "${CMAKE_SOURCE_DIR}/tests/TestConfigIsolation.cpp")
  target_compile_definitions(${TARGET_NAME}
                             PRIVATE MMM_TEST_CONFIG_ROOT="${TEST_CONFIG_ROOT}")
  # 编译常量为直接运行提供默认隔离根，启动器则覆盖继承环境。 CTest 启动器覆盖继承环境中的配置根；直接执行测试时由公共启动代码提供同一路径。
  set_property(
    TARGET ${TARGET_NAME}
    PROPERTY TEST_LAUNCHER
             "${CMAKE_COMMAND};-E;env;MMM_CONFIG_ROOT=${TEST_CONFIG_ROOT}")

  if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # 这些设置仅影响测试可执行文件，不降低业务库自身的发布优化。 macOS 测试目标仍需从业务库的 ThinLTO bitcode
    # 生成本机代码；先禁用跨模块优化。
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
    # 枚举固定配置集合，避免不同生成器把测试二进制散落到配置子目录。 多配置生成器需要为每个标准配置显式覆盖运行时目录。
    string(TOUPPER "${CONFIG_NAME}" CONFIG_NAME_UPPER)
    set_target_properties(
      ${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CONFIG_NAME_UPPER}
                                "${TEST_RUNTIME_DIR}")
  endforeach()

  # shared 预编译依赖需要复制到测试可执行文件旁边，否则 Windows 动态加载无法从独立测试目录解析 DLL。
  if(COMMAND prebuilt_copy_runtime_files)
    # helper 由预编译依赖层可选提供；源码依赖构建不要求该命令存在。
    prebuilt_copy_runtime_files(${TARGET_NAME})
  endif()
endfunction()

# 统一声明项目测试目标；关闭测试时保留显式目标但排除出默认构建，便于各模块继续集中配置依赖。 统一入口确保任何新增测试都不会绕过配置隔离和运行时文件复制。
# MODULE_NAME 决定测试目录，TARGET_NAME 为目标名，其余参数原样传给 add_executable。
function(mmm_add_test_executable MODULE_NAME TARGET_NAME)
  if(BUILD_TESTING)
    # 开启测试时纳入默认构建，保证 CTest 注册目标始终存在。
    add_executable(${TARGET_NAME} ${ARGN})
  else()
    # 关闭测试时保留可按需构建的目标，避免配置逻辑产生两套分支。
    add_executable(${TARGET_NAME} EXCLUDE_FROM_ALL ${ARGN})
  endif()
  mmm_set_test_target(${MODULE_NAME} ${TARGET_NAME})
endfunction()

# 第三方依赖默认使用预编译库；需要重建依赖时显式开启。 OFF 模式缺失任何预编译内容都应由 Find 模块报错，不能自动回退源码。
option(SOURCES_BUILD "Build Deps from sources." OFF)

# Windows 原生文件对话框默认会用最近访问目录覆盖 defaultPath； 另存为需要强制打开当前项目目录。 该缓存值传递给 NFD
# 子项目，确保保存对话框尊重业务提供的目录。
set(NFD_OVERRIDE_RECENT_WITH_DEFAULT
    ON
    CACHE BOOL "Use NFD defaultPath instead of Windows recent folders." FORCE)

# Vulkan 是图形模块的硬依赖，包装查找器会按本机或交叉平台选择 SDK。
find_package(Vulkan REQUIRED)

# 第三方目录必须先建立统一 target，业务模块随后只依赖 target 名称。 SOURCES_BUILD 的选择在该子目录内部解析，主工程只消费稳定
# target。
add_subdirectory("3rdpty")

# PROJECT_LINKAGE 只描述第三方依赖链接偏好；业务模块维持静态库组织，避免未指定类型的 add_library 被 shared 偏好改写。
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL
          "Build project modules as static libraries after third-party setup."
          FORCE)

# PGO 只作用于本项目业务模块，预编译库和用于生成预编译库的第三方源码都不插桩。
include("pgo")

# 业务模块在依赖、全局编译配置和 PGO 选项确定后统一加入。
add_subdirectory("Modules")

# macOS 需要把应用束、Vulkan 运行时和更新器组合为可分发的 DMG；Windows/Linux 继续直接发布单可执行文件。
if(APPLE)
  # 打包逻辑仅在 Apple 主机构建中定义，其他平台不解析其专用工具。
  include("macos-package")
endif()
