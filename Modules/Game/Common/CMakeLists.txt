# Modules/Game/Common/CMakeLists.txt

add_library(
  Common STATIC
  src/common/AudioInfoUtils.cpp src/common/RenderSnapshotBuffer.cpp
  src/common/VideoFrameDecoder.cpp)

target_include_directories(
  Common
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
         ${PROJECT_SOURCE_DIR}/3rdpty/sources/entt/src
         ${PROJECT_SOURCE_DIR}/3rdpty/sources/nlohmann_json/include
         ${PROJECT_SOURCE_DIR}/Modules/Config/include
         ${PROJECT_SOURCE_DIR}/Modules/MMM/include
         ${PROJECT_SOURCE_DIR}/Modules/Log/include)

target_link_libraries(
  Common
  PUBLIC Log ICE::IonCachyEngine 3rd_concurrentqueue
  PRIVATE 3rd_ffmpeg)

mmm_add_test_executable(Common VideoFrameDecoderTest
                        tests/VideoFrameDecoderTest.cpp)
target_link_libraries(VideoFrameDecoderTest PRIVATE Common 3rd_ffmpeg)
add_test(NAME VideoFrameDecoderTest
         COMMAND VideoFrameDecoderTest
                 "${CMAKE_BINARY_DIR}/test_output/video_frame_decoder")

# 音频资源拖拽载荷测试保证跨模块共享的固定布局与长度拒绝规则稳定。
mmm_add_test_executable(Common AudioResourceDragPayloadTest
                        tests/AudioResourceDragPayloadTest.cpp)
target_link_libraries(AudioResourceDragPayloadTest PRIVATE Common)
add_test(NAME AudioResourceDragPayloadTest COMMAND AudioResourceDragPayloadTest)

# 单文件兼容测试覆盖复合音频时间线的显式拒绝条件。
mmm_add_test_executable(Common BeatmapAudioTimelineCompatibilityTest
                        tests/BeatmapAudioTimelineCompatibilityTest.cpp)
target_link_libraries(BeatmapAudioTimelineCompatibilityTest PRIVATE Common MMM)
add_test(NAME BeatmapAudioTimelineCompatibilityTest
         COMMAND BeatmapAudioTimelineCompatibilityTest)
