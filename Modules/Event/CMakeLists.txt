# Modules/Event/CMakeLists.txt

add_library(Event src/core/EventBus.cpp)

target_link_libraries(Event PUBLIC Graphic Common)

target_include_directories(Event PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
