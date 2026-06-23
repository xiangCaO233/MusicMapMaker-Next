# 导入 Clay 头文件包，导出源码构建同名目标：3rd_clay。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_clay_include_dir clay)

if(NOT TARGET 3rd_clay)
  add_library(3rd_clay INTERFACE)
  target_include_directories(3rd_clay SYSTEM INTERFACE "${_clay_include_dir}")
endif()

set(clay_FOUND TRUE)
