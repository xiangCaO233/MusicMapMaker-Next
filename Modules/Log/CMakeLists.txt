# Modules/Log/CMakeLists.txt

add_library(Log SHARED src/colorful-log.cpp)

target_link_libraries(Log PUBLIC spdlog::spdlog)
target_include_directories(Log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
