# 导入 lunasvg 预编译库，并提供项目包装目标：3rd_lunasvg。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
find_package(freetype REQUIRED)
prebuilt_include_dir(_lunasvg_include_dir lunasvg)

if(NOT TARGET plutovg::plutovg)
  add_library(plutovg::plutovg UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    plutovg::plutovg PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                "${_lunasvg_include_dir}")

  prebuilt_target_configs(_plutovg_configs)
  set(_plutovg_imported_configs "")
  set(_plutovg_default_library "")
  foreach(_plutovg_config IN LISTS _plutovg_configs)
    string(TOUPPER "${_plutovg_config}" _plutovg_config_upper)
    prebuilt_find_library(_plutovg_library lunasvg "${_plutovg_config}" plutovg
                          libplutovg)
    list(APPEND _plutovg_imported_configs "${_plutovg_config_upper}")
    set_target_properties(
      plutovg::plutovg PROPERTIES "IMPORTED_LOCATION_${_plutovg_config_upper}"
                                  "${_plutovg_library}")
    if(_plutovg_default_library STREQUAL "")
      set(_plutovg_default_library "${_plutovg_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      plutovg::plutovg PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                  "${_plutovg_default_library}")
  endif()
  set_target_properties(
    plutovg::plutovg
    PROPERTIES IMPORTED_CONFIGURATIONS "${_plutovg_imported_configs}"
               IMPORTED_LOCATION "${_plutovg_default_library}")
  target_compile_definitions(plutovg::plutovg INTERFACE PLUTOVG_BUILD_STATIC)
endif()

if(NOT TARGET lunasvg::lunasvg)
  add_library(lunasvg::lunasvg UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    lunasvg::lunasvg PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                "${_lunasvg_include_dir}")

  prebuilt_target_configs(_lunasvg_configs)
  set(_lunasvg_imported_configs "")
  set(_lunasvg_default_library "")
  foreach(_lunasvg_config IN LISTS _lunasvg_configs)
    string(TOUPPER "${_lunasvg_config}" _lunasvg_config_upper)
    prebuilt_find_library(_lunasvg_library lunasvg "${_lunasvg_config}" lunasvg
                          liblunasvg)
    list(APPEND _lunasvg_imported_configs "${_lunasvg_config_upper}")
    set_target_properties(
      lunasvg::lunasvg PROPERTIES "IMPORTED_LOCATION_${_lunasvg_config_upper}"
                                  "${_lunasvg_library}")
    if(_lunasvg_default_library STREQUAL "")
      set(_lunasvg_default_library "${_lunasvg_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      lunasvg::lunasvg PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                  "${_lunasvg_default_library}")
  endif()
  set_target_properties(
    lunasvg::lunasvg
    PROPERTIES IMPORTED_CONFIGURATIONS "${_lunasvg_imported_configs}"
               IMPORTED_LOCATION "${_lunasvg_default_library}")
  target_compile_definitions(lunasvg::lunasvg INTERFACE LUNASVG_BUILD_STATIC
                                                        PLUTOVG_BUILD_STATIC)
  target_link_libraries(lunasvg::lunasvg INTERFACE plutovg::plutovg
                                                   freetype::freetype)
endif()

if(NOT TARGET 3rd_lunasvg)
  add_library(3rd_lunasvg INTERFACE)
  target_link_libraries(3rd_lunasvg INTERFACE lunasvg::lunasvg)
endif()

set(lunasvg_FOUND TRUE)
