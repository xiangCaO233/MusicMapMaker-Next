# cmake/modules/FindVulkan.cmake 包装器 (Wrapper)。 在 Windows 上，它会尝试设置路径提示以使用项目内置的
# Vulkan SDK。 在 Linux/macOS 上，它直接透传给系统的标准 FindVulkan。
function(mmm_set_vulkan_sdk_from_env)
  set(_MMM_VULKAN_SDK_ROOT_SET
      FALSE
      PARENT_SCOPE)

  if(NOT DEFINED ENV{VULKAN_SDK} OR "$ENV{VULKAN_SDK}" STREQUAL "")
    return()
  endif()

  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VULKAN_SDK_ROOT)
  set(_MMM_VULKAN_INCLUDE_DIR "${VULKAN_SDK_ROOT}/Include")
  set(_MMM_VULKAN_LIBRARY "${VULKAN_SDK_ROOT}/Lib/vulkan-1.lib")

  if(NOT EXISTS "${_MMM_VULKAN_INCLUDE_DIR}")
    message(FATAL_ERROR "VULKAN_SDK points to '${VULKAN_SDK_ROOT}', but "
                        "'${_MMM_VULKAN_INCLUDE_DIR}' does not exist.")
  endif()

  if(NOT EXISTS "${_MMM_VULKAN_LIBRARY}")
    message(FATAL_ERROR "VULKAN_SDK points to '${VULKAN_SDK_ROOT}', but "
                        "'${_MMM_VULKAN_LIBRARY}' does not exist.")
  endif()

  set(ENV{VULKAN_SDK} "${VULKAN_SDK_ROOT}")
  set(VULKAN_SDK_ROOT
      "${VULKAN_SDK_ROOT}"
      PARENT_SCOPE)
  set(Vulkan_INCLUDE_DIR
      "${_MMM_VULKAN_INCLUDE_DIR}"
      CACHE PATH "Vulkan include directory" FORCE)
  set(Vulkan_LIBRARY
      "${_MMM_VULKAN_LIBRARY}"
      CACHE FILEPATH "Vulkan loader library" FORCE)
  set(Vulkan_INCLUDE_DIR
      "${_MMM_VULKAN_INCLUDE_DIR}"
      PARENT_SCOPE)
  set(Vulkan_LIBRARY
      "${_MMM_VULKAN_LIBRARY}"
      PARENT_SCOPE)
  set(_MMM_VULKAN_SDK_ROOT_SET
      TRUE
      PARENT_SCOPE)
endfunction()

if(CMAKE_CROSSCOMPILING)
  if(WIN32)
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
      message(
        FATAL_ERROR
          "VULKAN_SDK environment variable is required when cross-compiling "
          "for Windows.")
    endif()

    # 也可以通过设置环境变量让标准模块去找
    list(PREPEND CMAKE_INCLUDE_PATH "${VULKAN_SDK_ROOT}/Include")
    list(PREPEND CMAKE_LIBRARY_PATH "${VULKAN_SDK_ROOT}/Lib")
    include("${CMAKE_ROOT}/Modules/FindVulkan.cmake")
  else()
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
            "FindVulkan Wrapper: Added hint for ${_VULKAN_LIB_SUBDIR} libraries."
        )

        # 也可以直接设置环境变量 VULKAN_SDK 指向我们的 vendored 目录 这样标准模块会误以为这就是安装的 SDK
        # set(ENV{VULKAN_SDK} "${VULKAN_VENDORED_DIR}")
      else()
        message(
          WARNING
            "FindVulkan Wrapper: Vendored lib directory not found: ${_VULKAN_LIB_PATH}"
        )
      endif()
    else()
      # =============================================================================
      # 调用系统内置的标准 FindVulkan
      # =============================================================================
      # CMAKE_ROOT 是 CMake 安装路径，这就相当于调用了 #include <FindVulkan.cmake>
      if(MSVC)
        mmm_set_vulkan_sdk_from_env()
        if(_MMM_VULKAN_SDK_ROOT_SET)
          list(PREPEND CMAKE_INCLUDE_PATH "${VULKAN_SDK_ROOT}/Include")
          list(PREPEND CMAKE_LIBRARY_PATH "${VULKAN_SDK_ROOT}/Lib")
        endif()
      endif()
      include("${CMAKE_ROOT}/Modules/FindVulkan.cmake")
    endif()
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
  # 打印一下到底找到了哪个
  message(STATUS "FindVulkan Wrapper: Found Library: ${Vulkan_LIBRARY}")
  message(STATUS "FindVulkan Wrapper: Found Include: ${Vulkan_INCLUDE_DIR}")
endif()
