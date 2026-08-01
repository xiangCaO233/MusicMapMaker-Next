# Modules/Event/CMakeLists.txt

add_library(Event src/core/EventBus.cpp
                  src/input/translators/GLFWTranslator.cpp)

target_link_libraries(
  Event
  PUBLIC Common 3rd_glm 3rd_imgui
  PRIVATE 3rd_glfw)

target_include_directories(Event PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
