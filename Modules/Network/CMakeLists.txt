# Network 主库负责启动资源同步和版本更新检查。
add_library(Network STATIC src/AssetSyncService.cpp src/UpdateChecker.cpp
                           src/CurlHandle.cpp src/DownloadRouting.cpp)

# 联机写谱作为 Network 的内部子模块，统一承载信令、P2P 传输和房间协议。
add_subdirectory("Collaboration")

# 公网协作目录与信令中继使用独立可执行文件，部署时不依赖游戏 UI。 APK 只携带客户端，手机启动时不创建服务端命令行进程。
# 此分支不裁掉编辑器已有的网络与房间客户端接口。
if(NOT ANDROID)
  add_subdirectory("CollaborationServer")
endif()

# 对消费模块公开网络服务接口，不暴露 src 内部帮助器。
target_include_directories(Network PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# curl、miniz、JSON 与项目基础模块构成公开同步接口依赖。
target_link_libraries(Network PUBLIC 3rd_curl 3rd_miniz 3rd_nlohmann_json
                                     Config Log Event)
# Runtime 仅用于内部后台任务调度，不向上层传播。
target_link_libraries(Network PRIVATE Runtime)

if(WIN32)
  # 更新替换使用 Shell API；后台选路通过系统 Winsock 解析当前地址。
  target_link_libraries(
    Network
    PUBLIC shell32
    PRIVATE ws2_32)
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

# 默认检查离线信任边界；设备验收可向同一用例显式传入证书文件和 HTTPS 地址。 不把外网或模拟器可用性作为桌面完整测试的前置条件。
mmm_add_test_executable(Network CurlTrustTest tests/CurlTrustTest.cpp)
target_link_libraries(CurlTrustTest PRIVATE Network Log)
mmm_register_test_case(NAME CurlTrustTest COMMAND CurlTrustTest)

# 共用生产下载器验证取消、文件身份、错误 Range 与中断续传。 入口进入既有 Network 套件，不增加独立测试程序数量。
# 输出路径由构建树传入，不能写入 tests/data。 单项入口可供 CTest 与人工公网验收复用。
mmm_add_test_executable(Network DownloadRoutingTest
                        tests/DownloadRoutingTest.cpp)
target_link_libraries(DownloadRoutingTest PRIVATE Network Log)
mmm_register_test_case(NAME DownloadRoutingTest COMMAND DownloadRoutingTest
                       "${CMAKE_BINARY_DIR}/test_output/download-routing")
# TLS 夹具只监听回环地址；证书和输出均生成于构建树，不访问公网。 Python 驱动已编译的生产下载器，不使用传输实现替身。 OpenSSL
# 只生成临时证书，不链接业务程序或进入安装包。 完整测试开关关闭时不探测这些可选工具。 缺少工具仍能显式运行离线契约测试。 Android
# 不在设备内启动桌面夹具，仍编译同一业务实现。 每个故障场景独立启动进程，避免选路缓存污染后续结果。
if(BUILD_TESTING AND NOT ANDROID)
  # 解释器仅用于桌面测试驱动，不传播为 Network 的公开依赖。
  find_package(Python3 QUIET COMPONENTS Interpreter)
  # 查找命令行证书工具，不改变项目预编译 OpenSSL 的选择。
  find_program(MMM_DOWNLOAD_TEST_OPENSSL openssl)
  # 需要两项工具均存在，才能注册完整本地 TLS 故障夹具。
  if(Python3_Interpreter_FOUND AND MMM_DOWNLOAD_TEST_OPENSSL)
    # 对象测试目标没有独立程序，必须传入所属套件的真实入口。
    get_target_property(MMM_DOWNLOAD_TEST_SUITE DownloadRoutingTest
                        MMM_TEST_SUITE)
    # 生成器表达式保留多配置构建的 Debug 与发布配置路径。
    add_test(
      NAME DownloadRoutingHttpsTest
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/download-routing-https.py"
        "$<TARGET_FILE:${MMM_DOWNLOAD_TEST_SUITE}>"
        "${CMAKE_BINARY_DIR}/test_output/download-routing-https"
        "${MMM_DOWNLOAD_TEST_OPENSSL}")
    # 证书和结果与离线用例分目录，失败时可独立保留请求证据。
  endif()
  # 缺少工具不执行外网替代测试；离线入口仍可单独运行。
endif()
