# 导入 ImGuiFileDialog 预编译库，并提供项目包装目标：3rd_ImGuiFileDialog。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
find_package(imgui REQUIRED)
prebuilt_include_dir(_imgui_file_dialog_include_dir ImGuiFileDialog)

if(NOT TARGET ImGuiFileDialog)
  add_library(ImGuiFileDialog UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    ImGuiFileDialog PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                               "${_imgui_file_dialog_include_dir}")

  prebuilt_target_configs(_imgui_file_dialog_configs)
  set(_imgui_file_dialog_imported_configs "")
  set(_imgui_file_dialog_default_library "")
  foreach(_imgui_file_dialog_config IN LISTS _imgui_file_dialog_configs)
    string(TOUPPER "${_imgui_file_dialog_config}"
                   _imgui_file_dialog_config_upper)
    prebuilt_find_library(
      _imgui_file_dialog_library ImGuiFileDialog
      "${_imgui_file_dialog_config}" ImGuiFileDialog)
    list(APPEND _imgui_file_dialog_imported_configs
         "${_imgui_file_dialog_config_upper}")
    set_target_properties(
      ImGuiFileDialog
      PROPERTIES "IMPORTED_LOCATION_${_imgui_file_dialog_config_upper}"
                 "${_imgui_file_dialog_library}")
    if(_imgui_file_dialog_default_library STREQUAL "")
      set(_imgui_file_dialog_default_library "${_imgui_file_dialog_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      ImGuiFileDialog PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                 "${_imgui_file_dialog_default_library}")
  endif()
  set_target_properties(
    ImGuiFileDialog
    PROPERTIES IMPORTED_CONFIGURATIONS "${_imgui_file_dialog_imported_configs}"
               IMPORTED_LOCATION "${_imgui_file_dialog_default_library}")
  target_link_libraries(ImGuiFileDialog INTERFACE 3rd_imgui)
endif()

if(NOT TARGET 3rd_ImGuiFileDialog)
  add_library(3rd_ImGuiFileDialog INTERFACE)
  target_link_libraries(3rd_ImGuiFileDialog INTERFACE ImGuiFileDialog)
endif()

set(ImGuiFileDialog_FOUND TRUE)
