# 导入 concurrentqueue 头文件包，导出源码构建同名目标：3rd_concurrentqueue。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_concurrentqueue_include_dir concurrentqueue)

if(NOT TARGET 3rd_concurrentqueue)
  add_library(3rd_concurrentqueue INTERFACE)
  target_include_directories(3rd_concurrentqueue SYSTEM
                             INTERFACE "${_concurrentqueue_include_dir}")
endif()

set(concurrentqueue_FOUND TRUE)
