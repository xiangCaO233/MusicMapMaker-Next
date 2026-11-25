# 导入 ImPlot 预编译库，导出源码构建同名目标：3rd_implot。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 绘图核心和图元按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 先完成 imgui 的依赖解析，使绘图核心和图元的缺失在配置阶段可见。
find_package(imgui REQUIRED)
# ImPlot 的公开声明使用 ImGui 类型，两个依赖的头文件及二进制须成套。
prebuilt_include_dir(_implot_include_dir implot)

# 实际绘图对象来自当前配置的归档，业务目标始终通过 3rd_implot 消费。
if(NOT TARGET 3rd_implot)
  # 将绘图核心和图元作为全局导入发布，兄弟目录使用同一选择。
  add_library(3rd_implot UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取与 ImGui 配套的绘图声明，不必手写额外包含目录。
  set_target_properties(3rd_implot PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_implot_include_dir}")

  # 根据生成器启用的配置验证当前 ImPlot 绘图库，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_implot_configs)
  # 记录实际完成解析的绘图核心和图元配置，以便给缺失产物报错。
  set(_implot_imported_configs "")
  # 波形与频谱面板可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_implot_default_library "")
  # 绘图核心和图元每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_implot_config IN LISTS _implot_configs)
    # 3rd_implot 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_implot_config}" _implot_config_upper)
    # 只在已选工具链和配置目录尝试当前 ImPlot 绘图库，不会命中系统安装。
    prebuilt_find_library(_implot_library implot "${_implot_config}" implot
                          3rd_implot)
    # 处理成功才登记 3rd_implot 当前配置，未知产物不产生空导入条目。
    list(APPEND _implot_imported_configs "${_implot_config_upper}")
    # 记录 3rd_implot 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_implot PROPERTIES "IMPORTED_LOCATION_${_implot_config_upper}"
                            "${_implot_library}")
    # 3rd_implot 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_implot_default_library STREQUAL "")
      set(_implot_default_library "${_implot_library}")
    endif()
  endforeach()
  # 如果波形与频谱面板走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 3rd_implot 缺省链接，已指定配置不受它影响。
    set_target_properties(3rd_implot PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                "${_implot_default_library}")
  endif()
  # 将 3rd_implot 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_implot PROPERTIES IMPORTED_CONFIGURATIONS "${_implot_imported_configs}"
                          IMPORTED_LOCATION "${_implot_default_library}")
  # 绘图接口引用 ImGui 数据类型，3rd_imgui 的包含和链接要求须完整转发。
  target_link_libraries(3rd_implot INTERFACE 3rd_imgui)
  if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
    # shared ImPlot 预编译包需要让使用方按 dllimport 访问 API 符号。
    target_compile_definitions(3rd_implot
                               INTERFACE "IMPLOT_API=__declspec(dllimport)")
  endif()
endif()

# 波形与频谱面板使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(implot_FOUND TRUE)
