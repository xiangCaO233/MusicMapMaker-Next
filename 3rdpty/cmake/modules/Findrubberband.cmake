# 导入 Rubber Band 预编译库，导出源码构建同名目标：3rd_rubberband。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
find_package(fftw REQUIRED)
find_package(libsamplerate REQUIRED)
prebuilt_include_dir(_rubberband_include_dir rubberband)

if(NOT TARGET 3rd_rubberband)
  add_library(3rd_rubberband UNKNOWN IMPORTED GLOBAL)
  set_target_properties(3rd_rubberband PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_rubberband_include_dir}")

  prebuilt_target_configs(_rubberband_configs)
  set(_rubberband_imported_configs "")
  set(_rubberband_default_library "")
  foreach(_rubberband_config IN LISTS _rubberband_configs)
    string(TOUPPER "${_rubberband_config}" _rubberband_config_upper)
    prebuilt_find_library(
      _rubberband_library
      rubberband
      "${_rubberband_config}"
      rubberband
      rubberband-static
      librubberband
      librubberband-static)
    list(APPEND _rubberband_imported_configs "${_rubberband_config_upper}")
    set_target_properties(
      3rd_rubberband PROPERTIES "IMPORTED_LOCATION_${_rubberband_config_upper}"
                                "${_rubberband_library}")
    if(_rubberband_default_library STREQUAL "")
      set(_rubberband_default_library "${_rubberband_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      3rd_rubberband PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_rubberband_default_library}")
  endif()
  set_target_properties(
    3rd_rubberband
    PROPERTIES IMPORTED_CONFIGURATIONS "${_rubberband_imported_configs}"
               IMPORTED_LOCATION "${_rubberband_default_library}")
  target_link_libraries(3rd_rubberband INTERFACE 3rd_fftw3 3rd_libsamplerate)
  if(WIN32)
    if(NOT MSVC)
      target_link_libraries(3rd_rubberband INTERFACE pthread)
    endif()
  else()
    target_link_libraries(3rd_rubberband INTERFACE m)
    if(NOT APPLE)
      target_link_libraries(3rd_rubberband INTERFACE pthread)
    endif()
  endif()
endif()

set(rubberband_FOUND TRUE)
