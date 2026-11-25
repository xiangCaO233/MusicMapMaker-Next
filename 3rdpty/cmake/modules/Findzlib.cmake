# 导入 zlib 预编译库，导出源码构建同名目标：3rd_zlib。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# flate API 及归档校验按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 压缩接口使用分发包的头，静态配置不可误配系统 zlib 的导入规则。
prebuilt_include_dir(_zlib_include_dir zlib)

# 不同工具链以及 Debug 后缀命名差异在当前配置目录内消化。
if(NOT TARGET 3rd_zlib)
  # 将 flate API 及归档校验作为全局导入发布，兄弟目录使用同一选择。
  add_library(3rd_zlib UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取与产物配对的 zlib.h，不必手写额外包含目录。
  set_target_properties(3rd_zlib PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_zlib_include_dir}")

  # 根据生成器启用的配置验证 z、zlib 与静态调试后缀候选，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_zlib_configs)
  # 记录实际完成解析的 flate API 及归档校验配置，以便给缺失产物报错。
  set(_zlib_imported_configs "")
  # 压缩资源的下层解码可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_zlib_default_library "")
  # flate API 及归档校验每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_zlib_config IN LISTS _zlib_configs)
    # 3rd_zlib 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_zlib_config}" _zlib_config_upper)
    # 只在已选工具链和配置目录尝试 z、zlib 与静态调试后缀候选，不会命中系统安装。
    prebuilt_find_library(
      _zlib_library
      zlib
      "${_zlib_config}"
      z
      libz
      zlib
      libzs
      libzsd)
    # 处理成功才登记 3rd_zlib 当前配置，未知产物不产生空导入条目。
    list(APPEND _zlib_imported_configs "${_zlib_config_upper}")
    # 记录 3rd_zlib 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_zlib PROPERTIES "IMPORTED_LOCATION_${_zlib_config_upper}"
                          "${_zlib_library}")
    # 3rd_zlib 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_zlib_default_library STREQUAL "")
      set(_zlib_default_library "${_zlib_library}")
    endif()
  endforeach()
  # 如果压缩资源的下层解码走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 3rd_zlib 缺省链接，已指定配置不受它影响。
    set_target_properties(3rd_zlib PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_zlib_default_library}")
  endif()
  # 将 3rd_zlib 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_zlib PROPERTIES IMPORTED_CONFIGURATIONS "${_zlib_imported_configs}"
                        IMPORTED_LOCATION "${_zlib_default_library}")
endif()

# 压缩资源的下层解码使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(zlib_FOUND TRUE)
