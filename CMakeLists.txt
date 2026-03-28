cmake_minimum_required(VERSION 3.31)

project(MusicMapMaker-Next)

if(MSVC)
	# 设置msvc需要的 VCPKG_ROOT
	set(VCPKG_ROOT "C:/msys64/home/xiang/projects/vcpkg")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain")

include("project-configure")

# 从源码编译依赖 默认开启
option(SOURCES_BUILD "Build Deps from sources." ON)

find_package(Vulkan REQUIRED)

add_subdirectory("3rdpty")
add_subdirectory("Modules")