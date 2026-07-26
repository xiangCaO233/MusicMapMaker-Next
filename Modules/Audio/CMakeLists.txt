# Modules/Audio/CMakeLists.txt

add_library(
  Audio STATIC
  src/BackgroundSpectrumAnalyzer.cpp
  src/AudioManager.cpp
  src/AudioManager_BGM.cpp
  src/AudioManager_EQ.cpp
  src/AudioManager_Mix.cpp
  src/AudioManager_Playback.cpp
  src/AudioManager_SFX.cpp
  src/AudioSpeedExportService.cpp
  src/SoundEffectPool.cpp)

target_link_libraries(
  Audio
  PUBLIC ICE::IonCachyEngine
         Log
         Event
         Runtime
         3rd_rubberband
         MMM
         Config)

if(TARGET 3rd_fftw3)
  target_link_libraries(Audio PUBLIC 3rd_fftw3)
elseif(MSVC)
  set(FFTW3_DIR "${VCPKG_ROOT}/installed/x64-windows-static/share/fftw3")
  find_package(FFTW3 CONFIG REQUIRED)
  target_link_libraries(Audio PUBLIC fftw3)
  target_include_directories(Audio PUBLIC ${FFTW3_INCLUDE_DIRS})
else()
  target_link_libraries(Audio PUBLIC fftw3)
endif()

if(WIN32)
  # Windows 音频加速导出路径直接使用采样率转换和 SDL 头文件；通过包装 target 继承 shared/static 链接偏好。
  if(TARGET 3rd_libsamplerate)
    target_link_libraries(Audio PUBLIC 3rd_libsamplerate)
  else()
    target_link_libraries(Audio PUBLIC samplerate)
  endif()

  if(TARGET 3rd_sdl3)
    target_link_libraries(Audio PUBLIC 3rd_sdl3)
  elseif(PROJECT_LINKAGE STREQUAL "static" AND TARGET SDL3-static)
    target_link_libraries(Audio PUBLIC SDL3-static)
  endif()
endif()

target_include_directories(Audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

mmm_add_test_executable(Audio AudioSpeedExportServiceTest
                        tests/AudioSpeedExportServiceTest.cpp)
target_link_libraries(AudioSpeedExportServiceTest PRIVATE Audio Log Config)
add_test(NAME AudioSpeedExportServiceTest
         COMMAND AudioSpeedExportServiceTest "${MMM_TEST_RESOURCE_DIR}/ma"
                 "${CMAKE_BINARY_DIR}/test_output/audio_speed_export_coverage")

mmm_add_test_executable(Audio BackgroundSpectrumAnalyzerTest
                        tests/BackgroundSpectrumAnalyzerTest.cpp)
target_include_directories(BackgroundSpectrumAnalyzerTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(BackgroundSpectrumAnalyzerTest PRIVATE Audio Log Config)
add_test(NAME BackgroundSpectrumAnalyzerTest
         COMMAND BackgroundSpectrumAnalyzerTest)

# 绑定采样并发测试覆盖数十个独立播放进度，防止退化为单播放器切换资源。
mmm_add_test_executable(Audio BoundSoundConcurrencyTest
                        tests/BoundSoundConcurrencyTest.cpp)
target_link_libraries(BoundSoundConcurrencyTest PRIVATE Audio Log Config)
add_test(
  NAME BoundSoundConcurrencyTest
  COMMAND
    BoundSoundConcurrencyTest
    "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/audio/note.wav")
