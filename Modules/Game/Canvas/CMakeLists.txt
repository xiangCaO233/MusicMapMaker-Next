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
         Runtime
         NetworkCollaboration
         3rd_glm
         3rd_entt)
target_include_directories(Canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 协作视野投影是无图形依赖的纯计算，覆盖缺少 ScrollSegment 时的锚点映射。
mmm_add_test_executable(Canvas CollaborationViewportProjectionTest
                        tests/CollaborationViewportProjectionTest.cpp)
target_include_directories(CollaborationViewportProjectionTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME CollaborationViewportProjectionTest
         COMMAND CollaborationViewportProjectionTest)

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

# 布局编辑使用无图形依赖的纯计算，覆盖轨道、判定线和可选组件。
mmm_add_test_executable(Canvas LayoutEditingTest tests/LayoutEditingTest.cpp)
target_include_directories(LayoutEditingTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(LayoutEditingTest PRIVATE Common Config)
add_test(NAME LayoutEditingTest COMMAND LayoutEditingTest)

# 画布标签测试覆盖长标题省略时仍固定显示前置脏标志。
mmm_add_test_executable(Canvas CanvasTabTitleTest tests/CanvasTabTitleTest.cpp)
target_include_directories(CanvasTabTitleTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME CanvasTabTitleTest COMMAND CanvasTabTitleTest)

# 欢迎占位页可见性测试覆盖无谱面时隐藏状态与悬浮检视内容。
mmm_add_test_executable(Canvas CanvasContentVisibilityTest
                        tests/CanvasContentVisibilityTest.cpp)
target_include_directories(CanvasContentVisibilityTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME CanvasContentVisibilityTest COMMAND CanvasContentVisibilityTest)

# Timeline Timing 悬浮提示使用纯映射，独立覆盖界面术语与数值单位。
mmm_add_test_executable(Canvas TimelineTimingTooltipTest
                        tests/TimelineTimingTooltipTest.cpp)
target_include_directories(TimelineTimingTooltipTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(TimelineTimingTooltipTest PRIVATE MMM)
add_test(NAME TimelineTimingTooltipTest COMMAND TimelineTimingTooltipTest)

# 主画布悬浮层选择使用纯候选去重，覆盖同一物件的多个相交命中框。
mmm_add_test_executable(Canvas HoverLayerSelectionTest
                        tests/HoverLayerSelectionTest.cpp)
target_include_directories(HoverLayerSelectionTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(HoverLayerSelectionTest PRIVATE Common 3rd_entt)
add_test(NAME HoverLayerSelectionTest COMMAND HoverLayerSelectionTest)
