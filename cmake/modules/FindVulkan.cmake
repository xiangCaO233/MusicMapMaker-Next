# cmake/modules/FindVulkan.cmake 包装器 (Wrapper)。 在 Windows 上，它会尝试设置路径提示以使用项目内置的
# Vulkan SDK。 在 Linux/macOS 上，它直接透传给系统的标准 FindVulkan。 将首次配置选择的 SDK 保存到构建缓存，后续
# Ninja 自动重配置不依赖原 shell。 环境变量仍允许显式覆盖；旧构建目录可从已验证的头文件位置恢复 SDK 根目录。 包装器最终仍由 CMake
# 官方模块创建 Vulkan targets，业务代码不感知路径来源。 Windows SDK 根必须同时提供 Include 与
# Lib/vulkan-1.lib，防止混合两个 SDK。 该 helper 只在 Windows 路径调用，成功标志通过父作用域显式返回。

# 从环境、缓存或既有探测结果恢复 Windows Vulkan SDK，并校验其完整性。 成功时同步环境、缓存、当前作用域输出以及父作用域状态标志。
function(mmm_set_vulkan_sdk_from_env)
  # 调用方必须显式观察成功标志，旧值不能跨多次调用泄漏。
  set(_MMM_VULKAN_SDK_ROOT_SET
      FALSE
      PARENT_SCOPE)

  if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    # 当前进程环境具有最高优先级，便于 CI 选择挂载 SDK。
    file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VULKAN_SDK_ROOT)
  elseif(MMM_VULKAN_SDK_ROOT)
    # 自动重配置优先复用首次选择并写入缓存的稳定 SDK 根。
    set(VULKAN_SDK_ROOT "${MMM_VULKAN_SDK_ROOT}")
  elseif(
    CMAKE_CROSSCOMPILING
    AND Vulkan_INCLUDE_DIR
    AND Vulkan_LIBRARY)
    # 旧构建缓存没有专用变量时，可从已验证 include 路径迁移根目录。
    get_filename_component(VULKAN_SDK_ROOT "${Vulkan_INCLUDE_DIR}" DIRECTORY)
  else()
    # 没有任何可信来源时保持失败标志，由调用场景决定是否必须报错。
    return()
  endif()
  set(_MMM_VULKAN_INCLUDE_DIR "${VULKAN_SDK_ROOT}/Include")
  # loader 导入库使用 SDK 的标准 Lib 路径。
  set(_MMM_VULKAN_LIBRARY "${VULKAN_SDK_ROOT}/Lib/vulkan-1.lib")

  if(NOT EXISTS "${_MMM_VULKAN_INCLUDE_DIR}")
    # 头目录缺失表示环境变量指向错误层级或 SDK 安装不完整。
    message(FATAL_ERROR "VULKAN_SDK points to '${VULKAN_SDK_ROOT}', but "
                        "'${_MMM_VULKAN_INCLUDE_DIR}' does not exist.")
  endif()

  if(NOT EXISTS "${_MMM_VULKAN_LIBRARY}")
    # Windows loader 导入库是链接必需项，不能只接受 headers-only SDK。
    message(FATAL_ERROR "VULKAN_SDK points to '${VULKAN_SDK_ROOT}', but "
                        "'${_MMM_VULKAN_LIBRARY}' does not exist.")
  endif()

  set(ENV{VULKAN_SDK} "${VULKAN_SDK_ROOT}")
  # 专用缓存变量在 Ninja 自动重配置时不依赖启动 shell 的环境。
  set(MMM_VULKAN_SDK_ROOT
      "${VULKAN_SDK_ROOT}"
      CACHE PATH "Windows Vulkan SDK selected for this build." FORCE)
  set(VULKAN_SDK_ROOT
      "${VULKAN_SDK_ROOT}"
      PARENT_SCOPE)
  set(Vulkan_INCLUDE_DIR
      "${_MMM_VULKAN_INCLUDE_DIR}"
      CACHE PATH "Vulkan include directory" FORCE)
  set(Vulkan_LIBRARY
      "${_MMM_VULKAN_LIBRARY}"
      CACHE FILEPATH "Vulkan loader library" FORCE)
  # 官方 FindVulkan 和后续调用方分别读取缓存与父作用域值。
  set(Vulkan_INCLUDE_DIR
      "${_MMM_VULKAN_INCLUDE_DIR}"
      PARENT_SCOPE)
  set(Vulkan_LIBRARY
      "${_MMM_VULKAN_LIBRARY}"
      PARENT_SCOPE)
  # 成功标志只在全部路径完成验证后写入。
  set(_MMM_VULKAN_SDK_ROOT_SET
      TRUE
      PARENT_SCOPE)
endfunction()

