/// @file Basic2DCanvas_AnnotationMenu.cpp
/// @brief 批注纯鼠标菜单只管理读取目标和编辑器请求，不承担谱面写入。
/// @details 交互约束：
/// - 普通右键只在纯鼠标方案启用，经典详情卡仍沿用原有修饰键入口。
/// - 相机 ID 隔离窗口命名空间，同一谱面的不同画布不会共用菜单状态。
/// - 目标身份来自当前发布快照，不读取可变会话、Registry 或工程文件。
/// - 上一条与下一条只改变阅读目标，不产生谱面历史或网络修改。
/// - 点击编辑后才复制正文；复制结果交给既有确认弹窗统一保存。
/// - 同路径重载、分组变化和目标删除都关闭旧菜单，不猜测替代目标。
#include "canvas/Basic2DCanvasInteraction.h"
#include "common/render/AnnotationRenderData.h"
#include "common/render/RenderSnapshot.h"
#include "config/skin/translation/Translation.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <imgui.h>
#include <string>

namespace MMM::Canvas
{
/// @brief 在纯鼠标方案提供稳定的批注阅读与编辑菜单，移动到菜单后不丢失目标。
/// @param snapshot 当前不可变快照，所有目标检查都以它为准。
/// @param hovered 本帧真实命中的气泡或卡片，仅在右键开始时借用。
/// @param itemIndex 命中的详情卡索引；气泡则沿用最近阅读条目。
/// @param linkHovered Markdown 链接拥有右键时禁止抢占。
/// @return 菜单打开、保持及关闭帧都返回真，防止同一次操作穿透到编辑画布。
/// @pre hovered 非空时必须指向 snapshot.annotationMarkers 内的实际标记。
/// @post 打开及关闭边沿的输入均由本层消费，不交给底层擦除或选择工具。
/// @note 播放和只读期间仍允许阅读，所有写入口在激活前再次验证权限。
/// @note 菜单关闭不等于编辑提交；准备请求后还需要用户在原编辑器确认。
/// @note 单条批注仍提供编辑入口，但导航按钮保持禁用。
/// @details 保存稳定 ID 和谱面实例，不保存指向上一帧快照的指针。
/// 菜单本身不发送谱面修改；编辑和新增仍进入既有确认弹窗。
/// @warning 每帧只校验已锁存分组和组内条目；不读 ECS、磁盘或持有会话锁。
bool Basic2DCanvasInteraction::renderMouseAnnotationMenu(
    const Common::Render::RenderSnapshot&         snapshot,
    const Common::Render::AnnotationRenderMarker* hovered,
    std::size_t itemIndex, bool linkHovered)
{
    ImGui::PushID(m_cameraId.c_str());
    // - 记录上一帧拥有权，ImGui 已因外部点击关闭时仍须吞掉对应释放事件。
    // - 菜单状态属于当前画布，不得用另一个标签页的菜单状态替代。
    const bool wasOpen =
        m_annotationMenu.m_wasOpen || ImGui::IsPopupOpen("MouseAnnotationMenu");
    const bool enabled =
        UI::Interaction::activeProfile().selectionContextMenu();
    bool opened = false;
    // 活动绘制、擦除和平移拥有自己的结束事件，右键不得插入另一笔菜单操作。
    // 只读和播放状态仍允许阅读，编辑按钮另行统一禁用。
    if ( enabled && !wasOpen && hovered && !hovered->items.empty() &&
         !linkHovered && !hasActivePointerGesture() &&
         !UI::Interaction::mouseDown(ImGuiMouseButton_Left) &&
         !UI::Interaction::mouseDown(ImGuiMouseButton_Middle) &&
         !ImGui::IsPopupOpen(
             nullptr,
             ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
         UI::Interaction::mouseClicked(ImGuiMouseButton_Right) ) {
        // 该观察指针来自当前快照的标记数组，只在起笔保存其值语义位置。
        // 首条 ID 锁定分组，选中条目 ID 防止组内删除后索引误指向另一批注。
        // - 菜单索引只用于快速核验，不能单独证明跨快照的对象身份。
        // - 首条与选中条目分别锁定分组和成员，组内位置变化不会误改邻居。
        // - 捕获发生在右键边沿，菜单停留时不反复复制路径与正文。
        m_annotationMenu.m_markerIndex = static_cast<std::size_t>(
            hovered - snapshot.annotationMarkers.data());
        m_annotationMenu.m_markerId = hovered->items.front().id;
        m_annotationMenu.m_selectedId =
            hovered->items[std::min(itemIndex, hovered->items.size() - 1)].id;
        m_annotationMenu.m_beatmapKey = snapshot.beatmapPathKey;
        m_annotationMenu.m_instance   = snapshot.beatmapInstanceId;
        ImGui::OpenPopup("MouseAnnotationMenu");
        opened = true;
    }
    if ( ImGui::BeginPopup("MouseAnnotationMenu") ) {
        // 同路径重载、切换谱面、分组排序变化或删除都不能重定向旧菜单。
        // 失效时关闭而不猜测邻近条目，避免编辑到另一批注。
        const auto  index  = m_annotationMenu.m_markerIndex;
        const auto* marker = index < snapshot.annotationMarkers.size()
                                 ? &snapshot.annotationMarkers[index]
                                 : nullptr;
        const bool  valid =
            enabled && snapshot.hasBeatmap && marker &&
            !marker->items.empty() &&
            marker->items.front().id == m_annotationMenu.m_markerId &&
            snapshot.beatmapPathKey == m_annotationMenu.m_beatmapKey &&
            snapshot.beatmapInstanceId == m_annotationMenu.m_instance;
        if ( !valid )
            ImGui::CloseCurrentPopup();
        else {
            // - 组内插入或删除可能改变索引，阅读身份以选中 ID 为准。
            // - 若选中条目本身消失，不自动选择下一条来继续写入。
            const auto selected = std::find_if(
                marker->items.begin(),
                marker->items.end(),
                [&](const auto& item) {
                    return item.id == m_annotationMenu.m_selectedId;
                });
            if ( selected == marker->items.end() )
                ImGui::CloseCurrentPopup();
            else {
                auto current =
                    static_cast<std::size_t>(selected - marker->items.begin());
                ImGui::Text("%zu / %zu", current + 1, marker->items.size());
                // 导航按钮不自动关闭菜单，用户可以连续阅读同时间戳的多条批注。
                // 首尾循环只发生在明确点击时，不随悬停或快照更新自动前进。
                ImGui::BeginDisabled(marker->items.size() < 2);
                const bool previous = UI::FeedbackSmallButton(
                    TR("ui.annotation.previous").data());
                ImGui::SameLine();
                const bool next =
                    UI::FeedbackSmallButton(TR("ui.annotation.next").data());
                ImGui::EndDisabled();
                // - 显式导航只在当前分组循环，不跨时间戳寻找另一条批注。
                // - 不使用加锁或队列等待更新阅读状态，点击当帧立即可见。
                if ( previous || next ) {
                    current = previous ? (current + marker->items.size() - 1) %
                                             marker->items.size()
                                       : (current + 1) % marker->items.size();
                    m_annotationMenu.m_selectedId = marker->items[current].id;
                    m_annotationHoverDetailIndex  = current;
                }
                // -
                // 导航与正文使用同一索引，不能出现序号已经变化而编辑仍指向旧条目。
                // - 条目按引用读取，常态菜单帧不复制作者或完整正文。
                const auto& item = marker->items[current];
                ImGui::Separator();
                // 菜单内提供可交互的阅读位置，指针无需追逐气泡的悬浮 tooltip。
                // 采用纯文本预览，链接动作仍留给原卡片中的 Markdown 命中处理。
                ImGui::TextUnformatted(
                    item.author.empty()
                        ? TR("ui.annotation.unknown_author").data()
                        : item.author.c_str());
                // 固定高度的预览可滚动，超长批注不能把编辑与新增按钮推出屏幕。
                if ( ImGui::BeginChild("AnnotationMenuPreview",
                                       { 360, 140 },
                                       ImGuiChildFlags_Borders) ) {
                    // 切换条目复用同一个子窗口
                    // ID，必须清除上一条长正文的滚动位置。
                    if ( previous || next ) ImGui::SetScrollY(0);
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextWrapped("%s", item.content.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndChild();
                ImGui::Separator();
                // 菜单停留期间每帧复核写入权限，不能只信打开时的播放或只读状态。
                // 这里只准备已有编辑器缓冲，保存、删除与撤销仍走原命令路由。
                // - 打开后进入播放或只读状态也须立即禁止写入，不信任旧的许可。
                // - 非有限时间不能进入编辑器，避免向谱面发布不可保存的位置。
                ImGui::BeginDisabled(snapshot.isPlaying ||
                                     !snapshot.acceptsInteraction ||
                                     !std::isfinite(marker->timestamp));
                const bool edit =
                    UI::FeedbackButton(TR("ui.annotation.edit").data());
                const bool add =
                    UI::FeedbackButton(TR("ui.annotation.add_here").data());
                ImGui::EndDisabled();
                // - ID 决定编辑与新增，历史作者为空不能令编辑误变为新建。
                // - 作者与正文来自同一快照同一 ID，不分别读取实时 hover。
                // - 时间来自对应标记，不受指针移动到菜单后的播放头位置影响。
                if ( edit || add ) {
                    m_annotationEditor.annotationId =
                        edit ? item.id : std::string{};
                    m_annotationEditor.author =
                        edit ? item.author : std::string{};
                    m_annotationEditor.timestamp = marker->timestamp;
                    m_annotationEditor.content.fill('\0');
                    // 固定缓冲区始终保留终结符，历史超长文本不能写出边界。
                    if ( edit )
                        std::copy_n(
                            item.content.data(),
                            std::min(item.content.size(),
                                     m_annotationEditor.content.size() - 1),
                            m_annotationEditor.content.data());
                    m_annotationEditor.requestOpen = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }
    // 关闭帧仍属于菜单，下一次独立按下才重新交给主画布。
    // 不清空已准备的编辑缓冲，否则弹窗延迟打开请求会丢失。
    m_annotationMenu.m_wasOpen = ImGui::IsPopupOpen("MouseAnnotationMenu");
    ImGui::PopID();
    return wasOpen || opened || m_annotationMenu.m_wasOpen;
}
}  // namespace MMM::Canvas
