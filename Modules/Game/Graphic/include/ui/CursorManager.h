#pragma once

#include "graphic/imguivk/VKTexture.h"
#include <deque>
#include <imgui.h>
#include <memory>

namespace MMM
{
namespace Graphic::UI
{

class CursorManager
{
private:
    // 定义拖尾点的数据结构
    struct TrailPoint {
        ImVec2 pos;   // 位置
        float  life;  // 生命周期 (1.0f 降到 0.0f)
    };

    std::deque<TrailPoint> m_trailPoints;

    // --- 可调节的参数 ---

    /// @brief 拖尾存活时间(秒)
    float m_trailLifeTime = 0.3f;
    /// @brief 拖尾纹理的基础大小
    float m_trailSize = 32.0f;
    /// @bried 光标头纹理的大小
    float m_cursorSize = 48.0f;
    /// @brief 鼠标移动多远才生成一个新的拖尾点(防止密集堆叠)
    float m_emitDistance = 2.0f;

    /// @brief 光标主体纹理
    std::unique_ptr<VKTexture> m_texCursor;

    /// @brief 光标拖尾纹理
    std::unique_ptr<VKTexture> m_texTrail;

public:
    CursorManager(vk::PhysicalDevice& phyDevice, vk::Device& logicalDevice,
                  vk::CommandPool commandPool, vk::Queue queue);
    ~CursorManager();
    // 渲染更新函数，每帧调用一次
    void UpdateAndDraw();
};

}  // namespace Graphic::UI
}  // namespace MMM
