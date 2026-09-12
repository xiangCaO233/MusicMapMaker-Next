#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "mmm/project/Project.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"

namespace MMM::UI
{

/// @brief 渲染项目设置页。
///
/// 页面只在已有工程时展示，包含只读工程路径、项目级音符调色板和自动备份覆盖
/// 三组信息。所有修改直接落到当前 Project 设置草稿，函数末尾统一同步备份配置
/// 并保存工程，避免每个控件分别写盘。
///
/// 项目级调色板允许继承软件默认、显式使用皮肤默认或选择命名方案。自动备份
/// 可以继承全局配置，也可以复制当前全局值作为项目覆盖起点。
///
/// 布局由 Clay 负责分配矩形，具体控件仍由 ImGui 在对应矩形中绘制。页面因此
/// 遵循两阶段流程：先登记行、折叠区和绘制回调，再一次性执行布局树。回调捕获
/// 的值必须在本帧渲染结束前有效，不能保存到页面生命周期之外。
///
/// 修改状态遵循以下约定：
/// - 控件只修改 Project 中已经加载的设置对象；
/// - `changed` 汇总本帧是否需要保存；
/// - 覆盖开关延迟到控件布局完成后再增删 optional；
/// - 保存前把最终备份覆盖值同步给 EditorEngine；
/// - 只读路径和折叠状态不计入工程修改。
///
/// 自动备份覆盖使用 optional 表达继承关系，而不是复制后持续同步。无值时运行时
/// 读取软件级配置；有值时项目配置完全接管模式、间隔、触发条件和保留数量。
/// 这种区分必须保留，否则用户修改软件默认后，继承中的项目不会获得新配置。
///
/// 所有 ImGui 控件使用稳定的隐藏 ID。可见文本来自翻译表，不能作为唯一持久化
/// 标识；调色板保存稳定名称，自动备份则保存枚举和值类型字段。
///
/// 页面不承担以下职责：
/// - 不创建、关闭或迁移工程目录；
/// - 不解析调色板颜色，具体颜色由配置与皮肤系统解析；
/// - 不执行备份任务，EditorEngine 只接收本页提交的策略；
/// - 不验证备份文件内容，数量裁剪由备份服务处理；
/// - 不把窗口折叠状态写入工程文件。
///
/// `Project*` 是 EditorEngine 管理的非拥有观察指针。页面渲染期间不会切换工程，
/// 因此登记到 Clay 的回调可以捕获该指针和设置引用；若未来允许回调跨帧执行，
/// 必须改为稳定工程 ID 并在执行时重新查询对象。
///
/// 翻译字符串通过 `TR_CACHE` 保持到本帧控件提交完成。项目路径则先转换成独立的
/// UTF-8 字符串并按值捕获，避免文件系统 path 的平台编码泄漏到 ImGui。设置项
/// 的可见标签与 `##` 后的内部 ID 分离，使切换语言不会改变控件身份。
///
/// 数值范围只在界面层限制用户输入；加载已有工程时本函数不会静默修正旧值。
/// 若需要配置迁移，应由 Project 加载流程集中完成并记录版本，而不是在每帧渲染
/// 中改写数据。
/// @warning UI 热路径：设置窗口打开且当前页可见时每帧执行；工程保存只在用户
/// 实际修改后触发，不得添加无条件文件访问。
void SettingsView::drawProjectSettings()
{
    // Engine 提供当前 Project 观察指针，本函数不取得额外所有权。
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();

    if ( !project ) {
        // 无工程时只显示明确提示，不构建指向空项目的 Clay Lambda。
        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::TextColored(
            dangerCol, "%s", TR("ui.settings.project.no_project").data());
        return;
    }

    // Clay 描述树每帧重建，SettingsView 按行和节索引复用布局对象。
    // clear() 只清空本帧的父子关系，行和 section 的存储由页面统一持有。
    // 统一的内边距和间距让折叠标题、设置行与其他设置页保持视觉一致。
    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex                        = 0;
    size_t sectionIndex                    = 0;
    bool   changed                         = false;
    bool   autoBackupOverrideToggleChanged = false;
    // changed 代表工程持久化需要更新；另一个标志只描述 optional 结构变化。
    // 两者分离后，普通备份字段编辑不会重复创建覆盖配置。
    // 可选值存在即表示项目覆盖全局自动备份配置。
    // 此布尔快照同时驱动复选框；不能直接把 optional 地址交给 ImGui。
    bool useProjectAutoBackupConfig =
        project->m_settings.m_autoBackupOverride.has_value();
    const float labelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());
    // 标签宽度由当前页内容和窗口缩放共同决定，所有设置行共享同一对齐基线。

