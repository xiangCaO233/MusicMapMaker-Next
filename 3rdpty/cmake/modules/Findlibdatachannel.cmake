# 导入 libdatachannel 预编译静态库，并显式恢复其静态传递依赖。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_libdatachannel_include_dir libdatachannel)

if(NOT TARGET 3rd_mbedtls)
  find_package(mbedtls REQUIRED)
endif()
if(NOT TARGET 3rd_libjuice)
  find_package(libjuice REQUIRED)
endif()
if(NOT TARGET 3rd_usrsctp)
  find_package(usrsctp REQUIRED)
endif()

if(NOT TARGET 3rd_libdatachannel_library)
  add_library(3rd_libdatachannel_library UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    3rd_libdatachannel_library
    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_libdatachannel_include_dir}"
               INTERFACE_COMPILE_DEFINITIONS
               "RTC_STATIC;RTC_ENABLE_WEBSOCKET=1;RTC_ENABLE_MEDIA=0")

  prebuilt_target_configs(_libdatachannel_configs)
  set(_libdatachannel_imported_configs "")
  set(_libdatachannel_default_library "")
  foreach(_libdatachannel_config IN LISTS _libdatachannel_configs)
    string(TOUPPER "${_libdatachannel_config}" _libdatachannel_config_upper)
    prebuilt_find_library(
      _libdatachannel_library libdatachannel "${_libdatachannel_config}"
      datachannel-static libdatachannel-static)
    list(APPEND _libdatachannel_imported_configs
         "${_libdatachannel_config_upper}")
    set_target_properties(
      3rd_libdatachannel_library
      PROPERTIES "IMPORTED_LOCATION_${_libdatachannel_config_upper}"
                 "${_libdatachannel_library}")
    if(_libdatachannel_default_library STREQUAL "")
      set(_libdatachannel_default_library "${_libdatachannel_library}")
    endif()
  endforeach()
  set_target_properties(
    3rd_libdatachannel_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_libdatachannel_imported_configs}"
               IMPORTED_LOCATION "${_libdatachannel_default_library}"
               INTERFACE_LINK_LIBRARIES "3rd_usrsctp;3rd_libjuice;3rd_mbedtls")
endif()

if(NOT TARGET 3rd_libdatachannel)
  find_package(Threads REQUIRED)
  add_library(3rd_libdatachannel INTERFACE)
  target_link_libraries(3rd_libdatachannel INTERFACE 3rd_libdatachannel_library
                                                     Threads::Threads)
  if(WIN32)
    target_link_libraries(3rd_libdatachannel INTERFACE ws2_32)
  endif()
endif()

set(libdatachannel_FOUND TRUE)
