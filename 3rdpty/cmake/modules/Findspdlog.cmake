# 导入 spdlog 预编译库，导出源码构建同名目标：spdlog::spdlog。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 日志写入与格式化按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 先完成 fmt 的依赖解析，使日志写入与格式化的缺失在配置阶段可见。
find_package(fmt REQUIRED)
# 日志模板与编译好的 spdlog 共享版本，外置 fmt 由统一格式化包提供。
prebuilt_include_dir(_spdlog_include_dir spdlog)

# 归档按生成器配置解析，fmt 依赖继续沿接口到调用者，避免引入另一套格式化实现。
if(NOT TARGET spdlog::spdlog)
  # 将日志写入与格式化作为全局导入发布，兄弟目录使用同一选择。
  add_library(spdlog::spdlog UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取与 SPDLOG_COMPILED_LIB 匹配的公开头，不必手写额外包含目录。
  set_target_properties(spdlog::spdlog PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_spdlog_include_dir}")

  # 根据生成器启用的配置验证当前配置的 spdlog 归档，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_spdlog_configs)
  # 记录实际完成解析的日志写入与格式化配置，以便给缺失产物报错。
  set(_spdlog_imported_configs "")
  # 各模块诊断可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_spdlog_default_library "")
  # 日志写入与格式化每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_spdlog_config IN LISTS _spdlog_configs)
    # spdlog::spdlog 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_spdlog_config}" _spdlog_config_upper)
    # 只在已选工具链和配置目录尝试当前配置的 spdlog 归档，不会命中系统安装。
    prebuilt_find_library(
      _spdlog_library
      spdlog
      "${_spdlog_config}"
      spdlog
      spdlogd
      libspdlog
      libspdlogd)
    # 处理成功才登记 spdlog::spdlog 当前配置，未知产物不产生空导入条目。
    list(APPEND _spdlog_imported_configs "${_spdlog_config_upper}")
    # 记录 spdlog::spdlog 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      spdlog::spdlog PROPERTIES "IMPORTED_LOCATION_${_spdlog_config_upper}"
                                "${_spdlog_library}")
    # spdlog::spdlog 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_spdlog_default_library STREQUAL "")
      set(_spdlog_default_library "${_spdlog_library}")
    endif()
  endforeach()
  # 如果各模块诊断走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 spdlog::spdlog 缺省链接，已指定配置不受它影响。
    set_target_properties(
      spdlog::spdlog PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_spdlog_default_library}")
  endif()
  # 将 spdlog::spdlog 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    spdlog::spdlog
    PROPERTIES IMPORTED_CONFIGURATIONS "${_spdlog_imported_configs}"
               IMPORTED_LOCATION "${_spdlog_default_library}")
  # 预编译包链接的是 spdlog 编译库，消费者必须关闭 header-only 实现。
  target_compile_definitions(spdlog::spdlog INTERFACE SPDLOG_COMPILED_LIB
                                                      SPDLOG_FMT_EXTERNAL)
  # 把 fmt::fmt 与外置 fmt 编译定义一同传出，确保日志模板和归档符号一致。
  target_link_libraries(spdlog::spdlog INTERFACE fmt::fmt)
endif()

# 各模块诊断使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(spdlog_FOUND TRUE)
