#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <array>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace MMM::UI
{

/// @brief 渲染视觉设置页。
///
/// 本页编辑软件级 `VisualConfig`，覆盖谱面时间偏移、拍线、预览区、画布交互和
/// 频谱细节五组参数。控件直接修改 AppConfig 中的内存值，本帧存在任一实际交互
/// 时才发布 `CmdUpdateEditorConfig` 并保存配置。
///
/// 配置传播分为两步：
/// - EventBus 把最新 EditorConfig 交给逻辑线程，使画布和分析视图即时更新；
/// - AppConfig::save() 把同一份值持久化，供下次启动恢复。
///
/// 页面不直接重建频谱纹理、时间线或预览画布。消费者收到逻辑命令后自行判断
/// 哪些派生资源需要失效，避免 UI 同时承担渲染资源生命周期。
///
/// Clay 负责分配每一行的屏幕矩形，ImGui 控件由登记的回调绘制。所有捕获仅在
/// 当前帧布局执行期间有效；若布局系统未来改为延迟到后续帧执行，必须把配置引用
/// 换成稳定 ID 后重新查询。
///
/// 折叠状态存入当前 ImGui 窗口的 StateStorage，不属于 VisualConfig。展开或折叠
/// 分组不会触发保存，也不会发布逻辑命令。
///
/// 数值控件的范围体现当前 UI 契约，但本页不迁移或静默修正从旧配置加载的值。
/// 配置版本迁移应在 AppConfig 加载阶段统一处理。
///
/// 各数值的单位必须保持稳定：偏移和动画时长使用秒，吸附阈值使用像素，缩放与
/// 灵敏度使用无量纲倍率，频谱内存说明使用 MiB。显示后缀只负责提示，不能改变
/// 底层字段的存储单位。
///
/// 连续拖动会产生多个中间配置值。此函数不阻塞等待逻辑线程确认，也不执行固定
/// 时长消抖；EventBus 消费者应以最新命令覆盖旧状态，使本地交互保持即时反馈。
///
/// `Feedback*` 控件统一提供悬浮和点击反馈。控件内部 ID 采用 `##` 前缀与翻译
/// 文本分离，因此切换语言或皮肤文案不会重置活动控件。新增设置项时必须继续
/// 使用页面内唯一 ID，避免不同折叠区共享编辑状态。
///
/// 频谱标签会生成少量临时字符串，仅在设置页可见时发生；不得把实际频谱计算、
/// 文件扫描或纹理上传加入这些布局回调。高成本更新应由配置消费者按需调度。
/// @warning UI 热路径：设置窗口打开且视觉页可见时每帧执行；不得在无修改帧中
/// 发布事件、保存文件或重建高成本图形资源。
void SettingsView::drawVisualSettings()
{
    // AppConfig 单例持有设置对象，visual 引用只在本次页面绘制期间使用。
    // 页面不缓存该引用，配置对象的所有权始终保留在 AppConfig。
    auto& appConfig = Config::AppConfig::instance();
    auto& visual    = appConfig.getVisualConfig();
    // changed 汇总全部控件结果，保证一次交互帧只提交一次配置。
    // 没有控件返回 true 时，函数末尾的提交分支完全跳过。
    bool changed = false;

    // 根 VBox 每帧重建子节点，行和 section 实例由 SettingsView 缓存复用。
    m_contentVBox.clear();
    // 统一间距和内边距维持各设置标签页一致的节奏。
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    // 行索引覆盖标题与设置项，section 索引只覆盖当前展开的内容区。
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 使用布局缓存中的统一标签列宽，避免设置页每帧重复测量全部标签。
    // 该宽度在标签页内容变化或缩放变化时由 SettingsView 统一刷新。
    const float maxLabelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());

    /// 创建一个可折叠分组标题，并返回当前帧可填充的内容区。
    ///
    /// @param label 本地化后的可见标题，也参与构造当前页面内的稳定 ID。
    /// @param defaultOpen 首次出现且 StateStorage 无记录时的默认展开状态。
    /// @return 展开时返回 SettingsView 缓存的 section，折叠时返回空指针。
    ///
    /// 返回指针不转移所有权，只能在当前布局树构建期间使用。调用方应以空指针
    /// 作为“本帧不登记隐藏控件”的信号。
    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        // 页面前缀、节序号、行序号和标签共同隔离不同标题的 ImGui 状态。
        std::string baseIdStr = "VS_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        // 状态先于回调登记读取，用来决定本帧是否构建内容 section。
        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        // 标题独占一行，使用当前 ImGui frame 高度以自动适应 DPI。
        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                // Clay 给出绝对屏幕矩形，ImGui 游标需移动到对应起点。
                ImGui::SetCursorScreenPos({ r.x, r.y });
                // 悬浮和按下颜色从主题 Header 色增亮，保持主题基调。
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                // 三次 PushStyleColor 与回调末尾一次 PopStyleColor(3) 配对。
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

                // 临时收窄 WorkRect，使标题点击范围不会越过 Clay 分配宽度。
                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                // 标题自身取消窗口内边距，完整占满分配矩形。
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                // 数值 ID 仅借用指针重载传入，不代表可解引用的对象地址。
                // CollapsingHeader 形成整行标题，不需要配对调用 TreePop。
                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                // 在回调结束前恢复临时修改，避免影响后续设置控件。
                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                // 把用户本帧的点击结果写回，下一帧布局据此增删 section。
                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        // 标题布局总是加入根 VBox，内容布局则取决于进入本帧时的展开状态。
        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            // 装饰 section 提供统一背景、间距和内边距。
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            // Fit 高度仅包裹实际设置行，根 VBox 负责组间空隙。
            return &sec;
        }
        // 折叠分组不创建任何设置行，因此也不会提交隐藏控件。
        return nullptr;
    };

    // 时间偏移契约：
    // - 三个字段都使用秒作为存储单位；
    // - UI 允许在 -0.5 至 0.5 秒内进行毫秒级调整；
    // - 通用偏移与波形、频谱专用偏移分别由对应消费者解释；
    // - 修改只校准视觉呈现，不移动音符或改写音频文件；
    // - 页面不把秒转换成采样帧，避免耦合具体音频采样率。
    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.offset").data(), true) ) {
        // 偏移分组把不同视觉层与音频时间轴校准，单位均为秒。
        // 三项允许正负值：负值提前绘制，正值延后绘制。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.visual_offset").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 通用视觉偏移影响未单独配置偏移量的时间相关元素。
                // 0.001 秒拖动步长便于进行毫秒级校准。
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat("##VisualOffset",
                                                  &visual.visualOffset,
                                                  0.001f,
                                                  -0.5f,
                                                  0.5f,
                                                  "%.3f s") )
                    changed = true;
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.waveform_visual_offset").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 波形偏移独立校准预计算波形与实际听感之间的误差。
                // 值直接写入配置，波形消费者负责应用符号和时间换算。
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat("##WaveformVisualOffset",
                                                  &visual.waveformVisualOffset,
                                                  0.001f,
                                                  -0.5f,
                                                  0.5f,
                                                  "%.3f s") )
                    changed = true;
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.spectrum_visual_offset").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 频谱偏移与波形偏移分离，允许补偿两种分析链路的差异。
                // 显示精度固定为毫秒，避免浮点值在界面中产生冗余位数。
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat("##SpectrumVisualOffset",
                                                  &visual.spectrumVisualOffset,
                                                  0.001f,
                                                  -0.5f,
                                                  0.5f,
                                                  "%.3f s") )
                    changed = true;
            });
    }

    // 首个 timing 点之前可能没有可直接沿用的拍号信息，绘制系统依据该开关选择
    // 是否向负时间或前置空白区域延伸拍线。设置页只提供策略，不推导拍线位置。
    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.beat_line").data(),
                               true) ) {
        // 拍线分组控制首个变速点之前的时间区域是否继续生成网格线。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.beat_line_before_first_timing").data(),
            maxLabelW,
            [&](Clay_BoundingBox, bool) {
                // 该布尔值只改变绘制策略，不修改谱面中的 timing 数据。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##BeatLineBeforeFirstTiming",
                    &visual.drawBeatLinesBeforeFirstTiming);
            });
    }

    // 预览区配置契约：
    // - areaRatio 描述布局比例，不是固定像素宽高；
    // - edgeScrollSensitivity 描述交互强度，零值表示关闭边缘推动；
    // - margin 四字段由预览区分别应用，不要求对称；
    // - 拍线与 timing 线开关仅控制预览层辅助线；
    // - 配置更新后由预览消费者重新计算派生视口。
    //
    // 本页不约束不同边距之和是否超过极窄窗口的尺寸。PreviewCanvas 必须在布局
    // 阶段对最终可用矩形做非负钳制，UI 仅保证单项输入在产品范围内。
    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.preview").data(), true) ) {
        // 预览区参数描述视口占比、边缘滚动和四向安全边距。
        // 所有值由 PreviewCanvas 消费，本页不直接计算最终像素矩形。
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.preview_ratio").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // areaRatio 控制主画布与预览区域的相对尺寸。
                           // 范围 1 至 10 防止预览区退化为零宽或吞占全部空间。
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ::MMM::UI::FeedbackSliderFloat(
                               "##PreviewRatio",
                               &visual.previewConfig.areaRatio,
                               1.0f,
                               10.0f);
                       });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_edge_scroll_sensitivity")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 灵敏度为零时关闭边缘推动，较大值加快指针靠边时的滚动。
                // 四位小数显示支持小幅调节，不改变底层 float 存储。
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##EdgeSens",
                    &visual.previewConfig.edgeScrollSensitivity,
                    0.0f,
                    5.0f,
                    "%.4f");
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_left").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 左边距从预览内容左侧向内收缩可用绘制区域。
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##MarginL",
                    &visual.previewConfig.margin.left,
                    0.0f,
                    20.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_top").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 上边距为顶部覆盖元素保留视觉安全区。
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##MarginT", &visual.previewConfig.margin.top, 0.0f, 20.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_right").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 右边距与左边距独立，支持非对称面板装饰。
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##MarginR",
                    &visual.previewConfig.margin.right,
                    0.0f,
                    20.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_bottom").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 下边距为底部控制条或判定线装饰预留空间。
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##MarginB",
                    &visual.previewConfig.margin.bottom,
                    0.0f,
                    20.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_draw_beat_lines").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 拍线开关只影响预览区，独立于主画布拍线策略。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##DrawBeatLines", &visual.previewConfig.drawBeatLines);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // Timing 线可单独关闭，便于降低密集变速谱面的视觉噪声。
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##DrawTimingLines", &visual.previewConfig.drawTimingLines);
            });
    }

    // 画布交互配置契约：
    // - timelineZoom 控制时间尺度倍率，而不是当前临时缩放中心；
    // - scrollAnimationDuration 控制导航过渡时长，零值表示无动画；
    // - enableLinearScrollMapping 选择滚动输入映射算法；
    // - snapThreshold 以屏幕像素保存，便于在视觉距离上保持一致；
    // - 页面只发布新配置，不模拟鼠标、滚轮或吸附事件。
    //
    // 这些值可能在连续拖动中每帧改变，EventBus 消费者应覆盖旧配置并避免为每个
    // 中间值进行阻塞等待。最终释放控件时，最后一个值仍通过相同命令提交。
    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.visual.canvas_interaction").data(), true) ) {
        // 画布交互分组调整缩放映射、滚动动画和吸附判定，不改谱面数据。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.timeline_zoom").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // timelineZoom 是全局倍率，显示后缀明确其无量纲语义。
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat("##TimelineZoom",
                                                          &visual.timelineZoom,
                                                          0.1f,
                                                          5.0f,
                                                          "%.4fx");
                if ( ImGui::IsItemHovered() ) {
                    // 提示解释倍率与时间线手势的关系，仅在悬浮时创建。
                    Utils::renderTooltip(
                        TR("ui.settings.visual.timeline_zoom_tooltip").data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.scroll_animation_duration").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 零秒表示即时跳转，其余值作为平滑滚动持续时间。
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##ScrollAnimationDuration",
                    &visual.scrollAnimationDuration,
                    0.0f,
                    0.5f,
                    "%.4f s");
                if ( ImGui::IsItemHovered() ) {
                    // 提示位于控件右侧，避免遮挡正在调整的数值。
                    Utils::renderTooltip(
                        TR("ui.settings.visual.scroll_animation_tooltip")
                            .data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.linear_scroll").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // 线性映射开关决定滚轮输入到时间位移的转换曲线。
                           changed |= ::MMM::UI::FeedbackCheckbox(
                               "##LinearScroll",
                               &visual.enableLinearScrollMapping);
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.snap_threshold").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // 吸附阈值以屏幕像素计，零值允许完全关闭邻近吸附。
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ::MMM::UI::FeedbackSliderFloat(
                               "##SnapThreshold",
                               &visual.snapThreshold,
                               0.0f,
                               48.0f,
                               "%.4f px");
                       });
    }

    // 频谱细节配置契约：
    // - 等级枚举是持久化值，数组索引只用于本页组合框；
    // - profile 参数由 Config::spectrumDetailProfile 集中维护；
    // - 内存数字是按分钟估算的说明信息，不读取实时显存占用；
    // - 双声道与 BPM 单声道示例采用不同声道数和层数；
    // - 选择等级后，频谱系统自行使旧缓存失效并按需重建。
    //
    // 等级数组与名称数组都采用固定长度，新增枚举时必须同步增加显示名和选项。
    // 如果配置中出现当前数组不认识的值，预览回退到 Balanced，但只有用户主动
    // 选择时才写回配置，避免每帧渲染静默覆盖未知的新版本值。
    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.spectrum").data(), true) ) {
        // 频谱细节等级同时决定时间分段数、频率 bin 数和纹理内存占用。
        // 选项按性能成本从低到高固定排列，不能按本地化名称排序。
        // Experimental 仍显式展示，便于用户理解其资源代价后主动选择。
        const std::array<Config::SpectrumDetailLevel, 6> detailLevels = {
            Config::SpectrumDetailLevel::Performance,
            Config::SpectrumDetailLevel::Balanced,
            Config::SpectrumDetailLevel::Fine,
            Config::SpectrumDetailLevel::Ultra,
            Config::SpectrumDetailLevel::Extreme,
            Config::SpectrumDetailLevel::Experimental
        };
        // 可见名称与 detailLevels 使用相同索引，数组长度必须保持一致。
        const std::array<std::string_view, 6> detailNames = {
            TR_CACHE("ui.settings.visual.spectrum_detail.performance").view(),
            TR_CACHE("ui.settings.visual.spectrum_detail.balanced").view(),
            TR_CACHE("ui.settings.visual.spectrum_detail.fine").view(),
            TR_CACHE("ui.settings.visual.spectrum_detail.ultra").view(),
            TR_CACHE("ui.settings.visual.spectrum_detail.extreme").view(),
            TR_CACHE("ui.settings.visual.spectrum_detail.experimental").view()
        };
        /// 把字节估算转换成设置页使用的 MiB 单位。
        /// @param bytes 频谱纹理估算字节数。
        /// @return 以 1024 平方为分母的 MiB 值。
        auto bytesToMiB = [](std::uint64_t bytes) {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        };
        /// 构造一个包含采样密度和内存估算的频谱等级标签。
        ///
        /// @param level 用于查询固定频谱 profile 的等级枚举。
        /// @param name 已本地化的等级名称。
        /// @return 可直接交给 ImGui 组合框显示的独立字符串。
        ///
        /// 内存估算展示两种典型负载：双声道单层纹理，以及 BPM 分层的单声道
        /// 纹理。该值用于选择提示，不代表当前工程已经分配的实际显存。
        auto makeDetailLabel = [bytesToMiB](Config::SpectrumDetailLevel level,
                                            std::string_view            name) {
            // profile 是配置层定义的权威等级参数，UI 不复制其数值表。
            const auto profile = Config::spectrumDetailProfile(level);
            // 双声道场景按两个声道和单层估算每分钟纹理大小。
            const double stereoMiB = bytesToMiB(
                Config::estimateSpectrumTextureBytesPerMinute(level, 2, 1));
            // BPM 单声道示例按一个声道和四层纹理估算。
            const double monoMiB = bytesToMiB(
                Config::estimateSpectrumTextureBytesPerMinute(level, 1, 4));
            // 各片段文本单独翻译，最终由固定 fmt 模板组合。
            const std::string_view segmentsText =
                TR_CACHE(
                    "ui.settings.visual.spectrum_detail.segments_per_second")
                    .view();
            const std::string_view binsText =
                TR_CACHE("ui.settings.visual.spectrum_detail.bins").view();
            const std::string_view memoryPrefixText =
                TR_CACHE("ui.settings.visual.spectrum_detail.memory_prefix")
                    .view();
            const std::string_view minuteText =
                TR_CACHE("ui.settings.visual.spectrum_detail.minute").view();
            const std::string_view stereoText =
                TR_CACHE("ui.settings.visual.spectrum_detail.stereo").view();
            const std::string_view monoText =
                TR_CACHE("ui.settings.visual.spectrum_detail.bpm_mono").view();
            // 标签字段顺序：等级名、每秒分段、频率 bin、内存前缀、双声道估算、
            // 单声道 BPM 分层估算。数值精度固定为两位小数，便于横向比较。
            // 不把翻译文本作为 fmt 格式串解析，避免皮肤文案异常导致设置页崩溃。
            // 固定格式串也保证所有语言沿用相同的数值精度和单位顺序。
            return fmt::format(
                "{} - {:.0f} {} x {} {}; {} {:.2f} MiB/{} ({}), {:.2f} "
                "MiB/{} ({})",
                name,
                profile.segmentsPerSecond,
                segmentsText,
                profile.frequencyBins,
                binsText,
                memoryPrefixText,
                stereoMiB,
                minuteText,
                stereoText,
                monoMiB,
                minuteText,
                monoText);
        };

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.spectrum_detail").data(),
            maxLabelW,
            [&, detailLevels, detailNames, makeDetailLabel](Clay_BoundingBox r,
                                                            bool) {
                // detailLevels、detailNames
                // 和标签构造器按值捕获，确保布局回调执行时
                // 局部数组仍然有效；visual 与 changed
                // 则按引用连接本帧配置状态。 Balanced
                // 是无法识别旧枚举值时的安全预览回退索引。
                // 正常配置会在循环中映射到数组里的实际等级。
                int currentIndex = 1;
                for ( size_t i = 0; i < detailLevels.size(); ++i ) {
                    if ( visual.spectrumDetailLevel == detailLevels[i] ) {
                        currentIndex = static_cast<int>(i);
                        break;
                    }
                }

                // 标签含动态内存估算，每帧按当前翻译生成，不持久化到配置。
                // 固定大小数组避免等级数量与标签容量在运行期发生分离。
                std::array<std::string, 6> labels;
                for ( size_t i = 0; i < detailLevels.size(); ++i ) {
                    // 使用同一索引配对枚举和本地化名称，生成完整资源成本说明。
                    labels[i] =
                        makeDetailLabel(detailLevels[i], detailNames[i]);
                }

                // 组合框使用整个值列，并以当前等级的完整说明作为预览。
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo(
                         "##SpectrumDetail", labels[currentIndex].c_str()) ) {
                    for ( size_t i = 0; i < detailLevels.size(); ++i ) {
                        // 选中状态由当前索引决定，点击后写回对应枚举。
                        const bool selected =
                            currentIndex == static_cast<int>(i);
                        if ( ::MMM::UI::FeedbackSelectable(labels[i].c_str(),
                                                           selected) ) {
                            visual.spectrumDetailLevel = detailLevels[i];
                            changed                    = true;
                        }
                        if ( selected ) {
                            // 默认焦点让键盘导航从当前等级继续。
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    // FeedbackBeginCombo 成功后必须结束配对作用域。
                    ::MMM::UI::FeedbackEndCombo();
                }
                if ( ImGui::IsItemHovered() ) {
                    // Tooltip 补充说明等级影响，不在常驻标签中占用更多宽度。
                    Utils::renderTooltip(
                        TR("ui.settings.visual.spectrum_detail.tooltip").data(),
                        Utils::TooltipDir::Right);
                }
            });
    }

    // 统一执行 Clay 布局渲染，只有展开 section 中登记的回调会运行。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    // 根布局使用当前剩余宽度，高度由所有标题和设置行自动计算。
    ImVec2 sz = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    // 推进 ImGui 游标，确保父滚动区域正确计算本页内容高度。
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( changed ) {
        // 先通知逻辑线程采用新 EditorConfig，再持久化同一份 AppConfig。
        // 单帧多个控件变化仍只产生一次事件和一次磁盘保存。
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        // 保存发生在事件发布之后；运行态已经收到新值，即使磁盘写入稍后完成。
        // AppConfig 负责具体路径、序列化和失败记录，本页不直接操作文件系统。
        Config::AppConfig::instance().save();
    }
}

}  // namespace MMM::UI
