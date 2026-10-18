# * 归档依赖的增量预编译入口，可单独补齐新包而不重建音视频依赖。 正式源码构建通过同一 wrapper，避免两套编译选项发生分叉。
cmake_minimum_required(VERSION 3.31)
project(MmmArchiveDependencies LANGUAGES C CXX)
# * 独立入口必须显式选择链接类型，产物仍按平台和配置分别暂存。
set(PROJECT_LINKAGE
    static
    CACHE STRING "依赖链接类型：static 或 shared")
if(NOT PROJECT_LINKAGE MATCHES "^(static|shared)$")
  message(FATAL_ERROR "PROJECT_LINKAGE 必须是 static 或 shared")
endif()
# * Windows CRT 与主工程保持一致，不允许共享模式静态链接运行库。
if(PROJECT_LINKAGE STREQUAL "static")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
# * 本入口不配置应用，也不应用业务模块 PGO。
# * 输出仍是外部工程安装树，必须显式运行 staging 后才能作为预编译消费。
# * 与主工程复用一个依赖图，更新编码器选项时只需维护一份实现。
include("${CMAKE_CURRENT_LIST_DIR}/../../3rdpty/sources/cmake/libarchive.cmake")
