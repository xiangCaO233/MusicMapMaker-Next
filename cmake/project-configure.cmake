if(MSVC)
else()
    if(WIN32)
        # windows非msvc直接静态链接标准库之类的
        add_link_options(-static)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # ==============================================================================
        #  全局为 Clang + Release 模式开启 ThinLTO
        # ==============================================================================

        # 检查编译器是否是 Clang
        message(STATUS "Compiler is Clang. Enabling global ThinLTO for Release builds.")

        # 同时，也为链接器添加 LTO 标志
        # 仅在 Release 或 RelWithDebInfo 模式下添加编译选项
        add_compile_options("$<$<CONFIG:Release,RelWithDebInfo>:-flto=thin>")
        
        # 仅在 Release 或 RelWithDebInfo 模式下添加链接选项
        add_link_options("$<$<CONFIG:Release,RelWithDebInfo>:-flto=thin>")
    else()
        message(STATUS "Compiler is GCC. Disable LTO.")
    endif()
endif()

# ==============================================================================
#  定义一个宏函数 (macro)，用于为目标添加 strip 命令
# ==============================================================================

# 宏 (macro) 与函数 (function) 的区别在于：
# 宏是简单的文本替换，变量作用域与调用处相同。
# 函数有自己的变量作用域。对于这种简单的命令添加，宏更直观。
macro(add_strip_command_for_release TARGET_NAME)
    # $<TARGET_FILE:...>: 获取目标的完整路径
    # CONFIGURATIONS Release: 只在 Release 模式下执行此命令
    # POST_BUILD:             在目标成功构建之后执行
    add_custom_command(TARGET ${TARGET_NAME}
            POST_BUILD
            # 关键：使用生成器表达式。只有在 Release 模式下，命令才会被生成。
            COMMAND $<$<CONFIG:Release>:${CMAKE_STRIP}> $<$<CONFIG:Release>:-s> $<$<CONFIG:Release>:$<TARGET_FILE:${TARGET_NAME}>>
            COMMENT "Stripping symbols from ${TARGET_NAME} in Release mode"
            VERBATIM # 建议加上这个，处理路径中的空格更稳健
    )
    message(STATUS "Added post-build strip command for target '${TARGET_NAME}' in Release mode.")
endmacro()

# 设置所有可执行文件的输出目录为 build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# 把生成的动态链接库 (DLL/SO) 放到 bin 下
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# 把静态库 (.a/.lib) 放到 build/lib 下
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 是否为Debug模式的宏
add_definitions(-DBUILD_TYPE_DEBUG=$<CONFIG:Debug>)

# 强制 编译器 以 UTF-8 处理输入和执行字符集
if(MSVC)
    add_compile_options(/utf-8)
else()
    add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

