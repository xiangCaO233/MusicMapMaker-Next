#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>

namespace MMM::UI
{

/// @brief 渲染编辑器设置页。
///
/// 本页编辑 `EditorConfig::settings` 中的交互规则，并同时提供少量属于 visual 和
/// 运行态 EditorEngine 的选项。设置分为编辑行为、框选和音效三组，每组通过
/// Clay 分配布局矩形，再由 ImGui 控件完成交互。
///
/// 持久化配置遵循统一提交路径：控件修改 AppConfig 内存对象并设置 `changed`，
/// 函数末尾发布 `CmdUpdateEditorConfig`
/// 后保存。仅属于运行态的“同主音频画布同步” 直接调用
/// EditorEngine，不设置持久化标志。
///
/// 音效同步速度是一个需要即时副作用的例外。字段变化时既设置 `changed`，又通知
/// AudioManager 切换 SFX 路由；最终配置仍通过正常保存路径持久化。
///
/// 页面不执行复制粘贴、吸附、选择、滚动或音效播放。这些消费者读取更新后的
/// EditorConfig，并在各自线程或交互阶段应用策略。
///
/// Clay 回调在当前帧根布局渲染时执行。配置引用、局部数组和状态快照不得逃逸到
/// 后续帧；如果布局执行模型改变，应改用稳定 ID 在回调执行时重新查询。
///
/// 数值控件范围是当前交互契约，旧配置的迁移和校验属于 AppConfig 加载流程。
/// 页面只对允许自由拖动的重叠时间窗口做非负钳制。
///
/// 设置生效时机分为三类：
/// - 普通字段在本帧末尾随完整 EditorConfig 发布；
/// - 画布同步开关立即调用 EditorEngine，且不进入 AppConfig；
/// - SFX 变速同步既写配置，也立即刷新 AudioManager 路由。
///
/// 这种差异来自状态所有权，不应为了表面统一而把运行态字段塞进持久化配置，也
/// 不应让普通配置控件直接寻找并逐个更新所有消费者。
///
/// 所有 `Feedback*` 控件使用 `##` 后的稳定内部 ID。翻译字符串仅作为可见标签，
/// 切换语言不应重置活动控件或让两个设置项共享状态。
///
/// 动态行只用于滑键宽度音量倍率：关闭总开关时隐藏倍率控件，但不清空倍率值。
/// 用户重新开启功能后应恢复原先调整结果。
///
/// 连续拖动可能每帧产生一个配置命令。逻辑线程应使用最新值推进本地状态，不得
/// 通过固定时长等待阻塞 UI；鼠标释放时最后一个值仍沿相同路径提交。
///
/// 默认调色板只保存方案名称，不复制颜色表。实际方案解析仍由调色板配置与皮肤
/// 系统完成；配置中保留的未知名称会继续显示，便于诊断而非被静默覆盖。
///
/// 复制粘贴时间基准、选择模式和折线音效策略都以枚举持久化。这里借助通用 ImGui
/// helper 的整数接口编辑，但选项值必须继续显式对应枚举常量，不能依赖未声明的
/// 排序假设。
///
/// 框选线宽和圆角属于渲染外观，框选模式与 timing 包含开关属于命中逻辑。消费
/// 者应分别读取这些字段，不能用加粗后的可见边框扩大实际选择区域。
///
/// 本页使用 AppConfig、EditorEngine 与 AudioManager 三种所有者。局部引用和指针
/// 均不转移所有权；函数结束后不保留任何跨帧观察对象。
///
/// 保存失败的日志与重试策略由 AppConfig 负责。本函数仍先发布内存中的新配置，
/// 从而保证当前会话的编辑反馈不会依赖同步磁盘写入结果。
/// @warning UI 热路径：设置窗口打开且编辑器页可见时每帧执行；无修改帧不得发布
/// 配置事件、保存文件或调整音频路由。
void SettingsView::drawEditorSettings()
{
    // EditorConfig 是 AppConfig 持有的内存对象，本函数只借用当前帧引用。
    auto& editorConfig = Config::AppConfig::instance().getEditorConfig();
    // settings 保存编辑行为，visual 中这里只编辑命中特效开关。
    auto& settings = editorConfig.settings;
    auto& visual   = editorConfig.visual;
    // EditorEngine 提供不持久化的画布运行态同步策略。
    auto& engine = Logic::EditorEngine::instance();
    // 局部快照交给 ImGui，用户切换后通过 setter 立即写回 Engine。
    bool syncSameMainAudioCanvases =
        engine.isSyncSameMainAudioCanvasesEnabled();
    // changed 只汇总需要发布和持久化的 AppConfig 字段。
    bool changed = false;

    // 根 VBox 每帧重建父子关系，具体行和 section 由 SettingsView 缓存复用。
    // 清理布局树不会清空 AppConfig 中已经编辑的字段。
    m_contentVBox.clear();
    // 统一间距和内边距保持各设置标签页视觉一致。
    // 标题行自身会覆盖内边距，内容 section 则继续使用该页面基准。
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    // 行索引包含标题和控件，section 索引只对应本帧展开的分组。
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 使用布局缓存中的统一标签列宽，避免设置页每帧重复测量全部标签。
    // 当前窗口缩放参与缓存选择，保证 DPI 改变后标签和值列仍对齐。
    const float maxLabelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());

