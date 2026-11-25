# 导入 libjuice 预编译静态库，并导出源码构建同名包装目标：3rd_libjuice。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# ICE 地址候选协商按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 地址协商层是静态 DataChannel 依赖，不能与不同版本的系统安装混用。
prebuilt_include_dir(_libjuice_include_dir libjuice)

# 候选名兼容上游两种产物布局，线程和 Windows 网络支持由包装接口补全。
if(NOT TARGET 3rd_libjuice_library)
  # 将 ICE 地址候选协商作为全局导入发布，兄弟目录使用同一选择。
  add_library(3rd_libjuice_library UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取候选地址与数据回调 ABI，不必手写额外包含目录。
  set_target_properties(
    3rd_libjuice_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                    "${_libjuice_include_dir}")

  # 根据生成器启用的配置验证 juice-static 或 juice，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_libjuice_configs)
  # 记录实际完成解析的 ICE 地址候选协商配置，以便给缺失产物报错。
  set(_libjuice_imported_configs "")
  # 协作直连可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_libjuice_default_library "")
  # ICE 地址候选协商每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_libjuice_config IN LISTS _libjuice_configs)
    # 3rd_libjuice_library 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_libjuice_config}" _libjuice_config_upper)
    # 只在已选工具链和配置目录尝试 juice-static 或 juice，不会命中系统安装。
    prebuilt_find_library(_libjuice_library libjuice "${_libjuice_config}"
                          juice-static juice)
    # 处理成功才登记 3rd_libjuice_library 当前配置，未知产物不产生空导入条目。
    list(APPEND _libjuice_imported_configs "${_libjuice_config_upper}")
    # 记录 3rd_libjuice_library 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_libjuice_library
      PROPERTIES "IMPORTED_LOCATION_${_libjuice_config_upper}"
                 "${_libjuice_library}")
    # 3rd_libjuice_library 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_libjuice_default_library STREQUAL "")
      set(_libjuice_default_library "${_libjuice_library}")
    endif()
  endforeach()
  # 将 3rd_libjuice_library 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_libjuice_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_libjuice_imported_configs}"
               IMPORTED_LOCATION "${_libjuice_default_library}")
endif()

# 包装目标供传输层统一获得协商依赖，不直接把内部归档名写入业务模块。
if(NOT TARGET 3rd_libjuice)
  # 先完成 Threads 的依赖解析，使 ICE 地址候选协商的缺失在配置阶段可见。
  find_package(Threads REQUIRED)
  # 包装接口供协作直连获得线程、Winsock 和 BCrypt，无需在该处重新声明系统库。
  add_library(3rd_libjuice INTERFACE)
  # 后台候选协商传递 Threads::Threads；Windows 另需套接字与随机数系统接口。
  target_link_libraries(3rd_libjuice INTERFACE 3rd_libjuice_library
                                               Threads::Threads)
  # Windows 最终链接需闭合协作直连的本机依赖，具体接口只在这个平台传播。
  if(WIN32)
    target_link_libraries(3rd_libjuice INTERFACE ws2_32 bcrypt)
  endif()
endif()

# 协作直连使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(libjuice_FOUND TRUE)
