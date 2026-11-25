# 导入 libdatachannel 预编译静态库，并显式恢复其静态传递依赖。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 可靠通道及 WebSocket 信令按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 协作只使用固定构建的 DataChannel 和信令接口，依赖启动之前补全。
prebuilt_include_dir(_libdatachannel_include_dir libdatachannel)

# 统一静态传输库与 TLS、地址协商和 SCTP 的链接闭包，避免最终应用逐个猜依赖。
if(NOT TARGET 3rd_mbedtls)
  # 先完成 mbedtls 的依赖解析，使可靠通道及 WebSocket 信令的缺失在配置阶段可见。
  find_package(mbedtls REQUIRED)
endif()
# 协作模块消费单个静态传输包装目标，三个协议依赖由下层显式传递。
if(NOT TARGET 3rd_libjuice)
  # 先完成 libjuice 的依赖解析，使可靠通道及 WebSocket 信令的缺失在配置阶段可见。
  find_package(libjuice REQUIRED)
endif()
if(NOT TARGET 3rd_usrsctp)
  # 先完成 usrsctp 的依赖解析，使可靠通道及 WebSocket 信令的缺失在配置阶段可见。
  find_package(usrsctp REQUIRED)
endif()

if(NOT TARGET 3rd_libdatachannel_library)
  # 将可靠通道及 WebSocket 信令作为全局导入发布，兄弟目录使用同一选择。
  add_library(3rd_libdatachannel_library UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取与地址协商和 TLS 匹配的传输公共头，不必手写额外包含目录。
  set_target_properties(
    3rd_libdatachannel_library
    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_libdatachannel_include_dir}"
               INTERFACE_COMPILE_DEFINITIONS
               "RTC_STATIC;RTC_ENABLE_WEBSOCKET=1;RTC_ENABLE_MEDIA=0")

  # 根据生成器启用的配置验证固定编译的静态传输库，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_libdatachannel_configs)
  # 记录实际完成解析的可靠通道及 WebSocket 信令配置，以便给缺失产物报错。
  set(_libdatachannel_imported_configs "")
  # 协作会话可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_libdatachannel_default_library "")
  # 可靠通道及 WebSocket 信令每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_libdatachannel_config IN LISTS _libdatachannel_configs)
    # 3rd_libdatachannel_library 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_libdatachannel_config}" _libdatachannel_config_upper)
    # 只在已选工具链和配置目录尝试固定编译的静态传输库，不会命中系统安装。
    prebuilt_find_library(
      _libdatachannel_library libdatachannel "${_libdatachannel_config}"
      datachannel-static libdatachannel-static)
    # 处理成功才登记 3rd_libdatachannel_library 当前配置，未知产物不产生空导入条目。
    list(APPEND _libdatachannel_imported_configs
         "${_libdatachannel_config_upper}")
    # 记录 3rd_libdatachannel_library 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_libdatachannel_library
      PROPERTIES "IMPORTED_LOCATION_${_libdatachannel_config_upper}"
                 "${_libdatachannel_library}")
    # 3rd_libdatachannel_library 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_libdatachannel_default_library STREQUAL "")
      set(_libdatachannel_default_library "${_libdatachannel_library}")
    endif()
  endforeach()
  # 将 3rd_libdatachannel_library 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_libdatachannel_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_libdatachannel_imported_configs}"
               IMPORTED_LOCATION "${_libdatachannel_default_library}"
               INTERFACE_LINK_LIBRARIES "3rd_usrsctp;3rd_libjuice;3rd_mbedtls")
endif()

if(NOT TARGET 3rd_libdatachannel)
  # 先完成 Threads 的依赖解析，使可靠通道及 WebSocket 信令的缺失在配置阶段可见。
  find_package(Threads REQUIRED)
  # 包装接口供协作会话获得 TLS、地址协商、SCTP 和线程依赖，无需在该处重新声明系统库。
  add_library(3rd_libdatachannel INTERFACE)
  # 包装接口绑定 mbedTLS、libjuice、usrsctp 和线程目标；Windows 套接字符号另补。
  target_link_libraries(3rd_libdatachannel INTERFACE 3rd_libdatachannel_library
                                                     Threads::Threads)
  # Windows 最终链接需闭合协作会话的本机依赖，具体接口只在这个平台传播。
  if(WIN32)
    target_link_libraries(3rd_libdatachannel INTERFACE ws2_32)
  endif()
endif()

# 协作会话使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(libdatachannel_FOUND TRUE)
