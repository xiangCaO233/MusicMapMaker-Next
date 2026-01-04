# Modules/Game/Canvas/CMakeLists.txt

add_library(Canvas STATIC src/TestCanvas.cpp)

target_link_libraries(Canvas PUBLIC Log 3rd_imgui Graphic Font Config)
target_include_directories(Canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
