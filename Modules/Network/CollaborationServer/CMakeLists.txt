# 中心信令服务只维护房间目录并转发 WebRTC 协商消息，不承载协作数据。
add_library(NetworkCollaborationServer STATIC
            src/CollaborationSignalingServer.cpp)

target_include_directories(NetworkCollaborationServer
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(NetworkCollaborationServer PUBLIC 3rd_libdatachannel
                                                        3rd_nlohmann_json Log)

# 独立部署入口从 JSON 配置读取监听地址、TLS 证书与 ICE 服务列表。
add_executable(MusicMapMaker-CollaborationServer src/main.cpp)
target_link_libraries(MusicMapMaker-CollaborationServer
                      PRIVATE NetworkCollaborationServer)

# 覆盖房间注册、目录广播、访客配对和双向透明信令转发。
mmm_add_test_executable(CollaborationServer CollaborationSignalingServerTest
                        tests/CollaborationSignalingServerTest.cpp)
target_link_libraries(CollaborationSignalingServerTest
                      PRIVATE NetworkCollaborationServer)
add_test(NAME CollaborationSignalingServerTest
         COMMAND CollaborationSignalingServerTest)
