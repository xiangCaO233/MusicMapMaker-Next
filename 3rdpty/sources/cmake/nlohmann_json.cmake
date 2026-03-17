add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json SYSTEM)

add_library(3rd_nlohmann_json INTERFACE)
target_link_libraries(3rd_nlohmann_json INTERFACE nlohmann_json::nlohmann_json)
