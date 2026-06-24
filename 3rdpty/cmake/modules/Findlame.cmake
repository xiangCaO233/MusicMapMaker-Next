# 导入 LAME 预编译库，导出源码构建同名目标：3rd_lame。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_lame_include_dir lame)

if(NOT TARGET 3rd_lame)
  add_library(3rd_lame UNKNOWN IMPORTED GLOBAL)
  set_target_properties(3rd_lame PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_lame_include_dir}")

  prebuilt_target_configs(_lame_configs)
  set(_lame_imported_configs "")
  set(_lame_default_library "")
  foreach(_lame_config IN LISTS _lame_configs)
    string(TOUPPER "${_lame_config}" _lame_config_upper)
    prebuilt_find_library(_lame_library lame "${_lame_config}" mp3lame
                          libmp3lame libmp3lame-static)
    list(APPEND _lame_imported_configs "${_lame_config_upper}")
    set_target_properties(
      3rd_lame PROPERTIES "IMPORTED_LOCATION_${_lame_config_upper}"
                          "${_lame_library}")
    if(_lame_default_library STREQUAL "")
      set(_lame_default_library "${_lame_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(3rd_lame PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_lame_default_library}")
  endif()
  set_target_properties(
    3rd_lame PROPERTIES IMPORTED_CONFIGURATIONS "${_lame_imported_configs}"
                        IMPORTED_LOCATION "${_lame_default_library}")
endif()

set(lame_FOUND TRUE)
