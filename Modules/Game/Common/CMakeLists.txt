# Modules/Game/Common/CMakeLists.txt

add_library(Common STATIC src/common/AudioInfoUtils.cpp
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
  PUBLIC Log ICE::IonCachyEngine
  PRIVATE 3rd_ffmpeg)

mmm_add_test_executable(Common VideoFrameDecoderTest
                        tests/VideoFrameDecoderTest.cpp)
target_link_libraries(VideoFrameDecoderTest PRIVATE Common 3rd_ffmpeg)
add_test(NAME VideoFrameDecoderTest
         COMMAND VideoFrameDecoderTest
                 "${CMAKE_BINARY_DIR}/test_output/video_frame_decoder")
