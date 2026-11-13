#pragma once
#include "canvas/HoverLayerSelection.h"
#include "canvas/PlanarObservationInput.h"
#include "common/render/HoverRenderData.h"
#include "config/skin/translation/Translation.h"
#include <glm/vec2.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace MMM::Canvas
{
/// @brief 已选物件的触控目标只借用不可变可见命中框。
/// @details 首尾胶囊分别贴在端点的左右边缘，原有外侧热区完整保留。
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
    /// @param preferred 长按菜单显式指定的重叠层，可覆盖最近距离优先。
    /// @param active 已锁存的拖动目标，仅用于按住强调色，不参与重新拾取。
    /// @return 最近的把手；没有命中时继续普通图形拾取。
    /// @details 原热区半径仍是 22 个逻辑像素，不能按可见胶囊尺寸缩放。
    /// 新热区只做并集扩展，旧中心位置和边缘距离保持原样。
    /// 短长条首尾热区可相交，最近距离和渲染顺序共同决定唯一目标。
    /// 单键轮廓不改变其中心抓取参照，防止移动时跳轨。
    /// 长条的首尾分别使用独立部件身份，不因为标记贴边改成整体移动。
    /// 平台起点归属仍由调用者验证，本函数不绕过窗口或弹层仲裁。
    /// 所有角点从同一快照投影，不读取逻辑线程正在写入的组件。
    static std::optional<HoverLayerCandidate> render(
        std::span<const Common::Render::Hitbox> boxes, ImVec2 origin,
        ImVec2 point, float scale,
        const PlanarObservationInputProjection* projection, float yOffset,
        const HoverLayerCandidate* active    = nullptr,
        const HoverLayerCandidate* preferred = nullptr)
    {
        std::optional<HoverLayerCandidate> result;
        // 严格比较在相同距离时保留逆序遍历的顶层对象。
        float best = 22 * scale * 22 * scale;
        auto* draw = ImGui::GetWindowDrawList();
        // 优先层只在自己的热区内生效，不能跨空白吸走触点。
        // 命中后仍绘制其余标记，不能提前结束框遍历。
        bool pinned{};
        // 可见标记使用主题颜色；按住时加深，不用悬浮作为唯一反馈。
        // 细图形与手指热区独立，不能用胶囊的宽度代替原有命中半径。
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
            const bool note =
                box.touchHandle == Common::Render::HoverPart::Head ||
                box.touchHandle == Common::Render::HoverPart::PolylineNode;
            // 采样本体与实际触发点拥有不同时间，左右分居避免零偏移重合误抓。
            // 两个目标仍使用原 SampleAnchor/SampleOffset 命令，不伪造音符身份。
            // 已选能力由渲染发布，平台解释器不读取可变采样组件。
            const bool sample = box.kind == Logic::ChartObjectKind::AudioSample;
            const bool sampleOffset =
                sample &&
                box.touchHandle == Common::Render::HoverPart::SampleOffset;
            const bool start =
                box.touchHandle == Common::Render::HoverPart::HoldStart ||
                (sample && !sampleOffset);
            const HoverLayerCandidate candidate{ box.entity,
                                                 box.kind,
                                                 static_cast<std::uint8_t>(
                                                     box.touchHandle),
                                                 box.subIndex,
                                                 box.touchNodeToken };
            const bool                pressed = active && *active == candidate;
            const auto                color   = ImGui::GetColorU32(
                pressed ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab,
                pressed ? 1.0f : 0.85f);
            // 旧圆的中心仅用于命中兼容，视觉上不再绘制它或连接线。
            // 不移动旧中心，保证热区边缘仍覆盖改版前可操作的位置。
            const float offset =
                note ? 0 : (start ? -1 : 1) * (box.w * 0.5f + 24 * scale);
            // 胶囊贴在真实投影边缘，而非用未投影宽度猜测透视位置。
            // 屏幕尺寸固定，缩放物件不会让胶囊变成大面积遮挡。
            glm::vec2 edge{ start ? box.x : box.x + box.w,
                            box.y + box.h * 0.5f + yOffset };
            if ( projection ) {
                const auto projected =
                    planarObservationProject(*projection, edge);
                if ( !projected ) continue;
                edge = *projected;
            }
            const ImVec2 grip{ note ? origin.x + anchor.x : origin.x + edge.x,
                               origin.y + (note ? anchor.y : edge.y) };
            // 采样热区围绕实际边缘，不沿用长条的旧外侧圆心。
            // 锚点本体左边缘与触发点右边缘即使同拍仍可分别命中。
            const ImVec2 c   = sample ? grip
                                      : ImVec2{ origin.x + anchor.x + offset,
                                                origin.y + anchor.y };
            const auto   min = draw->GetClipRectMin(),
                         max = draw->GetClipRectMax();
            const auto   visible = [&](ImVec2 p) {
                return p.x >= min.x && p.x <= max.x && p.y >= min.y &&
                       p.y <= max.y;
            };
            // 图元只负责展示，不消费鼠标边沿；同一手势仍由通用控制器提交。
            // 按住状态借用上帧锁存身份，胶囊变色不会使目标随手指改选。
            if ( note ) {
                // Note 不再额外添加控件，只描出本体轮廓，原中心仍可拖动。
                // 四角分别投影，观察视角下也不会画成另一套轴对齐包围盒。
                std::array<glm::vec2, 4> corners{
                    { { box.x, box.y + yOffset },
                      { box.x + box.w, box.y + yOffset },
                      { box.x + box.w, box.y + box.h + yOffset },
                      { box.x, box.y + box.h + yOffset } }
                };
                bool valid = true;
                for ( auto& corner : corners ) {
                    if ( projection ) {
                        const auto projected =
                            planarObservationProject(*projection, corner);
                        if ( !projected ) {
                            valid = false;
                            break;
                        }
                        corner = *projected;
                    }
                    corner.x += origin.x;
                    corner.y += origin.y;
                }
                if ( valid )
                    draw->AddQuad({ corners[0].x, corners[0].y },
                                  { corners[1].x, corners[1].y },
                                  { corners[2].x, corners[2].y },
                                  { corners[3].x, corners[3].y },
                                  color,
                                  scale);
            } else if ( visible(grip) ) {
                // 窄胶囊沿端点侧边排列，头尾分居两侧，不覆盖长条主体。
                // 圆角服从当前主题；零圆角主题仍能保持一致的控件外观。
                draw->AddRectFilled(
                    { grip.x - 2 * scale, grip.y - 9 * scale },
                    { grip.x + 2 * scale, grip.y + 9 * scale },
                    color,
                    std::min(ImGui::GetStyle().FrameRounding, 2 * scale));
            }
            // 说明只在真实按住目标时出现，不把鼠标悬浮当作触屏反馈。
            // 使用既有译文，按主题字号绘制；没有全局样式或配置写入。
            if ( sample && pressed && visible(grip) ) {
                const auto label =
                    TR(sampleOffset ? "ui.canvas.hover.sample_trigger"
                                    : "ui.canvas.hover.sample_anchor");
                // 靠近上沿与右沿时把说明留在内容裁剪区内，不盖住页签。
                // 翻译宽度参与定位，不假定中英文长度相同。
                // 极窄视图仍以左边界为下限，不能给 clamp 传入倒置范围。
                const auto textSize = ImGui::CalcTextSize(label.data());
                // 画布图片不服从界面主题明暗，说明需自带主题底色保持对比度。
                // 包围盒连同样式内边距一起夹入内容区，不能盖住页签。
                const auto&  style   = ImGui::GetStyle();
                const auto   padding = style.FramePadding;
                const ImVec2 textPoint{
                    std::max(min.x + padding.x,
                             std::min(grip.x + 6 * scale,
                                      max.x - textSize.x - padding.x)),
                    std::max(min.y + padding.y,
                             grip.y - ImGui::GetFontSize() - 10 * scale)
                };
                // 圆角与间距直接跟随当前样式，不另外保存固定移动端外观。
                draw->AddRectFilled(
                    { textPoint.x - padding.x, textPoint.y - padding.y },
                    { textPoint.x + textSize.x + padding.x,
                      textPoint.y + textSize.y + padding.y },
                    ImGui::GetColorU32(ImGuiCol_PopupBg),
                    style.FrameRounding);
                draw->AddText(
                    textPoint, ImGui::GetColorU32(ImGuiCol_Text), label.data());
            }
            const float dx = point.x - c.x, dy = point.y - c.y;
            // 平方距离避免每个可见部件执行开方，也无需生成排序列表。
            // 一个触点只选最近目标，半径外不保留上一帧命中。
            // 热区取原圆与新胶囊邻域的并集，不能因视觉变细缩小可操作面积。
            // 两处重叠仍只返回一个完整身份，不额外创建编辑事务。
            const float gx = point.x - grip.x, gy = point.y - grip.y;
            const float distance = std::min(
                visible(c) ? dx * dx + dy * dy : 22 * scale * 22 * scale,
                visible(grip) ? gx * gx + gy * gy : 22 * scale * 22 * scale);
            // 距离以两个候选区域中最近者计算，重叠时仍保留顶层优先级。
            // 原中心在裁剪区外时沿用旧入口的拒绝规则，新的可见入口独立判断。
            // 不把触摸范围写回渲染包围盒，桌面及其它工具的拾取不受影响。
            if ( distance >= 22 * scale * 22 * scale ) continue;
            // 已显式选择的重叠层优先，不能让扩大的相邻热区抢回目标。
            const bool chosen = preferred && *preferred == candidate;
            if ( !chosen && (pinned || distance >= best) ) continue;
            pinned = chosen;
            // 完整复合身份随触点锁存，不用下一帧 Hover 推断端点。
            best = distance;
            result =
                HoverLayerCandidate{ box.entity,
                                     box.kind,
                                     static_cast<std::uint8_t>(box.touchHandle),
                                     box.subIndex,
                                     box.touchNodeToken };
        }
        return result;
    }
};
}  // namespace MMM::Canvas
