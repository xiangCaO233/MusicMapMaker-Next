# 导入 ImPlot 预编译库，导出源码构建同名目标：3rd_implot。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
find_package(imgui REQUIRED)
prebuilt_include_dir(_implot_include_dir implot)

if(NOT TARGET 3rd_implot)
  add_library(3rd_implot UNKNOWN IMPORTED GLOBAL)
  set_target_properties(3rd_implot PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_implot_include_dir}")

  prebuilt_target_configs(_implot_configs)
  set(_implot_imported_configs "")
  set(_implot_default_library "")
  foreach(_implot_config IN LISTS _implot_configs)
    string(TOUPPER "${_implot_config}" _implot_config_upper)
    prebuilt_find_library(_implot_library implot "${_implot_config}" implot
                          3rd_implot)
    list(APPEND _implot_imported_configs "${_implot_config_upper}")
    set_target_properties(
      3rd_implot PROPERTIES "IMPORTED_LOCATION_${_implot_config_upper}"
                            "${_implot_library}")
    if(_implot_default_library STREQUAL "")
      set(_implot_default_library "${_implot_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(3rd_implot PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                "${_implot_default_library}")
  endif()
  set_target_properties(
    3rd_implot PROPERTIES IMPORTED_CONFIGURATIONS "${_implot_imported_configs}"
                          IMPORTED_LOCATION "${_implot_default_library}")
  target_link_libraries(3rd_implot INTERFACE 3rd_imgui)
endif()

set(implot_FOUND TRUE)