    /// 添加持久于当前 ImGui 会话的折叠标题，并在展开时返回内容 section。
    /// 返回指针指向 SettingsView 缓存 section，仅在本帧布局构建期使用。
    ///
    /// @param label 本地化后的可见标题，同时参与构造当前页面内的 ImGui ID。
    /// @param defaultOpen StateStorage 尚无记录时采用的初始展开状态。
    /// @return 展开时返回可填充的 section，折叠时返回空指针。
    ///
    /// 折叠状态由 ImGui StateStorage 按窗口上下文保存，不进入 Project 文件。
    /// 调用方必须仅在返回非空时登记内容行，保证隐藏分组不占布局高度。
    /// sectionIndex 只为真实创建的内容区递增，rowIndex 则包含标题与内容行。
    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        // 节、行索引和标签共同构造稳定 ID，防止不同折叠区状态串扰。
        std::string baseIdStr = "PRJ_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        // StateStorage 缺少条目时使用调用方指定的默认展开状态。
        // 读取发生在登记回调之前，决定当前帧是否创建后续内容布局。
        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        // 标题独占一行并填满宽度，点击范围与 Clay 分配矩形一致。
        // 高度取当前 ImGui frame，能够随 DPI 和主题样式自动缩放。
        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                // Clay 给出屏幕矩形，ImGui 游标必须显式移动到标题起点。
                // 回调参数中的 hover 状态不用作标题状态，交互由 ImGui
                // 自己判定。
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                // Hovered 和 Active 颜色从主题 Header
                // 色轻微增亮，保持主题基调。 Push 三次必须与回调末尾的
                // PopStyleColor(3) 严格配对。
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

                // 临时收窄 WorkRect，避免 CollapsingHeader 越过分配宽度。
                // 这是对 ImGui 当前窗口内部布局状态的短期借用，离开回调前恢复。
                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                // 指针形式 ID 由数值 ImGuiID 转换，不引用实际对象地址。
                // CollapsingHeader 标志使节点本身成为整行可点击的无嵌套标题。
                // 默认展开标志只影响首次出现，之后以 StateStorage 的值为准。
                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                // 恢复 WindowPadding、WorkRect 和三项 Header
                // 色，保持样式栈平衡。
                // 状态在恢复样式后写回，下一帧创建布局时即可读取新值。
                // TreeNodeEx 使用 CollapsingHeader 时无需调用 TreePop。
                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        // 标题布局始终存在；内容布局仅在展开分支中追加到同一 VBox。
        // 元素和布局使用不同后缀，避免 Clay 的稳定 ID 发生碰撞。

