#pragma once

#include "event/core/EventBus.h"
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
};

}  // namespace MMM::Canvas
