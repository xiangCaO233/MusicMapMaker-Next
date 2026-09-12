# 运行期基础设施模块。 线程池与退出看门狗必须由同一目标提供，确保应用生命周期只有一个所有权入口。

add_library(Runtime STATIC src/AppThreadPool.cpp src/ShutdownWatchdog.cpp)

# 公共头目录暴露轻量运行期接口，具体线程池实现仍由源文件包含。
target_include_directories(Runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ICE 线程池类型属于公开接口依赖；日志仅在 Runtime 实现内部使用。
target_link_libraries(
  Runtime
  PUBLIC ICE::IonCachyEngine
  PRIVATE Log)

# 退出看门狗测试覆盖正常完成取消与超时触发，避免依赖真实进程终止。
mmm_add_test_executable(Runtime ShutdownWatchdogTest
                        tests/ShutdownWatchdogTest.cpp)
target_link_libraries(ShutdownWatchdogTest PRIVATE Runtime)
# 直接注册短时独立测试，避免应用主循环参与退出时序验证。
add_test(NAME ShutdownWatchdogTest COMMAND ShutdownWatchdogTest)
