#pragma once

#include "graphic/imguivk/VulkanHandleFwd.h"
#include <memory>

namespace MMM::Graphic
{
class VKSwapchain;
}

namespace MMM::Main
{
/// @brief 启动窗口专用的单色 Logo Vulkan 管线。
/// @details 复用主窗口的 ImGui 渲染通道，不创建第二个交换链或离屏纹理。
class StartupLogoShader
{
public:
    /// @brief 为启动窗口当前颜色附件建立兼容管线。
    /// @warning 低频初始化：只在启动窗口首次准备资源时调用。
    StartupLogoShader(vk::PhysicalDevice& physicalDevice, vk::Device& device,
                      Graphic::VKSwapchain&  swapchain,
                      const vk::CommandPool& commandPool,
                      const vk::Queue&       queue);

    /// @brief 回收管线前等待已提交的启动帧结束。
    /// @warning 仅在销毁启动窗口之前调用，设备必须仍然存活。
    ~StartupLogoShader();

    /// @brief 判断动画能否通过 GPU 正常绘制。
    /// @warning 每帧读取固定句柄，失败时 draw 使用同一 SVG 的静态纹理。
    [[nodiscard]] bool isValid() const;

    /// @brief 在窗口绘制列表中加入居中的 Logo shader 命令。
    /// @param x 绘制区域左边界，使用 ImGui 屏幕坐标。
    /// @param y 绘制区域上边界，使用 ImGui 屏幕坐标。
    /// @param size 绘制区域边长。
    /// @param progress 动画已完成的归一化时间。
    /// @warning 每帧只登记 draw callback，不创建资源或等待设备。
    void draw(float x, float y, float size, float progress) const;

private:
    /// @brief 隐藏 Vulkan 管线句柄，避免公开头引入完整 Vulkan 定义。
    struct State;
    /// @brief 保持设备与管线布局直到最后一个回调结束。
    std::unique_ptr<State> m_state;
};
}  // namespace MMM::Main
