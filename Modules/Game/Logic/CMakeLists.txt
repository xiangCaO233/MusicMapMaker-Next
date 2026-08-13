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
  src/logic/session/NoteIdentity.cpp
  src/logic/session/EditorAction.cpp
  src/logic/session/SampleAction.cpp
  src/logic/session/tool/GrabTool.cpp
  src/logic/session/tool/MarqueeTool.cpp
  src/logic/session/tool/DrawTool.cpp
  src/logic/EditorClipboard.cpp
  src/logic/EditorClipboardProtocol.cpp
  src/logic/BeatmapLoadDiagnosticPublisher.cpp
  src/logic/SessionRegistry.cpp
  src/logic/RenderSyncRegistry.cpp
  src/logic/ProjectDirectoryScanner.cpp
  src/logic/ProjectDirectoryWatcher.cpp
  src/logic/ProjectStorage.cpp
  src/logic/ProjectDraftLaneService.cpp
  src/logic/ProjectResourceService.cpp
  src/logic/MalodyPackageCompatibility.cpp
  src/logic/ProjectCommandService.cpp
  src/logic/ProjectController.cpp
  src/logic/EditorEngine.cpp
  src/logic/audio/AudioTimelineDescriptor.cpp
  src/logic/audio/PlaybackVisualClock.cpp
  src/logic/BeatmapSyncBuffer.cpp
  src/logic/PreviewDensity.cpp
  src/logic/ecs/system/render/NoteRenderSystem.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Layout.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes_Types.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes_Polyline.cpp
  src/logic/ecs/system/render/AudioObjectLabelRenderer.cpp
  src/logic/ecs/system/render/SampleRenderSystem.cpp
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

mmm_add_test_executable(Logic ObjectPlacementSnapTest
                        tests/ObjectPlacementSnapTest.cpp)
target_link_libraries(ObjectPlacementSnapTest PRIVATE Logic Log)
add_test(NAME ObjectPlacementSnapTest COMMAND ObjectPlacementSnapTest)

# 拍号累计测试覆盖短 BPM 段与整拍边界。
mmm_add_test_executable(Logic BeatIndexTest tests/BeatIndexTest.cpp)
target_link_libraries(BeatIndexTest PRIVATE Logic Log)
add_test(NAME BeatIndexTest COMMAND BeatIndexTest)

# 项目分片存储测试覆盖职责拆分、完整往返和旧单文件迁移。
mmm_add_test_executable(Logic ProjectStorageTest tests/ProjectStorageTest.cpp)
target_link_libraries(ProjectStorageTest PRIVATE Logic Log)
add_test(NAME ProjectStorageTest COMMAND ProjectStorageTest)

mmm_add_test_executable(Logic ProjectAudioReferenceTest
                        tests/ProjectAudioReferenceTest.cpp)
target_link_libraries(ProjectAudioReferenceTest PRIVATE Logic Log)
add_test(NAME ProjectAudioReferenceTest COMMAND ProjectAudioReferenceTest)

# Malody 导出模式测试覆盖临时模式覆盖、Key 自动转换与会话元数据恢复。
mmm_add_test_executable(Logic MalodyExportModeTest
                        tests/MalodyExportModeTest.cpp)
target_link_libraries(MalodyExportModeTest PRIVATE Logic Log)
add_test(NAME MalodyExportModeTest COMMAND MalodyExportModeTest)

mmm_add_test_executable(Logic AudioTimelineDescriptorTest
                        tests/AudioTimelineDescriptorTest.cpp)
target_link_libraries(AudioTimelineDescriptorTest PRIVATE Logic Log)
add_test(NAME AudioTimelineDescriptorTest COMMAND AudioTimelineDescriptorTest)

mmm_add_test_executable(Logic ProjectDirectoryWatcherTest
                        tests/ProjectDirectoryWatcherTest.cpp)
target_link_libraries(ProjectDirectoryWatcherTest PRIVATE Logic Log)
add_test(NAME ProjectDirectoryWatcherTest COMMAND ProjectDirectoryWatcherTest)

mmm_add_test_executable(Logic PlaybackRestartTest tests/PlaybackRestartTest.cpp)
target_link_libraries(PlaybackRestartTest PRIVATE Logic Log)
add_test(NAME PlaybackRestartTest COMMAND PlaybackRestartTest)

# 连续播放时钟测试覆盖 block 内推演、校准、暂停、Seek、循环与 UI 补间。
mmm_add_test_executable(Logic PlaybackVisualClockTest
                        tests/PlaybackVisualClockTest.cpp)
