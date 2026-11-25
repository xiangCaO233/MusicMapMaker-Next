# 导入 miniz 预编译库，导出源码构建同名目标：3rd_miniz。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# ZIP 结构、解压和压缩按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# ZIP 和压缩接口共用项目生成的导出头，该头与库的链接方式必须匹配。
prebuilt_include_dir(_miniz_include_dir miniz)

# 查找时包含上游名与项目包装归档名，避免源码构建的产物改名时破坏消费者。
if(NOT TARGET 3rd_miniz)
  # 将 ZIP 结构、解压和压缩作为全局导入发布，兄弟目录使用同一选择。
  add_library(3rd_miniz UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取生成导出宏及压缩接口，不必手写额外包含目录。
  set_target_properties(3rd_miniz PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                             "${_miniz_include_dir}")

  # 根据生成器启用的配置验证 miniz 或 3rd_miniz 归档，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_miniz_configs)
  # 记录实际完成解析的 ZIP 结构、解压和压缩配置，以便给缺失产物报错。
  set(_miniz_imported_configs "")
  # 资产包与协作资源交付可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_miniz_default_library "")
  # ZIP 结构、解压和压缩每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_miniz_config IN LISTS _miniz_configs)
    # 3rd_miniz 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_miniz_config}" _miniz_config_upper)
    # 只在已选工具链和配置目录尝试 miniz 或 3rd_miniz 归档，不会命中系统安装。
    prebuilt_find_library(_miniz_library miniz "${_miniz_config}" miniz
                          3rd_miniz)
    # 处理成功才登记 3rd_miniz 当前配置，未知产物不产生空导入条目。
    list(APPEND _miniz_imported_configs "${_miniz_config_upper}")
    # 记录 3rd_miniz 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_miniz PROPERTIES "IMPORTED_LOCATION_${_miniz_config_upper}"
                           "${_miniz_library}")
    # 3rd_miniz 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_miniz_default_library STREQUAL "")
      set(_miniz_default_library "${_miniz_library}")
    endif()
  endforeach()
  # 如果资产包与协作资源交付走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 3rd_miniz 缺省链接，已指定配置不受它影响。
    set_target_properties(3rd_miniz PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                               "${_miniz_default_library}")
  endif()
  # 将 3rd_miniz 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_miniz PROPERTIES IMPORTED_CONFIGURATIONS "${_miniz_imported_configs}"
                         IMPORTED_LOCATION "${_miniz_default_library}")
endif()

# 资产包与协作资源交付使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(miniz_FOUND TRUE)
