# Modules/Game/Logic/CMakeLists.txt

add_library(
  Logic STATIC
  src/logic/session/BeatmapSession.cpp
  src/logic/session/BeatmapSession_Commands.cpp
  src/logic/session/InteractionController.cpp
  src/logic/session/PlaybackController.cpp
  src/logic/session/ActionController_Editing.cpp
  src/logic/session/SessionUtils_Loading.cpp
  src/logic/session/BeatmapSession_Rendering.cpp
  src/logic/session/SessionUtils.cpp
  src/logic/session/ActionController.cpp
  src/logic/session/EditorAction.cpp
  src/logic/session/tool/GrabTool.cpp
  src/logic/session/tool/MarqueeTool.cpp
  src/logic/session/tool/DrawTool.cpp
  src/logic/EditorClipboard.cpp
  src/logic/EditorClipboardProtocol.cpp
  src/logic/SessionRegistry.cpp
  src/logic/RenderSyncRegistry.cpp
  src/logic/ProjectDirectoryScanner.cpp
  src/logic/ProjectDirectoryWatcher.cpp
  src/logic/ProjectResourceService.cpp
  src/logic/ProjectCommandService.cpp
  src/logic/ProjectController.cpp
  src/logic/EditorEngine.cpp
  src/logic/BeatmapSyncBuffer.cpp
  src/logic/PreviewDensity.cpp
  src/logic/ecs/system/render/NoteRenderSystem.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Layout.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes_Types.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes_Polyline.cpp
  src/logic/ecs/system/render/CanvasComponentRenderSystem.cpp
  src/logic/ecs/system/render/BackgroundRenderSystem.cpp
  src/logic/ecs/system/render/BackgroundSpectrumRenderSystem.cpp
  src/logic/ecs/system/NoteTransformSystem.cpp
  src/logic/ecs/system/ScrollCache.cpp
  src/logic/ecs/system/HitFXSystem.cpp)

target_link_libraries(
  Logic
  PUBLIC Log
         MMM
         Common
         UI
         Graphic
         3rd_concurrentqueue
         3rd_entt
         3rd_glm
         3rd_miniz
         Audio
         Config
         Runtime)
target_include_directories(Logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

mmm_add_test_executable(Logic EditorClipboardProtocolTest
                        tests/EditorClipboardProtocolTest.cpp)
target_link_libraries(EditorClipboardProtocolTest PRIVATE Logic Log)
add_test(NAME EditorClipboardProtocolTest COMMAND EditorClipboardProtocolTest)

mmm_add_test_executable(Logic PreviewDensityTest tests/PreviewDensityTest.cpp)
target_link_libraries(PreviewDensityTest PRIVATE Logic Log)
add_test(NAME PreviewDensityTest COMMAND PreviewDensityTest)

mmm_add_test_executable(Logic TimelineBatchMetadataTest
                        tests/TimelineBatchMetadataTest.cpp)
target_link_libraries(TimelineBatchMetadataTest PRIVATE Logic Log)
add_test(NAME TimelineBatchMetadataTest COMMAND TimelineBatchMetadataTest)

mmm_add_test_executable(Logic ProjectCompatibilityTest
                        tests/ProjectCompatibilityTest.cpp)
target_link_libraries(ProjectCompatibilityTest PRIVATE Logic Log)
add_test(NAME ProjectCompatibilityTest COMMAND ProjectCompatibilityTest)

mmm_add_test_executable(Logic ProjectAudioReferenceTest
                        tests/ProjectAudioReferenceTest.cpp)
target_link_libraries(ProjectAudioReferenceTest PRIVATE Logic Log)
add_test(NAME ProjectAudioReferenceTest COMMAND ProjectAudioReferenceTest)

mmm_add_test_executable(Logic ProjectDirectoryWatcherTest
                        tests/ProjectDirectoryWatcherTest.cpp)
target_link_libraries(ProjectDirectoryWatcherTest PRIVATE Logic Log)
add_test(NAME ProjectDirectoryWatcherTest COMMAND ProjectDirectoryWatcherTest)

mmm_add_test_executable(Logic PlaybackRestartTest tests/PlaybackRestartTest.cpp)
target_link_libraries(PlaybackRestartTest PRIVATE Logic Log)
add_test(NAME PlaybackRestartTest COMMAND PlaybackRestartTest)

# 主画布相机测试覆盖中键二维平移、逻辑像素投影和 Resize 比例保持。
mmm_add_test_executable(Logic CanvasCameraTest tests/CanvasCameraTest.cpp)
target_link_libraries(CanvasCameraTest PRIVATE Logic Log)
add_test(NAME CanvasCameraTest COMMAND CanvasCameraTest)

mmm_add_test_executable(Logic BackgroundRenderSystemTest
                        tests/BackgroundRenderSystemTest.cpp)
target_link_libraries(BackgroundRenderSystemTest PRIVATE Logic Log)
add_test(NAME BackgroundRenderSystemTest COMMAND BackgroundRenderSystemTest)

mmm_add_test_executable(Logic CanvasComponentRenderSystemTest
                        tests/CanvasComponentRenderSystemTest.cpp)
target_link_libraries(CanvasComponentRenderSystemTest PRIVATE Logic Log)
add_test(NAME CanvasComponentRenderSystemTest
         COMMAND CanvasComponentRenderSystemTest)

# Logo 占位画布测试确保空谱面图标不继承轨道布局裁剪。
mmm_add_test_executable(Logic LogoPlaceholderScissorTest
                        tests/LogoPlaceholderScissorTest.cpp)
target_link_libraries(LogoPlaceholderScissorTest PRIVATE Logic Log)
add_test(NAME LogoPlaceholderScissorTest COMMAND LogoPlaceholderScissorTest)

mmm_add_test_executable(Logic BrushPaletteSessionStateTest
                        tests/BrushPaletteSessionStateTest.cpp)
target_link_libraries(BrushPaletteSessionStateTest PRIVATE Logic Log)
add_test(NAME BrushPaletteSessionStateTest COMMAND BrushPaletteSessionStateTest)

# 打击音效立体声测试覆盖普通物件固定声像和 Flick 全程线性移动。
mmm_add_test_executable(Logic HitEffectStereoTest tests/HitEffectStereoTest.cpp)
target_link_libraries(HitEffectStereoTest PRIVATE Logic Audio Log)
add_test(NAME HitEffectStereoTest COMMAND HitEffectStereoTest)
