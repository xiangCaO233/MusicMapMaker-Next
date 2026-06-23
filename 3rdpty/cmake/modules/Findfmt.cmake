# 导入 fmt 预编译库，导出源码构建同名目标：fmt::fmt。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_fmt_include_dir fmt)

if(NOT TARGET fmt::fmt)
  add_library(fmt::fmt UNKNOWN IMPORTED GLOBAL)
  set_target_properties(fmt::fmt PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_fmt_include_dir}")

  prebuilt_target_configs(_fmt_configs)
  set(_fmt_imported_configs "")
  set(_fmt_default_library "")
  foreach(_fmt_config IN LISTS _fmt_configs)
    string(TOUPPER "${_fmt_config}" _fmt_config_upper)
    prebuilt_find_library(
      _fmt_library
      fmt
      "${_fmt_config}"
      fmt
      fmtd
      libfmt
      libfmtd)
    list(APPEND _fmt_imported_configs "${_fmt_config_upper}")
    set_target_properties(
      fmt::fmt PROPERTIES "IMPORTED_LOCATION_${_fmt_config_upper}"
                          "${_fmt_library}")
    if(_fmt_default_library STREQUAL "")
      set(_fmt_default_library "${_fmt_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(fmt::fmt PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_fmt_default_library}")
  endif()
  set_target_properties(
    fmt::fmt PROPERTIES IMPORTED_CONFIGURATIONS "${_fmt_imported_configs}"
                        IMPORTED_LOCATION "${_fmt_default_library}")
endif()

set(fmt_FOUND TRUE)
