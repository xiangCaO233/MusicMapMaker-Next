add_library(Network STATIC src/AssetSyncService.cpp src/UpdateChecker.cpp)

# 联机写谱作为 Network 的内部子模块，统一承载信令、P2P 传输和房间协议。
add_subdirectory("Collaboration")

# 公网协作目录与信令中继使用独立可执行文件，部署时不依赖游戏 UI。
add_subdirectory("CollaborationServer")

target_include_directories(Network PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(Network PUBLIC 3rd_curl 3rd_miniz 3rd_nlohmann_json
                                     Config Log Event)
target_link_libraries(Network PRIVATE Runtime)

if(WIN32)
  target_link_libraries(Network PUBLIC shell32)
endif()

# 单元测试
mmm_add_test_executable(Network UpdateCheckerTest tests/UpdateCheckerTest.cpp)
if(WIN32 AND NOT MSVC)
  target_sources(UpdateCheckerTest PRIVATE tests/UpdateCheckerTest.rc)
endif()
target_link_libraries(UpdateCheckerTest PRIVATE Network Log)
target_include_directories(UpdateCheckerTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

mmm_add_test_executable(Network AssetSyncServiceTest
                        tests/AssetSyncServiceTest.cpp)
target_link_libraries(AssetSyncServiceTest PRIVATE Network Log 3rd_miniz)
target_include_directories(AssetSyncServiceTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_test(NAME UpdateCheckerTest COMMAND UpdateCheckerTest)
add_test(NAME AssetSyncServiceTest COMMAND AssetSyncServiceTest)
