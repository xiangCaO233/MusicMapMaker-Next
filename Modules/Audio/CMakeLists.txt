# Modules/Audio/CMakeLists.txt

# cmake-format: off
# Audio 静态库集中承载播放后端、复合时间线、音效池、离线导出和分析服务。
# 源文件按职责显式列出，避免通配符让新增实现未经评审自动进入所有平台构建。
# AudioManager 的分文件实现仍链接为同一 target，公开 API 只从 include/audio 导出。
#
# 源文件职责：
# - AudioTimeline* 负责复合时间线、资源准备、传输和导出。
# - AudioManager.cpp 负责后端生命周期和根音频图。
# - AudioManager_BGM.cpp 负责主时间线、试听与缓存。
# - AudioManager_Playback.cpp 负责播放和时间控制。
# - AudioManager_SFX.cpp 与 SoundEffectPool.cpp 负责并发音效。
# - AudioManager_EQ.cpp 与 AudioManager_Mix.cpp 负责图参数控制。
# - BackgroundSpectrumAnalyzer 负责后台频谱快照。
# - AudioOriginAlignmentService 与 AudioSpeedExportService 负责离线处理。
# - KeySoundControl 负责固定大小实时控制快照。
#
# 测试边界：
# - 纯算法测试直接驱动节点，不打开系统声卡。
# - 资源测试只读项目短音频或 tests/data。
# - 生成文件统一写入构建树 test_output。
# - AudioManager 集成测试使用 SDL dummy 后端。
# - ICE 内部实时测试仅在 SOURCES_BUILD 下创建。
# - 普通公开行为测试在预编译依赖模式继续构建。
# - 资源路径由 CMake 展开，不依赖执行工作目录。
# - tests/data 中的 Git LFS 资源保持只读。
#
# 依赖模式约束：
# - SOURCES_BUILD 关闭时不得引用当前 ICE 私有实现。
# - SOURCES_BUILD 开启时实时测试链接同次 ICE 源码。
# - Windows 优先使用项目包装的第三方 target。
# - PUBLIC 依赖保持 Audio 公开接口的传递契约。
# cmake-format: on

add_library(
  Audio STATIC
  # 复合时间线从离线导出到实时传输保持同一资源语义。
  src/AudioTimelineExportService.cpp
  src/AudioTimelineMixerNode.cpp
  src/AudioTimelineResourceProcessor.cpp
  src/AudioTimelineTransport.cpp
  # 频谱与原点对齐是资源分析和修正服务，不参与播放状态机。
  src/BackgroundSpectrumAnalyzer.cpp
  src/AudioManager.cpp
  src/AudioOriginAlignmentService.cpp
  # AudioManager 按职责拆分实现，仍共同访问同一私有状态。
  src/AudioManager_BGM.cpp
  src/AudioManager_EQ.cpp
  src/AudioManager_Mix.cpp
  src/AudioManager_Playback.cpp
  src/AudioManager_SFX.cpp
  # 独立服务与固定控制库放在管理器实现之后，便于审查源清单。
  src/AudioSpeedExportService.cpp
  src/KeySoundControl.cpp
  src/SoundEffectPool.cpp)

target_link_libraries(
  Audio
  # ICE、事件、运行时、项目模型和配置均出现在公开 AudioManager 契约中，使用 PUBLIC
  # 保证消费方获得一致包含路径与链接依赖；Rubber Band 支撑变速处理。
  PUBLIC ICE::IonCachyEngine
         # 日志与事件由公开音频操作及其调用方共同使用。
         Log
         Event
         # Runtime 提供解码线程池，Rubber Band 提供资源和预览变速。
         Runtime
         3rd_rubberband
         # 项目模型和配置类型出现在 AudioManager 的公开加载接口中。
         MMM
         Config)

if(WIN32)
  # Windows 音频加速导出路径直接使用采样率转换和 SDL 头文件；优先链接项目 包装 target，以继承 static/shared
  # 选择和对应运行库配置。
  if(TARGET 3rd_libsamplerate)
    target_link_libraries(Audio PUBLIC 3rd_libsamplerate)
  else()
    target_link_libraries(Audio PUBLIC samplerate)
  endif()

  if(TARGET 3rd_sdl3)
    target_link_libraries(Audio PUBLIC 3rd_sdl3)
  elseif(PROJECT_LINKAGE STREQUAL "static" AND TARGET SDL3-static)
    # 源码依赖未提供包装 target 时，静态构建兼容 SDL 官方导出名称。
    target_link_libraries(Audio PUBLIC SDL3-static)
  endif()
