# 3rdpty/sources/cmake/BuildIonCachyEngine.cmake

# 关闭编译测试程序
set(ICE_BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE)
# 宿主选项在集成边界转换，引擎内部只消费自己的 LTO 配置名。
set(ICE_DISABLE_CLANG_LTO "${MMM_DISABLE_CLANG_LTO}")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/IonCachyEngine")
