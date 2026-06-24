# 导入 EnTT 头文件包，并提供项目包装目标：3rd_entt。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_entt_include_dir entt)

if(NOT TARGET EnTT::EnTT)
  add_library(EnTT::EnTT INTERFACE IMPORTED GLOBAL)
  set_target_properties(EnTT::EnTT PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_entt_include_dir}")
endif()

if(NOT TARGET 3rd_entt)
  add_library(3rd_entt INTERFACE)
  target_link_libraries(3rd_entt INTERFACE EnTT::EnTT)
endif()

set(EnTT_FOUND TRUE)