endif()

target_include_directories(Audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 所有测试通过统一 helper 输出到 build/tests/Audio，并自动带入项目测试隔离源。 测试生成文件必须写入
# build/test_output，源码 tests/data 只作为只读资源基线。

# Key 音控制测试覆盖默认值、增益组合、越界和并发完整快照。
mmm_add_test_executable(Audio KeySoundControlTest tests/KeySoundControlTest.cpp)
# 只链接正式 Audio 接口和日志封装，不依赖文件资源。
target_link_libraries(KeySoundControlTest PRIVATE Audio Log)
add_test(NAME KeySoundControlTest COMMAND KeySoundControlTest)

mmm_add_test_executable(Audio AudioSpeedExportServiceTest
                        tests/AudioSpeedExportServiceTest.cpp)
# 导出覆盖遍历共享谱面音频数据，并把转码结果写到独立构建输出目录。
target_link_libraries(AudioSpeedExportServiceTest PRIVATE Audio Log Config)
# 两个参数分别提供只读输入资源根和构建树输出目录。
add_test(NAME AudioSpeedExportServiceTest
         COMMAND AudioSpeedExportServiceTest "${MMM_TEST_RESOURCE_DIR}/ma"
                 "${CMAKE_BINARY_DIR}/test_output/audio_speed_export_coverage")

# 原点对齐测试覆盖裁切开头、补前置静音和零相位重编码。
mmm_add_test_executable(Audio AudioOriginAlignmentServiceTest
                        tests/AudioOriginAlignmentServiceTest.cpp)
target_link_libraries(AudioOriginAlignmentServiceTest PRIVATE Audio Log Runtime)
# 输出路径在编译期固定，测试不根据执行目录推断构建树。
target_compile_definitions(
  AudioOriginAlignmentServiceTest
  PRIVATE MMM_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test_output")
add_test(NAME AudioOriginAlignmentServiceTest
         COMMAND AudioOriginAlignmentServiceTest)

mmm_add_test_executable(Audio BackgroundSpectrumAnalyzerTest
                        tests/BackgroundSpectrumAnalyzerTest.cpp)
# 分析器实现头位于私有 src，只有白盒测试额外获取该目录，不扩大 Audio 接口。
target_include_directories(BackgroundSpectrumAnalyzerTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# 私有包含仅向该白盒测试暴露，不进入 Audio 的 PUBLIC 接口。
target_link_libraries(BackgroundSpectrumAnalyzerTest PRIVATE Audio Log Config)
add_test(NAME BackgroundSpectrumAnalyzerTest
         COMMAND BackgroundSpectrumAnalyzerTest)

# 绑定采样并发测试覆盖数十个独立播放进度，防止退化为单播放器切换资源。 默认皮肤 note.wav 是短小、稳定且由项目管理的已缓存音效夹具。
mmm_add_test_executable(Audio BoundSoundConcurrencyTest
                        tests/BoundSoundConcurrencyTest.cpp)
target_link_libraries(BoundSoundConcurrencyTest PRIVATE Audio Log Config)
# 测试直接驱动 MixBus，不需要设置系统音频后端环境。
add_test(
  NAME BoundSoundConcurrencyTest
  COMMAND
    BoundSoundConcurrencyTest
    "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/audio/note.wav")

# 音频时间线传输测试不依赖声卡，覆盖 seek、循环和负起点等确定性调度边界。 该目标只链接 Audio，不需要日志或应用配置初始化。
mmm_add_test_executable(Audio AudioTimelineTransportTest
                        tests/AudioTimelineTransportTest.cpp)
target_link_libraries(AudioTimelineTransportTest PRIVATE Audio)
# 纯状态机测试没有额外资源参数，可由目标文件直接执行。
add_test(NAME AudioTimelineTransportTest COMMAND AudioTimelineTransportTest)

# 时间线混音测试使用已缓存短音频，覆盖负起点、多 voice、Seek、暂停、循环和 自然结束；直接驱动节点 process，不要求系统音频输出设备。
mmm_add_test_executable(Audio AudioTimelineMixerNodeTest
                        tests/AudioTimelineMixerNodeTest.cpp)
target_link_libraries(AudioTimelineMixerNodeTest PRIVATE Audio Log Config)
add_test(
  NAME AudioTimelineMixerNodeTest
  COMMAND
    AudioTimelineMixerNodeTest
    "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/audio/note.wav")

# 资源级离线 DSP 测试覆盖独立音量、静音、变速、变调和 EQ 语义。 与混音测试共用短样本，重点检查处理后 PCM 身份和帧数而非设备播放。
mmm_add_test_executable(Audio AudioTimelineResourceProcessorTest
                        tests/AudioTimelineResourceProcessorTest.cpp)
target_link_libraries(AudioTimelineResourceProcessorTest PRIVATE Audio Log
                                                                 Config)
add_test(
  NAME AudioTimelineResourceProcessorTest
  COMMAND
    AudioTimelineResourceProcessorTest
    "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/audio/note.wav")

# AudioManager 集成测试通过 SDL dummy 后端覆盖零、单个和复合自动采样时间线。 dummy
# 环境只替换系统输出驱动，仍启动真实回调线程验证调度接管和自然结束。
mmm_add_test_executable(Audio AudioManagerTimelineIntegrationTest
                        tests/AudioManagerTimelineIntegrationTest.cpp)
target_link_libraries(AudioManagerTimelineIntegrationTest PRIVATE Audio Log
                                                                  Config)
add_test(
  NAME AudioManagerTimelineIntegrationTest
  COMMAND
    AudioManagerTimelineIntegrationTest
    "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/audio/note.wav")
set_tests_properties(AudioManagerTimelineIntegrationTest
                     PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")

# 下列实时安全测试直接依赖当前 ICE 内部实现和新增接口。使用预编译 ICE 时不创建 目标，避免新测试源与旧二进制 ABI 混用；主 Audio
# 行为测试仍保持可构建。
if(SOURCES_BUILD)
  # MixBus 实时安全测试覆盖不可变来源快照、并发回收、稳定顺序和预备容量边界。
  mmm_add_test_executable(Audio MixBusRealtimeSafetyTest
                          tests/MixBusRealtimeSafetyTest.cpp)
  target_link_libraries(MixBusRealtimeSafetyTest PRIVATE ICE::IonCachyEngine
                                                         Log)
  add_test(NAME MixBusRealtimeSafetyTest COMMAND MixBusRealtimeSafetyTest)

  # GraphicEqualizer 实时安全测试覆盖固定预备容量、并发参数发布、控制侧回收和 回调零堆操作。
  mmm_add_test_executable(Audio GraphicEqualizerRealtimeTest
                          tests/GraphicEqualizerRealtimeTest.cpp)
  target_link_libraries(GraphicEqualizerRealtimeTest PRIVATE ICE::IonCachyEngine
                                                             Log)
  add_test(NAME GraphicEqualizerRealtimeTest
           COMMAND GraphicEqualizerRealtimeTest)

  # SourceNode 实时安全测试覆盖 block 内起播、provider 并发发布和回调零分配。 同样使用短 note.wav，但测试关注绝对调度
  # provider 和无分配回调契约。
  mmm_add_test_executable(Audio SourceNodeScheduledRealtimeTest
                          tests/SourceNodeScheduledRealtimeTest.cpp)
  target_link_libraries(SourceNodeScheduledRealtimeTest
                        PRIVATE ICE::IonCachyEngine Log)
  add_test(
    NAME SourceNodeScheduledRealtimeTest
    COMMAND
      SourceNodeScheduledRealtimeTest
      "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/audio/note.wav")
endif()

# 流式回归必须链接本次引擎源码，旧预编译包不具备当前页缓存与精确回退能力。
if(SOURCES_BUILD)
  # 覆盖阻塞 IO 下回调立即返回、页淘汰与精确回退，以及真实文件策略切换。
  mmm_add_test_executable(Audio StreamingDecoderTest
                          tests/StreamingDecoderTest.cpp)
  target_link_libraries(StreamingDecoderTest PRIVATE Audio Config Log)
  add_test(
    NAME StreamingDecoderTest
    COMMAND
      StreamingDecoderTest
      "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/audio/note.wav")
  # 门闩或析构协议回归不能让整个 CTest 无期限挂起。 三十秒只约束测试进程上界，不进入产品播放或同步逻辑。
  set_tests_properties(StreamingDecoderTest PROPERTIES TIMEOUT 30)
endif()
