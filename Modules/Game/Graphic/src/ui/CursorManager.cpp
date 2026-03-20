#include "ui/CursorManager.h"
#include "config/skin/SkinConfig.h"
#include "event/core/DebugEvent.h"
#include "event/core/EventBus.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include <filesystem>

namespace MMM::Graphic::UI
{

CursorManager::CursorManager(vk::PhysicalDevice& phyDevice,
                             vk::Device&         logicalDevice,
                             vk::CommandPool commandPool, vk::Queue queue)
{
    auto skin = Config::SkinManager::instance();

    // RAII 自动加载、注册 ImGui 描述符、并在析构时自动销毁
    m_texCursor =
        std::make_unique<VKTexture>(skin.getAssetPath("cursor").string(),
                                    phyDevice,
                                    logicalDevice,
                                    commandPool,
                                    queue);
    m_texTrail =
        std::make_unique<VKTexture>(skin.getAssetPath("cursortrail").string(),
                                    phyDevice,
                                    logicalDevice,
                                    commandPool,
                                    queue);

    /// @brief 拖尾存活时间(秒)
    // float m_trailLifeTime = 0.3f;
    /// @bried 光标头纹理的大小
    // float m_cursorSize = 48.0f;
    /// @brief 拖尾纹理的基础大小
    // float m_trailSize = 32.0f;
    /// @brief 鼠标移动多远才生成一个新的拖尾点(防止密集堆叠)
    // float m_emitDistance = 2.0f;

    Event::EventBus::instance().subscribe<Event::DebugEvent>(
        [&](Event::DebugEvent e) {
            float* data = static_cast<float*>(e.mem);
            XINFO("trailLifeTime:[{}]", data[0]);
            m_trailLifeTime = data[0];
            XINFO("cursorSize:[{}]", data[1]);
            m_cursorSize = data[1];
            XINFO("trailSize:[{}]", data[2] * m_cursorSize);
            m_trailSize = data[2] * m_cursorSize;
            XINFO("emitDistance :[{}]", data[3]);
            m_emitDistance = data[3];
        });
}

CursorManager::~CursorManager() {}

// 渲染更新函数，每帧调用一次
void CursorManager::UpdateAndDraw()
{
    ImGuiIO& io        = ImGui::GetIO();
    ImVec2   mousePos  = io.MousePos;
    float    deltaTime = io.DeltaTime;

    // 1. 生成新的拖尾点
    // 只有当鼠标移动超过一定距离时，才添加新的点，这样拖尾更平滑
    if ( m_trailPoints.empty() ||
         ImLengthSqr(ImVec2(mousePos.x - m_trailPoints.front().pos.x,
                            mousePos.y - m_trailPoints.front().pos.y)) >
             (m_emitDistance * m_emitDistance) ) {
        m_trailPoints.push_front({ mousePos, 1.0f });
    }

    // 2. 获取前景绘制列表（确保光标在所有 UI 之上）
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    // 3. 更新并绘制拖尾
    // 从后往前遍历，以便安全地删除死亡的粒子
    for ( int i = (int)m_trailPoints.size() - 1; i >= 0; --i ) {
        TrailPoint& p = m_trailPoints[i];

        // 随时间减少生命值
        p.life -= deltaTime / m_trailLifeTime;

        if ( p.life <= 0.0f ) {
            // 如果生命周期结束，从队列末尾移除
            m_trailPoints.pop_back();
            continue;
        }

        // 计算当前点的透明度 (0~255)
        // 可以使用 ease-out 函数让淡出更自然，这里直接用线性 p.life
        int   alpha     = (int)(p.life * 255.0f);
        ImU32 tintColor = IM_COL32(255, 255, 255, alpha);

        // 计算当前点的大小（随着生命周期减小而缩小）
        // 例如：刚生成时是 100% 大小，快消失时是 30% 大小
        float currentSize = m_trailSize * (0.3f + 0.7f * p.life);
        float halfSize    = currentSize * 0.5f;

        // 绘制拖尾纹理 (使用 cursortrail.png)
        if ( m_texTrail ) {
            drawList->AddImage(
                m_texTrail->getImTextureID(),
                ImVec2(p.pos.x - halfSize, p.pos.y - halfSize),  // 左上角
                ImVec2(p.pos.x + halfSize, p.pos.y + halfSize),  // 右下角
                ImVec2(0, 0),
                ImVec2(1, 1),  // UV
                tintColor      // 带透明度的颜色
            );
        }
    }

    // 4. 绘制主光标头 (使用 cursor.png)
    // 主光标永远在最后绘制（最上层），永远是 100% 不透明
    if ( m_texCursor ) {
        float halfCursorSize = m_cursorSize * 0.5f;
        drawList->AddImage(
            m_texCursor->getImTextureID(),
            ImVec2(mousePos.x - halfCursorSize, mousePos.y - halfCursorSize),
            ImVec2(mousePos.x + halfCursorSize, mousePos.y + halfCursorSize));
    }
}

}  // namespace MMM::Graphic::UI
