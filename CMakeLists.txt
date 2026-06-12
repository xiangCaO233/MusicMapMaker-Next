cmake_minimum_required(VERSION 3.31)

project(MusicMapMaker-Next VERSION 0.4.1 LANGUAGES CXX)

if(MSVC)
	# 设置msvc需要的 VCPKG_ROOT
	set(VCPKG_ROOT "C:/coding/vcpkg")
	# 强制 MSVC 使用静态运行时 (/MT)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain")

include("project-configure")
include("pgo")

enable_testing()

# 从源码编译依赖 默认开启
option(SOURCES_BUILD "Build Deps from sources." ON)

# Windows 原生文件对话框默认会用最近访问目录覆盖 defaultPath；
# 另存为需要强制打开当前项目目录。
set(
	NFD_OVERRIDE_RECENT_WITH_DEFAULT
	ON
	CACHE
		BOOL
		"Use NFD defaultPath instead of Windows recent folders."
	FORCE
)

find_package(Vulkan REQUIRED)

add_subdirectory("3rdpty")
add_subdirectory("Modules")
