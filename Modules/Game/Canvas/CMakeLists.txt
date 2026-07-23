# Modules/Game/Canvas/CMakeLists.txt

add_library(
  Canvas STATIC
  src/BackgroundVideoPlayer.cpp
  src/Basic2DCanvas.cpp
  src/Basic2DCanvas_Interaction.cpp
  src/Basic2DCanvas_Rendering.cpp
  src/MarqueeAutoScroll.cpp
  src/PreviewCanvas.cpp
  src/TimelineCanvas.cpp
  src/TimelineCanvas_Interaction.cpp
  src/TimelineCanvas_Popups.cpp)

target_link_libraries(
  Canvas
  PUBLIC Log
         Common
         Config
         UI
         Graphic
         Logic
         3rd_glm
         3rd_entt)
target_include_directories(Canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 密度栏坐标映射为无图形依赖的纯函数，测试目标无需链接完整 Canvas。
mmm_add_test_executable(Canvas PreviewDensityInteractionTest
                        tests/PreviewDensityInteractionTest.cpp)
target_include_directories(PreviewDensityInteractionTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME PreviewDensityInteractionTest
         COMMAND PreviewDensityInteractionTest)

# 背景视频时钟是无图形依赖的纯计算，单独覆盖暂停、倍速与开始偏移。
mmm_add_test_executable(Canvas BackgroundVideoTimingTest
                        tests/BackgroundVideoTimingTest.cpp)
target_include_directories(BackgroundVideoTimingTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME BackgroundVideoTimingTest COMMAND BackgroundVideoTimingTest)

# 轨道布局编辑使用无图形依赖的纯计算，覆盖边界限制和整体平移。
mmm_add_test_executable(Canvas TrackLayoutEditingTest
                        tests/TrackLayoutEditingTest.cpp)
target_include_directories(TrackLayoutEditingTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(TrackLayoutEditingTest PRIVATE Config)
add_test(NAME TrackLayoutEditingTest COMMAND TrackLayoutEditingTest)
