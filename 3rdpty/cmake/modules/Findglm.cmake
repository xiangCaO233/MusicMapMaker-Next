# 导入 GLM 头文件包，并提供项目包装目标：3rd_glm。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 矩阵、向量和布局实现由消费者实例化；与源码构建共用同版公共头。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 此包没有 Debug 和发布归档，不按平台选择数学类型。
prebuilt_include_dir(_glm_include_dir glm)

# 保留上游目标名，依赖方之间仍共享同一个包含路径。
if(NOT TARGET glm::glm)
  add_library(glm::glm INTERFACE IMPORTED GLOBAL)
  set_target_properties(glm::glm PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_glm_include_dir}")
endif()

# 包装依赖随链接接口向下传播，使用数学类型的公开头也能找到 GLM。
if(NOT TARGET 3rd_glm)
  add_library(3rd_glm INTERFACE)
  target_link_libraries(3rd_glm INTERFACE glm::glm)
endif()

# 头目录校验成功即可报告，纯头文件依赖无二进制探测步骤。
set(glm_FOUND TRUE)
