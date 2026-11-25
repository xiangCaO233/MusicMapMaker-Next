# 导入 LuaJIT 预编译库，导出源码构建同名目标：luajit::luajit。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# LuaJIT 虚拟机及 C API 按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# LuaJIT 与 Lua 5.4 的 ABI 不同，绑定层必须取得实际链接对象对应的声明。
prebuilt_include_dir(_luajit_include_dir luajit)

# 静态和动态构建生成不同命名的库，按链接模式构造候选避免混装运行时。
if(NOT TARGET luajit::luajit)
  # 将 LuaJIT 虚拟机及 C API 作为全局导入发布，兄弟目录使用同一选择。
  add_library(luajit::luajit UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取与 JIT ABI 匹配的 Lua C 接口，不必手写额外包含目录。
  set_target_properties(luajit::luajit PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_luajit_include_dir}")

  # 根据生成器启用的配置验证静态或动态匹配的 LuaJIT 命名，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_luajit_configs)
  # 记录实际完成解析的 LuaJIT 虚拟机及 C API 配置，以便给缺失产物报错。
  set(_luajit_imported_configs "")
  # 主题和工具脚本可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_luajit_default_library "")
  # LuaJIT 虚拟机及 C API 每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_luajit_config IN LISTS _luajit_configs)
    # luajit::luajit 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_luajit_config}" _luajit_config_upper)
    if(PROJECT_LINKAGE STREQUAL "shared")
      set(_luajit_library_names lua51 luajit libluajit)
      # LuaJIT 虚拟机及 C API 在此配置使用另一组发布文件名或系统依赖，仍限定本库的作用域。
    else()
      set(_luajit_library_names luajit libluajit lua51)
    endif()
    # 只在已选工具链和配置目录尝试静态或动态匹配的 LuaJIT 命名，不会命中系统安装。
    prebuilt_find_library(_luajit_library luajit "${_luajit_config}"
                          ${_luajit_library_names})
    # 处理成功才登记 luajit::luajit 当前配置，未知产物不产生空导入条目。
    list(APPEND _luajit_imported_configs "${_luajit_config_upper}")
    # 记录 luajit::luajit 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      luajit::luajit PROPERTIES "IMPORTED_LOCATION_${_luajit_config_upper}"
                                "${_luajit_library}")
    # luajit::luajit 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_luajit_default_library STREQUAL "")
      set(_luajit_default_library "${_luajit_library}")
    endif()
  endforeach()
  # 如果主题和工具脚本走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 luajit::luajit 缺省链接，已指定配置不受它影响。
    set_target_properties(
      luajit::luajit PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_luajit_default_library}")
  endif()
  # 将 luajit::luajit 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    luajit::luajit
    PROPERTIES IMPORTED_CONFIGURATIONS "${_luajit_imported_configs}"
               IMPORTED_LOCATION "${_luajit_default_library}")

  if(UNIX)
    # POSIX 动态加载、数学库及线程需求与 Windows 计时接口分平台传播。
    target_link_libraries(luajit::luajit INTERFACE m dl)
    if(NOT APPLE)
      target_link_libraries(luajit::luajit INTERFACE pthread)
    endif()
  endif()
  # Windows 最终链接需闭合主题和工具脚本的本机依赖，具体接口只在这个平台传播。
  if(WIN32)
    target_link_libraries(luajit::luajit INTERFACE winmm)
  endif()
endif()

# 主题和工具脚本使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(luajit_FOUND TRUE)
