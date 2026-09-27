# Network 主库负责启动资源同步和版本更新检查。
add_library(Network STATIC src/AssetSyncService.cpp src/UpdateChecker.cpp)

# 联机写谱作为 Network 的内部子模块，统一承载信令、P2P 传输和房间协议。
add_subdirectory("Collaboration")

# 公网协作目录与信令中继使用独立可执行文件，部署时不依赖游戏 UI。
add_subdirectory("CollaborationServer")

# 对消费模块公开网络服务接口，不暴露 src 内部帮助器。
target_include_directories(Network PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# curl、miniz、JSON 与项目基础模块构成公开同步接口依赖。
target_link_libraries(Network PUBLIC 3rd_curl 3rd_miniz 3rd_nlohmann_json
                                     Config Log Event)
# Runtime 仅用于内部后台任务调度，不向上层传播。
target_link_libraries(Network PRIVATE Runtime)

if(WIN32)
  # Windows 更新替换流程使用 Shell API。
  target_link_libraries(Network PUBLIC shell32)
endif()

# 单元测试
mmm_add_test_executable(Network UpdateCheckerTest tests/UpdateCheckerTest.cpp)
if(WIN32 AND NOT MSVC)
  # MinGW 测试嵌入 UTF-8 manifest，避免系统路径按旧代码页解释。
  target_sources(UpdateCheckerTest PRIVATE tests/UpdateCheckerTest.rc)
endif()
target_link_libraries(UpdateCheckerTest PRIVATE Network Log)
target_include_directories(UpdateCheckerTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

mmm_add_test_executable(Network AssetSyncServiceTest
                        tests/AssetSyncServiceTest.cpp)
# 资源同步测试直接检查 ZIP 产物，需要显式链接 miniz。
target_link_libraries(AssetSyncServiceTest PRIVATE Network Log 3rd_miniz)
target_include_directories(AssetSyncServiceTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 两个目标均在 CTest 隔离配置环境中运行。
mmm_register_test_case(NAME UpdateCheckerTest COMMAND UpdateCheckerTest)
mmm_register_test_case(NAME AssetSyncServiceTest COMMAND AssetSyncServiceTest)
