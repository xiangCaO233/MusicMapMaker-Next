# 导入 lunasvg 预编译库，并提供项目包装目标：3rd_lunasvg。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# SVG 和底层矢量光栅化按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 先完成 freetype 的依赖解析，使 SVG 和底层矢量光栅化的缺失在配置阶段可见。
find_package(freetype REQUIRED)
# SVG 接口使用同一配方产出的头文件，PlutoVG 与 LunaSVG 的目标显式分开。
prebuilt_include_dir(_lunasvg_include_dir lunasvg)

# * PlutoVG 提供底层路径与像素实现，LunaSVG 的静态归档需要显式依赖它。
# * 公开目标独立，使上游绘图接口可以直接使用而不把 SVG 解析反向引入。
# * 两个库的选择必须使用同一个套件及同一消费配置。
if(NOT TARGET plutovg::plutovg)
  # 将 SVG 和底层矢量光栅化作为全局导入发布，兄弟目录使用同一选择。
  add_library(plutovg::plutovg UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取同一版的绘图公共头，不必手写额外包含目录。
  set_target_properties(
    plutovg::plutovg PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                "${_lunasvg_include_dir}")

  # 根据生成器启用的配置验证配对的 PlutoVG 与 LunaSVG，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_plutovg_configs)
  # 记录实际完成解析的 SVG 和底层矢量光栅化配置，以便给缺失产物报错。
  set(_plutovg_imported_configs "")
  # 图标与封面渲染可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_plutovg_default_library "")
  # SVG 和底层矢量光栅化每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_plutovg_config IN LISTS _plutovg_configs)
    # plutovg::plutovg 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_plutovg_config}" _plutovg_config_upper)
    # 只在已选工具链和配置目录尝试配对的 PlutoVG 与 LunaSVG，不会命中系统安装。
    prebuilt_find_library(_plutovg_library lunasvg "${_plutovg_config}" plutovg
                          libplutovg)
    # 处理成功才登记 plutovg::plutovg 当前配置，未知产物不产生空导入条目。
    list(APPEND _plutovg_imported_configs "${_plutovg_config_upper}")
    # 记录 plutovg::plutovg 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      plutovg::plutovg PROPERTIES "IMPORTED_LOCATION_${_plutovg_config_upper}"
                                  "${_plutovg_library}")
    # plutovg::plutovg 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_plutovg_default_library STREQUAL "")
      set(_plutovg_default_library "${_plutovg_library}")
    endif()
  endforeach()
  # 如果图标与封面渲染走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 plutovg::plutovg 缺省链接，已指定配置不受它影响。
    set_target_properties(
      plutovg::plutovg PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                  "${_plutovg_default_library}")
  endif()
  # 将 plutovg::plutovg 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    plutovg::plutovg
    PROPERTIES IMPORTED_CONFIGURATIONS "${_plutovg_imported_configs}"
               IMPORTED_LOCATION "${_plutovg_default_library}")
  if(PROJECT_LINKAGE STREQUAL "static")
    # * 静态链接时调用者不能沿用 Windows DLL 导入宏。
    # * 只定义 PlutoVG 的模式，保留 LunaSVG 自己独立的接口定义。
    target_compile_definitions(plutovg::plutovg INTERFACE PLUTOVG_BUILD_STATIC)
  endif()
endif()

# * LunaSVG 消费底层 PlutoVG 和字体引擎，业务目标只挂这一层。
# * SVG 解析归档单独映射配置，保持与固定配方的依赖版本配套。
if(NOT TARGET lunasvg::lunasvg)
  add_library(lunasvg::lunasvg UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    lunasvg::lunasvg PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                "${_lunasvg_include_dir}")

  prebuilt_target_configs(_lunasvg_configs)
  set(_lunasvg_imported_configs "")
  set(_lunasvg_default_library "")
  foreach(_lunasvg_config IN LISTS _lunasvg_configs)
    # lunasvg::lunasvg 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_lunasvg_config}" _lunasvg_config_upper)
    prebuilt_find_library(_lunasvg_library lunasvg "${_lunasvg_config}" lunasvg
                          liblunasvg)
    # 处理成功才登记 lunasvg::lunasvg 当前配置，未知产物不产生空导入条目。
    list(APPEND _lunasvg_imported_configs "${_lunasvg_config_upper}")
    # 记录 lunasvg::lunasvg 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      lunasvg::lunasvg PROPERTIES "IMPORTED_LOCATION_${_lunasvg_config_upper}"
                                  "${_lunasvg_library}")
    # lunasvg::lunasvg 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_lunasvg_default_library STREQUAL "")
      set(_lunasvg_default_library "${_lunasvg_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 lunasvg::lunasvg 缺省链接，已指定配置不受它影响。
    set_target_properties(
      lunasvg::lunasvg PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                  "${_lunasvg_default_library}")
  endif()
  # 将 lunasvg::lunasvg 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    lunasvg::lunasvg
    PROPERTIES IMPORTED_CONFIGURATIONS "${_lunasvg_imported_configs}"
               IMPORTED_LOCATION "${_lunasvg_default_library}")
  if(PROJECT_LINKAGE STREQUAL "static")
    # * LunaSVG 和公开透出的 PlutoVG 两套声明都必须匹配静态归档。
    # * 共享布局不使用这些定义，避免取消上游动态接口要求。
    target_compile_definitions(lunasvg::lunasvg INTERFACE LUNASVG_BUILD_STATIC
                                                          PLUTOVG_BUILD_STATIC)
  endif()
  # LunaSVG 与 PlutoVG 成对分发，公开图形依赖要随包装目标传播。
  target_link_libraries(lunasvg::lunasvg INTERFACE plutovg::plutovg
                                                   freetype::freetype)
endif()

if(NOT TARGET 3rd_lunasvg)
  # 包装接口供图标与封面渲染获得矢量图覆盖与栅格化两级实现，无需在该处重新声明系统库。
  add_library(3rd_lunasvg INTERFACE)
  target_link_libraries(3rd_lunasvg INTERFACE lunasvg::lunasvg)
endif()

# 图标与封面渲染使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(lunasvg_FOUND TRUE)
