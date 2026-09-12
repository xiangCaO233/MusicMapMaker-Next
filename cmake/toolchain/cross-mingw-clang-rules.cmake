# clang 的 MinGW GNU driver 在部分 Linux 发行版上会默认寻找不匹配的 GCC runtime。
# 该规则文件只用于排查链接器差异；正常 clang64 构建使用 lld、libc++ 和 compiler-rt。 调试回退显式调用 MinGW GCC
# driver，让其负责补齐对应 libgcc 与系统库。
if(NOT DEFINED MINGW_TOOLCHAIN_PREFIX OR MINGW_TOOLCHAIN_PREFIX STREQUAL "")
  # 独立加载规则文件时使用传统 MinGW prefix，调用方仍可预先覆盖。
  set(MINGW_TOOLCHAIN_PREFIX "x86_64-w64-mingw32")
endif()

# C 与 C++ 分别交给对应 driver，确保 C++ 链接自动加入标准库。
set(CMAKE_C_LINK_EXECUTABLE
    "${MINGW_TOOLCHAIN_PREFIX}-gcc <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
)
set(CMAKE_CXX_LINK_EXECUTABLE
    "${MINGW_TOOLCHAIN_PREFIX}-g++ <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>"
)
