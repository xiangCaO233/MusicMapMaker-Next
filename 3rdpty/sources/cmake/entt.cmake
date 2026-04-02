add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/entt SYSTEM)

add_library(3rd_entt INTERFACE)
target_link_libraries(3rd_entt INTERFACE EnTT::EnTT)
