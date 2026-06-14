cmake_minimum_required(VERSION 3.31)

project(MusicMapMaker-Next VERSION 0.4.2 LANGUAGES CXX)

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

set(
	MMM_TEST_RESOURCE_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
	CACHE
		PATH
		"Shared test resource directory."
)

function(mmm_set_test_target MODULE_NAME TARGET_NAME)
	set(TEST_RUNTIME_DIR "${CMAKE_BINARY_DIR}/tests/${MODULE_NAME}")
	set_target_properties(${TARGET_NAME}
		PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_RUNTIME_DIR}"
	)

	foreach(CONFIG_NAME Debug Release RelWithDebInfo MinSizeRel)
		string(TOUPPER "${CONFIG_NAME}" CONFIG_NAME_UPPER)
		set_target_properties(${TARGET_NAME}
			PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CONFIG_NAME_UPPER} "${TEST_RUNTIME_DIR}"
		)
	endforeach()
endfunction()

function(mmm_add_test_executable MODULE_NAME TARGET_NAME)
	add_executable(${TARGET_NAME} ${ARGN})
	mmm_set_test_target(${MODULE_NAME} ${TARGET_NAME})
endfunction()

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
