# 全局关闭 C++20 模块依赖扫描，以加快构建速度
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)

else()
  if(WIN32)
    if(MINGW)
      # 在 Windows 非 MSVC 环境下直接静态链接标准库等运行时依赖。
      add_link_options(-static)
    endif()
    if(MSVC)

    endif()
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # ==============================================================================
    # 全局为 Clang + Release 模式开启 ThinLTO
    # ==============================================================================

    # 检查编译器是否是 Clang
    message(
      STATUS
        "Compiler is Clang. Enabling ThinLTO for Release/RelWithDebInfo/MinSizeRel."
    )

    if(WIN32)
      # 为 Clang 开启 PDB 支持 (CodeView 格式)
      add_compile_options("-gcodeview")
      add_link_options("-fuse-ld=lld")
      add_link_options("-Wl,/debug")
      message(
        STATUS "Windows Clang detected. Enabling PDB generation via lld-link.")
    endif()

    # --- 全局 Clang 优化 (所有配置) ---
    add_compile_options("-fno-math-errno")
    add_compile_options("-funique-internal-linkage-names")
    add_compile_options("-ftime-trace")

    # 同时，也为链接器添加 LTO 标志 仅在 Release、RelWithDebInfo、MinSizeRel 模式下添加编译选项
    add_compile_options(
      "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-flto=thin>")

    # 仅在 Release、RelWithDebInfo、MinSizeRel 模式下添加链接选项
    add_link_options(
      "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-flto=thin>")

    # --- Release 模式额外优化 ---
    add_compile_options(
      "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fwhole-program-vtables>")
    add_compile_options(
      "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-Xclang;-fmerge-functions>"
    )
    add_compile_options(
      "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-ffp-contract=fast>")

    # --- 每个函数/数据放入独立 section，供链接器 GC 丢弃未引用部分 ---
    add_compile_options("-ffunction-sections")
    add_compile_options("-fdata-sections")
    if(APPLE)
      # 在 macOS 上使用 Apple ld，死代码剥离参数不同于 GNU ld/lld。
      add_link_options("-Wl,-dead_strip")
    elseif(NOT WIN32)
      # 在 Linux 上由链接器丢弃死节
      add_link_options("-Wl,--gc-sections")
    endif()
  else()
    message(STATUS "Compiler is GCC. Disable LTO.")
    add_compile_options("-ftime-report")

    # 编译器 GNU 的 -O2/-O3 默认启用 strict aliasing，而 Vulkan-Hpp 的句柄类型
    # 和内部类型转换在该模式下可能触发未定义行为，导致渲染数据丢失。
    add_compile_options("-fno-strict-aliasing")

    # 与 Clang 对齐：分离函数/数据节以支持链接器死代码消除
    add_compile_options("-ffunction-sections")
    add_compile_options("-fdata-sections")
    if(APPLE)
      add_link_options("-Wl,-dead_strip")
    elseif(NOT WIN32)
      add_link_options("-Wl,--gc-sections")
    endif()
  endif()
endif()

if(APPLE)
  set(MMM_RELEASE_STRIP_FLAG "-x")
else()
  set(MMM_RELEASE_STRIP_FLAG "-s")
endif()

# ==============================================================================
# 定义一个宏函数 (macro)，用于为目标添加 strip 命令
# ==============================================================================

# 宏 (macro) 与函数 (function) 的区别在于： 宏是简单的文本替换，变量作用域与调用处相同。
# 函数有自己的变量作用域。对于这种简单的命令添加，宏更直观。
macro(add_strip_command_for_release TARGET_NAME)
  if(NOT CMAKE_STRIP OR CMAKE_STRIP MATCHES "-NOTFOUND$")
    message(
      STATUS
        "Skipping post-build strip command for target '${TARGET_NAME}': CMAKE_STRIP is not available."
    )
  else()
    # $<TARGET_FILE:...>: 获取目标的完整路径 配置 Release/MinSizeRel: 在这些模式下执行此命令 构建后步骤:
    # 在目标成功构建之后执行
    add_custom_command(
      TARGET ${TARGET_NAME}
      POST_BUILD
      COMMAND
        $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:${CMAKE_STRIP}>
        $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:${MMM_RELEASE_STRIP_FLAG}>
        $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:$<TARGET_FILE:${TARGET_NAME}>>
      COMMENT
        "Stripping symbols from ${TARGET_NAME} in Release/MinSizeRel/RelWithDebInfo mode"
      VERBATIM)
    message(
      STATUS
        "Added post-build strip command for target '${TARGET_NAME}' in Release/MinSizeRel/RelWithDebInfo."
    )
  endif()
endmacro()

# ==============================================================================
# 编译器 GNU (MinGW) 调试信息分离宏 将 DWARF 调试信息从 exe 中提取到 .dbg 文件，类似 Clang 的 PDB 仅对 Debug
# 和 RelWithDebInfo 生效
# ==============================================================================
macro(add_gcc_debug_extract TARGET_NAME)
  if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_custom_command(
      TARGET ${TARGET_NAME}
      POST_BUILD
      COMMAND
        ${CMAKE_COMMAND} -D TARGET_FILE="$<TARGET_FILE:${TARGET_NAME}>" -D
        OBJCOPY="${CMAKE_OBJCOPY}" -D STRIP_EXE="${CMAKE_STRIP}" -D
        CONFIG="$<CONFIG>" -P "${CMAKE_SOURCE_DIR}/cmake/GccExtractDebug.cmake"
      COMMENT "GCC: extracting debug to .dbg for ${TARGET_NAME}")
    message(STATUS "Added GCC debug extraction for target '${TARGET_NAME}'.")
  endif()
endmacro()

# 设置所有可执行文件的输出目录为 build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# 把生成的动态链接库 (DLL/SO) 放到 bin 下
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# 把静态库 (.a/.lib) 放到 build/lib 下
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(APPLE OR MSVC)
  set(CMAKE_CXX_STANDARD 23)
else()
  set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 是否为Debug模式的宏
add_definitions(-DBUILD_TYPE_DEBUG=$<CONFIG:Debug>)
add_definitions(-DVULKAN_HPP_NO_EXCEPTIONS)
add_definitions(-DVULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS)

if(WIN32)
  add_compile_definitions(NOMINMAX)
endif()

# 应用版本与平台信息 版本号由 CMake project(VERSION ...) 提供，通过 configure_file() 生成 version.h
# 调试构建类型保留为 generator expression 以适应多配置生成器

set(MMM_PROJECT_NAME "MusicMapMaker")
set(MMM_VERSION_SUFFIX
    ""
    CACHE STRING "可选版本后缀 (如 -alpha.1)")

if(WIN32)
  set(MMM_PLATFORM "windows")
elseif(APPLE)
  set(MMM_PLATFORM "macos")
else()
  set(MMM_PLATFORM "linux")
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mmmversion.h.in"
               "${CMAKE_BINARY_DIR}/generated/mmmversion.h" @ONLY)

# 让所有编译目标都能找到生成的头文件
include_directories("${CMAKE_BINARY_DIR}/generated")

# 强制 编译器 以 UTF-8 处理输入和执行字符集
if(MSVC)
  add_compile_options(/utf-8)
  add_compile_options(/wd4875)
else()
  add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()
