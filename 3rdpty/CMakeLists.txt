# 3rdpty/CMakeLists.txt

if(SOURCES_BUILD)
    # 从源码编译
    add_subdirectory(sources)
else()
    # 使用预编译
    if(WIN32) # 定义平台变量
        set(ICE_PLATFORM "windows-x86_64")
    elseif(APPLE)
        # 仅考虑arm的macos
        set(ICE_PLATFORM "macos-arm64")
    else()
        set(ICE_PLATFORM "linux-x86_64")
    endif()

    # 预编译库路径
    set(PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilts/${ICE_PLATFORM}")
endif()
