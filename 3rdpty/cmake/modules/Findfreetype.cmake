# 导入 freetype 预编译库，并提供项目包装目标：3rd_freetype。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_freetype_include_dir freetype)

if(NOT TARGET freetype::freetype)
  add_library(freetype::freetype UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    freetype::freetype PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                  "${_freetype_include_dir}")

  prebuilt_target_configs(_freetype_configs)
  set(_freetype_imported_configs "")
  set(_freetype_default_library "")
  foreach(_freetype_config IN LISTS _freetype_configs)
    string(TOUPPER "${_freetype_config}" _freetype_config_upper)
    prebuilt_find_library(
      _freetype_library
      freetype
      "${_freetype_config}"
      freetype
      freetyped
      libfreetype
      libfreetyped)
    list(APPEND _freetype_imported_configs "${_freetype_config_upper}")
    set_target_properties(
      freetype::freetype
      PROPERTIES "IMPORTED_LOCATION_${_freetype_config_upper}"
                 "${_freetype_library}")
    if(_freetype_default_library STREQUAL "")
      set(_freetype_default_library "${_freetype_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      freetype::freetype PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                    "${_freetype_default_library}")
  endif()
  set_target_properties(
    freetype::freetype
    PROPERTIES IMPORTED_CONFIGURATIONS "${_freetype_imported_configs}"
               IMPORTED_LOCATION "${_freetype_default_library}")
endif()

if(NOT TARGET 3rd_freetype)
  add_library(3rd_freetype INTERFACE)
  target_link_libraries(3rd_freetype INTERFACE freetype::freetype)
endif()

set(freetype_FOUND TRUE)
