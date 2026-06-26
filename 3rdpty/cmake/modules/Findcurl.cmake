# 导入 libcurl 预编译库，并提供项目包装目标：3rd_curl。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_curl_include_dir curl)

# MinGW 静态 curl 的 IDN2 附加依赖必须使用静态 archive；误用 .dll.a 会让全静态目标继续依赖 MSYS DLL。
function(_curl_find_mingw_static_dependency out_var)
  get_filename_component(_curl_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_curl_toolchain_root "${_curl_compiler_dir}"
                         DIRECTORY)
  set(_curl_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  find_library(
    _curl_static_dependency
    NAMES ${ARGN}
    PATHS "${_curl_toolchain_root}/lib"
    NO_DEFAULT_PATH
    NO_CACHE)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_curl_saved_suffixes})

  if(NOT _curl_static_dependency)
    message(FATAL_ERROR "缺少 curl 的 MinGW 静态依赖：${ARGN}")
  endif()

  get_filename_component(_curl_static_dependency_name
                         "${_curl_static_dependency}" NAME)
  if(_curl_static_dependency_name MATCHES "\\.dll\\.a$")
    message(
      FATAL_ERROR
        "curl 静态链接禁止使用 DLL import library：${_curl_static_dependency}")
  endif()

  set(${out_var}
      "${_curl_static_dependency}"
      PARENT_SCOPE)
endfunction()

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
  if(PROJECT_LINKAGE STREQUAL "static")
    target_compile_definitions(3rd_curl INTERFACE CURL_STATICLIB)
  endif()
  if(WIN32)
    target_link_libraries(3rd_curl INTERFACE ws2_32 crypt32 bcrypt iphlpapi)
    if(MINGW AND PROJECT_LINKAGE STREQUAL "static")
      # MSYS2 预编译 curl 可能启用了 IDN2；这里只允许链接静态 archive，避免 exe 引入 libidn2-0.dll 等运行时依赖。
      _curl_find_mingw_static_dependency(_curl_idn2_library idn2 libidn2)
      _curl_find_mingw_static_dependency(_curl_unistring_library unistring
                                         libunistring)
      _curl_find_mingw_static_dependency(_curl_iconv_library iconv libiconv)
      target_link_libraries(
        3rd_curl
        INTERFACE "${_curl_idn2_library}" "${_curl_unistring_library}"
                  "${_curl_iconv_library}")
    endif()
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
