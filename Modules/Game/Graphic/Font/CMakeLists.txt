# Modules/Game/Graphic/Font/CMakeLists.txt

add_library(Font SHARED src/FontPool.cpp)

target_include_directories(Font PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Font PRIVATE Log freetype::freetype)