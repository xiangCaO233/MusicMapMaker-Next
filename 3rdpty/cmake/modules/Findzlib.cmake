# 导入 zlib 预编译库，导出源码构建同名目标：3rd_zlib。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_zlib_include_dir zlib)

if(NOT TARGET 3rd_zlib)
  add_library(3rd_zlib UNKNOWN IMPORTED GLOBAL)
  set_target_properties(3rd_zlib PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_zlib_include_dir}")

  prebuilt_target_configs(_zlib_configs)
  set(_zlib_imported_configs "")
  set(_zlib_default_library "")
  foreach(_zlib_config IN LISTS _zlib_configs)
    string(TOUPPER "${_zlib_config}" _zlib_config_upper)
    prebuilt_find_library(
      _zlib_library
      zlib
      "${_zlib_config}"
      z
      libz
      zlib
      libzs
      libzsd)
    list(APPEND _zlib_imported_configs "${_zlib_config_upper}")
    set_target_properties(
      3rd_zlib PROPERTIES "IMPORTED_LOCATION_${_zlib_config_upper}"
                          "${_zlib_library}")
    if(_zlib_default_library STREQUAL "")
      set(_zlib_default_library "${_zlib_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(3rd_zlib PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_zlib_default_library}")
  endif()
  set_target_properties(
    3rd_zlib PROPERTIES IMPORTED_CONFIGURATIONS "${_zlib_imported_configs}"
                        IMPORTED_LOCATION "${_zlib_default_library}")
endif()

set(zlib_FOUND TRUE)
