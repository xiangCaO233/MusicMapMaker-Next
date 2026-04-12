# Modules/Game/Canvas/CMakeLists.txt

add_library(Canvas STATIC
    src/Basic2DCanvas.cpp
    src/Basic2DCanvas_Interaction.cpp
    src/Basic2DCanvas_Rendering.cpp
    src/PreviewCanvas.cpp
    src/TimelineCanvas.cpp
    src/TimelineCanvas_Interaction.cpp
    src/TimelineCanvas_Popups.cpp
)

target_link_libraries(Canvas PUBLIC Log Common Config UI Graphic Logic 3rd_glm 3rd_entt)
target_include_directories(Canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
