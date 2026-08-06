# 导入 libjuice 预编译静态库，并导出源码构建同名包装目标：3rd_libjuice。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_libjuice_include_dir libjuice)

if(NOT TARGET 3rd_libjuice_library)
  add_library(3rd_libjuice_library UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    3rd_libjuice_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                    "${_libjuice_include_dir}")

  prebuilt_target_configs(_libjuice_configs)
  set(_libjuice_imported_configs "")
  set(_libjuice_default_library "")
  foreach(_libjuice_config IN LISTS _libjuice_configs)
    string(TOUPPER "${_libjuice_config}" _libjuice_config_upper)
    prebuilt_find_library(_libjuice_library libjuice "${_libjuice_config}"
                          juice-static juice)
    list(APPEND _libjuice_imported_configs "${_libjuice_config_upper}")
    set_target_properties(
      3rd_libjuice_library
      PROPERTIES "IMPORTED_LOCATION_${_libjuice_config_upper}"
                 "${_libjuice_library}")
    if(_libjuice_default_library STREQUAL "")
      set(_libjuice_default_library "${_libjuice_library}")
    endif()
  endforeach()
  set_target_properties(
    3rd_libjuice_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_libjuice_imported_configs}"
               IMPORTED_LOCATION "${_libjuice_default_library}")
endif()

if(NOT TARGET 3rd_libjuice)
  find_package(Threads REQUIRED)
  add_library(3rd_libjuice INTERFACE)
  target_link_libraries(3rd_libjuice INTERFACE 3rd_libjuice_library
                                               Threads::Threads)
  if(WIN32)
    target_link_libraries(3rd_libjuice INTERFACE ws2_32 bcrypt)
  endif()
endif()

set(libjuice_FOUND TRUE)
