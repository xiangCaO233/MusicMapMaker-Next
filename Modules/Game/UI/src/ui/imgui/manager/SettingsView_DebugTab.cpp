#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "network/collaboration/RtcDiagnosticLogging.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdint>
#include <string>

/// @file SettingsView_DebugTab.cpp
/// @brief 调试设置页的折叠分组、渲染诊断和 RTC 日志选项实现。
/// @details 页面复用 SettingsView 的 Clay 容器池，每帧从配置快照构建控件；
/// 只有值实际变化时才发布逻辑配置命令并持久化软件设置。

namespace MMM::UI
{

/// @brief 渲染调试设置页。
/// @details 渲染与网络选项分为两个可折叠段落，展开状态存放在当前 ImGui
/// 窗口 StateStorage 中，不写入项目或软件配置。
/// @note 摄像机变化逐帧发布 CmdUpdateEditorConfig，磁盘保存延迟到控件释放后。
/// @note RTC 日志开关除写入配置外还立即更新网络层运行时门闩。
/// @note 摄像机参数仅用于只读实验观察，本页不改变画布世界几何或背景。
/// @note 任一参数偏离默认值时进入观察模式，右键拖动调整角度、滚轮调整距离。
/// @note 固定一个世界单位的高度是渲染端约束，不另设可编辑项。
/// @warning UI 热路径：设置窗口打开且当前页为调试页时每帧执行。
/// 禁止加入文件系统扫描或重型资源重建。
void SettingsView::drawDebugSettings()
{
    // 页面直接编辑 AppConfig 中的 UI 线程配置值。
    auto& appConfig = Config::AppConfig::instance();
    // 通用编辑器选项保存渲染日志与 RTC 诊断开关。
    auto& settings = appConfig.getEditorSettings();
    /// 视觉配置保存画布摄像机参数、命中框显示和缩放系数。
    auto& visual = appConfig.getVisualConfig();
    /// 标准设置项使用本帧变化标记；摄像机另行延迟持久化。
    /// 标题展开状态只属于临时窗口状态，不触发配置更新。
    bool changed = false;
    /// 摄像机拖动逐帧同步运行配置，但只在交互结束后持久化一次。
    bool cameraChanged = false;

    // 内容容器来自成员缓存，每帧先移除上一帧节点。
    m_contentVBox.clear();
    // 根容器统一设置段落间距和页面内边距。
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    // 行与段落索引共同驱动对象池复用和稳定布局 ID。
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 当前标签页所有标准设置行共用最大标签宽度。
    const float maxLabelW =
        getCurrentTabLabelWidth(appConfig.getWindowContentScale());

    // 折叠标题辅助函数返回展开后的内容容器，关闭时返回 nullptr。
    // 捕获行与段落索引引用，使每个标题及内容按出现顺序消费对象池槽位。
    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        // ID 包含段落、行和标签，避免同页多个标题碰撞。
        std::string baseIdStr = "DBG_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        // ImGuiID 用于跨帧保存折叠状态，不依赖 Clay 节点地址。
        // 可见标签参与哈希，因此同一页不同本地化标题仍保持相互独立。
        // 折叠状态不包含可变行号；搜索展开前面的组时不能重置后面的标题。
        ImGuiID id = ImGui::GetID((std::string("DBG_Header_") + label).c_str());

        // 首次出现使用 defaultOpen，之后读取窗口 StateStorage 中的选择。
        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;
        // 搜索只展开对应组，不能触发配置修改或保存。
        const bool forceOpen = revealSettingsSearchSection(label, id);
        if ( forceOpen ) isOpen = true;

        // 标题占用对象池中的标准横向行。
        auto& row = getRow(rowIndex++);
        // 标题自身绘制完整背景，不需要额外行内边距。
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        // 高度与当前字体和 ImGui FramePadding 保持一致。
        float h = ImGui::GetFrameHeight();

        // Clay 负责标题区域几何，回调内部使用 ImGui CollapsingHeader。
        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [this, label, id, defaultOpen, forceOpen](Clay_BoundingBox r,
                                                      bool) {
                // 将 ImGui 游标移动到 Clay 计算出的标题左上角。
                // 记录真实标题边界，条件隐藏的设置可以定位到所属组。
                reportSettingsSearchHeader(label, r);
                ImGui::SetCursorScreenPos({ r.x, r.y });
                // 以当前 Header 色为基准构造悬浮与按下变体。
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                // 普通状态保留主题原始 Header 色。
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                // 悬浮状态轻微提高 RGB 和透明度以保持主题一致。
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                // 按下状态进一步提亮，提供连续交互反馈。
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                // 临时收窄 WorkRect，使标题背景严格覆盖 Clay 分配宽度。
                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                // CollapsingHeader 自身不应继承窗口默认内边距。
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                // 指针形式 ID 仅由整数哈希转换，不解引用地址。
                // Clay 和实际标题必须保持相同的展开状态。
                if ( forceOpen ) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                // 恢复临时样式和窗口工作区，避免影响后续控件。
                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                // 将本帧交互结果写回与读取相同的窗口状态存储。
                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                // 三项 Header 颜色按压栈顺序一次性恢复。
                ImGui::PopStyleColor(3);
            });

