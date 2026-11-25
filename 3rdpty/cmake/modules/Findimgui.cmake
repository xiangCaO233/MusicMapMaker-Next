# 导入 ImGui 预编译库，并提供项目包装目标：3rd_imgui。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 即时控件和平台后端按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 先完成 glfw 的依赖解析，使即时控件和平台后端的缺失在配置阶段可见。
find_package(glfw REQUIRED)
# 调用者使用头中定义的顶点和内部状态布局，不允许在编译和链接间混用版本。
prebuilt_include_dir(_imgui_include_dir imgui)

# ImGui 核心与渲染集成按固定构建产物链接，依赖 Vulkan 由接口目标转发。
if(NOT TARGET Vulkan::Vulkan)
  # 先完成 Vulkan 的依赖解析，使即时控件和平台后端的缺失在配置阶段可见。
  find_package(Vulkan REQUIRED)
endif()

# 窗口功能统一通过 3rd_imgui 链接，避免再次加载不同的 ImGui 上下文实现。
if(NOT TARGET imgui-static)
  # 将即时控件和平台后端作为全局导入发布，兄弟目录使用同一选择。
  add_library(imgui-static UNKNOWN IMPORTED GLOBAL)
  # 记录 imgui-static 对应本轮配置的库路径，生成器可在构建时按配置取用。
  set_target_properties(
    imgui-static
    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
               "${_imgui_include_dir};${_imgui_include_dir}/backends")

  # 根据生成器启用的配置验证当前 ImGui 编译产物，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_imgui_configs)
  # 记录实际完成解析的即时控件和平台后端配置，以便给缺失产物报错。
  set(_imgui_imported_configs "")
  # 业务窗口与 Vulkan 后端可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_imgui_default_library "")
  # 即时控件和平台后端每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_imgui_config IN LISTS _imgui_configs)
    # imgui-static 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_imgui_config}" _imgui_config_upper)
    # 只在已选工具链和配置目录尝试当前 ImGui 编译产物，不会命中系统安装。
    prebuilt_find_library(_imgui_library imgui "${_imgui_config}" imgui
                          imgui-static)
    # 处理成功才登记 imgui-static 当前配置，未知产物不产生空导入条目。
    list(APPEND _imgui_imported_configs "${_imgui_config_upper}")
    set_target_properties(
      imgui-static PROPERTIES "IMPORTED_LOCATION_${_imgui_config_upper}"
                              "${_imgui_library}")
    # imgui-static 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_imgui_default_library STREQUAL "")
      set(_imgui_default_library "${_imgui_library}")
    endif()
  endforeach()
  # 如果业务窗口与 Vulkan 后端走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 imgui-static 缺省链接，已指定配置不受它影响。
    set_target_properties(imgui-static PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                  "${_imgui_default_library}")
  endif()
  # 将 imgui-static 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    imgui-static PROPERTIES IMPORTED_CONFIGURATIONS "${_imgui_imported_configs}"
                            IMPORTED_LOCATION "${_imgui_default_library}")
  # 窗口和图形后端都是公开依赖，静态归档无法单独提供这两个接口。
  target_link_libraries(imgui-static INTERFACE 3rd_glfw Vulkan::Vulkan)
  if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
    # shared ImGui 预编译包需要让使用方按 dllimport 访问数据符号。
    target_compile_definitions(
      imgui-static INTERFACE "IMGUI_API=__declspec(dllimport)"
                             "IMGUI_IMPL_API=__declspec(dllimport)")
  endif()
endif()

if(NOT TARGET 3rd_imgui)
  # 包装接口供业务窗口与 Vulkan 后端获得 GLFW 与 Vulkan 后端，无需在该处重新声明系统库。
  add_library(3rd_imgui INTERFACE)
  target_link_libraries(3rd_imgui INTERFACE imgui-static)
endif()

# 业务窗口与 Vulkan 后端使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(imgui_FOUND TRUE)
