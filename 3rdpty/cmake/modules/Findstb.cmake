# 导入 stb 头文件包，导出源码构建同名目标：3rd_stb。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_stb_include_dir stb)

if(NOT TARGET 3rd_stb)
  add_library(3rd_stb INTERFACE)
  target_include_directories(3rd_stb SYSTEM INTERFACE "${_stb_include_dir}")
endif()

set(stb_FOUND TRUE)
