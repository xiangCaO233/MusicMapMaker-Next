# Modules/Game/Canvas/CMakeLists.txt

add_library(Canvas STATIC
    src/obj.cpp
)

target_link_libraries(Canvas PUBLIC Log Config)
target_include_directories(Canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
