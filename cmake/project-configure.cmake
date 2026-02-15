# ==============================================================================
#  全局为 Clang + Release 模式开启 ThinLTO
# ==============================================================================

# 检查编译器是否是 Clang
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "Compiler is Clang. Enabling global ThinLTO for Release builds.")

    # 使用 CMAKE_CXX_FLAGS_RELEASE 变量来全局添加编译选项
    # "APPEND" 可以在已有 Release 标志的基础上追加
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -flto=thin")

    # 同时，也为链接器添加 LTO 标志
    # 注意：对于可执行文件和共享库，链接标志变量不同
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -flto=thin")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " -flto=thin")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_RELEASE " -flto=thin")

else()
    message(STATUS "Compiler is not Clang. Global ThinLTO is disabled.")
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
            COMMAND ${CMAKE_STRIP} -s "$<TARGET_FILE:${TARGET_NAME}>"
            CONFIGURATIONS Release
            COMMENT "Stripping symbols from ${TARGET_NAME} in Release mode"
    )
    message(STATUS "Added post-build strip command for target '${TARGET_NAME}' in Release mode.")
endmacro()
