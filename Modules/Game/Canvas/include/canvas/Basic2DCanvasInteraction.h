#pragma once

#include "event/core/EventBus.h"
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <string>
#include <unordered_set>
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

    /// @brief 上一次发送的连续拖动编辑命令，用于过滤同一手势内的重复更新。
    struct LastContinuousEditCommand {
        /// @brief 是否已经记录过一次拖动编辑命令。
        bool valid{ false };
        /// @brief 上一次发送的本地鼠标坐标。
        glm::vec2 pos{ 0.0f, 0.0f };
        /// @brief 上一次发送时的主修饰键状态。
        bool primaryModifier{ false };
        /// @brief 上一次发送时的副修饰键状态。
        bool secondaryModifier{ false };
    };

    std::string              m_canvasName;
    std::string              m_cameraId;
    std::vector<PendingDrop> m_pendingDrops;
    Event::SubscriptionID    m_dropSubId;

    void handleDrops(UI::UIManager* sourceManager);
    void handleHotkeys(const Logic::RenderSnapshot* currentSnapshot);
    void handleInteractions(const Logic::RenderSnapshot* currentSnapshot,
                            float targetWidth, float targetHeight);
    /// @brief 判断连续拖动编辑命令是否需要发送，并在需要时更新缓存。
    /// @param last 上一次发送的拖动编辑命令状态。
    /// @param pos 当前本地鼠标坐标。
    /// @param primaryModifier 当前主修饰键状态。
    /// @param secondaryModifier 当前副修饰键状态。
    /// @return 需要发送命令时返回 true。
    /// @warning UI 热路径：拖动编辑期间每帧调用；只做常量级数值比较。
    bool shouldSendContinuousEditCommand(LastContinuousEditCommand& last,
                                         glm::vec2 pos, bool primaryModifier,
                                         bool secondaryModifier);
    /// @brief 清空同一左键手势下的连续拖动编辑命令缓存。
    void resetContinuousEditCommands();

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
    /// @brief 当前配色笔刷/橡皮拖动手势中已经处理过的实体。
    std::unordered_set<entt::entity> m_colorStrokeEntities;
    /// @brief 上一次发送的框选拖动更新。
    LastContinuousEditCommand m_lastMarqueeUpdateCommand;
    /// @brief 上一次发送的绘制笔刷拖动更新。
    LastContinuousEditCommand m_lastBrushUpdateCommand;
    /// @brief 上一次发送的移动拖拽更新。
    LastContinuousEditCommand m_lastMoveUpdateCommand;
    /// @brief 上一次发送的擦除拖动更新。
    LastContinuousEditCommand m_lastEraseUpdateCommand;
};

}  // namespace MMM::Canvas
