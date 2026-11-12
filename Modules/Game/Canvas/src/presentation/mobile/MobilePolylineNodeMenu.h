#pragma once
#include "canvas/HoverLayerSelection.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/skin/translation/Translation.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/Icons.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <cstdint>
#include <imgui.h>
#include <optional>

namespace MMM::Canvas
{
/// @brief 长按节点的离散操作，只冻结值身份，不保存快照或注册表引用。
/// @details 菜单展开期间先选择重叠层，随后按钮只修改该层的一个节点。
/// 删除发给逻辑线程，UI 不模拟橡皮擦或操作撤销栈。
class MobilePolylineNodeMenu
{
public:
    /// @brief 新菜单开始时清除旧身份，节点令牌由当前可见命中框提供。
    void capture(const Common::Render::RenderSnapshot&     snapshot,
                 const std::optional<HoverLayerCandidate>& candidate)
    {
        // 新菜单先清除旧操作，空白长按不能沿用上一节点。
        // 候选只有值身份，不依赖外部容器的存储寿命。
        // 节点令牌必须来自同一帧发布数据。
        m_candidate = candidate;
        m_target.reset();
        if ( !candidate ||
             candidate->part != static_cast<std::uint8_t>(
                                    Common::Render::HoverPart::PolylineNode) )
            return;
        // 只读可见几何，不跨线程读取可变节点组件。
        // 复合身份包含注册表领域，同号实体不能互相替代。
        // 空令牌无法证明节点身份，因此不提供结构操作。
        for ( const auto& hit : snapshot.hitboxes ) {
            if ( hit.entity != candidate->entity ||
                 hit.kind != candidate->kind ||
                 hit.subIndex != candidate->subIndex ||
                 hit.part != Common::Render::HoverPart::PolylineNode ||
                 hit.touchNodeToken == 0 ||
                 (candidate->nodeToken &&
                  candidate->nodeToken != hit.touchNodeToken) )
                continue;
            // 打开时冻结，后台更新不能在下一帧静默把下标换成另一个节点。
            m_target = Logic::CmdDeletePolylineNode{
                hit.entity, hit.kind, hit.subIndex, hit.touchNodeToken
            };
            break;
        }
    }
    /// @brief 显式切层后重取目标；相同身份不逐帧覆盖被冻结的命令。
    /// @param preferred 用户经重叠层列表明确选定的部位；空值不表示默认首层。
    /// @param ambiguous 当前长按处存在多个候选，未选层时禁止结构编辑。
    /// @param size 与通用动作卡片相同的尺寸，内容较长时由外层滚动。
    /// @param gap 两列卡片的主题间距，不另设覆盖整个画布的命中区域。
    /// @warning 弹层帧只比较值并扫描已发布的可见框，不等待逻辑线程。
    void render(const Common::Render::RenderSnapshot&     snapshot,
                const std::optional<HoverLayerCandidate>& preferred,
                bool ambiguous, ImVec2 size, float gap)
    {
        // 显式切层才重新捕获，相同目标的命令保持冻结。
        // 后台结构变化后旧下标不能自动换成相邻节点。
        // 重新打开菜单才会取得新的有效快照。
        if ( preferred && preferred != m_candidate )
            capture(snapshot, preferred);
        if ( ambiguous && !preferred ) {
            // 不能猜测重叠节点；说明常驻，不把 Hover 当作唯一提示入口。
            ImGui::TextWrapped("%s", TR("ui.mobile.node.choose_layer").data());
            return;
        }
        // 非节点部位继续使用原通用菜单，不伪造局部入口。
        // 节点和身体可能重叠，但其命令含义必须分开。
        if ( !m_target ) return;
        // 启用按钮前复核可见目标，逻辑命令到达时仍需再次验证。
        // 前者防止失效菜单误导，后者防止异步队列造成的过期写入。
        // UI 检查不能替代播放、联机只读和专业模式门禁。
        bool present{};
        for ( const auto& hit : snapshot.hitboxes ) {
            // 即使实体没变，节点插入删除后也必须复核冻结的稳定令牌。
            if ( hit.entity == m_target->entity && hit.kind == m_target->kind &&
                 hit.subIndex == m_target->subIndex &&
                 hit.touchNodeToken == m_target->nodeToken ) {
                present = true;
                break;
            }
        }
        ImGui::Text("%s #%d",
                    TR("ui.canvas.hover.polyline_node").data(),
                    m_target->subIndex + 1);
        // 删除按钮跨两列宽度，继续使用统一主题间距和反馈入口。
        // 首节点删除会按旧橡皮擦删除整条，菜单让用户使用明确的删除物件入口。
        // 触控卡片沿用统一反馈入口，原长按释放保护由外层维持。
        // 没有长按一成立就执行删除的路径，也不借用修饰键。
        // 禁用按钮不改变已有选区。
        // 不提供等分插点：连续 Hold/Flick 会破坏交替段模型。
        size.x = size.x * 2 + gap;
        ImGui::BeginDisabled(!present || m_target->subIndex == 0);
        if ( UI::workspaceActionButton(TR("ui.mobile.node.delete").data(),
                                       UI::ICON_MMM_ERASER,
                                       size) )
            submit();
        ImGui::EndDisabled();
        // 增加新转折只能使用原有正交绘制状态机，不插入普通 Note。
        ImGui::TextWrapped("%s", TR("ui.mobile.node.draw").data());
        if ( m_target->subIndex == 0 )
            ImGui::TextWrapped("%s", TR("ui.mobile.node.root_delete").data());
        ImGui::Separator();
    }

private:
    /// @brief 只在新的按钮释放时发送一次命令，长按起笔释放保护由外层维护。
    void submit()
    {
        // 点击时复制轻量载荷，关闭弹窗不会让命令悬空。
        // 队列持有自己的副本，不保存后台组件引用。
        // 失败不发 Undo，避免撤销上一笔操作。
        const auto command = *m_target;
        UI::Interaction::dispatchCommand(Event::LogicCommandEvent(command));
        ImGui::CloseCurrentPopup();
    }
    /// @brief 重叠层身份的最近选择，不持有任何 UI 对象。
    std::optional<HoverLayerCandidate> m_candidate;
    /// @brief 打开菜单时锁存的结构命令，失效时不刷新到相邻节点。
    std::optional<Logic::CmdDeletePolylineNode> m_target;
};
}  // namespace MMM::Canvas
