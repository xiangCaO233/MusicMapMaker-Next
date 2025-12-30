# Modules/Canvas/CMakeLists.txt

add_library(Canvas STATIC src/TestCanvas.cpp)

target_link_libraries(Canvas PUBLIC 3rd_imgui Vulkan::Vulkan Log)
target_include_directories(Canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
