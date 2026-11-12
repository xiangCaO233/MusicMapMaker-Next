#pragma once
#include "canvas/HoverLayerSelection.h"
#include "canvas/PlanarObservationInput.h"
#include "common/render/HoverRenderData.h"
#include <glm/vec2.hpp>
#include <imgui.h>

#include <cstdint>
#include <optional>
#include <span>

namespace MMM::Canvas
{
/// @brief 已选物件的触控目标只借用不可变可见命中框。
/// @details 把手在物件右侧，起点在左侧，短长条两端也不会共用同一热区。
/// @note 把手不是编辑工具，不保存选择集合，也不直接发送编辑命令。
/// @note 只有渲染线程发布的 touchHandle 能力非空时才生成目标。
/// @note 玩家与草稿的实体号可能相同，必须保留完整 ChartObjectKind。
/// @note 屏幕命中只用于起笔，连续拖动仍由公共控制器解释。
/// @note 主题颜色来自当前 ImGui 样式，不能缓存跨主题颜色。
/// @note 函数不拥有 draw list，必须在有效画布窗口内调用。
/// 不放大整个图形，不读取后台注册表，不为每个把手分配状态。
/// 距离相同时保留后绘制对象，与普通逆序拾取一致。
class MobileNoteHandles
{
public:
    /// @brief 绘制并命中可见把手，返回完整领域和部件身份。
    /// @warning 每帧线性遍历已有可见框，不排序、不访问 ECS、不分配容器。
    /// @param boxes 当前画布投影快照，不跨帧借用。
    /// @param origin 内容区屏幕原点，不含窗口标题栏。
    /// @param point 真实手指屏幕位置，起笔时由调用者锁存结果。
    /// @param scale 内容倍率只影响把手尺寸，不改变拖动时间映射。
    /// @param projection 当前画布正向投影，二维表面可为空。
    /// @param yOffset 快照图形尚未应用的动态纵向偏移。
    /// @return 最近的把手；没有命中时继续普通图形拾取。
    static std::optional<HoverLayerCandidate> render(
        std::span<const Common::Render::Hitbox> boxes, ImVec2 origin,
        ImVec2 point, float scale,
        const PlanarObservationInputProjection* projection, float yOffset)
    {
        std::optional<HoverLayerCandidate> result;
        // 严格比较在相同距离时保留逆序遍历的顶层对象。
        float best = 22 * scale * 22 * scale;
        auto* draw = ImGui::GetWindowDrawList();
        // 使用当前主题颜色，命中半径大于可见圆，留出手指容差。
        const auto color = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
        for ( auto it = boxes.rbegin(); it != boxes.rend(); ++it ) {
            const auto& box = *it;
            // 空能力覆盖未选中、复合物件以及不提供局部编辑的身体。
            // 因此不把全部拾取矩形无差别扩展成手指热区。
            if ( box.touchHandle == Common::Render::HoverPart::None ) continue;
            // 动态播放插值与普通拾取共享偏移，再投影到真实屏幕。
            // 透视模式也在投影之后偏置把手，保证触控尺寸恒定。
            glm::vec2 anchor{ box.x + box.w * 0.5f,
                              box.y + box.h * 0.5f + yOffset };
            if ( projection ) {
                const auto projected =
                    planarObservationProject(*projection, anchor);
                if ( !projected ) continue;
                anchor = *projected;
            }
            // 起点与终点分别放在左右侧，正文继续作为整体移动目标。
            // 单键没有可独立修改的持续时间，因此把手仍落在本体中心。
            // 横向偏置不影响端点的纵向时间，避免手指遮住正在调整的位置。
            // Note 保留本体中心，否则整体移动会因偏置跳到相邻轨道。
            // Note 必须位于本体中心，否则整体拖动会跳到相邻轨道。
            // 长条偏置只影响横轴，时间反投影仍取端点原始纵坐标。
            const float offset =
                box.touchHandle == Common::Render::HoverPart::Head
                    ? 0
                    : (box.touchHandle == Common::Render::HoverPart::HoldStart
                           ? -1
                           : 1) *
                          (box.w * 0.5f + 24 * scale);
            ImVec2 c{ anchor.x + offset, anchor.y };
            c.x += origin.x;
            c.y += origin.y;
            // 圆超出画布裁剪区时也不接受输入，不能穿透相邻窗口。
            const auto min = draw->GetClipRectMin(),
                       max = draw->GetClipRectMax();
            if ( c.x < min.x || c.x > max.x || c.y < min.y || c.y > max.y )
                continue;
            draw->AddLine({ origin.x + anchor.x, c.y }, c, color);
            draw->AddCircleFilled(
                c, 8 * scale, ImGui::GetColorU32(ImGuiCol_WindowBg));
            draw->AddCircle(c, 8 * scale, color, 0, 2 * scale);
            const float dx = point.x - c.x, dy = point.y - c.y;
            // 平方距离避免每个可见部件执行开方，也无需生成排序列表。
            // 一个触点只选最近目标，半径外不保留上一帧命中。
            const float distance = dx * dx + dy * dy;
            if ( distance >= best ) continue;
            // 完整复合身份随触点锁存，不用下一帧 Hover 推断端点。
            best = distance;
            result =
                HoverLayerCandidate{ box.entity,
                                     box.kind,
                                     static_cast<std::uint8_t>(box.touchHandle),
                                     box.subIndex };
        }
        return result;
    }
};
}  // namespace MMM::Canvas
