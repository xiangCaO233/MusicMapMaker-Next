# 导入 spdlog 预编译库，导出源码构建同名目标：spdlog::spdlog。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
find_package(fmt REQUIRED)
prebuilt_include_dir(_spdlog_include_dir spdlog)

if(NOT TARGET spdlog::spdlog)
  add_library(spdlog::spdlog UNKNOWN IMPORTED GLOBAL)
  set_target_properties(spdlog::spdlog PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_spdlog_include_dir}")

  prebuilt_target_configs(_spdlog_configs)
  set(_spdlog_imported_configs "")
  set(_spdlog_default_library "")
  foreach(_spdlog_config IN LISTS _spdlog_configs)
    string(TOUPPER "${_spdlog_config}" _spdlog_config_upper)
    prebuilt_find_library(
      _spdlog_library
      spdlog
      "${_spdlog_config}"
      spdlog
      spdlogd
      libspdlog
      libspdlogd)
    list(APPEND _spdlog_imported_configs "${_spdlog_config_upper}")
    set_target_properties(
      spdlog::spdlog PROPERTIES "IMPORTED_LOCATION_${_spdlog_config_upper}"
                                "${_spdlog_library}")
    if(_spdlog_default_library STREQUAL "")
      set(_spdlog_default_library "${_spdlog_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      spdlog::spdlog PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_spdlog_default_library}")
  endif()
  set_target_properties(
    spdlog::spdlog
    PROPERTIES IMPORTED_CONFIGURATIONS "${_spdlog_imported_configs}"
               IMPORTED_LOCATION "${_spdlog_default_library}")
  target_compile_definitions(spdlog::spdlog INTERFACE SPDLOG_FMT_EXTERNAL)
  target_link_libraries(spdlog::spdlog INTERFACE fmt::fmt)
endif()

set(spdlog_FOUND TRUE)
