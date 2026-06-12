#pragma once

#include "graphic/imguivk/VKTexture.h"
#include <deque>
#include <imgui.h>
#include <memory>

namespace MMM
{
namespace Graphic
{

class CursorManager
{
private:
    struct TrailPoint {
        ImVec2 pos;
        float  life;
    };

    // 分开存储拖尾和烟雾，因为它们的生命周期和行为不同
    std::deque<TrailPoint> m_trailPoints;
    std::deque<TrailPoint> m_smokePoints;

    // --- 拖尾参数 ---
    float m_trailLifeTime = 0.4f;
    float m_trailSize     = 48.0f;
    float m_cursorSize    = 72.0f;
    float m_emitDistance  = 1.5f;

    // --- 烟雾参数 ---
    float m_smokeLifeTime  = 0.8f;   // 烟雾存活久一点
    float m_smokeSize      = 32.0f;  // 烟雾初始大小
    float m_smokeExpansion = 1.5f;   // 烟雾随时间扩张的倍率
    float m_smokeOpacity   = 0.25f;  // 烟雾的基础不透明度（烟雾通常很淡）

    std::unique_ptr<VKTexture> m_texCursor;
    std::unique_ptr<VKTexture> m_texTrail;
    std::unique_ptr<VKTexture> m_texSmoke;  // 新增烟雾纹理

public:
    CursorManager(vk::PhysicalDevice& phyDevice, vk::Device& logicalDevice,
                  vk::CommandPool commandPool, vk::Queue queue);
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

    void UpdateAndDraw(float smokeLifeOverride = -1.0f);
};

}  // namespace Graphic
}  // namespace MMM
