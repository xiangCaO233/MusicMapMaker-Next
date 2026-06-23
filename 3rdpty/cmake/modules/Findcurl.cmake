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
  target_link_libraries(3rd_curl INTERFACE libcurl_static)
  target_compile_definitions(3rd_curl INTERFACE CURL_STATICLIB)
  if(WIN32)
    target_link_libraries(3rd_curl INTERFACE ws2_32 crypt32 bcrypt iphlpapi)
  elseif(APPLE)
    target_link_libraries(
      3rd_curl INTERFACE "-framework CoreFoundation"
                         "-framework SystemConfiguration" "-framework Security")
  else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(3rd_curl INTERFACE OpenSSL::SSL OpenSSL::Crypto)
  endif()
endif()

set(curl_FOUND TRUE)
