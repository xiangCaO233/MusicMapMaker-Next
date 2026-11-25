# 导入 freetype 预编译库，并提供项目包装目标：3rd_freetype。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 字体字形和位图解码按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 公开接口由头目录统一提供，字体对象不能交给系统 FreeType 的另一版实现释放。
prebuilt_include_dir(_freetype_include_dir freetype)

# 路径搜索集中在分发目录，不能让已安装系统库替代固定构建的字体后端。
if(NOT TARGET freetype::freetype)
  # 将字体字形和位图解码作为全局导入发布，兄弟目录使用同一选择。
  add_library(freetype::freetype UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取字体引擎公共声明，不必手写额外包含目录。
  set_target_properties(
    freetype::freetype PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                  "${_freetype_include_dir}")

  # 根据生成器启用的配置验证 freetype 与 freetyped 候选，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_freetype_configs)
  # 记录实际完成解析的字体字形和位图解码配置，以便给缺失产物报错。
  set(_freetype_imported_configs "")
  # 文本字形缓存可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_freetype_default_library "")
  # 字体字形和位图解码每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_freetype_config IN LISTS _freetype_configs)
    # freetype::freetype 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_freetype_config}" _freetype_config_upper)
    # 只在已选工具链和配置目录尝试 freetype 与 freetyped 候选，不会命中系统安装。
    prebuilt_find_library(
      _freetype_library
      freetype
      "${_freetype_config}"
      freetype
      freetyped
      libfreetype
      libfreetyped)
    # 处理成功才登记 freetype::freetype 当前配置，未知产物不产生空导入条目。
    list(APPEND _freetype_imported_configs "${_freetype_config_upper}")
    # 记录 freetype::freetype 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      freetype::freetype
      PROPERTIES "IMPORTED_LOCATION_${_freetype_config_upper}"
                 "${_freetype_library}")
    # freetype::freetype 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_freetype_default_library STREQUAL "")
      set(_freetype_default_library "${_freetype_library}")
    endif()
  endforeach()
  # 如果文本字形缓存走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 freetype::freetype 缺省链接，已指定配置不受它影响。
    set_target_properties(
      freetype::freetype PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                    "${_freetype_default_library}")
  endif()
  # 将 freetype::freetype 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    freetype::freetype
    PROPERTIES IMPORTED_CONFIGURATIONS "${_freetype_imported_configs}"
               IMPORTED_LOCATION "${_freetype_default_library}")
endif()

if(UNIX AND NOT APPLE)
  # Linux 预编译 FreeType 启用了 PNG 支持；静态导入时需要显式恢复 libpng 传递依赖。

  # 先完成 PNG 的依赖解析，使字体字形和位图解码的缺失在配置阶段可见。
  find_package(PNG REQUIRED)
  # Linux 静态归档编入了 PNG 支持；将 PNG::PNG 传给最终字体消费者闭合链接。
  target_link_libraries(freetype::freetype INTERFACE PNG::PNG)
endif()

# 上游命名目标仍可由依赖使用，业务侧只绑定项目稳定的字体包装名。
if(NOT TARGET 3rd_freetype)
  # 包装接口供文本字形缓存获得 Linux PNG 字体路径及字体 API，无需在该处重新声明系统库。
  add_library(3rd_freetype INTERFACE)
  target_link_libraries(3rd_freetype INTERFACE freetype::freetype)
endif()

# 文本字形缓存使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(freetype_FOUND TRUE)