        // 折叠时不创建内容 section，也不提交其中的隐藏控件。
        if ( isOpen ) {
            // section 使用装饰背景和统一内边距，承载实际设置项。
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    // 信息区与备份区相互独立；折叠其中一组不会改变另一组的行内容。
    // addHeader 返回空指针即代表该组本帧只有标题，不进入控件登记流程。
    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.project.info").data(), true) ) {
        // “项目信息”默认展开；当前只包含路径与音符调色板两项。
        // 路径用于识别当前工程但不可在设置页直接编辑，避免绕过迁移流程。
        // 工程路径转换为 UTF-8 值，Lambda 按值捕获以覆盖布局延迟调用。
        std::string projPath = Config::pathToUtf8(project->m_projectRoot);
        // 路径行关闭额外装饰交互，只在 Clay 提供的值区域绘制文本。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.project.path").data(),
            labelW,
            [projPath](Clay_BoundingBox r, bool) {
                // 文本高度用于垂直居中，宽度决定静态显示或跑马灯。
                float textW  = ImGui::CalcTextSize(projPath.c_str()).x;
                float textH  = ImGui::CalcTextSize(projPath.c_str()).y;
                float offset = (r.height - textH) * 0.5f;

                // 宽度判断每帧执行，以适应窗口缩放和 DPI 变化。
                if ( textW <= r.width ) {
                    // 路径可完整容纳时静态显示，避免不必要的动画。
                    ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                    ImGui::TextUnformatted(projPath.c_str());
                } else {
                    // 超宽路径以固定速度循环滚动，并在两端各停留一段时间。
                    // 动画周期由文本溢出距离计算，长路径获得更长滚动阶段。
                    float scrollSpeed = 40.0f;  // 每秒滚动像素数。
                    float maxScroll =
                        textW - r.width + 30.0f;  // 尾端保留辨识余量。
                    float pauseTime      = 1.5f;  // 起点与终点停顿秒数。
                    float scrollDuration = maxScroll / scrollSpeed;
                    float totalCycleTime = scrollDuration + pauseTime * 2.0f;

                    // ImGui 单调时间只驱动视觉动画，不修改工程路径状态。
                    float time      = (float)ImGui::GetTime();
                    float cycleTime = fmodf(time, totalCycleTime);

                    // 分段时间轴为“起点停留—滚动—终点停留”。
                    float scrollX = 0.0f;
                    if ( cycleTime < pauseTime ) {
                        scrollX = 0.0f;  // 起点停顿。
                    } else if ( cycleTime < pauseTime + scrollDuration ) {
                        scrollX = (cycleTime - pauseTime) *
                                  scrollSpeed;  // 匀速滑动。
                    } else {
                        scrollX = maxScroll;  // 终点停顿。
                    }

                    // 裁剪到 Clay 值区域，滚动文字不会覆盖标签或相邻设置行。
                    // 裁剪栈仅包围文本提交，结束后立即恢复父窗口裁剪范围。
                    ImGui::PushClipRect(
                        { r.x, r.y }, { r.x + r.width, r.y + r.height }, true);
                    ImGui::SetCursorScreenPos({ r.x - scrollX, r.y + offset });
                    ImGui::TextUnformatted(projPath.c_str());
                    ImGui::PopClipRect();
                }
            });
        // 调色板行可编辑项目设置，因此所有选项共享本帧 changed 标志。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.project.note_palette").data(),
            labelW,
            [&](Clay_BoundingBox r, bool) {
                // 调色板列表来自软件配置，项目字段只保存方案稳定名称。
                auto& paletteConfig = Config::AppConfig::instance()
                                          .getEditorSettings()
                                          .colorPalettes;
                auto& projectScheme =
                    project->m_settings.m_colorPaletteSchemeName;
                // 引用直接指向工程设置，选择项生效后无需额外复制回 Project。

                // 组合框预览只负责显示；持久化值始终保留在 projectScheme。
                std::string previewName;
                if ( projectScheme.empty() ) {
                    // 空名称表示跟随软件默认，而不是缺失或错误。
                    previewName =
                        TR_CACHE("ui.settings.project.note_palette.inherit")
                            .data();
                } else if ( projectScheme ==
                            Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ) {
                    // 保留 ID 使用本地化可见名称，项目仍持久化稳定常量。
                    previewName =
                        TR_CACHE("ui.toolbar.note_palette.skin_default_scheme")
                            .data();
                } else {
                    // 普通方案直接显示其稳定名称；方案不存在时也保留原值供诊断。
                    previewName = projectScheme;
                }

                // 组合框占满 Clay 分配的值列，隐藏 ID 避免显示内部标识。
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo("##ProjectNotePalette",
                                                   previewName.c_str()) ) {
                    // 继承项清空项目字段，使未来软件默认变化自动生效。
                    const bool inheritSelected = projectScheme.empty();
                    if ( ::MMM::UI::FeedbackSelectable(
                             TR_CACHE(
                                 "ui.settings.project.note_palette.inherit")
                                 .data(),
                             inheritSelected) ) {
                        projectScheme.clear();
                        changed = true;
                    }
                    // 当前选择取得默认键盘焦点，打开列表时能直接定位。
                    if ( inheritSelected ) ImGui::SetItemDefaultFocus();

                    // 皮肤默认是显式项目选择，与软件默认继承语义不同。
                    const bool skinSelected =
                        projectScheme ==
                        Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                    if ( ::MMM::UI::FeedbackSelectable(
                             TR_CACHE(
                                 "ui.toolbar.note_palette.skin_default_scheme")
                                 .data(),
                             skinSelected) ) {
                        projectScheme =
                            Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                        changed = true;
                    }
                    // 与继承项相同，显式皮肤默认也保持键盘导航位置。
                    if ( skinSelected ) ImGui::SetItemDefaultFocus();

                    // 配置顺序即用户在软件设置中维护的展示顺序，不在此处重排。
                    for ( const auto& scheme : paletteConfig.schemes ) {
                        // 命名方案按配置顺序展示，选择后只复制其名称。
                        const bool selected = projectScheme == scheme.name;
                        if ( ::MMM::UI::FeedbackSelectable(scheme.name.c_str(),
                                                           selected) ) {
                            projectScheme = scheme.name;
                            changed       = true;
                        }
                        if ( selected ) ImGui::SetItemDefaultFocus();
                    }
                    // 列表为空时仍保留继承与皮肤默认两个固定入口。
                    // FeedbackBeginCombo 成功后必须由配对入口结束弹窗作用域。
                    ::MMM::UI::FeedbackEndCombo();
                }
            });
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.project.auto_backup").data(), true) ) {
        // “自动备份”默认展开，先提供继承开关，再按实际 optional 展示详情。
        // 本帧切换开关时，详情仍反映进入本帧时的 optional，下一帧完成切换。
        // 覆盖开关只改变本地布尔值，函数末尾再创建或清除 optional。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.project.auto_backup.override").data(),
            labelW,
            [&](Clay_BoundingBox, bool) {
                // 复选框编辑的是局部快照，避免在布局回调中使引用失效。
                if ( ::MMM::UI::FeedbackCheckbox(
                         "##ProjectAutoBackupOverride",
                         &useProjectAutoBackupConfig) ) {
                    autoBackupOverrideToggleChanged = true;
                    // 标记工程设置改变，但此刻不访问尚未建立的 optional 值。
                    changed = true;
                }
            });

        if ( !project->m_settings.m_autoBackupOverride ) {
            // 继承状态展示说明文本，不复制全局具体模式到不可编辑控件。
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.mode").data(),
                labelW,
                [&](Clay_BoundingBox r, bool) {
                    // 提示文本明确说明值来自软件设置，而不是项目功能被禁用。
                    const auto hint = TR_CACHE(
                        "ui.settings.project.auto_backup.inherit_hint");
                    // TextWrapped 以当前值列可用宽度换行，适配窄设置窗口。
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    ImGui::TextWrapped("%s", hint.data());
                },
                false,
                false);
        } else {
            // optional 存在时引用在本帧内稳定，后续控件直接编辑项目覆盖草稿。
            auto& backup = *project->m_settings.m_autoBackupOverride;
            // 模式决定显示定时参数、事件触发开关或仅禁用状态。
            // 枚举以整数引用传给通用 helper，选项值必须与枚举常量一致。
            addRadioSetting(
                *sec,
                rowIndex,
                sectionIndex,
                TR_CACHE("ui.settings.software.auto_backup.mode").data(),
                labelW,
                { { TR_CACHE("ui.settings.software.auto_backup.mode.disabled")
                        .data(),
                    (int)Config::AutoSaveMode::Disabled },
                  { TR_CACHE("ui.settings.software.auto_backup.mode.timed")
                        .data(),
                    (int)Config::AutoSaveMode::Timed },
                  { TR_CACHE("ui.settings.software.auto_backup.mode.event")
                        .data(),
                    (int)Config::AutoSaveMode::EventTriggered } },
                (int&)backup.mode,
                changed,
                false);
            // 通用单选行通过 changed 引用汇总交互结果，不在 helper 内保存工程。

            // 两种模式参数互斥展示，隐藏参数仍保留以便来回切换不丢配置。
            if ( backup.mode == Config::AutoSaveMode::Timed ) {
                // 定时模式先选择秒/分钟单位，再编辑限定范围内的间隔值。
                // 时间单位是配置语义的一部分，不在 UI 中换算 intervalValue。
                addRadioSetting(
                    *sec,
                    rowIndex,
                    sectionIndex,
                    TR_CACHE("ui.settings.software.auto_backup.interval_unit")
                        .data(),
                    labelW,
                    { { TR_CACHE("ui.settings.software.auto_backup.interval_"
                                 "unit.seconds")
                            .data(),
                        (int)Config::AutoSaveIntervalUnit::Seconds },
                      { TR_CACHE("ui.settings.software.auto_backup.interval_"
                                 "unit.minutes")
                            .data(),
                        (int)Config::AutoSaveIntervalUnit::Minutes } },
                    (int&)backup.intervalUnit,
                    changed,
                    false);
                // 间隔值只在定时模式出现，并与单位共同解释。
                addSettingItem(
                    *sec,
                    rowIndex,
                    TR_CACHE("ui.settings.software.auto_backup.interval")
                        .data(),
                    labelW,
                    [&](Clay_BoundingBox r, bool) {
                        // 滑块范围 5 至 60，由单位决定实际时间尺度。
                        // Slider 使用完整值列宽度，范围由产品约束固定。
                        ImGui::SetNextItemWidth(r.width);
                        changed |= ::MMM::UI::FeedbackSliderInt(
                            "##ProjectAutoBackupInterval",
                            &backup.intervalValue,
                            5,
                            60);
                    },
                    false,
                    false);
                // `false, false` 保持该行采用设置页的紧凑布局和普通交互状态。
            } else if ( backup.mode == Config::AutoSaveMode::EventTriggered ) {
                // 事件模式分别控制对象修改、换谱、ImGui 失焦和原生窗口失焦。
                // 各触发器彼此独立，可以同时启用；UI 不强制至少选中一个。
                // 触发条件由备份服务解释，本页只负责持久化布尔配置。
                addSettingItem(
                    *sec,
                    rowIndex,
                    TR_CACHE(
                        "ui.settings.software.auto_backup.on_object_modified")
                        .data(),
                    labelW,
                    [&](Clay_BoundingBox, bool) {
                        // 对象修改覆盖谱面内容发生变化的事件源。
                        changed |= ::MMM::UI::FeedbackCheckbox(
                            "##ProjectAutoBackupObjectModified",
                            &backup.onObjectModified);
                    },
                    false,
                    false);
                addSettingItem(
                    *sec,
                    rowIndex,
                    TR_CACHE(
                        "ui.settings.software.auto_backup.on_beatmap_switch")
                        .data(),
                    labelW,
                    [&](Clay_BoundingBox, bool) {
                        // 换谱事件用于离开当前谱面前保留最新状态。
                        changed |= ::MMM::UI::FeedbackCheckbox(
                            "##ProjectAutoBackupBeatmapSwitch",
                            &backup.onBeatmapSwitch);
                    },
                    false,
                    false);
                addSettingItem(
                    *sec,
                    rowIndex,
                    TR_CACHE("ui.settings.software.auto_backup.on_imgui_focus_"
                             "lost")
                        .data(),
                    labelW,
                    [&](Clay_BoundingBox, bool) {
                        // ImGui 窗口失焦与原生窗口失焦是不同层级的事件。
                        changed |= ::MMM::UI::FeedbackCheckbox(
                            "##ProjectAutoBackupImGuiFocusLost",
                            &backup.onImGuiWindowFocusLost);
                    },
                    false,
                    false);
                addSettingItem(
                    *sec,
                    rowIndex,
                    TR_CACHE("ui.settings.software.auto_backup.on_native_focus_"
                             "lost")
                        .data(),
                    labelW,
                    [&](Clay_BoundingBox, bool) {
                        // 原生焦点事件覆盖用户切换到其他桌面应用的情况。
                        changed |= ::MMM::UI::FeedbackCheckbox(
                            "##ProjectAutoBackupNativeFocusLost",
                            &backup.onNativeWindowFocusLost);
                    },
                    false,
                    false);
            }

            if ( backup.mode != Config::AutoSaveMode::Disabled ) {
                // 只有会产生备份的模式才展示保留数量上限。
                // 禁用时保留已有数量值，重新启用后继续使用先前配置。
                addSettingItem(
                    *sec,
                    rowIndex,
                    TR_CACHE("ui.settings.software.auto_backup.max_count")
                        .data(),
                    labelW,
                    [&](Clay_BoundingBox r, bool) {
                        // 上下限由 Config 公共常量维护，避免 UI
                        // 与服务约束漂移。
                        ImGui::SetNextItemWidth(r.width);
                        changed |= ::MMM::UI::FeedbackSliderInt(
                            "##ProjectAutoBackupMaxCount",
                            &backup.maxBackupCount,
                            Config::AUTO_BACKUP_COUNT_MIN,
                            Config::AUTO_BACKUP_COUNT_MAX);
                    },
                    false,
                    false);
            }
        }
    }

    // 渲染完整 Clay 树并推进 ImGui 游标，维持设置窗口滚动范围。
    // renderInCurrent 返回实际布局尺寸，外层 ImGui 需要知道占用的纵向空间。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    // 可用宽度传给根布局，高度设为自动计算以容纳当前展开的所有 section。
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( autoBackupOverrideToggleChanged ) {
        // optional 的结构变更延迟到所有捕获其成员的回调执行完毕之后。
        // 这样不会在同一帧使 backup 引用或已登记的控件回调失效。
        if ( useProjectAutoBackupConfig ) {
            // 开启覆盖时复制当前全局配置，提供与原行为一致的编辑起点。
            project->m_settings.m_autoBackupOverride =
                Config::AppConfig::instance().getEditorSettings().autoBackup;
        } else {
            // 关闭覆盖清空 optional，项目后续实时继承全局配置。
            project->m_settings.m_autoBackupOverride.reset();
        }
        // 此处只改变数据模型；下一帧根据新 optional 值重建对应控件集合。
    }

    if ( changed ) {
        // 先同步引擎当前项目备份策略，再统一保存项目配置和资源清单。
        // 调色板和备份修改共享一次保存，避免一次交互造成多次磁盘写入。
        // 折叠、滚动和普通悬浮不会设置 changed，因此不会触发保存。
        engine.setProjectAutoBackupOverride(
            project->m_settings.m_autoBackupOverride);
        engine.saveProject();
    }
}

}  // namespace MMM::UI
