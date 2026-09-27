# Modules/Game/Common/CMakeLists.txt

# Common 汇集跨 Logic、Canvas、UI 与 Graphic 共享的轻量实现。 实现文件包含音频探测、无锁渲染快照池和视频帧解码器。
add_library(
  Common STATIC
  # 音频探测实现通过 ICE 获取统一媒体信息。
  src/common/AudioInfoUtils.cpp src/common/RenderSnapshotBuffer.cpp
  # 视频解码实现私有使用 FFmpeg，不泄漏其头文件。
  src/common/VideoFrameDecoder.cpp)

# 公共头直接暴露项目模型、配置路径、日志及轻量第三方数据类型。 这些目录必须保持 PUBLIC，保证消费模块能够独立编译公共接口。
target_include_directories(
  Common
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
         ${PROJECT_SOURCE_DIR}/3rdpty/sources/entt/src
         ${PROJECT_SOURCE_DIR}/3rdpty/sources/nlohmann_json/include
         ${PROJECT_SOURCE_DIR}/Modules/Config/include
         ${PROJECT_SOURCE_DIR}/Modules/MMM/include
         ${PROJECT_SOURCE_DIR}/Modules/Log/include)

# Log、ICE、ConcurrentQueue 与 GLM 出现在公共契约或内联实现中。 FFmpeg 仅由 VideoFrameDecoder.cpp
# 使用，不向上层传播链接要求。
target_link_libraries(
  Common
  # 公共依赖随 Common 传播给使用内联 DTO 与快照类型的模块。
  PUBLIC Log ICE::IonCachyEngine 3rd_concurrentqueue 3rd_glm
  # 私有解码依赖只参与 Common 本身链接。
  PRIVATE 3rd_ffmpeg)

# 视频解码测试使用独立输出目录写入生成帧，禁止污染源码测试资源。
mmm_add_test_executable(Common VideoFrameDecoderTest
                        tests/VideoFrameDecoderTest.cpp)
target_link_libraries(VideoFrameDecoderTest PRIVATE Common 3rd_ffmpeg)
# 测试运行时不读取个人配置，输出仅写入构建树的 test_output。 CTest 将输出路径显式传给测试进程，便于隔离并复查产物。
mmm_register_test_case(NAME VideoFrameDecoderTest COMMAND VideoFrameDecoderTest
                       "${CMAKE_BINARY_DIR}/test_output/video_frame_decoder")

# 音频资源拖拽载荷测试保证跨模块共享的固定布局与长度拒绝规则稳定。 测试只依赖 Common 导出的协议和项目音频资源枚举。
mmm_add_test_executable(Common AudioResourceDragPayloadTest
                        tests/AudioResourceDragPayloadTest.cpp)
target_link_libraries(AudioResourceDragPayloadTest PRIVATE Common)
# 载荷测试无需命令行参数，直接注册可执行文件即可。
mmm_register_test_case(NAME AudioResourceDragPayloadTest COMMAND
                       AudioResourceDragPayloadTest)

# 单文件兼容测试覆盖复合音频时间线的显式拒绝条件。 MMM 提供真实 BeatMap 与 Project 模型，避免测试复制不完整 DTO。
mmm_add_test_executable(Common BeatmapAudioTimelineCompatibilityTest
                        tests/BeatmapAudioTimelineCompatibilityTest.cpp)
target_link_libraries(BeatmapAudioTimelineCompatibilityTest PRIVATE Common MMM)
mmm_register_test_case(NAME BeatmapAudioTimelineCompatibilityTest COMMAND
                       BeatmapAudioTimelineCompatibilityTest)

# Linux 旧预编译引擎尚未包含流级标签修复，只为源码构建和已同步的 MSVC 静态包登记此回归，避免默认构建把依赖版本差异误报为本层探测失败。
if(SOURCES_BUILD
   OR (WIN32
       AND MSVC
       AND PROJECT_LINKAGE STREQUAL "static"))
  # Ogg 标签可能在音频流而非容器字典；测试走向导使用的 Common DTO。
  mmm_add_test_executable(Common AudioInfoUtilsTest
                          tests/AudioInfoUtilsTest.cpp)
  # Common 负责传播 ICE 链接关系，测试不建立私有的探测实现。
  target_link_libraries(AudioInfoUtilsTest PRIVATE Common)
  # 两份夹具都是只读输入，测试不往源资源目录写生成文件。
  mmm_register_test_case(
    NAME
    AudioInfoUtilsTest
    COMMAND
    AudioInfoUtilsTest
    # 首个资源的音频流提供标题、艺术家和专辑标签。
    "${PROJECT_SOURCE_DIR}/tests/data/ma/Don't Fight The Music/Don't Fight The Music.ogg"
    # 第二个资源的缺失标签必须保持空或退回文件名。
    "${PROJECT_SOURCE_DIR}/tests/data/ma/雲散霧消/audio.ogg")
endif()
