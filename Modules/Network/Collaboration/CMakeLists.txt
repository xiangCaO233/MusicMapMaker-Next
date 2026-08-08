# 房主权威的增量协作内核；真实 WebRTC 与本地 Loopback 传输共用同一状态机。
add_library(
  NetworkCollaboration STATIC
  src/BeatmapDocumentCodec.cpp
  src/CollaborationPeer.cpp
  src/CollaborationDirectoryClient.cpp
  src/CollaborationResourceSync.cpp
  src/CollaborationProtocol.cpp
  src/CollaborationRoom.cpp
  src/LoopbackTransport.cpp
  src/RtcDiagnosticLogging.cpp
  src/WebRtcTransport.cpp)

target_include_directories(NetworkCollaboration
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(NetworkCollaboration PUBLIC MMM)
target_link_libraries(
  NetworkCollaboration PRIVATE 3rd_libdatachannel 3rd_nlohmann_json 3rd_miniz
                               Config Log Runtime)

# RTC 诊断测试覆盖配置持久化以及 TURN 长期凭据脱敏。
mmm_add_test_executable(Collaboration RtcDiagnosticLoggingTest
                        tests/RtcDiagnosticLoggingTest.cpp)
target_include_directories(RtcDiagnosticLoggingTest
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(RtcDiagnosticLoggingTest PRIVATE NetworkCollaboration
                                                       Config)
add_test(NAME RtcDiagnosticLoggingTest COMMAND RtcDiagnosticLoggingTest)

# 单进程创建 1 个房主和 7 个访客，验证增量顺序、去重和日志补发。
mmm_add_test_executable(Collaboration CollaborationPeerTest
                        tests/CollaborationPeerTest.cpp)
target_link_libraries(CollaborationPeerTest PRIVATE NetworkCollaboration)
add_test(NAME CollaborationPeerTest COMMAND CollaborationPeerTest)

# 视野同步设置测试覆盖默认频率以及 5～60 Hz 房间层边界限制。
mmm_add_test_executable(Collaboration CollaborationViewportSettingsTest
                        tests/CollaborationViewportSettingsTest.cpp)
target_link_libraries(CollaborationViewportSettingsTest
                      PRIVATE NetworkCollaboration)
add_test(NAME CollaborationViewportSettingsTest
         COMMAND CollaborationViewportSettingsTest)

# 谱面文档测试覆盖全部对象类型、元数据、快照和分类增量往返。
mmm_add_test_executable(Collaboration BeatmapDocumentCodecTest
                        tests/BeatmapDocumentCodecTest.cpp)
target_link_libraries(BeatmapDocumentCodecTest PRIVATE NetworkCollaboration)
add_test(NAME BeatmapDocumentCodecTest COMMAND BeatmapDocumentCodecTest)

# 资源状态机测试覆盖清单完整性、分块重组、缓存复用和单文件增量更新。
mmm_add_test_executable(Collaboration CollaborationResourceSyncTest
                        tests/CollaborationResourceSyncTest.cpp)
target_link_libraries(CollaborationResourceSyncTest
                      PRIVATE NetworkCollaboration 3rd_nlohmann_json Runtime)
add_test(NAME CollaborationResourceSyncTest
         COMMAND CollaborationResourceSyncTest)

# 本机真实 WebRTC 集成测试覆盖中心信令、瞬时释放、DataChannel、身份和资源同步。
mmm_add_test_executable(Collaboration CollaborationRoomTest
                        tests/CollaborationRoomTest.cpp)
target_link_libraries(CollaborationRoomTest PRIVATE NetworkCollaboration
                                                    NetworkCollaborationServer)
# 同进程混用 libdatachannel 服务端与 WebRTC 客户端会受全局句柄调度影响。 `p2p`、`resource` 与 `external`
# 链路保留为手动部署探针。 稳定自动覆盖由状态机、资源同步、目录与服务端测试共同承担。

# 公网目录客户端测试覆盖房间列表订阅、刷新与异常消息隔离。
mmm_add_test_executable(Collaboration CollaborationDirectoryClientTest
                        tests/CollaborationDirectoryClientTest.cpp)
target_link_libraries(CollaborationDirectoryClientTest
                      PRIVATE NetworkCollaboration NetworkCollaborationServer)
add_test(NAME CollaborationDirectoryClientTest
         COMMAND CollaborationDirectoryClientTest)
