# 房主权威的增量协作内核；真实 WebRTC 与本地 Loopback 传输共用同一状态机。
add_library(
  Collaboration STATIC src/CollaborationPeer.cpp src/CollaborationProtocol.cpp
                       src/LoopbackTransport.cpp)

target_include_directories(Collaboration
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(Collaboration PRIVATE 3rd_libdatachannel)

# 单进程创建 1 个房主和 7 个访客，验证增量顺序、去重和日志补发。
mmm_add_test_executable(Collaboration CollaborationPeerTest
                        tests/CollaborationPeerTest.cpp)
target_link_libraries(CollaborationPeerTest PRIVATE Collaboration)
add_test(NAME CollaborationPeerTest COMMAND CollaborationPeerTest)
