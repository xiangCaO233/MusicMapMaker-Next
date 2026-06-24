# 导入 GLM 头文件包，并提供项目包装目标：3rd_glm。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_glm_include_dir glm)

if(NOT TARGET glm::glm)
  add_library(glm::glm INTERFACE IMPORTED GLOBAL)
  set_target_properties(glm::glm PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_glm_include_dir}")
endif()

if(NOT TARGET 3rd_glm)
  add_library(3rd_glm INTERFACE)
  target_link_libraries(3rd_glm INTERFACE glm::glm)
endif()

set(glm_FOUND TRUE)
