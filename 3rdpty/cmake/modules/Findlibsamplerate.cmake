# 导入 libsamplerate 预编译库，并提供项目包装目标：3rd_libsamplerate。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_samplerate_include_dir libsamplerate)

if(NOT TARGET samplerate)
  add_library(samplerate UNKNOWN IMPORTED GLOBAL)
  set_target_properties(samplerate PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_samplerate_include_dir}")

  prebuilt_target_configs(_samplerate_configs)
  set(_samplerate_imported_configs "")
  set(_samplerate_default_library "")
  foreach(_samplerate_config IN LISTS _samplerate_configs)
    string(TOUPPER "${_samplerate_config}" _samplerate_config_upper)
    prebuilt_find_library(_samplerate_library libsamplerate
                          "${_samplerate_config}" samplerate libsamplerate)
    list(APPEND _samplerate_imported_configs "${_samplerate_config_upper}")
    set_target_properties(
      samplerate PROPERTIES "IMPORTED_LOCATION_${_samplerate_config_upper}"
                            "${_samplerate_library}")
    if(_samplerate_default_library STREQUAL "")
      set(_samplerate_default_library "${_samplerate_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      samplerate PROPERTIES IMPORTED_LOCATION_NOCONFIG
                            "${_samplerate_default_library}")
  endif()
  set_target_properties(
    samplerate
    PROPERTIES IMPORTED_CONFIGURATIONS "${_samplerate_imported_configs}"
               IMPORTED_LOCATION "${_samplerate_default_library}")
endif()

if(NOT TARGET 3rd_libsamplerate)
  add_library(3rd_libsamplerate INTERFACE)
  target_link_libraries(3rd_libsamplerate INTERFACE samplerate)
endif()

set(libsamplerate_FOUND TRUE)
