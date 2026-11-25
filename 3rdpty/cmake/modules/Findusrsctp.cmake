# 导入 usrsctp 预编译静态库，并导出源码构建同名包装目标：3rd_usrsctp。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 用户态 SCTP 数据传输按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 用户态 SCTP 给 DataChannel 提供传输，公共 ABI 与归档采用同一打包版本。
prebuilt_include_dir(_usrsctp_include_dir usrsctp)

# 包装目标补充平台网络与线程依赖，避免调用者按平台重复写底层链接表。
if(NOT TARGET 3rd_usrsctp_library)
  # 将用户态 SCTP 数据传输作为全局导入发布，兄弟目录使用同一选择。
  add_library(3rd_usrsctp_library UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取编译协商层所用 SCTP 声明，不必手写额外包含目录。
  set_target_properties(
    3rd_usrsctp_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                   "${_usrsctp_include_dir}")

  # 根据生成器启用的配置验证同配置 usrsctp 归档，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_usrsctp_configs)
  # 记录实际完成解析的用户态 SCTP 数据传输配置，以便给缺失产物报错。
  set(_usrsctp_imported_configs "")
  # 可靠有序 DataChannel 可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_usrsctp_default_library "")
  # 用户态 SCTP 数据传输每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_usrsctp_config IN LISTS _usrsctp_configs)
    # 3rd_usrsctp_library 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_usrsctp_config}" _usrsctp_config_upper)
    # 只在已选工具链和配置目录尝试同配置 usrsctp 归档，不会命中系统安装。
    prebuilt_find_library(_usrsctp_library usrsctp "${_usrsctp_config}" usrsctp)
    # 处理成功才登记 3rd_usrsctp_library 当前配置，未知产物不产生空导入条目。
    list(APPEND _usrsctp_imported_configs "${_usrsctp_config_upper}")
    # 记录 3rd_usrsctp_library 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_usrsctp_library
      PROPERTIES "IMPORTED_LOCATION_${_usrsctp_config_upper}"
                 "${_usrsctp_library}")
    # 3rd_usrsctp_library 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_usrsctp_default_library STREQUAL "")
      set(_usrsctp_default_library "${_usrsctp_library}")
    endif()
  endforeach()
  # 将 3rd_usrsctp_library 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_usrsctp_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_usrsctp_imported_configs}"
               IMPORTED_LOCATION "${_usrsctp_default_library}")
endif()

# 协作传输只使用 3rd_usrsctp，平台系统库和原始归档由这一层汇总。
if(NOT TARGET 3rd_usrsctp)
  # 先完成 Threads 的依赖解析，使用户态 SCTP 数据传输的缺失在配置阶段可见。
  find_package(Threads REQUIRED)
  # 包装接口供可靠有序 DataChannel 获得线程、套接字及 Windows 网络配置，无需在该处重新声明系统库。
  add_library(3rd_usrsctp INTERFACE)
  if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # MinGW GNU ld 按序解析静态库，需重扫描 usrsctp 与 iphlpapi 以保持有效顺序。

    # Windows 的网络实现还需要 IP Helper；MinGW 链接组保持归档与导入库解析顺序。
    target_link_libraries(
      3rd_usrsctp INTERFACE "$<LINK_GROUP:RESCAN,3rd_usrsctp_library,iphlpapi>"
                            Threads::Threads ws2_32)
    # 用户态 SCTP 数据传输在此配置使用另一组发布文件名或系统依赖，仍限定本库的作用域。
  else()
    target_link_libraries(3rd_usrsctp INTERFACE 3rd_usrsctp_library
                                                Threads::Threads)
    # Windows 最终链接需闭合可靠有序 DataChannel 的本机依赖，具体接口只在这个平台传播。
    if(WIN32)
      target_link_libraries(3rd_usrsctp INTERFACE ws2_32 iphlpapi)
    endif()
  endif()
endif()

# 可靠有序 DataChannel 使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(usrsctp_FOUND TRUE)
