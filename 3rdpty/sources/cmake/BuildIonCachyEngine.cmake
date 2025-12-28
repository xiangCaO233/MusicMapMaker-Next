# 3rdpty/sources/cmake/BuildIonCachyEngine.cmake

# 关闭编译测试程序
set(ICE_BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/IonCachyEngine")
