# cmake/modules/FindVulkan.cmake 包装器 (Wrapper)。 在 Windows 上，它会尝试设置路径提示以使用项目内置的
# Vulkan SDK。 在 Linux/macOS 上，它直接透传给系统的标准 FindVulkan。

if(WIN32)
    # 定义项目内预编译 Vulkan 的位置
    set(VULKAN_VENDORED_DIR "${CMAKE_SOURCE_DIR}/3rdpty/prebuilts/windows/vulkan")

    message(
        STATUS
        "FindVulkan Wrapper: Checking for vendored Vulkan in ${VULKAN_VENDORED_DIR}..."
    )

    if(EXISTS "${VULKAN_VENDORED_DIR}")
        # 设置头文件搜索路径提示
        list(APPEND CMAKE_INCLUDE_PATH "${VULKAN_VENDORED_DIR}/include")

        # 根据编译器确定库文件子目录
        set(_VULKAN_LIB_SUBDIR "")

        if(MSVC)
            set(_VULKAN_LIB_SUBDIR "msvc")
        elseif(MINGW)
            set(_VULKAN_LIB_SUBDIR "mingw")
        else()
            # 其他编译器默认回退到 msvc 或根目录，视情况而定
            set(_VULKAN_LIB_SUBDIR "msvc")
        endif()

        set(_VULKAN_LIB_PATH "${VULKAN_VENDORED_DIR}/lib/${_VULKAN_LIB_SUBDIR}")

        if(EXISTS "${_VULKAN_LIB_PATH}")
            # 将路径加入到 CMAKE_LIBRARY_PATH 中，这样标准 find_library 就能找到它
            list(APPEND CMAKE_LIBRARY_PATH "${_VULKAN_LIB_PATH}")
            message(
                STATUS
                "FindVulkan Wrapper: Added hint for ${_VULKAN_LIB_SUBDIR} libraries.")

        # 也可以直接设置环境变量 VULKAN_SDK 指向我们的 vendored 目录 这样标准模块会误以为这就是安装的 SDK
        # set(ENV{VULKAN_SDK} "${VULKAN_VENDORED_DIR}")
        else()
            message(
                WARNING
                "FindVulkan Wrapper: Vendored lib directory not found: ${_VULKAN_LIB_PATH}"
            )
        endif()
    endif()
else()
    # =============================================================================
    # 调用系统内置的标准 FindVulkan
    # =============================================================================
    # CMAKE_ROOT 是 CMake 安装路径，这就相当于调用了 #include <FindVulkan.cmake>
    include("${CMAKE_ROOT}/Modules/FindVulkan.cmake")
endif()

# =============================================================================
# 修正与后处理
# =============================================================================
# 有时候标准模块在 MinGW 下找到的库可能有点问题，或者需要手动指定 DLL 位置
if(Vulkan_FOUND AND WIN32)
    # 打印一下到底找到了哪个
    message(STATUS "FindVulkan Wrapper: Found Library: ${Vulkan_LIBRARY}")
    message(STATUS "FindVulkan Wrapper: Found Include: ${Vulkan_INCLUDE_DIR}")
endif()
