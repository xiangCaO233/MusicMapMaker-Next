# clang 的 MinGW GNU driver 在部分 Linux 发行版上会默认寻找共享 libgcc_s 或
# libgcc_eh；当前项目的 Gentoo MinGW sysroot 只提供静态 GCC runtime。这里仅把
# 最终可执行文件和动态库链接交给 MinGW GCC driver，编译阶段仍然使用 clang。
if(NOT DEFINED MINGW_TOOLCHAIN_PREFIX OR MINGW_TOOLCHAIN_PREFIX STREQUAL "")
  set(MINGW_TOOLCHAIN_PREFIX "x86_64-w64-mingw32")
endif()

set(CMAKE_C_LINK_EXECUTABLE
    "${MINGW_TOOLCHAIN_PREFIX}-gcc <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
)
set(CMAKE_CXX_LINK_EXECUTABLE
    "${MINGW_TOOLCHAIN_PREFIX}-g++ <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
)
