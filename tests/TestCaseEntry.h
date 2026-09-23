#pragma once

#include <type_traits>

namespace MMM::Test
{
/// @brief 调用测试源码的原始 main，并适配无参数及 argc/argv 两种入口。
/// @tparam MainFunction 被包装测试入口的函数指针类型。
/// @param mainFunction 已更名为独立符号的测试入口。
/// @param argc 传给原测试的参数数量，包含其自身的 argv[0]。
/// @param argv 传给原测试的参数数组，不包含套件分派参数。
/// @return 原测试的退出码，保持细分 CTest 的失败语义。
/// @note 每个 CTest 用例独立启动套件进程，避免静态状态或 ImGui 上下文串扰。
template<typename MainFunction>
int invokeTestMain(MainFunction mainFunction, int argc, char** argv)
{
    if constexpr ( std::is_invocable_r_v<int, MainFunction, int, char**> )
        return mainFunction(argc, argv);
    else {
        static_assert(std::is_invocable_r_v<int, MainFunction>);
        return mainFunction();
    }
}
}  // namespace MMM::Test
