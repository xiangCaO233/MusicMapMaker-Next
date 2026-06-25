# 导入 nativefiledialog-extended 预编译库，并提供项目包装目标：3rd_nfd。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_nfd_include_dir nativefiledialog-extended)

if(NOT TARGET nfd::nfd)
  add_library(nfd::nfd UNKNOWN IMPORTED GLOBAL)
  set_target_properties(nfd::nfd PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_nfd_include_dir}")

  prebuilt_target_configs(_nfd_configs)
  set(_nfd_imported_configs "")
  set(_nfd_default_library "")
  foreach(_nfd_config IN LISTS _nfd_configs)
    string(TOUPPER "${_nfd_config}" _nfd_config_upper)
    prebuilt_find_library(_nfd_library nativefiledialog-extended
                          "${_nfd_config}" nfd nativefiledialog-extended)
    list(APPEND _nfd_imported_configs "${_nfd_config_upper}")
    set_target_properties(
      nfd::nfd PROPERTIES "IMPORTED_LOCATION_${_nfd_config_upper}"
                          "${_nfd_library}")
    if(_nfd_default_library STREQUAL "")
      set(_nfd_default_library "${_nfd_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(nfd::nfd PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_nfd_default_library}")
  endif()
  set_target_properties(
    nfd::nfd PROPERTIES IMPORTED_CONFIGURATIONS "${_nfd_imported_configs}"
                        IMPORTED_LOCATION "${_nfd_default_library}")
endif()

if(NOT TARGET 3rd_nfd)
  add_library(3rd_nfd INTERFACE)
  target_link_libraries(3rd_nfd INTERFACE nfd::nfd)
  if(UNIX AND NOT APPLE)
    # Linux 版 NFD 静态库使用 GTK/GDK 后端；源码构建时这些依赖来自 nfd::nfd，预编译导入目标需要显式恢复。
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(
      NFD_GTK
      REQUIRED
      IMPORTED_TARGET
      gtk+-3.0
      gdk-x11-3.0
      gdk-wayland-3.0
      wayland-client)
    target_link_libraries(3rd_nfd INTERFACE PkgConfig::NFD_GTK)
  endif()
endif()

set(nfd_FOUND TRUE)
