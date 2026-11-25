# 导入 fmt 预编译库，导出源码构建同名目标：fmt::fmt。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 编译格式化与调用点模板按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 日志和格式化模板在各消费者处实例化，头文件版本必须匹配符号命名空间。
prebuilt_include_dir(_fmt_include_dir fmt)

# 兼容 fmt 的 release 名和 Debug 后缀名，但候选仍限定在当前配置目录。
if(NOT TARGET fmt::fmt)
  # 将编译格式化与调用点模板作为全局导入发布，兄弟目录使用同一选择。
  add_library(fmt::fmt UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取与格式化符号版本匹配的头，不必手写额外包含目录。
  set_target_properties(fmt::fmt PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_fmt_include_dir}")

  # 根据生成器启用的配置验证 fmt、fmtd 及对应 lib 名，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_fmt_configs)
  # 记录实际完成解析的编译格式化与调用点模板配置，以便给缺失产物报错。
  set(_fmt_imported_configs "")
  # 诊断日志及消息格式化可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_fmt_default_library "")
  # 编译格式化与调用点模板每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_fmt_config IN LISTS _fmt_configs)
    # fmt::fmt 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_fmt_config}" _fmt_config_upper)
    # 只在已选工具链和配置目录尝试 fmt、fmtd 及对应 lib 名，不会命中系统安装。
    prebuilt_find_library(
      _fmt_library
      fmt
      "${_fmt_config}"
      fmt
      fmtd
      libfmt
      libfmtd)
    # 处理成功才登记 fmt::fmt 当前配置，未知产物不产生空导入条目。
    list(APPEND _fmt_imported_configs "${_fmt_config_upper}")
    # 记录 fmt::fmt 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      fmt::fmt PROPERTIES "IMPORTED_LOCATION_${_fmt_config_upper}"
                          "${_fmt_library}")
    # fmt::fmt 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_fmt_default_library STREQUAL "")
      set(_fmt_default_library "${_fmt_library}")
    endif()
  endforeach()
  # 如果诊断日志及消息格式化走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 fmt::fmt 缺省链接，已指定配置不受它影响。
    set_target_properties(fmt::fmt PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_fmt_default_library}")
  endif()
  # 将 fmt::fmt 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    fmt::fmt PROPERTIES IMPORTED_CONFIGURATIONS "${_fmt_imported_configs}"
                        IMPORTED_LOCATION "${_fmt_default_library}")
endif()

# 诊断日志及消息格式化使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(fmt_FOUND TRUE)
