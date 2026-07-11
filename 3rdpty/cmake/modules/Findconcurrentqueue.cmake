# 导入 concurrentqueue 头文件包，导出源码构建同名目标：3rd_concurrentqueue。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_concurrentqueue_include_dir concurrentqueue)

if(NOT TARGET 3rd_concurrentqueue)
  add_library(3rd_concurrentqueue INTERFACE)
  target_include_directories(3rd_concurrentqueue SYSTEM
                             INTERFACE "${_concurrentqueue_include_dir}")
  if(WIN32 AND MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # MinGW GCC win32 线程模型在进程退出阶段可能先销毁 concurrentqueue
    # ThreadExitNotifier 依赖的函数内静态 mutex，再执行主线程 thread_local
    # 析构；禁用该 TLS 通知器以避免退出时访问已销毁的 CRITICAL_SECTION。
    target_compile_definitions(3rd_concurrentqueue
                               INTERFACE MOODYCAMEL_NO_THREAD_LOCAL)
  endif()
endif()

set(concurrentqueue_FOUND TRUE)
