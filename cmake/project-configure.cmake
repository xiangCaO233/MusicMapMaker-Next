# 全局关闭 C++20 模块依赖扫描，以加快构建速度 项目当前不使用标准模块，关闭扫描可避免 Ninja 为每个翻译单元生成额外规则。
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# 生成第三方预编译库时禁用 Clang ThinLTO，避免把 bitcode / whole-program-vtables 状态写入可复用静态库。
option(MMM_DISABLE_CLANG_LTO "Disable Clang ThinLTO for this configure." OFF)

# 编译器家族决定后续全局优化参数语法。
if(MSVC)
  # MSVC 的运行库和字符集已在根 CMake 中统一配置，此处无需 GNU 风格参数。
else()
  if(WIN32)
    if(MINGW AND PROJECT_LINKAGE STREQUAL "static")
      # MinGW 静态链接偏好直接静态链接标准库等运行时依赖；shared 偏好 不能携带 -static，避免把运行库强行并入本体。
      add_link_options(-static)
    endif()
    if(MSVC)
      # 保留平台分支结构，MSVC 不应接收 MinGW 专用链接参数。
    endif()
  endif()

  # Clang 分支同时覆盖本机与 GNU frontend 交叉模式。
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # ==============================================================================
    # 全局为 Clang + Release 模式开启 ThinLTO
    # ==============================================================================

    # 检查编译器是否是 Clang 状态消息用于 CI 日志确认后续优化分支确实生效。
    message(STATUS "Compiler is Clang.")

    if(WIN32)
      # clang-cl 使用 lld-link，可开启 PDB；MinGW clang 使用 GNU frontend 调用 lld， PDB
      # 输出路径需要在具体目标上通过 -Wl,-pdb=... 指定。
      add_compile_options("-gcodeview")
      if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl 采用 lld-link 参数风格，并显式生成可分发的 PDB。
        add_link_options("-fuse-ld=lld")
        add_link_options("-Wl,/debug")
        message(
          STATUS
            "Windows clang-cl detected. Enabling PDB generation via lld-link.")
      elseif(MINGW)
        # MinGW frontend 的 PDB 路径由具体目标宏设置，不能在全局共享。
        message(
          STATUS "Windows MinGW clang detected. Using GNU-style linker flags.")
      endif()
    endif()

    # --- 全局 Clang 优化 (所有配置) ---
    add_compile_options("-fno-math-errno")
    # 唯一内部链接名可避免 ThinLTO 合并不同翻译单元中的同名局部符号。
    add_compile_options("-funique-internal-linkage-names")
    # 时间跟踪文件供构建耗时分析脚本使用，不改变生成代码语义。
    add_compile_options("-ftime-trace")

    # LTO 可由依赖预编译流程显式关闭。 使用 GCC driver 做最终链接时不能启用 LLVM ThinLTO，否则链接器无法消费 clang
    # 生成的 bitcode。
    if(NOT MMM_DISABLE_CLANG_LTO)
      # 生成器表达式限制 ThinLTO 只进入发布型配置，Debug 保持快速链接。 同时，也为链接器添加 LTO 标志 仅在
      # Release、RelWithDebInfo、MinSizeRel 模式下添加编译选项
      add_compile_options(
        # 仅发布型配置把 bitcode 交给 ThinLTO 后端。
        "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-flto=thin>")

      # 仅在 Release、RelWithDebInfo、MinSizeRel 模式下添加链接选项
      add_link_options(
        "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-flto=thin>")

      # --- Release 模式额外优化 ---
      add_compile_options(
        "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fwhole-program-vtables>"
      )
      add_compile_options(
        "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-Xclang;-fmerge-functions>"
      )
    else()
      # 预编译依赖构建通过该开关确保产物不携带跨项目 bitcode 状态。
      message(STATUS "Clang ThinLTO disabled for this toolchain.")
    endif()

    # --- Release 模式额外优化 ---
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
    # 非 Clang 编译器统一走 GCC 兼容配置，不启用 LLVM ThinLTO。
    message(STATUS "Compiler is GCC. Disable LTO.")
    if(WIN32 AND CMAKE_CROSSCOMPILING)
      # Windows 交叉 GCC 的 CI 构建日志需要保持可读，默认不输出每个翻译单元的 GCC 内部耗时表。
      message(STATUS "GCC time report disabled for Windows cross builds.")
    elseif(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14
           AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
      # GCC 14 的 -ftime-report 会在实例化 std::span 的迭代器时触发编译器内部错误；
      # 该参数仅生成编译耗时报告，禁用后不影响目标代码与音频混音行为。
      message(STATUS "GCC time report disabled for GCC 14 compiler stability.")
    else()
      add_compile_options("-ftime-report")
    endif()

    # 编译器 GNU 的 -O2/-O3 默认启用 strict aliasing，而 Vulkan-Hpp 的句柄类型
    # 和内部类型转换在该模式下可能触发未定义行为，导致渲染数据丢失。
    add_compile_options("-fno-strict-aliasing")

    # 与 Clang 对齐：分离函数/数据节以支持链接器死代码消除
    add_compile_options("-ffunction-sections")
    add_compile_options("-fdata-sections")
    if(MINGW)
      # MinGW COFF 默认 section 数量较小；Debug 下 Vulkan/ImGui 相关翻译单元
      # 叠加独立函数/数据节后可能超过限制，需要启用 big object。
      add_compile_options("-Wa,-mbig-obj")
      # GCC14 win32 静态 libstdc++ 与 Debug 预编译 C++ 静态库会共同实例化少量标准库内联符号；仅 Debug
      # 放宽重复定义，发布型配置仍保持严格链接。
      add_link_options("$<$<CONFIG:Debug>:-Wl,--allow-multiple-definition>")
    endif()
    if(APPLE)
      # Apple ld 使用 dead_strip，不能传入 GNU 链接器的 gc-sections。
      add_link_options("-Wl,-dead_strip")
    elseif(NOT WIN32)
      add_link_options("-Wl,--gc-sections")
    endif()
  endif()
endif()

if(APPLE)
  # Apple strip 的 -x 仅移除局部符号，保留应用束需要的外部符号信息。
  set(MMM_RELEASE_STRIP_FLAG "-x")
else()
  # GNU/LLVM strip 使用 -s 移除发布可执行文件中的符号表。
  set(MMM_RELEASE_STRIP_FLAG "-s")
endif()

# ==============================================================================
# 定义一个宏函数 (macro)，用于为目标添加 strip 命令
# ==============================================================================

# 宏 (macro) 与函数 (function) 的区别在于： 宏是简单的文本替换，变量作用域与调用处相同。
# 函数有自己的变量作用域。对于这种简单的命令添加，宏更直观。 strip helper 在目标定义完成后按需调用。
macro(add_strip_command_for_release TARGET_NAME)
  # 某些交叉工具链不提供 strip，此时保持目标可构建并输出明确状态。
  if(NOT CMAKE_STRIP OR CMAKE_STRIP MATCHES "-NOTFOUND$")
    message(
      STATUS
        "Skipping post-build strip command for target '${TARGET_NAME}': CMAKE_STRIP is not available."
    )
  else()
    # 生成器表达式让同一多配置目标仅在发布型配置执行 strip。 $<TARGET_FILE:...>: 获取目标的完整路径 配置
    # Release/MinSizeRel: 在这些模式下执行此命令 构建后步骤: 在目标成功构建之后执行
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
  # 仅 MinGW GCC 需要旁路 DWARF；MSVC 与 Clang 分别使用 PDB 流程。
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

# ==============================================================================
# MinGW clang PDB 生成宏：GNU frontend 不接受 /debug，但 lld 可通过 -pdb 输出 CodeView
# 调试信息。路径必须跟随具体目标，否则多个 exe/dll 会争用同一个 PDB。
# ==============================================================================
macro(add_mingw_clang_pdb TARGET_NAME)
  # 使用 GCC driver 链接时不支持 lld 的 -pdb，因此显式排除回退模式。
  if(WIN32
     AND MINGW
     AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
     AND NOT MMM_MINGW_CLANG_USE_GCC_LINKER)
    target_link_options(
      ${TARGET_NAME}
      PRIVATE
      "$<$<CONFIG:Debug,RelWithDebInfo>:-Wl,-pdb=$<TARGET_FILE_DIR:${TARGET_NAME}>/$<TARGET_FILE_BASE_NAME:${TARGET_NAME}>.pdb>"
    )
    message(
      STATUS
        "Added MinGW clang PDB output for target '${TARGET_NAME}' in Debug/RelWithDebInfo."
    )
  endif()
endmacro()

# 设置所有可执行文件的输出目录为 build/bin 统一布局让运行时文件复制、打包与本地启动脚本无需识别生成器差异。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# 把生成的动态链接库 (DLL/SO) 放到 bin 下
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# 把静态库 (.a/.lib) 放到 build/lib 下
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(APPLE
   OR MSVC
   OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
  # MinGW GCC 16 的 gnu++26 与静态 libstdc++ 链接会重复定义 stdexcept 符号。 项目当前规范为
  # C++20/C++23，Windows GCC 固定到 C++23 以保持静态预编译构建可链接。
  set(CMAKE_CXX_STANDARD 23)
else()
  set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 导出编译数据库供 clangd、静态分析和格式化工具复用真实参数。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 是否为Debug模式的宏
add_definitions(-DBUILD_TYPE_DEBUG=$<CONFIG:Debug>)
add_definitions(-DVULKAN_HPP_NO_EXCEPTIONS)
add_definitions(-DVULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS)

if(WIN32)
  # Windows 头中的 min/max 宏会污染标准库与 GLM 调用，统一从编译层禁用。
  add_compile_definitions(NOMINMAX)
endif()

# 应用版本与平台信息 版本号由 CMake project(VERSION ...) 提供，通过 configure_file() 生成 version.h
# 调试构建类型保留为 generator expression 以适应多配置生成器

set(MMM_PROJECT_NAME "MusicMapMaker")
set(MMM_VERSION_SUFFIX
    ""
    CACHE STRING "可选版本后缀 (如 -alpha.1)")

if(WIN32)
  # 发布元数据使用稳定的小写平台标识，与更新服务协议保持一致。
  set(MMM_PLATFORM "windows")
elseif(APPLE)
  set(MMM_PLATFORM "macos")
else()
  set(MMM_PLATFORM "linux")
endif()

# 编译器展示名默认沿用 CMake ID。 clang-cl 的 CMake ID 仍为 Clang，但它使用 MSVC ABI。
# 关于窗口需要显式标识，避免和 GNU-like Clang 构建混淆。
set(MMM_BUILD_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   AND ("${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "MSVC"
        OR "${CMAKE_CXX_SIMULATE_ID}" STREQUAL "MSVC"))
  set(MMM_BUILD_COMPILER_ID "Clang-cl(MSVC ABI)")
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mmmversion.h.in"
               "${CMAKE_BINARY_DIR}/generated/mmmversion.h" @ONLY)

# 让所有编译目标都能找到生成的头文件 生成目录只包含项目配置头，作为全局 include 路径不会引入第三方实现。
include_directories("${CMAKE_BINARY_DIR}/generated")

# 强制 编译器 以 UTF-8 处理输入和执行字符集
if(MSVC)
  add_compile_options(/utf-8)
  add_compile_options(/wd4875)
else()
  add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()