target_link_libraries(PlaybackVisualClockTest PRIVATE Logic Log)
add_test(NAME PlaybackVisualClockTest COMMAND PlaybackVisualClockTest)

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

# 自动采样渲染测试确保 BGM 物件复用当前皮肤的 Note 纹理。
mmm_add_test_executable(Logic SampleRenderSystemTest
                        tests/SampleRenderSystemTest.cpp)
target_link_libraries(SampleRenderSystemTest PRIVATE Logic Log)
add_test(NAME SampleRenderSystemTest COMMAND SampleRenderSystemTest)

# 玩家物件标签测试确保绑定音效显示只在主画布启用并复用采样标签滚动。
mmm_add_test_executable(Logic NoteBoundSampleLabelRenderTest
                        tests/NoteBoundSampleLabelRenderTest.cpp)
target_link_libraries(NoteBoundSampleLabelRenderTest PRIVATE Logic Log)
add_test(NAME NoteBoundSampleLabelRenderTest
         COMMAND NoteBoundSampleLabelRenderTest)

# 重叠键遮罩测试确保播放快照仍会生成顶层重叠提示。
mmm_add_test_executable(Logic NoteOverlapMaskRenderTest
                        tests/NoteOverlapMaskRenderTest.cpp)
target_link_libraries(NoteOverlapMaskRenderTest PRIVATE Logic Log)
add_test(NAME NoteOverlapMaskRenderTest COMMAND NoteOverlapMaskRenderTest)

# 交互拾取包围盒测试覆盖独立横纵缩放、中心保持和非法值回退。
mmm_add_test_executable(Logic InteractionHitboxScaleTest
                        tests/InteractionHitboxScaleTest.cpp)
target_link_libraries(InteractionHitboxScaleTest PRIVATE Logic Log)
add_test(NAME InteractionHitboxScaleTest COMMAND InteractionHitboxScaleTest)

# Logo 占位画布测试确保空谱面图标不继承轨道布局裁剪。
mmm_add_test_executable(Logic LogoPlaceholderScissorTest
                        tests/LogoPlaceholderScissorTest.cpp)
target_link_libraries(LogoPlaceholderScissorTest PRIVATE Logic Log)
add_test(NAME LogoPlaceholderScissorTest COMMAND LogoPlaceholderScissorTest)

mmm_add_test_executable(Logic BrushPaletteSessionStateTest
                        tests/BrushPaletteSessionStateTest.cpp)
target_link_libraries(BrushPaletteSessionStateTest PRIVATE Logic Log)
add_test(NAME BrushPaletteSessionStateTest COMMAND BrushPaletteSessionStateTest)

# 注册表快照生命周期测试确保关闭会话和替换图集后旧快照可回收。
mmm_add_test_executable(Logic RegistrySnapshotLifetimeTest
                        tests/RegistrySnapshotLifetimeTest.cpp)
target_link_libraries(RegistrySnapshotLifetimeTest PRIVATE Logic Log)
add_test(NAME RegistrySnapshotLifetimeTest COMMAND RegistrySnapshotLifetimeTest)

# 打击音效立体声测试覆盖普通物件固定声像和 Flick 全程线性移动。
mmm_add_test_executable(Logic HitEffectStereoTest tests/HitEffectStereoTest.cpp)
target_link_libraries(HitEffectStereoTest PRIVATE Logic Audio Log)
add_test(NAME HitEffectStereoTest COMMAND HitEffectStereoTest)

# 旧 MMM 加载诊断测试覆盖一次载入内的事件生成、去重与空输入行为。
mmm_add_test_executable(Logic BeatmapLoadDiagnosticPublisherTest
                        tests/BeatmapLoadDiagnosticPublisherTest.cpp)
target_link_libraries(BeatmapLoadDiagnosticPublisherTest PRIVATE Logic Log)
add_test(NAME BeatmapLoadDiagnosticPublisherTest
         COMMAND BeatmapLoadDiagnosticPublisherTest)

# 协作观察者绑定测试确保访客不会把收到的房主快照回传为本地编辑。
mmm_add_test_executable(Logic BeatmapMutationObserverBindingTest
                        tests/BeatmapMutationObserverBindingTest.cpp)
target_link_libraries(BeatmapMutationObserverBindingTest PRIVATE Logic Log)
add_test(NAME BeatmapMutationObserverBindingTest
         COMMAND BeatmapMutationObserverBindingTest)
