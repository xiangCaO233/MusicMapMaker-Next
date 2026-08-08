# 运行期基础设施模块。

add_library(Runtime STATIC src/AppThreadPool.cpp src/ShutdownWatchdog.cpp)

target_include_directories(Runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
  Runtime
  PUBLIC ICE::IonCachyEngine
  PRIVATE Log)

# 退出看门狗测试覆盖正常完成取消与超时触发，避免依赖真实进程终止。
mmm_add_test_executable(Runtime ShutdownWatchdogTest
                        tests/ShutdownWatchdogTest.cpp)
target_link_libraries(ShutdownWatchdogTest PRIVATE Runtime)
add_test(NAME ShutdownWatchdogTest COMMAND ShutdownWatchdogTest)
