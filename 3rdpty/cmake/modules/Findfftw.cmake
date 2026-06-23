# 导入 FFTW 预编译库，并提供项目包装目标：3rd_fftw3。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_fftw_include_dir fftw)

if(NOT TARGET fftw3)
  add_library(fftw3 UNKNOWN IMPORTED GLOBAL)
  set_target_properties(fftw3 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                         "${_fftw_include_dir}")

  prebuilt_target_configs(_fftw_configs)
  set(_fftw_imported_configs "")
  set(_fftw_default_library "")
  foreach(_fftw_config IN LISTS _fftw_configs)
    string(TOUPPER "${_fftw_config}" _fftw_config_upper)
    prebuilt_find_library(_fftw_library fftw "${_fftw_config}" fftw3 libfftw3)
    list(APPEND _fftw_imported_configs "${_fftw_config_upper}")
    set_target_properties(
      fftw3 PROPERTIES "IMPORTED_LOCATION_${_fftw_config_upper}"
                       "${_fftw_library}")
    if(_fftw_default_library STREQUAL "")
      set(_fftw_default_library "${_fftw_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(fftw3 PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                           "${_fftw_default_library}")
  endif()
  set_target_properties(
    fftw3 PROPERTIES IMPORTED_CONFIGURATIONS "${_fftw_imported_configs}"
                     IMPORTED_LOCATION "${_fftw_default_library}")
endif()

if(NOT TARGET 3rd_fftw3)
  add_library(3rd_fftw3 INTERFACE)
  target_link_libraries(3rd_fftw3 INTERFACE fftw3)
endif()

set(fftw_FOUND TRUE)
