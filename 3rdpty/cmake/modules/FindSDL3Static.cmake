# 导入 SDL3 预编译库，并提供项目包装目标：3rd_sdl3。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_sdl_include_dir sdl)

if(NOT TARGET SDL3-static)
  add_library(SDL3-static UNKNOWN IMPORTED GLOBAL)
  set_target_properties(SDL3-static PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                               "${_sdl_include_dir}")

  prebuilt_target_configs(_sdl_configs)
  set(_sdl_imported_configs "")
  set(_sdl_default_library "")
  foreach(_sdl_config IN LISTS _sdl_configs)
    string(TOUPPER "${_sdl_config}" _sdl_config_upper)
    if(PROJECT_LINKAGE STREQUAL "shared")
      set(_sdl_library_names SDL3 libSDL3 SDL3-static)
    else()
      set(_sdl_library_names SDL3-static SDL3 libSDL3)
    endif()
    prebuilt_find_library(_sdl_library sdl "${_sdl_config}"
                          ${_sdl_library_names})
    list(APPEND _sdl_imported_configs "${_sdl_config_upper}")
    set_target_properties(
      SDL3-static PROPERTIES "IMPORTED_LOCATION_${_sdl_config_upper}"
                             "${_sdl_library}")
    if(_sdl_default_library STREQUAL "")
      set(_sdl_default_library "${_sdl_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(SDL3-static PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                 "${_sdl_default_library}")
  endif()
  set_target_properties(
    SDL3-static PROPERTIES IMPORTED_CONFIGURATIONS "${_sdl_imported_configs}"
                           IMPORTED_LOCATION "${_sdl_default_library}")
endif()

if(NOT TARGET 3rd_sdl3)
  add_library(3rd_sdl3 INTERFACE)
  target_link_libraries(3rd_sdl3 INTERFACE SDL3-static)
endif()

set(SDL3Static_FOUND TRUE)
