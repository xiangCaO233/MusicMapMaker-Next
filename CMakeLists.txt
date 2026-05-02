cmake_minimum_required(VERSION 3.31)

project(MusicMapMaker-Next)

if(MSVC)
	# 设置msvc需要的 VCPKG_ROOT
	set(VCPKG_ROOT "C:/msys64/home/odoko/vcpkg")
	# 强制 MSVC 使用静态运行时 (/MT)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain")

include("project-configure")

enable_testing()

# 从源码编译依赖 默认开启
option(SOURCES_BUILD "Build Deps from sources." ON)

find_package(Vulkan REQUIRED)

add_subdirectory("3rdpty")
add_subdirectory("Modules")
