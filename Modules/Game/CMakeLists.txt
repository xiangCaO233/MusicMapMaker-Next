# Modules/Game/CMakeLists.txt

add_subdirectory("Common")
add_subdirectory("Graphic")
add_subdirectory("Logic")
add_subdirectory("UI")
add_subdirectory("Canvas")

target_link_libraries(UI PRIVATE Canvas)

add_library(Game STATIC src/GameLoop.cpp)

target_link_libraries(
  Game
  PUBLIC Log
         Runtime
         Common
         UI
         Canvas
         Graphic
         Logic
         Audio
         3rd_entt
         3rd_glm
         MMM
         Network)

if(WIN32)
  target_link_libraries(Game PUBLIC dbghelp)
endif()

target_include_directories(Game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
