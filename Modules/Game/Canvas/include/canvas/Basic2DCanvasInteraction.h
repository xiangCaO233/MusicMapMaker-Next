#pragma once

#include "event/core/EventBus.h"
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace MMM::Logic
{
struct RenderSnapshot;
}

namespace MMM::UI
{
class UIManager;
}

namespace MMM::Canvas
{

class Basic2DCanvasInteraction
{
public:
    Basic2DCanvasInteraction(const std::string& canvasName,
                             const std::string& cameraId);
    ~Basic2DCanvasInteraction();

    void update(UI::UIManager*               sourceManager,
                const Logic::RenderSnapshot* currentSnapshot, float targetWidth,
                float targetHeight);

private:
    struct PendingDrop {
        std::vector<std::string> paths;
        glm::vec2                pos;
    };

    /// @brief 上一次发送给逻辑线程的鼠标状态，用于过滤重复交互命令。
    struct LastMouseCommand {
        /// @brief 是否已经记录过一次鼠标命令。
        bool valid{ false };
        /// @brief 上一次发送的本地鼠标坐标。
        glm::vec2 pos{ 0.0f, 0.0f };
        /// @brief 上一次发送的视口宽度。
        float viewportWidth{ 0.0f };
        /// @brief 上一次发送的视口高度。
        float viewportHeight{ 0.0f };
        /// @brief 上一次发送的窗口悬浮状态。
        bool isHovering{ false };
        /// @brief 上一次发送的鼠标拖拽状态。
        bool isDragging{ false };
    };

    std::string              m_canvasName;
    std::string              m_cameraId;
    std::vector<PendingDrop> m_pendingDrops;
    Event::SubscriptionID    m_dropSubId;

    void handleDrops(UI::UIManager* sourceManager);
    void handleHotkeys(const Logic::RenderSnapshot* currentSnapshot);
    void handleInteractions(const Logic::RenderSnapshot* currentSnapshot,
                            float targetWidth, float targetHeight);

    float m_speedTooltipTimer{ 0.0f };
    float m_speedTooltipValue{ 1.0f };
    /// @brief 当前鼠标下所有悬浮候选层的签名，用于检测是否切换到其他物件集合。
    std::string m_hoverLayerSignature;
    /// @brief 当前生效的悬浮候选层索引。
    int m_hoverLayerIndex{ 0 };
    /// @brief 当前鼠标下可切换的悬浮候选层数量。
    int m_hoverLayerCount{ 0 };
    /// @brief 上一次发送给逻辑线程的鼠标状态。
    LastMouseCommand m_lastMouseCommand;
    /// @brief 上一次发送给逻辑线程的悬浮实体。
    entt::entity m_lastHoveredEntity{ entt::null };
    /// @brief 上一次发送给逻辑线程的悬浮部位。
    uint8_t m_lastHoveredPart{ 0 };
    /// @brief 上一次发送给逻辑线程的悬浮子索引。
    int m_lastHoveredSubIndex{ -1 };
    /// @brief 是否已经发送过悬浮状态。
    bool m_hasLastHovered{ false };
    /// @brief 左键按下时是否位于画布内。
    bool m_leftPressStartedOnCanvas{ false };
    /// @brief 左键按下时是否命中实体。
    bool m_leftPressStartedOnEntity{ false };
    /// @brief 当前左键手势是否已经发生拖动。
    bool m_leftPressDragged{ false };
};

}  // namespace MMM::Canvas
