#include "canvas/Basic2DCanvasInteraction.h"
#include "common/LogicCommands.h"
#include "common/render/HoverRenderData.h"
#include "common/render/RenderSnapshot.h"
#include "config/skin/translation/Translation.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <fmt/format.h>
#include <imgui.h>

namespace MMM::Canvas
{
/// @brief 选择右键列表中的有效物件部位，并保持其后续拾取优先级。
/// @param index 右键打开时冻结列表中的位置，不是实体编号。
/// @param snapshot 本帧不可变快照，负责证明候选仍属于当前谱面。
/// @return 已通过身份检查并发布选择及悬浮命令时返回真。
/// @post 无效候选不改变优先项，也不向逻辑线程发送编辑命令。
/// @details 菜单项表示部位而非整个实体；同一折线的两个节点可以分别出现。
/// 音符和采样来自独立 Registry，实体整数相同也不能合并。
/// 仅保留值语义身份，不捕获 ECS 引用或异步快照所有权。
/// @pre 调用方处于主画布输入域，且已验证菜单仍然允许编辑。
/// @warning 菜单点击的低频路径，只核对不可变快照并发布既有选择命令。
bool Basic2DCanvasInteraction::selectHoverLayer(
    std::size_t index, const Common::Render::RenderSnapshot& snapshot)
{
    // 索引和谱面实例先验证，避免跨文件或同路径重载后的实体号复用。
    // 此处不能把列表位置直接解释成新快照 hitbox 的位置。
    if ( index >= m_menuHoverLayers.size() ||
         m_selectionMenuBeatmapKey != snapshot.beatmapPathKey ||
         m_selectionMenuBeatmapInstance != snapshot.beatmapInstanceId )
        return false;
    const auto candidate = m_menuHoverLayers[index];
    // 菜单冻结的是身份，不保证对象永久存在；激活时用新快照重新核验。
    // 异步编辑可能在菜单停留期间删除节点，失效项应安静忽略。
    // 屏幕坐标不参与身份比较：相机滚动不能把候选变成别的对象。
    // 扫描只发生在菜单激活帧，常规帧不为每个列表项重复扫描 hitbox。
    const bool present = std::any_of(
        snapshot.hitboxes.begin(),
        snapshot.hitboxes.end(),
        [&](const auto& hitbox) {
            return HoverLayerCandidate{ hitbox.entity,
                                        hitbox.kind,
                                        static_cast<std::uint8_t>(hitbox.part),
                                        hitbox.subIndex } == candidate;
        });
    if ( !present ) return false;
    // 优先项跨鼠标移入菜单的短暂空命中保留；返回重叠处才能继续抓取。
    // 索引用于当前展示，后续拾取仍按完整身份重新定位，不依赖旧顺序。
    m_preferredHoverLayer = candidate;
    m_hoverLayerIndex     = static_cast<int>(index);
    m_hasLastHovered      = false;
    // 让下一次画布命中重新发布悬浮，不沿用打开菜单前的最上层缓存。
    // 与画布点击复用选择模式；减选不会因候选菜单变成反向切换。
    UI::Interaction::dispatchCommand(
        Event::LogicCommandEvent(Logic::CmdSelectEntity{
            candidate.entity,
            true,
            candidate.kind,
            UI::Interaction::activeProfile().selectionOperation() }));
    // 同一部位立即成为逻辑 hover，下一次拖动也携带该部位和子索引。
    // 选择本身不产生拖动命令，用户仍能先从菜单检查或改变选择。
    UI::Interaction::dispatchCommand(
        Event::LogicCommandEvent(Logic::CmdSetHoveredEntity{ candidate.entity,
                                                             candidate.part,
                                                             candidate.subIndex,
                                                             candidate.kind }));
    return true;
}

/// @brief 展示右键起点的重叠部位并提交明确的选择意图。
/// @param snapshot 用于菜单激活时校验的新快照，不用于重建右键候选。
/// @details 冻结列表使菜单项在鼠标移动到子菜单时保持稳定。
/// 候选顺序沿用画布拾取顺序，避免菜单和键盘切层各自维护另一套排序。
/// 标签只展示翻译后的类型和部位，不向用户泄露 Registry 内部句柄。
/// 勾选表示后续拾取优先项；追加或减选结果仍由当前选择模式决定。
/// @warning 菜单热路径只绘制少量候选；对象存在性只在点击时核验。
void Basic2DCanvasInteraction::renderHoverLayerMenu(
    const Common::Render::RenderSnapshot& snapshot)
{
    // 候选按原渲染层次展示，序号只表示当前列表顺序，不暴露内部实体号。
    // 零个或一个候选不需要切换入口，避免在普通右键中增加无效操作。
    // Begin/End 使用统一反馈封装，保持菜单音效与悬浮样式一致。
    if ( m_menuHoverLayers.size() > 1 &&
         UI::FeedbackBeginMenu(TR("ui.selection.overlapping").data()) ) {
        for ( std::size_t i = 0; i < m_menuHoverLayers.size(); ++i ) {
            // 菜单保持实体和子部位一一对应，不把折线所有节点折叠成一项。
            const auto& candidate = m_menuHoverLayers[i];
            const char* partKey   = "ui.canvas.hover.head";
            // 部位名称与画布 hover 提示共用翻译，头部保留为默认回退。
            // 长条主体和末端分别命中，直接决定后续拖动编辑的语义。
            switch ( static_cast<Common::Render::HoverPart>(candidate.part) ) {
            case Common::Render::HoverPart::HoldBody:
                partKey = "ui.canvas.hover.hold_body";
                break;
            case Common::Render::HoverPart::HoldEnd:
                partKey = "ui.canvas.hover.hold_end";
                break;
            case Common::Render::HoverPart::FlickArrow:
                partKey = "ui.canvas.hover.flick_end";
                break;
            case Common::Render::HoverPart::PolylineNode:
                // 节点索引保留在候选里；相同标签仍用列表序号和 ID 区分。
                partKey = "ui.canvas.hover.polyline_node";
                break;
            case Common::Render::HoverPart::SampleAnchor:
                // 采样锚点与实际触发位置在绘制上可能分离，不能混用部位。
                partKey = "ui.canvas.hover.sample_anchor";
                break;
            case Common::Render::HoverPart::SampleOffset:
                partKey = "ui.canvas.hover.sample_trigger";
                break;
            default: break;
            }
            const char* kindKey =
                candidate.kind == Logic::ChartObjectKind::AudioSample
                    ? "ui.selection.audio_sample"
                    : "ui.selection.note";
            // 全局序号同时区分多个同类对象，不依赖实体号作为可见标签。
            const auto label = fmt::format(
                "{}: {} · {}", i + 1, TR(kindKey).data(), TR(partKey).data());
            ImGui::PushID(static_cast<int>(i));
            // 独立 ID 防止同名部位共享点击状态，选中项只用完整身份匹配。
            // 该勾选不等价于逻辑多选集合，减选模式下也能指定后续拾取层。
            if ( UI::FeedbackMenuItem(label.c_str(),
                                      nullptr,
                                      m_preferredHoverLayer &&
                                          *m_preferredHoverLayer == candidate) )
                selectHoverLayer(i, snapshot);
            ImGui::PopID();
        }
        UI::FeedbackEndMenu();
        // 只对实际展开的子菜单配对结束，单一候选分支不改变 ImGui 栈。
    }
}
}  // namespace MMM::Canvas
