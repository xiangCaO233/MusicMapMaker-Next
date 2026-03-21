# Modules/Game/Canvas/CMakeLists.txt

add_library(Canvas STATIC
    src/Basic2DCanvas.cpp
)

target_link_libraries(Canvas PUBLIC Log Config Graphic 3rd_glm)
target_include_directories(Canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
