#include <vulkan/vulkan_hpp_macros.hpp>

// 本编译单元不包含 vulkan.hpp，避免完整 SDK 间接补齐所缺的声明或宏。
// 新 SDK 也显式模拟旧 SDK 缺少导出宏的环境，避免本机版本掩盖回归。
// 命名空间宏仍由 SDK 配置提供，不能为了测试而另造一套 vk 类型。
#ifdef VULKAN_HPP_EXPORT
#    undef VULKAN_HPP_EXPORT
#endif

#include "graphic/imguivk/IGraphicUserHook.h"

#include <type_traits>

/// @file
/// @brief 无完整 Vulkan 句柄定义、无模块导出宏时也能独立包含图形回调接口。
/// @note 另一个编译单元验证完整 SDK 先被包含时，前向声明仍与真实类型兼容。
namespace
{
// 仅形成成员函数指针，不构造句柄，也不需要运行 GPU 或加载 Vulkan 设备。
using Hook    = MMM::Graphic::IGraphicUserHook;
using Record  = void (Hook::*)(vk::CommandBuffer&, uint32_t);
using Prepare = void (Hook::*)(vk::PhysicalDevice&, vk::Device&,
                               MMM::Graphic::VKSwapchain&, vk::CommandPool&,
                               vk::Queue&);
static_assert(std::is_same_v<decltype(&Hook::onRecordOffscreen), Record>);
static_assert(std::is_same_v<decltype(&Hook::onPrepareResources), Prepare>);
static_assert(std::is_abstract_v<Hook>);
}  // namespace

/// @brief 编译通过即验证接口的前向声明与签名，无运行时图形依赖。
/// @return 所有静态断言成立时返回零。
int main()
{
    return 0;
}