        // 标题行宽度撑满页面，高度固定为标准控件帧高。
        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            // 仅展开状态才取得内容段落，避免关闭分组保留空白高度。
            auto& sec = getSection(sectionIndex++);
            // 内容段落使用统一装饰、控件间距和八像素内边距。
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            // 分组宽度随页面增长，高度由其中设置行决定。
            // ID 后缀区分标题节点与内容节点，避免 Clay 查询冲突。
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        // 关闭标题没有可接收设置项的内容容器。
        return nullptr;
    };

    // 渲染诊断分组默认展开，方便开发构建快速访问命中框工具。
    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.debug.rendering").data(), true) ) {
        /// 俯仰角限定在配置公开范围内，零度对应默认二维视图。
        /// 三个摄像机滑杆均启用 AlwaysClamp，手动文本输入也受边界约束。
        /// 这样持久化值与观察相机收到的范围保持一致。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.canvas_camera_angle").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                /// 角度滑杆占满控件列，度数后缀明确表达俯仰角。
                ImGui::SetNextItemWidth(r.width);
                /// 俯仰边界由 VisualConfig 统一维护，UI
                /// 与配置反序列化共享同一范围。
                /// 控件只写入观察相机参数，不创建或修改谱面物件。
                cameraChanged |= ::MMM::UI::FeedbackSliderFloat(
                    "##DebugCanvasCameraAngle",
                    &visual.debugCanvasCameraAngleDegrees,
                    Config::VisualConfig::MIN_DEBUG_CANVAS_CAMERA_ANGLE_DEGREES,
                    Config::VisualConfig::MAX_DEBUG_CANVAS_CAMERA_ANGLE_DEGREES,
                    "%.1f°",
                    ImGuiSliderFlags_AlwaysClamp);
                if ( ImGui::IsItemHovered() ) {
                    // 实验边界只在用户检查控件时显示，避免挤占设置行。
                    Utils::renderTooltip(
                        TR("ui.settings.debug.canvas_camera_angle_tooltip")
                            .data(),
                        Utils::TooltipDir::Right);
                }
            });

        /// 水平旋转允许绕画布完整旋转一周，度数格式与俯仰角一致。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.canvas_camera_yaw").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                /// 水平角范围包含两端点，可完整转向且边界稳定。
                cameraChanged |= ::MMM::UI::FeedbackSliderFloat(
                    "##DebugCanvasCameraYaw",
                    &visual.debugCanvasCameraYawDegrees,
                    Config::VisualConfig::MIN_DEBUG_CANVAS_CAMERA_YAW_DEGREES,
                    Config::VisualConfig::MAX_DEBUG_CANVAS_CAMERA_YAW_DEGREES,
                    "%.1f°",
                    ImGuiSliderFlags_AlwaysClamp);
            });

        /// 距离倍率只影响观察相机，不改动画、谱面或画布背景。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.canvas_camera_distance").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                /// 距离下限保持正值，避免观察相机越过画布平面；上限限制观察尺度。
                /// 该倍率只交给画布观察相机使用，不作为谱面缩放或内容变换保存。
                cameraChanged |= ::MMM::UI::FeedbackSliderFloat(
                    "##DebugCanvasCameraDistance",
                    &visual.debugCanvasCameraDistance,
                    Config::VisualConfig::MIN_DEBUG_CANVAS_CAMERA_DISTANCE,
                    Config::VisualConfig::MAX_DEBUG_CANVAS_CAMERA_DISTANCE,
                    "%.2f",
                    ImGuiSliderFlags_AlwaysClamp);
            });

        /// 复位一次恢复三个摄像机参数，便于立即返回默认二维编辑状态。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.canvas_camera_actions").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                if ( ::MMM::UI::FeedbackButton(
                         TR("ui.settings.debug.canvas_camera_reset").data()) ) {
                    /// 先比较三个值再赋值，使重复点击默认状态保持幂等且不触发写盘。
                    /// 默认值来自配置类型，避免 UI 维护与序列化默认值分叉。
                    /// 复位只触碰 visual 摄像机字段，不覆盖其它编辑器选项。
                    const float defaultAngle = Config::VisualConfig::
                        DEFAULT_DEBUG_CANVAS_CAMERA_ANGLE_DEGREES;
                    const float defaultYaw = Config::VisualConfig::
                        DEFAULT_DEBUG_CANVAS_CAMERA_YAW_DEGREES;
                    const float defaultDistance = Config::VisualConfig::
                        DEFAULT_DEBUG_CANVAS_CAMERA_DISTANCE;
                    cameraChanged =
                        visual.debugCanvasCameraAngleDegrees != defaultAngle ||
                        visual.debugCanvasCameraYawDegrees != defaultYaw ||
                        visual.debugCanvasCameraDistance != defaultDistance;
                    visual.debugCanvasCameraAngleDegrees = defaultAngle;
                    visual.debugCanvasCameraYawDegrees   = defaultYaw;
                    visual.debugCanvasCameraDistance     = defaultDistance;
                }
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.debug.canvas_camera_reset_tooltip")
                            .data(),
                        Utils::TooltipDir::Right);
                }
            });

        // 其余诊断项用于检查二维拾取几何与渲染阶段。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.draw_hitboxes").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // Checkbox 按标准帧高在 Clay 行内垂直居中。
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                // 命中框开关只改变可视化，不触发几何重建。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##DebugDrawHitboxes", &visual.debugDrawHitboxes);
            });

        // 第二项调整命中框横向缩放，用于定位过宽或过窄的拾取区域。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.hitbox_scale_x").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 滑杆占满右侧控件列宽度。
                ImGui::SetNextItemWidth(r.width);
                // 横向命中框倍率限制在 VisualConfig 公开安全范围内。
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##InteractionHitboxScaleX",
                    &visual.interactionHitboxScaleX,
                    Config::VisualConfig::MIN_INTERACTION_HITBOX_SCALE,
                    Config::VisualConfig::MAX_INTERACTION_HITBOX_SCALE,
                    "%.2f");
            });

        // 第三项独立调整纵向缩放，便于验证轨道高度方向的命中范围。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.hitbox_scale_y").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 纵向倍率与横向倍率使用相同的布局和格式精度。
                ImGui::SetNextItemWidth(r.width);
                // 配置边界防止零尺寸或过大命中框破坏交互。
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##InteractionHitboxScaleY",
                    &visual.interactionHitboxScaleY,
                    Config::VisualConfig::MIN_INTERACTION_HITBOX_SCALE,
                    Config::VisualConfig::MAX_INTERACTION_HITBOX_SCALE,
                    "%.2f");
            });

        // 最后一项启用渲染 profile 日志，不改变实际画布视觉状态。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.render_profile_logging").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 日志开关按标准 Checkbox 高度居中。
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                // renderProfileLogging 只控制低频统计日志输出。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##RenderProfileLogging", &settings.renderProfileLogging);
            });
    }

    // 网络诊断分组与渲染诊断独立保存展开状态。
    // 默认展开参数只在 StateStorage 尚无记录时生效。
    if ( auto* sec = addHeader(TR_CACHE("ui.settings.debug.networking").data(),
                               true) ) {
        // 网络分组当前仅包含 RTC 诊断日志门闩。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.rtc_diagnostic_logging").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // RTC 开关按标准帧高在设置行中居中。
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                // 单独保留本控件变化结果，以触发即时日志后端更新。
                const bool rtcLoggingChanged = ::MMM::UI::FeedbackCheckbox(
                    "##RtcDiagnosticLogging", &settings.rtcDiagnosticLogging);
                changed |= rtcLoggingChanged;
                if ( rtcLoggingChanged ) {
                    // 网络诊断开关立即同步到 RTC 全局日志门闩。
                    Network::Collaboration::setRtcDiagnosticLoggingEnabled(
                        settings.rtcDiagnosticLogging);
                }
            });
    }

    // Clay 布局从当前 ImGui 内容起点开始，宽度使用页面剩余区域。
    // 高度传零让容器按展开段落和实际控件数量适配。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    // 将 ImGui 游标推进到 Clay 实际高度之后，维持滚动内容范围。
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    /// StateStorage 跨帧保存脏状态，避免连续拖动时同步执行磁盘写入。
    const ImGuiID cameraDirtyId = ImGui::GetID("DebugCanvasCameraDirty");
    /// 固定 ID 使同一设置窗口每帧访问同一状态槽位。
    /// dirty 属于设置窗口而非单个控件，因此切换角度、yaw、距离滑杆时不会丢失。
    /// 复位操作也走同一脏标记，使一次多参数变更与滑杆调整使用相同提交路径。
    bool cameraDirty = ImGui::GetStateStorage()->GetInt(cameraDirtyId, 0) != 0;
    if ( cameraChanged ) cameraDirty = true;
    /// 任一 ImGui 项仍处于交互态时保留脏标记；松开后在本帧提交最终内存值。
    /// 使用全局活跃项判断可覆盖鼠标拖动、键盘调节及控件之间的连续切换。
    const bool saveCamera = cameraDirty && !ImGui::IsAnyItemActive();
    /// 提交后立即清除标记，避免重新进入调试页时重复落盘。
    ImGui::GetStateStorage()->SetInt(cameraDirtyId,
                                     cameraDirty && !saveCamera ? 1 : 0);

    /// 切换出调试页时此函数暂停，脏标记保留到重新进入后继续检查。
    /// 关闭设置窗口期间不访问状态槽，应用退出保存仍读取当前内存配置。
    if ( changed || cameraChanged || saveCamera ) {
        /// 折叠标题变化不设置标记，因此不会造成无意义配置保存。
        /// 逻辑命令只发布配置快照；谱面事件与谱面文件内容不在此处传递。
        // 逻辑线程接收完整 EditorConfig 快照，避免跨线程读取 AppConfig。
        if ( changed || cameraChanged ) {
            /// 只在配置值变化时发布快照，等待磁盘提交时不重复发送逻辑命令。
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateEditorConfig{ appConfig.getEditorConfig() }));
        }
        // UI 线程随后持久化本帧所有变化，合并多个控件写盘。
        /// 摄像机拖动只在所有控件释放后写盘；普通设置仍沿用即时保存。
        /// AppConfig::save 同步写文件，拖动期间只更新内存和逻辑快照。
        /// 若用户切换设置分类，脏值仍保存在窗口状态中，回到调试页后完成低频保存。
        /// 关闭设置窗口时最终配置仍由应用既有退出保存路径覆盖。
        /// 无待保存变更时重复复位不会置脏，避免无变化的文件写入。
        if ( changed || saveCamera ) appConfig.save();
    }
}

}  // namespace MMM::UI
