# 导入 ImGuiFileDialog 预编译库，并提供项目包装目标：3rd_ImGuiFileDialog。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 文件选择器控件实现按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 先完成 imgui 的依赖解析，使文件选择器控件实现的缺失在配置阶段可见。
find_package(imgui REQUIRED)
# 选择器公开类型使用 ImGui 类型，二者要来自互相匹配的头。
prebuilt_include_dir(_imgui_file_dialog_include_dir ImGuiFileDialog)

# 上游产物名称与项目包装接口分离，以便兼容 Windows 导入库和 Unix 归档。
if(NOT TARGET ImGuiFileDialog)
  # 将文件选择器控件实现作为全局导入发布，兄弟目录使用同一选择。
  add_library(ImGuiFileDialog UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取普通扩展包与固定的 ImGui 头，不必手写额外包含目录。
  set_target_properties(
    ImGuiFileDialog PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                               "${_imgui_file_dialog_include_dir}")

  # 根据生成器启用的配置验证项目所选静态或共享选择器，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_imgui_file_dialog_configs)
  # 记录实际完成解析的文件选择器控件实现配置，以便给缺失产物报错。
  set(_imgui_file_dialog_imported_configs "")
  # 目录选择窗口可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_imgui_file_dialog_default_library "")
  # 文件选择器控件实现每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_imgui_file_dialog_config IN LISTS _imgui_file_dialog_configs)
    # ImGuiFileDialog 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_imgui_file_dialog_config}"
                   _imgui_file_dialog_config_upper)
    # 只在已选工具链和配置目录尝试项目所选静态或共享选择器，不会命中系统安装。
    prebuilt_find_library(_imgui_file_dialog_library ImGuiFileDialog
                          "${_imgui_file_dialog_config}" ImGuiFileDialog)
    # 处理成功才登记 ImGuiFileDialog 当前配置，未知产物不产生空导入条目。
    list(APPEND _imgui_file_dialog_imported_configs
         "${_imgui_file_dialog_config_upper}")
    # 记录 ImGuiFileDialog 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      ImGuiFileDialog
      PROPERTIES "IMPORTED_LOCATION_${_imgui_file_dialog_config_upper}"
                 "${_imgui_file_dialog_library}")
    # ImGuiFileDialog 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_imgui_file_dialog_default_library STREQUAL "")
      set(_imgui_file_dialog_default_library "${_imgui_file_dialog_library}")
    endif()
  endforeach()
  # 如果目录选择窗口走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 ImGuiFileDialog 缺省链接，已指定配置不受它影响。
    set_target_properties(
      ImGuiFileDialog PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                 "${_imgui_file_dialog_default_library}")
  endif()
  # 将 ImGuiFileDialog 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    ImGuiFileDialog
    PROPERTIES IMPORTED_CONFIGURATIONS "${_imgui_file_dialog_imported_configs}"
               IMPORTED_LOCATION "${_imgui_file_dialog_default_library}")
  # 预编译 ImGui 提供选择器公开接口中引用的绘制与上下文类型。
  target_link_libraries(ImGuiFileDialog INTERFACE 3rd_imgui)
endif()

# 只在包装目标缺失时发布入口，已经使用该接口的其他模块保留相同的 ImGui 约束。
if(NOT TARGET 3rd_ImGuiFileDialog)
  # 包装接口供目录选择窗口获得可见选择器及同一 ImGui 上下文，无需在该处重新声明系统库。
  add_library(3rd_ImGuiFileDialog INTERFACE)
  target_link_libraries(3rd_ImGuiFileDialog INTERFACE ImGuiFileDialog)
endif()

# 目录选择窗口使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(ImGuiFileDialog_FOUND TRUE)
