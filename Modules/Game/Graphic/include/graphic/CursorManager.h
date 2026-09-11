#pragma once

#include <deque>
#include <imgui.h>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace MMM
{
namespace Graphic
{
class VKTexture;

/// @brief 管理软件光标纹理以及每帧拖尾、烟雾粒子的更新与绘制。
///
/// 纹理由资源重载路径独占持有，粒子只保存屏幕位置和归一化剩余寿命；
/// Vulkan 上传与 ImGui 绘制阶段分离，避免在每帧更新中读取文件。
class CursorManager
{
private:
    /// @brief 软件光标粒子的最小跨帧状态。
    struct TrailPoint {
        /// @brief 粒子生成时的屏幕坐标。
        ImVec2 pos;
        /// @brief 从 1 递减至 0 的归一化剩余寿命。
        float life;
    };

    /// @brief 光标拖尾粒子；队首为最新采样点。
    std::deque<TrailPoint> m_trailPoints;
    /// @brief 烟雾粒子；与拖尾分开衰减和缩放。
    std::deque<TrailPoint> m_smokePoints;

    /// @brief 默认拖尾寿命，供光标效果参数初始化使用。
    float m_trailLifeTime = 0.4f;
    /// @brief 默认拖尾纹理边长。
    float m_trailSize = 48.0f;
    /// @brief 默认主光标纹理边长。
    float m_cursorSize = 72.0f;
    /// @brief 相邻粒子采样所需的最小屏幕距离。
    float m_emitDistance = 1.5f;

    /// @brief 默认烟雾寿命。
    float m_smokeLifeTime = 0.8f;
    /// @brief 默认烟雾纹理初始边长。
    float m_smokeSize = 32.0f;
    /// @brief 烟雾在生命周期内的扩张倍率。
    float m_smokeExpansion = 1.5f;
    /// @brief 烟雾基础不透明度。
    float m_smokeOpacity = 0.25f;

    /// @brief 光标按压缩放动画进度，0 表示原始大小，1 表示按下大小。
    float m_pressAmount{ 0.0f };

    /// @brief 主光标皮肤纹理。
    std::unique_ptr<VKTexture> m_texCursor;
    /// @brief 拖尾皮肤纹理。
    std::unique_ptr<VKTexture> m_texTrail;
    /// @brief 烟雾皮肤纹理。
    std::unique_ptr<VKTexture> m_texSmoke;

public:
    /// @brief 创建软件光标管理器并加载当前皮肤纹理。
    /// @param phyDevice Vulkan 物理设备。
    /// @param logicalDevice Vulkan 逻辑设备。
    /// @param commandPool 纹理上传命令池。
    /// @param queue 纹理上传队列。
    CursorManager(vk::PhysicalDevice& phyDevice, vk::Device& logicalDevice,
                  vk::CommandPool commandPool, vk::Queue queue);
    /// @brief 在完整 VKTexture 类型可见的实现文件中销毁纹理。
    ~CursorManager();

    /// @brief 重新加载软件光标相关皮肤纹理。
    /// @param phyDevice Vulkan 物理设备。
    /// @param logicalDevice Vulkan 逻辑设备。
    /// @param commandPool 上传命令池。
    /// @param queue 上传队列。
    /// @warning 低频资源重载路径：皮肤热切换时调用，会等待设备空闲并替换
    /// ImGui 光标纹理，禁止放入每帧绘制路径。
    void reloadSkinTextures(vk::PhysicalDevice& phyDevice,
                            vk::Device&         logicalDevice,
                            vk::CommandPool commandPool, vk::Queue queue);

    /// @brief 更新并绘制软件光标、拖尾与烟雾效果。
    /// @param smokeLifeOverride 烟雾存活时间覆盖值，小于等于 0 时使用配置值。
    /// @warning UI 热路径：每帧调用；只更新光标粒子缓存、按压动画状态并提交
    /// 固定类型 ImGui 绘制命令。
    void UpdateAndDraw(float smokeLifeOverride = -1.0f);
};

}  // namespace Graphic
}  // namespace MMM
