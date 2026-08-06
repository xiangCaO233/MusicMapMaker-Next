# 导入 usrsctp 预编译静态库，并导出源码构建同名包装目标：3rd_usrsctp。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_usrsctp_include_dir usrsctp)

if(NOT TARGET 3rd_usrsctp_library)
  add_library(3rd_usrsctp_library UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    3rd_usrsctp_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                   "${_usrsctp_include_dir}")

  prebuilt_target_configs(_usrsctp_configs)
  set(_usrsctp_imported_configs "")
  set(_usrsctp_default_library "")
  foreach(_usrsctp_config IN LISTS _usrsctp_configs)
    string(TOUPPER "${_usrsctp_config}" _usrsctp_config_upper)
    prebuilt_find_library(_usrsctp_library usrsctp "${_usrsctp_config}" usrsctp)
    list(APPEND _usrsctp_imported_configs "${_usrsctp_config_upper}")
    set_target_properties(
      3rd_usrsctp_library
      PROPERTIES "IMPORTED_LOCATION_${_usrsctp_config_upper}"
                 "${_usrsctp_library}")
    if(_usrsctp_default_library STREQUAL "")
      set(_usrsctp_default_library "${_usrsctp_library}")
    endif()
  endforeach()
  set_target_properties(
    3rd_usrsctp_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_usrsctp_imported_configs}"
               IMPORTED_LOCATION "${_usrsctp_default_library}")
endif()

if(NOT TARGET 3rd_usrsctp)
  find_package(Threads REQUIRED)
  add_library(3rd_usrsctp INTERFACE)
  target_link_libraries(3rd_usrsctp INTERFACE 3rd_usrsctp_library
                                              Threads::Threads)
  if(WIN32)
    target_link_libraries(3rd_usrsctp INTERFACE ws2_32 iphlpapi)
  endif()
endif()

set(usrsctp_FOUND TRUE)
