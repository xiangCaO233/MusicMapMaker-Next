# 导入 libcurl 预编译库，并提供项目包装目标：3rd_curl。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_curl_include_dir curl)

if(NOT TARGET libcurl_static)
  add_library(libcurl_static UNKNOWN IMPORTED GLOBAL)
  set_target_properties(libcurl_static PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_curl_include_dir}")

  prebuilt_target_configs(_curl_configs)
  set(_curl_imported_configs "")
  set(_curl_default_library "")
  foreach(_curl_config IN LISTS _curl_configs)
    string(TOUPPER "${_curl_config}" _curl_config_upper)
    prebuilt_find_library(_curl_library curl "${_curl_config}" curl libcurl
                          libcurl_static)
    list(APPEND _curl_imported_configs "${_curl_config_upper}")
    set_target_properties(
      libcurl_static PROPERTIES "IMPORTED_LOCATION_${_curl_config_upper}"
                                "${_curl_library}")
    if(_curl_default_library STREQUAL "")
      set(_curl_default_library "${_curl_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(libcurl_static PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                    "${_curl_default_library}")
  endif()
  set_target_properties(
    libcurl_static
    PROPERTIES IMPORTED_CONFIGURATIONS "${_curl_imported_configs}"
               IMPORTED_LOCATION "${_curl_default_library}")
endif()

if(NOT TARGET 3rd_curl)
  add_library(3rd_curl INTERFACE)
  if(PROJECT_LINKAGE STREQUAL "static")
    target_compile_definitions(3rd_curl INTERFACE CURL_STATICLIB)
  endif()
  if(WIN32)
    # 当前预编译 curl 静态包禁用 IDN2/PSL/SSH/HTTP2/压缩后端； 这里只保留 Windows 系统库，避免把 MSYS2
    # 附加库塞进全静态 exe。
    if(MINGW)
      # MinGW 的 GNU ld 单向扫描静态归档，而 ws2_32 等依赖也会被其他目标引用并提前去重；使用 RESCAN
      # 保证系统导入库始终能解析 libcurl 后产生的 Winsock 与 BCrypt 符号。
      target_link_libraries(
        3rd_curl
        INTERFACE
          "$<LINK_GROUP:RESCAN,libcurl_static,ws2_32,crypt32,bcrypt,iphlpapi>")
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
    find_package(OpenSSL REQUIRED)
    target_link_libraries(3rd_curl INTERFACE libcurl_static OpenSSL::SSL
                                             OpenSSL::Crypto)
  endif()
endif()

set(curl_FOUND TRUE)
