# 导入 sol2 头文件包，并提供项目包装目标：3rd_sol2。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_sol2_include_dir sol2)

if(NOT TARGET sol2::sol2)
  add_library(sol2::sol2 INTERFACE IMPORTED GLOBAL)
  set_target_properties(sol2::sol2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_sol2_include_dir}")
endif()

if(NOT TARGET 3rd_sol2)
  add_library(3rd_sol2 INTERFACE)
  target_link_libraries(3rd_sol2 INTERFACE sol2::sol2)
endif()

set(sol2_FOUND TRUE)
