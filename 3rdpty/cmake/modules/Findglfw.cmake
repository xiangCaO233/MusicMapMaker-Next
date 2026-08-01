# 导入 GLFW 预编译库，并提供项目包装目标：3rd_glfw。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_glfw_include_dir glfw)

if(NOT TARGET Vulkan::Vulkan)
  find_package(Vulkan REQUIRED)
endif()

if(NOT TARGET glfw)
  add_library(glfw UNKNOWN IMPORTED GLOBAL)
  set_target_properties(glfw PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                        "${_glfw_include_dir}")

  prebuilt_target_configs(_glfw_configs)
  set(_glfw_imported_configs "")
  set(_glfw_default_library "")
  foreach(_glfw_config IN LISTS _glfw_configs)
    string(TOUPPER "${_glfw_config}" _glfw_config_upper)
    prebuilt_find_library(_glfw_library glfw "${_glfw_config}" glfw glfw3)
    list(APPEND _glfw_imported_configs "${_glfw_config_upper}")
    set_target_properties(
      glfw PROPERTIES "IMPORTED_LOCATION_${_glfw_config_upper}"
                      "${_glfw_library}")
    if(_glfw_default_library STREQUAL "")
      set(_glfw_default_library "${_glfw_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(glfw PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                          "${_glfw_default_library}")
  endif()
  set_target_properties(
    glfw PROPERTIES IMPORTED_CONFIGURATIONS "${_glfw_imported_configs}"
                    IMPORTED_LOCATION "${_glfw_default_library}")
  target_link_libraries(glfw INTERFACE Vulkan::Vulkan)

  if(APPLE)
    # macOS 静态 GLFW 不会把系统框架封装进归档，导入目标必须恢复源码目标的传递依赖。
    find_library(_glfw_cocoa_framework Cocoa REQUIRED)
    find_library(_glfw_iokit_framework IOKit REQUIRED)
    find_library(_glfw_quartz_core_framework QuartzCore REQUIRED)
    find_library(_glfw_core_foundation_framework CoreFoundation REQUIRED)
    target_link_libraries(
      glfw
      INTERFACE "${_glfw_cocoa_framework}" "${_glfw_iokit_framework}"
                "${_glfw_quartz_core_framework}"
                "${_glfw_core_foundation_framework}")
  endif()
endif()

if(NOT TARGET 3rd_glfw)
  add_library(3rd_glfw INTERFACE)
  target_link_libraries(3rd_glfw INTERFACE glfw)
endif()

set(glfw_FOUND TRUE)
