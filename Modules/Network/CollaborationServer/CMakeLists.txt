# 中心信令服务只维护房间目录并转发 WebRTC 协商消息，不承载协作数据。
add_library(NetworkCollaborationServer STATIC
            src/CollaborationSignalingServer.cpp)

# 公共头暴露可嵌入测试的信令服务类。
target_include_directories(NetworkCollaborationServer
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# libdatachannel 提供 WebSocket，JSON 承载信令帧，Log 记录连接诊断。
target_link_libraries(NetworkCollaborationServer PUBLIC 3rd_libdatachannel
                                                        3rd_nlohmann_json Log)

# 独立部署入口从 JSON 配置读取监听地址、TLS 证书与 ICE 服务列表。
add_executable(MusicMapMaker-CollaborationServer src/main.cpp)
# 部署入口只依赖本子模块，不链接游戏客户端模块。
target_link_libraries(MusicMapMaker-CollaborationServer
                      PRIVATE NetworkCollaborationServer)

# 覆盖房间注册、目录广播、访客配对和双向透明信令转发。
mmm_add_test_executable(CollaborationServer CollaborationSignalingServerTest
                        tests/CollaborationSignalingServerTest.cpp)
target_link_libraries(CollaborationSignalingServerTest
                      PRIVATE NetworkCollaborationServer)
# 服务端测试由 CTest 直接运行，无需外部信令服务进程。
mmm_register_test_case(NAME CollaborationSignalingServerTest COMMAND
                       CollaborationSignalingServerTest)
