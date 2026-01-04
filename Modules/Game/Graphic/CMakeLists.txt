# Modules/Game/Graphic/CMakeLists.txt

# 找到系统中的 Vulkan 开发环境
find_package(Vulkan REQUIRED)

add_subdirectory("Font")

add_library(Graphic SHARED src/Renderer.cpp)

target_include_directories(Graphic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Graphic PUBLIC Log Vulkan::Vulkan)
