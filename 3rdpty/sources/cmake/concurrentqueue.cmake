add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/concurrentqueue SYSTEM)

add_library(3rd_concurrentqueue INTERFACE)
target_link_libraries(3rd_concurrentqueue INTERFACE concurrentqueue)
if(WIN32 AND MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # MinGW GCC win32 线程模型在进程退出阶段可能先销毁 concurrentqueue
  # ThreadExitNotifier 依赖的函数内静态 mutex，再执行主线程 thread_local
  # 析构；禁用该 TLS 通知器以避免退出时访问已销毁的 CRITICAL_SECTION。
  target_compile_definitions(3rd_concurrentqueue
                             INTERFACE MOODYCAMEL_NO_THREAD_LOCAL)
endif()
