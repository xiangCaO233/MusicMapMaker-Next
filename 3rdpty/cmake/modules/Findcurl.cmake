# 导入 libcurl 预编译库，并提供项目包装目标：3rd_curl。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# HTTP 请求、套接字和 TLS 按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 使用归档配套公共头；TLS 后端及系统网络依赖在目标接口另行补全。
prebuilt_include_dir(_curl_include_dir curl)

# 归档只含 curl 自有对象，平台分支负责把各传输后端依赖带到最终链接。
if(NOT TARGET libcurl_static)
  # 将 HTTP 请求、套接字和 TLS 作为全局导入发布，兄弟目录使用同一选择。
  add_library(libcurl_static UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取平台 CURL_STATICLIB 兼容的 curl 声明，不必手写额外包含目录。
  set_target_properties(libcurl_static PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_curl_include_dir}")

  # 根据生成器启用的配置验证固定构建的 curl 传输库，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_curl_configs)
  # 记录实际完成解析的 HTTP 请求、套接字和 TLS 配置，以便给缺失产物报错。
  set(_curl_imported_configs "")
  # 更新下载及资源同步可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_curl_default_library "")
  # HTTP 请求、套接字和 TLS 每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_curl_config IN LISTS _curl_configs)
    # libcurl_static 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_curl_config}" _curl_config_upper)
    # 只在已选工具链和配置目录尝试固定构建的 curl 传输库，不会命中系统安装。
    prebuilt_find_library(_curl_library curl "${_curl_config}" curl libcurl
                          libcurl_static)
    # 处理成功才登记 libcurl_static 当前配置，未知产物不产生空导入条目。
    list(APPEND _curl_imported_configs "${_curl_config_upper}")
    # 记录 libcurl_static 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      libcurl_static PROPERTIES "IMPORTED_LOCATION_${_curl_config_upper}"
                                "${_curl_library}")
    # libcurl_static 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_curl_default_library STREQUAL "")
      set(_curl_default_library "${_curl_library}")
    endif()
  endforeach()
  # 如果更新下载及资源同步走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 libcurl_static 缺省链接，已指定配置不受它影响。
    set_target_properties(libcurl_static PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                    "${_curl_default_library}")
  endif()
  # 将 libcurl_static 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    libcurl_static
    PROPERTIES IMPORTED_CONFIGURATIONS "${_curl_imported_configs}"
               IMPORTED_LOCATION "${_curl_default_library}")
endif()

# 包装接口集中恢复传输实现所需的系统依赖，不同业务下载入口无需重复接线。
if(NOT TARGET 3rd_curl)
  # 包装接口供更新下载及资源同步获得 WinSock、系统框架或 OpenSSL，无需在该处重新声明系统库。
  add_library(3rd_curl INTERFACE)
  if(PROJECT_LINKAGE STREQUAL "static")
    target_compile_definitions(3rd_curl INTERFACE CURL_STATICLIB)
  endif()
  # Windows 最终链接需闭合更新下载及资源同步的本机依赖，具体接口只在这个平台传播。
  if(WIN32)
    # 当前预编译 curl 静态包禁用 IDN2/PSL/SSH/HTTP2/压缩后端； 这里只保留 Windows 系统库，避免把 MSYS2
    # 附加库塞进全静态 exe。
    if(MINGW)
      # MinGW 的 GNU ld 单向扫描静态归档，而 ws2_32 等依赖也会被其他目标引用并提前去重；使用 RESCAN
      # 保证系统导入库始终能解析 libcurl 后产生的 Winsock 与 BCrypt 符号。

      # Windows 静态 curl 要显式携带 Winsock 和加密依赖；MinGW 重复扫描保证符号解析顺序。
      target_link_libraries(
        3rd_curl
        INTERFACE
          "$<LINK_GROUP:RESCAN,libcurl_static,ws2_32,crypt32,bcrypt,iphlpapi>")
      # HTTP 请求、套接字和 TLS 在此配置使用另一组发布文件名或系统依赖，仍限定本库的作用域。
    else()
      target_link_libraries(3rd_curl INTERFACE libcurl_static ws2_32 crypt32
                                               bcrypt iphlpapi)
    endif()
  elseif(APPLE)
    target_link_libraries(
      3rd_curl
      INTERFACE libcurl_static "-framework CoreFoundation"
                "-framework SystemConfiguration" "-framework Security"
                "-framework CoreServices")
  else()
    # 先完成 OpenSSL 的依赖解析，使 HTTP 请求、套接字和 TLS 的缺失在配置阶段可见。
    find_package(OpenSSL REQUIRED)
    target_link_libraries(3rd_curl INTERFACE libcurl_static OpenSSL::SSL
                                             OpenSSL::Crypto)
  endif()
endif()

# 更新下载及资源同步使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(curl_FOUND TRUE)
