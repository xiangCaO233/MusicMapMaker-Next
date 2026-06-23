# 导入 miniz 预编译库，导出源码构建同名目标：3rd_miniz。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_miniz_include_dir miniz)

if(NOT TARGET 3rd_miniz)
  add_library(3rd_miniz UNKNOWN IMPORTED GLOBAL)
  set_target_properties(3rd_miniz PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                             "${_miniz_include_dir}")

  prebuilt_target_configs(_miniz_configs)
  set(_miniz_imported_configs "")
  set(_miniz_default_library "")
  foreach(_miniz_config IN LISTS _miniz_configs)
    string(TOUPPER "${_miniz_config}" _miniz_config_upper)
    prebuilt_find_library(_miniz_library miniz "${_miniz_config}" miniz
                          3rd_miniz)
    list(APPEND _miniz_imported_configs "${_miniz_config_upper}")
    set_target_properties(
      3rd_miniz PROPERTIES "IMPORTED_LOCATION_${_miniz_config_upper}"
                           "${_miniz_library}")
    if(_miniz_default_library STREQUAL "")
      set(_miniz_default_library "${_miniz_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(3rd_miniz PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                               "${_miniz_default_library}")
  endif()
  set_target_properties(
    3rd_miniz PROPERTIES IMPORTED_CONFIGURATIONS "${_miniz_imported_configs}"
                         IMPORTED_LOCATION "${_miniz_default_library}")
endif()

set(miniz_FOUND TRUE)
