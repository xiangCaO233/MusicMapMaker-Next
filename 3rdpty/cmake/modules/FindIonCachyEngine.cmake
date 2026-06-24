# 导入 IonCachyEngine 预编译库，导出源码构建同名目标：ICE::IonCachyEngine。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
find_package(SDL3Static REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(OpenAL REQUIRED)
find_package(ffmpeg REQUIRED)
find_package(rubberband REQUIRED)
prebuilt_include_dir(_ice_include_dir IonCachyEngine)

if(NOT TARGET ICE::IonCachyEngine)
  add_library(ICE::IonCachyEngine UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    ICE::IonCachyEngine PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                   "${_ice_include_dir}")

  prebuilt_target_configs(_ice_configs)
  set(_ice_imported_configs "")
  set(_ice_default_library "")
  foreach(_ice_config IN LISTS _ice_configs)
    string(TOUPPER "${_ice_config}" _ice_config_upper)
    prebuilt_find_library(
      _ice_library
      IonCachyEngine
      "${_ice_config}"
      IonCachyEngine-static
      IonCachyEngine
      libIonCachyEngine-static
      libIonCachyEngine)
    list(APPEND _ice_imported_configs "${_ice_config_upper}")
    set_target_properties(
      ICE::IonCachyEngine PROPERTIES "IMPORTED_LOCATION_${_ice_config_upper}"
                                     "${_ice_library}")
    if(_ice_default_library STREQUAL "")
      set(_ice_default_library "${_ice_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      ICE::IonCachyEngine PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                     "${_ice_default_library}")
  endif()
  set_target_properties(
    ICE::IonCachyEngine
    PROPERTIES IMPORTED_CONFIGURATIONS "${_ice_imported_configs}"
               IMPORTED_LOCATION "${_ice_default_library}")
  target_link_libraries(
    ICE::IonCachyEngine INTERFACE 3rd_sdl3 fmt::fmt spdlog::spdlog
                                  OpenAL::OpenAL 3rd_ffmpeg 3rd_rubberband)
endif()

set(IonCachyEngine_FOUND TRUE)
