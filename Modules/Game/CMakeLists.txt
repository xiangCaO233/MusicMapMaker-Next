# Modules/Game/CMakeLists.txt

add_subdirectory("Graphic")
add_subdirectory("Canvas")

add_library(Game STATIC src/GameLoop.cpp)

target_link_libraries(Game PRIVATE Log Canvas)
target_include_directories(Game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
