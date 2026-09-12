# 房主权威的增量协作内核；真实 WebRTC 与本地 Loopback 传输共用同一状态机。
add_library(
  NetworkCollaboration STATIC
  src/BeatmapDocumentCodec.cpp
  src/CollaborationBuildFingerprint.cpp
  src/CollaborationPeer.cpp
  src/CollaborationDirectoryClient.cpp
  src/CollaborationResourceCipher.cpp
  src/CollaborationResourceSync.cpp
  src/CollaborationProtocol.cpp
  src/CollaborationRoom.cpp
  src/LoopbackTransport.cpp
  src/RtcDiagnosticLogging.cpp
  src/WebRtcTransport.cpp)

# 对外只暴露协作协议、房间状态机和传输抽象接口。
target_include_directories(NetworkCollaboration
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# MMM 模型出现在公开协作文档和操作类型中，必须向消费端传播。
target_link_libraries(NetworkCollaboration PUBLIC MMM)
# 其余加密、RTC、队列和运行时依赖均由实现私有使用。
target_link_libraries(
  NetworkCollaboration
  PRIVATE 3rd_concurrentqueue
          3rd_libdatachannel
          3rd_mbedtls
          3rd_nlohmann_json
          3rd_miniz
          Config
          Log
          Network
          Runtime)

# RTC 诊断测试覆盖配置持久化以及 TURN 长期凭据脱敏。
mmm_add_test_executable(Collaboration RtcDiagnosticLoggingTest
                        tests/RtcDiagnosticLoggingTest.cpp)
# 测试额外访问仅供本模块验证的脱敏内部头。
target_include_directories(RtcDiagnosticLoggingTest
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Config 用于验证诊断开关的应用级持久化边界。
target_link_libraries(RtcDiagnosticLoggingTest PRIVATE NetworkCollaboration
                                                       Config)
# 该测试不启动真实 RTC 连接，仅检查日志和配置契约。
add_test(NAME RtcDiagnosticLoggingTest COMMAND RtcDiagnosticLoggingTest)

# 构建指纹测试覆盖后台任务投递、非阻塞状态查询和最终 SHA-256 格式。
mmm_add_test_executable(Collaboration CollaborationBuildFingerprintTest
                        tests/CollaborationBuildFingerprintTest.cpp)
# Runtime 提供与应用一致的后台线程池生命周期。
target_link_libraries(CollaborationBuildFingerprintTest
                      PRIVATE NetworkCollaboration Runtime)
# 指纹测试会等待后台任务完成，但不依赖外部网络。
add_test(NAME CollaborationBuildFingerprintTest
         COMMAND CollaborationBuildFingerprintTest)

# 单进程创建 1 个房主和 7 个访客，验证增量顺序、去重和日志补发。
mmm_add_test_executable(Collaboration CollaborationPeerTest
                        tests/CollaborationPeerTest.cpp)
# Peer 测试使用 LoopbackTransport，不需要 RTC 服务。
target_link_libraries(CollaborationPeerTest PRIVATE NetworkCollaboration)
# 多参与者状态机作为普通 CTest 回归项运行。
add_test(NAME CollaborationPeerTest COMMAND CollaborationPeerTest)

# 视野同步设置测试覆盖默认频率以及 5～60 Hz 房间层边界限制。
mmm_add_test_executable(Collaboration CollaborationViewportSettingsTest
                        tests/CollaborationViewportSettingsTest.cpp)
# 视野频率边界只依赖房间公共接口。
target_link_libraries(CollaborationViewportSettingsTest
                      PRIVATE NetworkCollaboration)
# 测试不创建传输实例，可快速验证默认值和钳制。
add_test(NAME CollaborationViewportSettingsTest
         COMMAND CollaborationViewportSettingsTest)

# 谱面文档测试覆盖全部对象类型、元数据、快照和分类增量往返。
mmm_add_test_executable(Collaboration BeatmapDocumentCodecTest
                        tests/BeatmapDocumentCodecTest.cpp)
# 文档往返通过协作库访问正式 JSON 编解码实现。
target_link_libraries(BeatmapDocumentCodecTest PRIVATE NetworkCollaboration)
# 覆盖测试不写入源码资源目录。
add_test(NAME BeatmapDocumentCodecTest COMMAND BeatmapDocumentCodecTest)

# 资源状态机测试覆盖清单完整性、分块重组、缓存复用和单文件增量更新。
mmm_add_test_executable(Collaboration CollaborationResourceSyncTest
                        tests/CollaborationResourceSyncTest.cpp)
# 测试访问资源加密内部头以验证完整传输往返。
target_include_directories(CollaborationResourceSyncTest
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# JSON、miniz 和 Runtime 支撑清单、压缩及后台任务测试路径。
target_link_libraries(CollaborationResourceSyncTest
                      PRIVATE NetworkCollaboration 3rd_nlohmann_json Runtime)
# 生成资源只写入构建树测试输出目录。
add_test(NAME CollaborationResourceSyncTest
         COMMAND CollaborationResourceSyncTest)

# 本机真实 WebRTC 集成测试覆盖中心信令、瞬时释放、DataChannel、身份和资源同步。
mmm_add_test_executable(Collaboration CollaborationRoomTest
                        tests/CollaborationRoomTest.cpp)
# 房间集成目标同时链接客户端状态机和可嵌入信令服务。
target_link_libraries(
  CollaborationRoomTest PRIVATE NetworkCollaboration NetworkCollaborationServer
                                Runtime)
# 该目标故意不注册自动 CTest，避免同进程 RTC 客户端与服务端全局状态竞争。 同进程混用 libdatachannel 服务端与 WebRTC
# 客户端会受全局句柄调度影响。 `p2p`、`resource` 与 `external` 链路保留为手动部署探针。
# 稳定自动覆盖由状态机、资源同步、目录与服务端测试共同承担。

# 公网目录客户端测试覆盖房间列表订阅、刷新与异常消息隔离。
mmm_add_test_executable(Collaboration CollaborationDirectoryClientTest
                        tests/CollaborationDirectoryClientTest.cpp)
# 目录测试在进程内启动信令服务并由客户端订阅房间列表。
target_link_libraries(CollaborationDirectoryClientTest
                      PRIVATE NetworkCollaboration NetworkCollaborationServer)
# 该稳定路径保留为自动 CTest，替代易受全局 RTC 句柄影响的房间探针。
add_test(NAME CollaborationDirectoryClientTest
         COMMAND CollaborationDirectoryClientTest)
