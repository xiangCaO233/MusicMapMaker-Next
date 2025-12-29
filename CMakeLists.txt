cmake_minimum_required(VERSION 3.31)

project(MusicMapMaker-Next)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain")

include("project-configure")

# 找到系统中的 Vulkan 包
find_package(Vulkan REQUIRED)

option(SOURCES_BUILD "Build Deps from sources." ON)

add_subdirectory("3rdpty")
add_subdirectory("Modules")