    /// 创建一个可折叠标题，并返回当前帧可填充的内容 section。
    ///
    /// @param label 本地化后的标题，也参与构造页面内稳定 ID。
    /// @param defaultOpen StateStorage 尚无记录时采用的初始展开值。
    /// @return 展开时返回非拥有 section 指针，折叠时返回空指针。
    ///
    /// 折叠状态只保存在当前 ImGui 窗口上下文中，不写入 EditorConfig。调用方必须
    /// 仅在返回非空时登记控件，避免隐藏设置占用高度或参与交互。
    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        // 行、节、页面前缀和标题共同隔离不同分组的 ImGui 状态。
        std::string baseIdStr = "S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        // 在登记标题回调前读取状态，以决定本帧是否创建内容区。
        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        // 标题独占一行，并使用 ImGui 当前 frame 高度适配主题与 DPI。
        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                // Clay 给出绝对屏幕矩形，ImGui 游标需显式移动到标题起点。
                ImGui::SetCursorScreenPos({ r.x, r.y });

                // 标题颜色基于当前主题，仅为 Hovered 与 Active 状态轻微增亮。
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                // 三次颜色压栈与回调末尾 PopStyleColor(3) 严格配对。
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                // 临时收窄 WorkRect，避免标题点击区域越过 Clay 分配宽度。
                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                // 标题取消父窗口内边距，完整覆盖分配矩形。
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                // 数值 ID 借用指针重载传入，不表示可解引用对象地址。
                // CollapsingHeader 是无嵌套整行标题，不需要调用 TreePop。
                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                // 回调结束前恢复窗口状态，避免污染后续设置行。
                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                // 写回点击后的状态，下一帧布局据此创建或省略 section。
                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        // 标题布局始终加入根 VBox，内容布局只在展开分支加入。
        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            // 装饰 section 统一设置背景、行距和内边距。
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            // Fit 高度只包裹本组实际控件，根 VBox 负责组间间距。
            return &sec;
        }
        // 空指针明确表示折叠组本帧不应登记任何隐藏控件。
        return nullptr;
    };

    // 编辑行为组字段语义：
    // - 默认调色板只影响未指定项目级方案的编辑视图；
    // - 滚动相关开关改变输入映射与播放联动，不改当前播放位置；
    // - 折线绘制和粘贴开关只作用于之后提交的新编辑命令；
    // - 复制粘贴基准决定跨 timing 变化时保持时间戳还是拍位置；
    // - beatDivisor 为网格细分整数，必须保持大于零；
    // - overlapTimeWindowMs 是对象重叠比较容差，不是视觉线宽。
    //
    // 除同主音频同步外，这些字段都属于 AppConfig。页面不主动重放当前操作，
    // 也不为了应用新选项而修改撤销栈中的既有命令。
    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.editor.behavior").data(), true) ) {
        // 编辑行为组汇集默认调色板、时间线输入、粘贴策略与重叠判定参数。
        // 这些值只定义工具行为，不直接修改当前谱面对象。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.default_palette").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 默认调色板保存稳定方案名；空值与保留 ID 均兼容皮肤默认语义。
                auto& defaultScheme = settings.defaultColorPaletteSchemeName;
                // 历史空值继续显示为皮肤默认，避免旧配置出现无效预览。
                const bool skinSelected =
                    defaultScheme.empty() ||
                    defaultScheme ==
                        Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                // 空名称是兼容旧配置的读取语义；主动选择会归一化为保留 ID。
                // 组合框预览只借用本帧有效的翻译或 std::string 缓冲。
                const char* preview =
                    skinSelected
                        ? TR("ui.toolbar.note_palette.skin_default_scheme")
                              .data()
                        : defaultScheme.c_str();
                // 组合框占满 Clay 分配的值列，隐藏 ID 与可见翻译分离。
                ImGui::SetNextItemWidth(r.width);
                if ( FeedbackBeginCombo("##EditorDefaultPalette", preview) ) {
                    if ( FeedbackSelectable(
                             TR("ui.toolbar.note_palette.skin_default_scheme")
                                 .data(),
                             skinSelected) ) {
                        // 用户主动选择后写入明确保留 ID，统一后续持久化语义。
                        defaultScheme =
                            Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                        changed = true;
                    }
                    // 命名方案按用户配置顺序显示，不在此处排序或解析颜色。
                    for ( const auto& scheme :
                          settings.colorPalettes.schemes ) {
                        if ( FeedbackSelectable(
                                 scheme.name.c_str(),
                                 defaultScheme == scheme.name) ) {
                            // 仅保存名称，不复制颜色数据，方案后续修改可统一生效。
                            defaultScheme = scheme.name;
                            changed       = true;
                        }
                    }
                    // FeedbackBeginCombo 成功后必须结束配对的弹窗作用域。
                    FeedbackEndCombo();
                }
                if ( ImGui::IsItemHovered() ) {
                    // Tooltip 解释默认方案的继承范围，只在悬浮时渲染。
                    Utils::renderTooltip(
                        TR("ui.settings.editor.default_palette_tooltip").data(),
                        Utils::TooltipDir::Right);
                }
            });

        // 后续设置项采用统一标签宽度，使开关和数值控件共享对齐基线。

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.reverse_scroll").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // 反向滚动仅反转输入方向，不改变已有时间位置。
                           // 键盘导航和其他时间跳转不读取该鼠标滚动选项。
                           changed |= ::MMM::UI::FeedbackCheckbox(
                               "##ReverseScroll", &settings.reverseScroll);
                       });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.snap_floor").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // floor 吸附把计算结果限制在目标拍线的一侧。
                // 关闭时由吸附算法按最近网格点选择，不在 UI 中计算结果。
                changed |= ::MMM::UI::FeedbackCheckbox("##SnapFloor",
                                                       &settings.snapFloor);
                if ( ImGui::IsItemHovered() ) {
                    // 提示补充 floor 与普通最近点吸附的差异。
                    Utils::renderTooltip(
                        TR("ui.settings.editor.snap_floor_tooltip").data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.stop_playback_on_scroll").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 开启后滚动交互会先停止播放，具体停止动作由画布消费者执行。
                // 切换选项本身不会停止当前正在播放的音频。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##StopPlaybackOnScroll", &settings.stopPlaybackOnScroll);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.toolbar_value_wheel_adjustment")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 允许滚轮编辑工具栏值时，控件消费者仍需检查悬浮与焦点。
                // 此页仅配置许可，不截获当前滚轮输入。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##ToolbarValueWheelAdjustment",
                    &settings.enableToolbarValueWheelAdjustment);
                if ( ImGui::IsItemHovered() ) {
                    // Tooltip 说明滚轮作用范围，避免与时间线滚动混淆。
                    Utils::renderTooltip(TR("ui.settings.editor.toolbar_value_"
                                            "wheel_adjustment_tooltip")
                                             .data(),
                                         Utils::TooltipDir::Right);
                }
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.hit_effects").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // 命中特效属于 visual 配置，但与编辑反馈一同展示。
                           // 关闭只抑制后续特效生成，不清理已结束的动画记录。
                           changed |= ::MMM::UI::FeedbackCheckbox(
                               "##HitEffects", &visual.enableHitEffects);
                       });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sync_same_main_audio").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 该开关属于 EditorEngine 会话状态，不写入 AppConfig。
                // 局部值每帧从 Engine 重新读取，始终反映当前运行态真值。
                if ( ::MMM::UI::FeedbackCheckbox("##SyncSameMainAudioCanvases",
                                                 &syncSameMainAudioCanvases) ) {
                    // Setter 立即更新所有共享主音频的画布同步策略。
                    engine.setSyncSameMainAudioCanvases(
                        syncSameMainAudioCanvases);
                }
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.scroll_snap").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // 滚动吸附决定滚动结束后是否落到离散时间网格。
                           // 它与对象绘制吸附分离，由时间线滚动控制器读取。
                           changed |= ::MMM::UI::FeedbackCheckbox(
                               "##ScrollSnap", &settings.scrollSnap);
                       });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.disable_scroll_accel_while_drawing")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 绘制期间可禁用滚动加速度，减少连续落点的不确定偏移。
                // 绘制结束后正常滚动仍可继续使用加速曲线。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##DisableAccel",
                    &settings.disableScrollAccelerationWhileDrawing);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.remove_objects_on_polyline_path")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 折线经过既有物件时是否删除由绘制工具读取该策略。
                // 设置变化不扫描或删除当前谱面中的任何对象。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##RemoveObjectsOnPolylinePath",
                    &settings.removeObjectsOnPolylinePath);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.select_pasted_objects").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 粘贴后选择新对象便于继续整体移动，不影响粘贴内容本身。
                // 关闭时保留粘贴前选择状态的具体规则由命令系统决定。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##SelectPastedObjects", &settings.selectPastedObjects);
            });
        // 复制粘贴时间基准决定跨变速段时保持绝对时间还是音乐拍位置。
        // 枚举以整数引用传给通用单选 helper，选项值必须与枚举保持一致。
        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.editor.copy_paste_time_basis").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.editor.copy_paste_time_basis.timestamp")
                    .data(),
                (int)Config::CopyPasteTimeBasis::Timestamp },
              { TR_CACHE("ui.settings.editor.copy_paste_time_basis.beat")
                    .data(),
                (int)Config::CopyPasteTimeBasis::Beat } },
            (int&)settings.copyPasteTimeBasis,
            changed);
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.scroll_multiplier").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 滚动倍率是无量纲系数，消费者与基础滚动速度相乘。
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##ScrollMul",
                    &settings.scrollSpeedMultiplier,
                    1.0f,
                    10.0f,
                    "%.4f");
                if ( ImGui::IsItemHovered() ) {
                    // 提示解释倍率含义，仅在用户检查该控件时显示。
                    Utils::renderTooltip(
                        TR("ui.settings.editor.scroll_multiplier_tooltip")
                            .data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.beat_divisor").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 使用局部整数编辑，只有控件报告变化时才回写正式字段。
                int beatDivisor = settings.beatDivisor;
                // 1 至 64 覆盖整拍到高精度细分，保持除数始终为正。
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackSliderInt(
                         "##BeatDivisor", &beatDivisor, 1, 64) ) {
                    settings.beatDivisor = beatDivisor;
                    changed              = true;
                }
                if ( ImGui::IsItemHovered() ) {
                    // Tooltip 说明拍细分对吸附和绘制网格的共同影响。
                    Utils::renderTooltip(
                        TR("ui.settings.editor.beat_divisor_tooltip").data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.overlap_time_window").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 重叠时间窗口以毫秒保存，用于容忍浮点时间附近的对象命中。
                ImGui::SetNextItemWidth(r.width);
                changed |=
                    ::MMM::UI::FeedbackDragFloat("##OverlapTimeWindowMs",
                                                 &settings.overlapTimeWindowMs,
                                                 0.1f,
                                                 0.0f,
                                                 100.0f,
                                                 "%.1f ms");
                // DragFloat 在文本输入等路径可能越界，最终显式保证非负。
                settings.overlapTimeWindowMs =
                    std::max(0.0f, settings.overlapTimeWindowMs);
            });
    }

    // 框选配置边界：
    // - SelectionMode 定义对象与矩形的几何包含规则；
    // - timelineSelectionIncludesBpm 决定 timing 对象是否参加同次选择；
    // - marqueeThickness 和 marqueeRounding 只控制选框视觉；
    // - 像素外观参数不参与对象命中范围计算；
    // - 修改设置不会重算或清空当前选择集。
    if ( auto* sec = addHeader(TR_CACHE("ui.settings.editor.selection").data(),
                               true) ) {
        // 框选组定义几何命中语义和选框外观，全部为软件级编辑器配置。
        // 采用统一标签宽度，使单选、开关和滑块共享值列起点。

        // Strict 要求对象完全落入选框，Intersection 接受几何相交。
        // 选项枚举以整数引用交给通用单选 helper，必须保持常量映射稳定。
        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.editor.selection").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.editor.selection.strict").data(),
                (int)Config::SelectionMode::Strict },
              { TR_CACHE("ui.settings.editor.selection.intersection").data(),
                (int)Config::SelectionMode::Intersection } },
            (int&)settings.selectionMode,
            changed);
        // 通用 helper 只修改枚举与 changed，不直接发布选择事件。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.timeline_selection_includes_bpm")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 时间线框选是否包含 BPM/timing 对象与普通音符选择相互独立。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##TimelineSelectionIncludesBpm",
                    &settings.timelineSelectionIncludesBpm);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.selection.thickness").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 选框线宽以屏幕像素保存，只改变视觉轮廓而不扩大命中区域。
                ImGui::SetNextItemWidth(r.width);
                changed |=
                    ::MMM::UI::FeedbackSliderFloat("##MarqueeThick",
                                                   &settings.marqueeThickness,
                                                   1.0f,
                                                   10.0f,
                                                   "%.4f px");
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.selection.rounding").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // 圆角同样使用像素单位，零值绘制直角选框。
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ::MMM::UI::FeedbackSliderFloat(
                               "##MarqueeRound",
                               &settings.marqueeRounding,
                               0.0f,
                               20.0f,
                               "%.4f px");
                       });
    }

    // 音效配置边界：
    // - PolylineSfxStrategy 决定折线内部点和尾点映射到哪类采样；
    // - 宽度倍率只在对应总开关启用时参与音量计算；
    // - 立体声开关控制声像，不改变总体音量；
    // - 变速同步开关决定 SFX 是否经过随播放速度变化的路由；
    // - 页面不加载、解码或播放任何音频资源。
    //
    // 普通音效字段由后续播放读取；只有路由拓扑需要在切换瞬间通知 AudioManager。
    // AudioManager 调用必须位于控件确实变化的分支，不能在每帧无条件执行。
    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.editor.sfx").data(), true) ) {
        // 音效组控制折线音符映射、宽度音量、声像和变速同步。
        // 采用统一标签宽度，使动态出现的倍率行不会改变对齐方式。

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_strategy").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 组合框使用局部整数，避免直接把 enum 存储交给 ImGui API。
                int strategy = (int)settings.sfxConfig.polylineStrategy;
                // 数组顺序必须与 PolylineSfxStrategy 枚举值顺序保持一致。
                const char* strategies[] = {
                    TR_CACHE("ui.settings.editor.sfx_strategy.exact").data(),
                    TR_CACHE(
                        "ui.settings.editor.sfx_strategy.internal_as_normal")
                        .data(),
                    TR_CACHE("ui.settings.editor.sfx_strategy.only_tail_exact")
                        .data(),
                    TR_CACHE("ui.settings.editor.sfx_strategy.all_as_normal")
                        .data()
                };
                // 控件占满 Clay 分配的值列，选中后再强类型回写枚举。
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackCombo("##SfxStrategy",
                                              &strategy,
                                              strategies,
                                              IM_ARRAYSIZE(strategies)) ) {
                    // FeedbackCombo
                    // 保证索引来自当前四项数组，再转换回策略枚举。
                    settings.sfxConfig.polylineStrategy =
                        (Config::PolylineSfxStrategy)strategy;
                    // 策略只影响后续命中音效选择，不立即播放声音。
                    changed = true;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 开启后滑键宽度可参与音量计算，具体曲线由音效系统负责。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##FlickScale",
                    &settings.sfxConfig.enableFlickWidthVolumeScaling);
            });
        if ( settings.sfxConfig.enableFlickWidthVolumeScaling ) {
            // 倍率仅在对应功能启用时展示，隐藏期间仍保留已有配置值。
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.editor.sfx_flick_mul").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    // 零倍率允许完全压低宽度贡献，十倍为当前交互上限。
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ::MMM::UI::FeedbackSliderFloat(
                        "##FlickMul",
                        &settings.sfxConfig.flickWidthVolumeMultiplier,
                        0.0f,
                        10.0f);
                    // 倍率值保持在线性配置域，音频层负责最终响度钳制。
                });
        }
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_stereo_hit_effects").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 立体声命中特效根据轨道位置设置声像，不改变资源选择策略。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##StereoHitEffects",
                    &settings.sfxConfig.enableStereoHitEffects);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_sync_speed").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 保存控件返回值，副作用仅在用户实际切换时发生。
                bool syncSpeedChanged = ::MMM::UI::FeedbackCheckbox(
                    "##SyncSpeed", &settings.sfxConfig.hitSfxSyncSpeed);
                if ( syncSpeedChanged ) {
                    // 持久化标志和运行态路由必须同时更新，避免重启前后不一致。
                    changed = true;
                    // AudioManager 立即调整 SFX 变速路由，不等待设置页关闭。
                    Audio::AudioManager::instance().updateSFXSyncSpeedRouting(
                        settings.sfxConfig.hitSfxSyncSpeed);
                    // 路由刷新使用新的布尔值，避免等待配置事件跨线程往返。
                }
            });
    }

    // 统一执行 Clay 布局渲染，只有展开 section 中登记的回调会运行。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    // 根布局使用当前剩余宽度，高度由标题和设置行自动计算。
    ImVec2 sz = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    // 推进 ImGui 游标，使父窗口正确计算内容高度和滚动范围。
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( changed ) {
        // 先把完整 EditorConfig 发送给逻辑线程，再持久化同一内存对象。
        // 单帧多个控件变化只产生一次命令和一次保存操作。
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        // AppConfig 负责序列化路径和失败记录，本页不直接访问文件系统。
        // 运行态专用同步开关不会设置 changed，因此不会误触发该保存路径。
        Config::AppConfig::instance().save();
    }
}

}  // namespace MMM::UI
