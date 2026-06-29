# 导入 ImGui 预编译库，并提供项目包装目标：3rd_imgui。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
find_package(glfw REQUIRED)
prebuilt_include_dir(_imgui_include_dir imgui)

if(NOT TARGET Vulkan::Vulkan)
  find_package(Vulkan REQUIRED)
endif()

if(NOT TARGET imgui-static)
  add_library(imgui-static UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    imgui-static
    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
               "${_imgui_include_dir};${_imgui_include_dir}/backends")

  prebuilt_target_configs(_imgui_configs)
  set(_imgui_imported_configs "")
  set(_imgui_default_library "")
  foreach(_imgui_config IN LISTS _imgui_configs)
    string(TOUPPER "${_imgui_config}" _imgui_config_upper)
    prebuilt_find_library(_imgui_library imgui "${_imgui_config}" imgui
                          imgui-static)
    list(APPEND _imgui_imported_configs "${_imgui_config_upper}")
    set_target_properties(
      imgui-static PROPERTIES "IMPORTED_LOCATION_${_imgui_config_upper}"
                              "${_imgui_library}")
    if(_imgui_default_library STREQUAL "")
      set(_imgui_default_library "${_imgui_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(imgui-static PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                  "${_imgui_default_library}")
  endif()
  set_target_properties(
    imgui-static PROPERTIES IMPORTED_CONFIGURATIONS "${_imgui_imported_configs}"
                            IMPORTED_LOCATION "${_imgui_default_library}")
  target_link_libraries(imgui-static INTERFACE 3rd_glfw Vulkan::Vulkan)
  if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
    # shared ImGui 预编译包需要让使用方按 dllimport 访问数据符号。
    target_compile_definitions(
      imgui-static INTERFACE "IMGUI_API=__declspec(dllimport)"
                             "IMGUI_IMPL_API=__declspec(dllimport)")
  endif()
endif()

if(NOT TARGET 3rd_imgui)
  add_library(3rd_imgui INTERFACE)
  target_link_libraries(3rd_imgui INTERFACE imgui-static)
endif()

set(imgui_FOUND TRUE)