if(CMAKE_CROSSCOMPILING)
  # 交叉构建需要先约束 SDK 根，再进入官方查找模块。
  if(WIN32)
    # Windows 两类 ABI 工具链共享 SDK 入口。
    if(MSVC)
      message(STATUS "Using CMAKE_CROSSCOMPILING clang-cl msvc like Toolchain")
    elseif(MINGW)
      message(STATUS "Using CMAKE_CROSSCOMPILING MinGW Toolchain")
    endif()
    # =============================================================================
    # 调用系统内置的标准 FindVulkan
    # =============================================================================
    # CMAKE_ROOT 是 CMake 安装路径，这就相当于调用了 #include <FindVulkan.cmake>
    mmm_set_vulkan_sdk_from_env()
    if(NOT _MMM_VULKAN_SDK_ROOT_SET)
      # Windows 交叉构建禁止搜索 Linux 宿主的 Vulkan SDK。
      message(
        FATAL_ERROR
          "VULKAN_SDK environment variable is required when cross-compiling "
          "for Windows.")
    endif()

    # 也可以通过设置环境变量让标准模块去找
    list(PREPEND CMAKE_INCLUDE_PATH "${VULKAN_SDK_ROOT}/Include")
    # 提示路径只为官方模块服务，实际 imported target 仍由官方逻辑创建。
    list(PREPEND CMAKE_LIBRARY_PATH "${VULKAN_SDK_ROOT}/Lib")
    include("${CMAKE_ROOT}/Modules/FindVulkan.cmake")
  else()
    # 非 Windows 交叉平台不需要项目自定义 SDK 布局，直接透传。
    include("${CMAKE_ROOT}/Modules/FindVulkan.cmake")
  endif()
else()
  if(WIN32)
    # 定义项目内预编译 Vulkan 的位置
    set(VULKAN_VENDORED_DIR
        "${CMAKE_SOURCE_DIR}/3rdpty/prebuilts/binaries/windows/vulkan")

    message(
      STATUS
        "FindVulkan Wrapper: Checking for vendored Vulkan in ${VULKAN_VENDORED_DIR}..."
    )

    if(EXISTS "${VULKAN_VENDORED_DIR}")
      # vendored 目录存在时优先提供头提示，但仍检查具体库子目录。 设置头文件搜索路径提示
      list(APPEND CMAKE_INCLUDE_PATH "${VULKAN_VENDORED_DIR}/include")

      # 根据编译器确定库文件子目录
      set(_VULKAN_LIB_SUBDIR "")

      if(MSVC)
        # MSVC 与 clang-cl 共用 msvc ABI 目录。
        set(_VULKAN_LIB_SUBDIR "msvc")
      elseif(MINGW)
        # MinGW 使用其专用导入库目录，不能混入 msvc .lib。
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
            "FindVulkan Wrapper: Added hint for ${_VULKAN_LIB_SUBDIR} libraries."
        )

        # 也可以直接设置环境变量 VULKAN_SDK 指向我们的 vendored 目录 这样标准模块会误以为这就是安装的 SDK
        # set(ENV{VULKAN_SDK} "${VULKAN_VENDORED_DIR}")
      else()
        # 缺少库目录时保留后续官方搜索机会，并给出明确警告。
        message(
          WARNING
            "FindVulkan Wrapper: Vendored lib directory not found: ${_VULKAN_LIB_PATH}"
        )
      endif()
    else()
      if(MSVC)
        # 无 vendored SDK 时，本机 MSVC 仍可使用环境或缓存中的官方 SDK。
        mmm_set_vulkan_sdk_from_env()
        if(_MMM_VULKAN_SDK_ROOT_SET)
          list(PREPEND CMAKE_INCLUDE_PATH "${VULKAN_SDK_ROOT}/Include")
          list(PREPEND CMAKE_LIBRARY_PATH "${VULKAN_SDK_ROOT}/Lib")
        endif()
      endif()
    endif()
    # =============================================================================
    # 调用系统内置的标准 FindVulkan
    # =============================================================================
    # CMAKE_ROOT 是 CMake 安装路径，这就相当于调用了 #include <FindVulkan.cmake>
    include("${CMAKE_ROOT}/Modules/FindVulkan.cmake")
  else()
    # =============================================================================
    # 调用系统内置的标准 FindVulkan
    # =============================================================================
    # CMAKE_ROOT 是 CMake 安装路径，这就相当于调用了 #include <FindVulkan.cmake>
    include("${CMAKE_ROOT}/Modules/FindVulkan.cmake")
  endif()

endif()

# =============================================================================
# 修正与后处理
# =============================================================================
# 有时候标准模块在 MinGW 下找到的库可能有点问题，或者需要手动指定 DLL 位置
if(Vulkan_FOUND AND WIN32)
  # Windows 路径来源较多，配置日志记录最终库与头便于 ABI 诊断。 打印一下到底找到了哪个
  message(STATUS "FindVulkan Wrapper: Found Library: ${Vulkan_LIBRARY}")
  message(STATUS "FindVulkan Wrapper: Found Include: ${Vulkan_INCLUDE_DIR}")
endif()
