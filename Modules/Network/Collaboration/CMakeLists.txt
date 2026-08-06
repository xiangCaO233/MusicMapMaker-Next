# 房主权威的增量协作内核；真实 WebRTC 与本地 Loopback 传输共用同一状态机。
add_library(
  NetworkCollaboration STATIC
  src/BeatmapDocumentCodec.cpp src/CollaborationPeer.cpp
  src/CollaborationProtocol.cpp src/CollaborationRoom.cpp
  src/LoopbackTransport.cpp src/WebRtcTransport.cpp)

target_include_directories(NetworkCollaboration
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(NetworkCollaboration PUBLIC MMM)
target_link_libraries(NetworkCollaboration PRIVATE 3rd_libdatachannel
                                                   3rd_nlohmann_json Config)

# 单进程创建 1 个房主和 7 个访客，验证增量顺序、去重和日志补发。
mmm_add_test_executable(Collaboration CollaborationPeerTest
                        tests/CollaborationPeerTest.cpp)
target_link_libraries(CollaborationPeerTest PRIVATE NetworkCollaboration)
add_test(NAME CollaborationPeerTest COMMAND CollaborationPeerTest)

# 本机真实 WebRTC 集成测试覆盖信令、DataChannel、Creator 身份和增量提交。
mmm_add_test_executable(Collaboration CollaborationRoomTest
                        tests/CollaborationRoomTest.cpp)
target_link_libraries(CollaborationRoomTest PRIVATE NetworkCollaboration)
add_test(NAME CollaborationRoomTest COMMAND CollaborationRoomTest)
