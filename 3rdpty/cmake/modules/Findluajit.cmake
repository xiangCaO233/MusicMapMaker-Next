# 导入 LuaJIT 预编译库，导出源码构建同名目标：luajit::luajit。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_luajit_include_dir luajit)

if(NOT TARGET luajit::luajit)
  add_library(luajit::luajit UNKNOWN IMPORTED GLOBAL)
  set_target_properties(luajit::luajit PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_luajit_include_dir}")

  prebuilt_target_configs(_luajit_configs)
  set(_luajit_imported_configs "")
  set(_luajit_default_library "")
  foreach(_luajit_config IN LISTS _luajit_configs)
    string(TOUPPER "${_luajit_config}" _luajit_config_upper)
    prebuilt_find_library(_luajit_library luajit "${_luajit_config}" luajit
                          libluajit lua51)
    list(APPEND _luajit_imported_configs "${_luajit_config_upper}")
    set_target_properties(
      luajit::luajit PROPERTIES "IMPORTED_LOCATION_${_luajit_config_upper}"
                                "${_luajit_library}")
    if(_luajit_default_library STREQUAL "")
      set(_luajit_default_library "${_luajit_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      luajit::luajit PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_luajit_default_library}")
  endif()
  set_target_properties(
    luajit::luajit
    PROPERTIES IMPORTED_CONFIGURATIONS "${_luajit_imported_configs}"
               IMPORTED_LOCATION "${_luajit_default_library}")

  if(UNIX)
    target_link_libraries(luajit::luajit INTERFACE m dl)
    if(NOT APPLE)
      target_link_libraries(luajit::luajit INTERFACE pthread)
    endif()
  endif()
  if(WIN32)
    target_link_libraries(luajit::luajit INTERFACE winmm)
  endif()
endif()

set(luajit_FOUND TRUE)
