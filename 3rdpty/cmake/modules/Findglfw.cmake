# 导入 GLFW 预编译库，并提供项目包装目标：3rd_glfw。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 窗口和输入回调按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 公共头与打包的 glfw 归档一起选择，减少系统窗口版本与项目适配器不一致的风险。
prebuilt_include_dir(_glfw_include_dir glfw)

# 平台窗口和 Vulkan 依赖在链接接口上解决，归档查找不依赖系统路径。
if(NOT TARGET Vulkan::Vulkan)
  # 先完成 Vulkan 的依赖解析，使窗口和输入回调的缺失在配置阶段可见。
  find_package(Vulkan REQUIRED)
endif()

# 窗口模块和 ImGui 后端共享 3rd_glfw 入口，不各自导入不同版本的 GLFW。
if(NOT TARGET glfw)
  # 将窗口和输入回调作为全局导入发布，兄弟目录使用同一选择。
  add_library(glfw UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取窗口目标的包含目录，不必手写额外包含目录。
  set_target_properties(glfw PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                        "${_glfw_include_dir}")

  # 根据生成器启用的配置验证 glfw 与 glfw3 候选，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_glfw_configs)
  # 记录实际完成解析的窗口和输入回调配置，以便给缺失产物报错。
  set(_glfw_imported_configs "")
  # 窗口适配器可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_glfw_default_library "")
  # 窗口和输入回调每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_glfw_config IN LISTS _glfw_configs)
    # glfw 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_glfw_config}" _glfw_config_upper)
    # 只在已选工具链和配置目录尝试 glfw 与 glfw3 候选，不会命中系统安装。
    prebuilt_find_library(_glfw_library glfw "${_glfw_config}" glfw glfw3)
    # 处理成功才登记 glfw 当前配置，未知产物不产生空导入条目。
    list(APPEND _glfw_imported_configs "${_glfw_config_upper}")
    # 记录 glfw 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      glfw PROPERTIES "IMPORTED_LOCATION_${_glfw_config_upper}"
                      "${_glfw_library}")
    # glfw 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_glfw_default_library STREQUAL "")
      set(_glfw_default_library "${_glfw_library}")
    endif()
  endforeach()
  # 如果窗口适配器走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 glfw 缺省链接，已指定配置不受它影响。
    set_target_properties(glfw PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                          "${_glfw_default_library}")
  endif()
  # 将 glfw 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    glfw PROPERTIES IMPORTED_CONFIGURATIONS "${_glfw_imported_configs}"
                    IMPORTED_LOCATION "${_glfw_default_library}")
  # Vulkan 接口须随 GLFW 提供；Apple 分支另外补四个窗口相关系统框架。
  target_link_libraries(glfw INTERFACE Vulkan::Vulkan)

  if(APPLE)
    # macOS 静态 GLFW 不会把系统框架封装进归档，导入目标必须恢复源码目标的传递依赖。
    find_library(_glfw_cocoa_framework Cocoa REQUIRED)
    find_library(_glfw_iokit_framework IOKit REQUIRED)
    find_library(_glfw_quartz_core_framework QuartzCore REQUIRED)
    find_library(_glfw_core_foundation_framework CoreFoundation REQUIRED)
    target_link_libraries(
      glfw
      INTERFACE "${_glfw_cocoa_framework}" "${_glfw_iokit_framework}"
                "${_glfw_quartz_core_framework}"
                "${_glfw_core_foundation_framework}")
  endif()
endif()

if(NOT TARGET 3rd_glfw)
  # 包装接口供窗口适配器获得 Cocoa 事件与窗口系统所需框架，无需在该处重新声明系统库。
  add_library(3rd_glfw INTERFACE)
  target_link_libraries(3rd_glfw INTERFACE glfw)
endif()

# 窗口适配器使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(glfw_FOUND TRUE)
