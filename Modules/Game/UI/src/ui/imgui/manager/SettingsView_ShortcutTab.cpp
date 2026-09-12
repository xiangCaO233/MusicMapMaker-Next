#include "ui/imgui/manager/SettingsView.h"

#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <string>

namespace MMM::UI
{

/// @brief 绘制单个快捷键录制控件。
/// @param binding 正在编辑的快捷键绑定。
/// @param target 当前控件对应的录制目标。
/// @param id ImGui 控件 ID 后缀。
/// @param width 控件可用宽度。
/// @param conflicted 当前绑定是否与其他快捷键冲突。
/// @param changed 发生修改时写入 true。
///
/// 控件由当前绑定展示按钮、录制按钮和清除按钮组成。展示或录制按钮都会进入
/// 全局快捷键录制状态；Escape 取消但不修改原绑定，捕获到合法组合后才替换。
/// 冲突只改变展示颜色和设置行装饰，不阻止用户保留该绑定。
///
/// 录制状态约束：
/// - m_recordingShortcutTarget 全页同时只能指向一个动作；
/// - 目标行每帧设置 ShortcutUtils 全局录制标志；
/// - Escape 使用非重复按键检测并仅取消当前目标；
/// - 捕获成功后同步更新绑定、changed、目标和全局标志；
/// - 点击另一行会直接把录制目标切换到新动作；
/// - 清除当前录制行会同步结束录制；
/// - 未捕获到完整组合时保留原绑定不变。
///
/// 控件 ID 约束：
/// - 外层 PushID 使用动作稳定英文 ID；
/// - 展示按钮文本允许随录制状态和语言改变；
/// - ### 后缀维持展示、录制、清除三个内部身份；
/// - 三个按钮均经 FeedbackButton 绘制；
/// - 冲突样式只覆盖展示按钮并立即恢复。
/// @warning UI 热路径：设置窗口打开且当前页为快捷键页时每帧执行；
/// 只查询当前帧键盘状态并更新配置。
void SettingsView::drawShortcutBindingControl(Config::ShortcutBinding& binding,
                                              ShortcutRecordTarget     target,
                                              const char* id, float width,
                                              bool conflicted, bool& changed)
{
    // 每行建立独立 ID 作用域，允许三个按钮复用相同可见翻译文本。
    ImGui::PushID(id);

    const bool isRecording = m_recordingShortcutTarget == target;
    if ( isRecording ) {
        // 全局录制标志阻止快捷键路由把当前按键当作编辑命令执行。
        ShortcutUtils::setShortcutRecordingActive(true);
    }
    if ( isRecording && ImGui::IsKeyPressed(ImGuiKey_Escape, false) ) {
        // Escape 仅退出录制，不清空或覆盖用户原有绑定。
        m_recordingShortcutTarget = ShortcutRecordTarget::None;
        ShortcutUtils::setShortcutRecordingActive(false);
    } else if ( isRecording ) {
        // capturePressedShortcut 忽略纯修饰键，只有完整组合才返回值。
        auto captured = ShortcutUtils::capturePressedShortcut();
        if ( captured ) {
            binding = *captured;
            // 捕获成功形成配置变更，并立即结束全局录制隔离。
            changed                   = true;
            m_recordingShortcutTarget = ShortcutRecordTarget::None;
            ShortcutUtils::setShortcutRecordingActive(false);
        }
    }

    // 按钮宽度由翻译文本和当前主题 FramePadding 计算。
    const char* recordLabel = TR_CACHE("ui.settings.shortcut.record").data();
    const char* clearLabel  = TR_CACHE("ui.settings.shortcut.clear").data();
    const float spacing     = ImGui::GetStyle().ItemSpacing.x;
    const float recordW     = ImGui::CalcTextSize(recordLabel).x +
                              ImGui::GetStyle().FramePadding.x * 2.0f;
    const float clearW      = ImGui::CalcTextSize(clearLabel).x +
                              ImGui::GetStyle().FramePadding.x * 2.0f;
    const float displayW =
        // 展示区至少保留 80 像素，窄窗口也能看见当前绑定。
        std::max(80.0f, width - recordW - clearW - spacing * 2.0f);

    std::string displayText;
    if ( m_recordingShortcutTarget == target ) {
        // 当前行录制时显示状态文案，其他行继续显示各自绑定。
        displayText = TR_CACHE("ui.settings.shortcut.recording").data();
    } else {
        displayText = ShortcutUtils::formatShortcut(binding);
        if ( displayText.empty() ) {
            // 禁用或无键绑定使用本地化“无”，避免空按钮失去可点击面积。
            displayText = TR_CACHE("ui.settings.shortcut.none").data();
        }
    }

    // ### 后缀把可见文案与稳定控件 ID 分离，录制状态变化不会丢失交互。
    std::string displayButtonId = displayText + "###ShortcutDisplay_" + id;
    std::string recordButtonId =
        std::string(recordLabel) + "###ShortcutRecord_" + id;
    std::string clearButtonId =
        std::string(clearLabel) + "###ShortcutClear_" + id;

    if ( conflicted ) {
        // 危险色只覆盖当前绑定展示按钮，录制和清除仍使用普通样式。
        ImGui::PushStyleColor(ImGuiCol_Text,
                              Utils::UIThemeUtils::getDangerColor());
    }
    const bool displayClicked = ::MMM::UI::FeedbackButton(
        displayButtonId.c_str(), ImVec2(displayW, 0.0f));
    if ( conflicted ) {
        ImGui::PopStyleColor();
    }
    if ( displayClicked ) {
        // 点击绑定展示与点击“录制”具有相同进入录制语义。
        m_recordingShortcutTarget = target;
        ShortcutUtils::setShortcutRecordingActive(true);
    }
    ImGui::SameLine();
    if ( ::MMM::UI::FeedbackButton(recordButtonId.c_str(),
                                   ImVec2(recordW, 0.0f)) ) {
        m_recordingShortcutTarget = target;
        // 显式设置全局标志，让本帧剩余快捷键路由也立即停用。
        ShortcutUtils::setShortcutRecordingActive(true);
    }
    ImGui::SameLine();
    if ( ::MMM::UI::FeedbackButton(clearButtonId.c_str(),
                                   ImVec2(clearW, 0.0f)) ) {
        binding.enabled = false;
        // 同时清除规范键名，持久化配置不会残留不可见旧值。
        binding.key.clear();
        changed = true;
        if ( m_recordingShortcutTarget == target ) {
            // 清除正在录制的同一行时同步退出录制隔离。
            m_recordingShortcutTarget = ShortcutRecordTarget::None;
            ShortcutUtils::setShortcutRecordingActive(false);
        }
    }

    // 恢复行级 ID，避免影响后续设置项和 Clay 渲染元素。
    ImGui::PopID();
}

/// @brief 绘制快捷键设置页。
///
/// 页面枚举二十个可配置动作，在同一 Clay section 中按工具、编辑和切换动作
/// 排列。冲突检测基于当前帧完整绑定数组，因此任一修改在下一帧即可更新两端
/// 冲突提示。发生修改后发布编辑器配置命令并持久化 AppConfig。
///
/// 录制期间 ShortcutUtils 全局标志保持为 true，防止新组合触发正常编辑动作；
/// 没有录制目标时每帧防御性清除该标志。
///
/// 冲突检测约束：
/// - shortcutBindings 必须包含页面展示的每一个绑定；
/// - 比较跳过同一对象地址，避免绑定与自身冲突；
/// - Config::shortcutBindingsConflict 统一处理禁用、键名和修饰键；
/// - 同一组合的多个动作都会分别显示冲突；
/// - 页面不自动清除或改写冲突绑定；
/// - 冲突状态在下一帧布局重建时刷新。
///
/// 提交约束：
/// - 所有行共享一个 changed 标志；
/// - 捕获和清除是仅有的配置修改入口；
/// - 一帧多个修改合并成一个 CmdUpdateEditorConfig；
/// - 命令携带完整配置值，不持有 AppConfig 引用；
/// - AppConfig::save 只在 changed 为真时调用；
/// - 纯录制开始或取消不触发持久化。
///
/// 布局约束：
/// - 全部绑定位于同一个带装饰 Clay section；
/// - 标签列宽取当前页所有标签的最大测量值；
/// - 每行值列由展示、录制和清除三个按钮构成；
/// - 按钮宽度随翻译文本和主题内边距变化；
/// - 展示区在扣除操作按钮后至少保留 80 像素；
/// - Clay 返回实际内容高度后必须推进 ImGui 游标；
/// - 冲突行通过 addSettingItem 的 conflicted 参数附加视觉提示。
///
/// 生命周期约束：
/// - bindingPtr 只指向 AppConfig 内稳定成员；
/// - idValue 按值捕获，覆盖原始 const char 指针生命周期；
/// - changed 按引用捕获且所有 Clay Lambda 在函数返回前执行；
/// - 布局树不保存到下一帧，每帧重新建立捕获关系；
/// - 录制目标为枚举值，不持有具体控件或键盘事件对象。
/// - 翻译缓存文本只在当前渲染上下文中用于控件标签；
/// - 配置命令发布后不等待逻辑线程确认。
/// @warning UI 热路径：设置窗口打开且当前页为快捷键页时每帧执行；
/// 只查询当前帧键盘状态并更新配置。
void SettingsView::drawShortcutSettings()
{
    // 直接编辑 AppConfig 中的快捷键草稿，changed 控制本帧末尾统一提交。
    auto& appConfig = Config::AppConfig::instance();
    auto& settings  = appConfig.getEditorSettings();
    bool  changed   = false;

    if ( m_recordingShortcutTarget == ShortcutRecordTarget::None ) {
        // 页面重新打开或录制已结束时，确保全局路由不残留录制态。
        ShortcutUtils::setShortcutRecordingActive(false);
    }

    // Clay 描述树每帧重建，缓存行对象由 SettingsView 按索引复用。
    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    const float maxLabelW =
        // 使用当前标签页预计算最大标签宽度，使二十行控件起点对齐。
        getCurrentTabLabelWidth(appConfig.getWindowContentScale());

    // 所有快捷键共享一个装饰 section，便于整体滚动和冲突边框展示。
    auto& shortcutSection = getSection(sectionIndex++);
    shortcutSection.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
    m_contentVBox.addLayout(
        "ShortcutSection", shortcutSection, Sizing::Grow(), Sizing::Fit());

    // 指针数组既定义冲突比较全集，也避免复制每个 ShortcutBinding。
    auto& shortcutConfig = settings.shortcutConfig;
    const std::array<const Config::ShortcutBinding*, 20> shortcutBindings{
        // 工具选择组。
        &shortcutConfig.toolMove,
        &shortcutConfig.toolMarquee,
        &shortcutConfig.toolDraw,
        &shortcutConfig.toolColorBrush,
        &shortcutConfig.toolColorEraser,
        // 编辑与播放操作组。
        &shortcutConfig.mirror,
        &shortcutConfig.mirrorPaste,
        &shortcutConfig.editSelectedVolume,
        &shortcutConfig.addSelectedAnnotation,
        &shortcutConfig.deleteSelected,
        &shortcutConfig.togglePlayback,
        // 编辑器状态切换组。
        &shortcutConfig.toggleReverseScroll,
        &shortcutConfig.toggleScrollSnap,
        &shortcutConfig.toggleSnapFloor,
        &shortcutConfig.toggleScrollTimingMapping,
        &shortcutConfig.toggleBeatLines,
        &shortcutConfig.toggleStopPlaybackOnScroll,
        &shortcutConfig.toggleHitSfx,
        &shortcutConfig.toggleHitEffects,
        &shortcutConfig.toggleSyncSameMainAudio,
    };
    auto hasConflict =
        [&shortcutBindings](const Config::ShortcutBinding& binding) {
            // 与自身指针比较时跳过；任意其他绑定冲突即返回 true。
            return std::any_of(shortcutBindings.begin(),
                               shortcutBindings.end(),
                               [&binding](const auto* candidate) {
                                   return candidate != &binding &&
                                          Config::shortcutBindingsConflict(
                                              binding, *candidate);
                               });
        };

    /// 将一个绑定包装为标准设置行并捕获本帧冲突状态。
    /// bindingPtr 指向 AppConfig 稳定成员，Lambda 在本帧 Clay 渲染时调用。
    auto addShortcutRow = [&](CLayVBox&                sec,
                              const char*              label,
                              Config::ShortcutBinding& binding,
                              ShortcutRecordTarget     target,
                              const char*              id) {
        Config::ShortcutBinding* bindingPtr = &binding;
        // 值捕获 target 和 ID，避免 Lambda 依赖调用栈临时参数。
        ShortcutRecordTarget targetValue = target;
        std::string          idValue     = id ? id : "";
        const bool           conflicted  = hasConflict(binding);
        // 冲突值在构建本帧布局时冻结；编辑后下一帧重新计算。
        addSettingItem(
            sec,
            rowIndex,
            label,
            maxLabelW,
            [this, bindingPtr, targetValue, idValue, conflicted, &changed](
                Clay_BoundingBox r, bool) {
                // Clay 提供值列宽度，实际三个按钮由录制控件内部排版。
                drawShortcutBindingControl(*bindingPtr,
                                           targetValue,
                                           idValue.c_str(),
                                           r.width,
                                           conflicted,
                                           changed);
            },
            conflicted);
    };

    // 工具选择快捷键组：移动、框选、绘制、颜色刷与颜色擦除。
    // 五个工具动作按工具栏视觉顺序排列，便于用户逐项对照修改。
    // 每个英文 ID 与 ShortcutRecordTarget 和 ShortcutConfig 成员一一对应。
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_move").data(),
                   shortcutConfig.toolMove,
                   ShortcutRecordTarget::ToolMove,
                   "ToolMove");
    // 框选和移动虽然都用于选择交互，仍保留独立绑定。
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_marquee").data(),
                   shortcutConfig.toolMarquee,
                   ShortcutRecordTarget::ToolMarquee,
                   "ToolMarquee");
    // 绘制工具负责放置物件，不与颜色刷的属性修改语义合并。
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_draw").data(),
                   shortcutConfig.toolDraw,
                   ShortcutRecordTarget::ToolDraw,
                   "ToolDraw");
    // 颜色刷与颜色擦除分别占用稳定目标，支持不同快捷键组合。
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_color_brush").data(),
                   shortcutConfig.toolColorBrush,
                   ShortcutRecordTarget::ToolColorBrush,
                   "ToolColorBrush");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_color_eraser").data(),
                   shortcutConfig.toolColorEraser,
                   ShortcutRecordTarget::ToolColorEraser,
                   "ToolColorEraser");

    // 编辑操作组：镜像、镜像粘贴、批量音量、批注、删除与播放。
    // 此分组只用于页面阅读顺序，冲突检测仍覆盖全部二十个绑定。
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.mirror").data(),
                   shortcutConfig.mirror,
                   ShortcutRecordTarget::Mirror,
                   "Mirror");
    // 镜像粘贴与普通镜像是不同命令，不能共享录制目标。
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.mirror_paste").data(),
                   shortcutConfig.mirrorPaste,
                   ShortcutRecordTarget::MirrorPaste,
                   "MirrorPaste");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.edit_selected_volume").data(),
                   shortcutConfig.editSelectedVolume,
                   ShortcutRecordTarget::EditSelectedVolume,
                   "EditSelectedVolume");
    // 批注动作名称较长，统一标签列宽保证其录制控件仍与其他行对齐。
    addShortcutRow(
        shortcutSection,
        TR_CACHE("ui.settings.shortcut.add_selected_annotation").data(),
        shortcutConfig.addSelectedAnnotation,
        ShortcutRecordTarget::AddSelectedAnnotation,
        "AddSelectedAnnotation");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.delete_selected").data(),
                   shortcutConfig.deleteSelected,
                   ShortcutRecordTarget::DeleteSelected,
                   "DeleteSelected");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_playback").data(),
                   shortcutConfig.togglePlayback,
                   ShortcutRecordTarget::TogglePlayback,
                   "TogglePlayback");

    // 编辑器状态切换组：滚动方向、吸附、节拍线、音效和同步策略。
    // 这些动作改变持续设置而非选择工具，但使用相同录制与冲突规则。
    addShortcutRow(
        shortcutSection,
        TR_CACHE("ui.settings.shortcut.toggle_reverse_scroll").data(),
        shortcutConfig.toggleReverseScroll,
        ShortcutRecordTarget::ToggleReverseScroll,
        "ToggleReverseScroll");
    // 滚动吸附和吸附下取整是两个可独立启用的状态。
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_scroll_snap").data(),
                   shortcutConfig.toggleScrollSnap,
                   ShortcutRecordTarget::ToggleScrollSnap,
                   "ToggleScrollSnap");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_snap_floor").data(),
                   shortcutConfig.toggleSnapFloor,
                   ShortcutRecordTarget::ToggleSnapFloor,
                   "ToggleSnapFloor");
    addShortcutRow(
        shortcutSection,
        TR_CACHE("ui.settings.shortcut.toggle_scroll_timing_mapping").data(),
        shortcutConfig.toggleScrollTimingMapping,
        ShortcutRecordTarget::ToggleScrollTimingMapping,
        "ToggleScrollTimingMapping");
    // 节拍线切换只触发显示模式入口，模式历史由专用状态对象维护。
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_beat_lines").data(),
                   shortcutConfig.toggleBeatLines,
                   ShortcutRecordTarget::ToggleBeatLines,
                   "ToggleBeatLines");
    addShortcutRow(
        shortcutSection,
        TR_CACHE("ui.settings.shortcut.toggle_stop_playback_on_scroll").data(),
        shortcutConfig.toggleStopPlaybackOnScroll,
        ShortcutRecordTarget::ToggleStopPlaybackOnScroll,
        "ToggleStopPlaybackOnScroll");
    // 打击音效和视觉效果分别配置，避免强制绑定成同一个组合。
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_hit_sfx").data(),
                   shortcutConfig.toggleHitSfx,
                   ShortcutRecordTarget::ToggleHitSfx,
                   "ToggleHitSfx");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_hit_effects").data(),
                   shortcutConfig.toggleHitEffects,
                   ShortcutRecordTarget::ToggleHitEffects,
                   "ToggleHitEffects");
    // 同主音频同步策略位于末行，但仍包含在冲突全集中。
    addShortcutRow(
        shortcutSection,
        TR_CACHE("ui.settings.shortcut.toggle_sync_same_main_audio").data(),
        shortcutConfig.toggleSyncSameMainAudio,
        ShortcutRecordTarget::ToggleSyncSameMainAudio,
        "ToggleSyncSameMainAudio");

    // 渲染完整 Clay 树后推进 ImGui 游标，保持父设置窗口滚动范围正确。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( changed ) {
        // 逻辑线程先接收完整编辑器配置，磁盘保存复用同一 AppConfig 快照。
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdUpdateEditorConfig{ appConfig.getEditorConfig() }));
        // 保存失败由配置层记录；本函数不回滚已经呈现的快捷键草稿。
        appConfig.save();
    }
}

}  // namespace MMM::UI
