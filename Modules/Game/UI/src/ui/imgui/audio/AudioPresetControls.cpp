// 预设切换构造播放命令，布局不持有第二份可变听音配置。
#include "ui/imgui/audio/AudioPresetControls.h"
#include "audio/AudioManager.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/AudioPresets.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/translation/Translation.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <algorithm>
#include <array>
#include <imgui.h>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 界面状态仅保存分类与名称草稿，不复制音量或 DSP 参数。
/// @details 分类编号只属于本 UI，不写入谱面，也不充当预设库索引。
/// 草稿跨开合保留，重新打开时仅把分类恢复为最常用的混音页。
struct PresetUiState {
    /// @brief 三个分类拥有各自滚动位置，切换不会修改当前预设。
    int m_page{ 0 };
    /// @brief 固定容量的用户输入草稿，不参与语言切换。
    std::array<char, 128> m_name{};
    /// @brief 命名失败后保留提示，直到后续命名成功。
    bool m_invalidName{ false };
};

/// @brief 提交客户端方案并通知既有音频与引擎消费者。
/// @details 先音频、再引擎配置、最后保存，保持原有发布顺序。
/// @warning 仅用户操作时执行，不在空闲帧重建音频图或保存配置。
/// 预设选择可能发生在没有谱面的欢迎页，不能要求存在会话后才更新听音参数。
/// 全局配置通知保留在声音参数应用之后，避免其他面板读到一半的新方案。
void applySelection()
{
    auto&      app    = Config::AppConfig::instance();
    auto&      engine = Logic::EditorEngine::instance();
    const bool active = engine.hasActiveBeatmap();
    Audio::AudioManager::instance().applyAudioPreset(!active);
    // 活动会话先用旧倍率收取视觉时钟，再经播放命令更新拉伸器。
    // 没有谱面时直接恢复参数，避免命令被空会话丢弃。
    if ( active ) {
        const auto& preset = app.getAudioPresets().current();
        engine.pushCommand(Logic::CmdSetPlaybackSpeed{ preset.m_speed });
        engine.pushCommand(Logic::CmdSetPlaybackPitch{ preset.m_pitch });
    }
    // 发布相同配置，避免下次工具栏修改沿用切换前的 SFX 快照。
    Logic::EditorEngine::instance().setEditorConfig(app.getEditorConfig());
    app.save();
}

/// @brief 只为旧默认项提供显示别名，用户方案的存取身份保持原名称。
/// @warning 每帧只借用稳定字符串，不修改预设库或创建名称副本。
/// @param name 库项实际名称，调用方必须保证它在当前绘制期间有效。
/// @return 默认项使用翻译器稳定池，其他项直接借用原字符串。
/// @details 显示别名不参与重命名、删除、去重或下一次启动时的选择。
const char* presetDisplayName(const std::string& name)
{
    // 中文默认名称来自旧持久格式，显示转换不能反向改写用户配置。
    return name == "默认" ? TR_CACHE("ui.audio_presets.default").data()
                          : name.c_str();
}

/// @brief 为当前列中的字段提交换行标签和全宽输入区域。
/// @warning 只操作当前 ImGui 布局，不触发业务配置更新。
/// @param label 已由当前语言解析的显示文字，不承担控件 ID。
/// @details 宽度读取必须发生在标签提交之后，确保它属于当前表格列。
/// 调用方紧接着提交一个字段；本函数不为之后的其他控件设置默认宽度。
void presetFieldLabel(const char* label)
{
    // 宽屏表格和窄屏单列使用同一规则，文字不能挤占后续轨道宽度。
    ImGui::TextWrapped("%s", label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
}

/// @brief 在独立管理页执行复制、重命名和删除，日常混音不被管理说明占高。
/// @param app 当前客户端配置，所有修改继续通过既有库接口完成。
/// @param state 仅界面拥有的名称草稿及校验结果。
/// @param rowHeight 操作按钮的当前可触摸高度。
/// @warning 只有明确操作才保存配置；不在逐帧展示中复制预设数据。
void drawPresetManager(Config::AppConfig& app, PresetUiState& state,
                       float rowHeight)
{
    // 库对象由 AppConfig 长期拥有，管理操作只会使内部项引用失效。
    // 因此本页不提前借用 current()，避免复制或删除后继续访问旧项。
    auto& library = app.getAudioPresets();
    // 说明属于管理操作，不在每一条音量上方重复出现。
    // 用户名称保留原文，语言只影响界面提示。
    ImGui::TextWrapped("%s", TR_CACHE("ui.audio_presets.description").data());
    ImGui::Separator();
    presetFieldLabel(TR_CACHE("ui.audio_presets.name").data());
    // 输入框身份固定，当前方案重命名和语言变化都不会重建编辑缓冲。
    // 名称是否为空或重复仍由库接口判断，布局层不复制另一套校验规则。
    ImGui::InputText("##PresetName", state.m_name.data(), state.m_name.size());
    // 复制动作保留原有完整值语义；错误时库内方案不会改变。
    // 触摸图文按钮由工作区策略提供，不在业务中引入平台宏。
    if ( workspaceActionButton(TR_CACHE("ui.audio_presets.duplicate").data(),
                               ICON_MMM_COPY,
                               { -1, rowHeight }) ) {
        state.m_invalidName =
            !library.duplicate(state.m_name.data(), app.getEditorSettings());
        if ( !state.m_invalidName ) app.save();
    }
    // 两项短操作在空间充足时并排，否则各占一行，不能强行挤出面板。
    const float gap         = ImGui::GetStyle().ItemSpacing.x;
    const float scale       = app.getWindowContentScale();
    const char* rename      = TR_CACHE("ui.audio_presets.rename").data();
    const char* remove      = TR_CACHE("ui.audio_presets.delete").data();
    const float buttonWidth = (ImGui::GetContentRegionAvail().x - gap) * .5f;
    const bool paired = buttonWidth >= std::max(ImGui::CalcTextSize(rename).x,
                                                ImGui::CalcTextSize(remove).x) +
                                           56 * scale;
    if ( workspaceActionButton(
             rename, ICON_MMM_PEN, { paired ? buttonWidth : -1, rowHeight }) ) {
        // 重命名只改变身份，不重建播放路由，失败时仍保留输入草稿。
        state.m_invalidName = !library.rename(state.m_name.data());
        if ( !state.m_invalidName ) app.save();
    }
    // 分行时沿用当前列的完整宽度，不能继续使用上一次横排计算的半宽。
    // SameLine 只调整同组操作，不影响下方错误提示或下一类正文。
    if ( paired ) ImGui::SameLine();
    // current() 的前置条件是库非空，最后一个方案不能删除。
    // 删除后沿用原有选择与应用入口，不在界面维护另一份当前项。
    ImGui::BeginDisabled(library.m_items.size() <= 1);
    if ( workspaceActionButton(remove,
                               ICON_MMM_CLOSE,
                               { paired ? buttonWidth : -1, rowHeight }) &&
         library.remove(app.getEditorSettings()) )
        applySelection();
    ImGui::EndDisabled();
    // 校验失败不切换方案，用户可以直接修正草稿而不必重新打开面板。
    // 提示按当前列换行，较长英文不能给正文引入横向滚动范围。
    if ( state.m_invalidName )
        ImGui::TextWrapped("%s",
                           TR_CACHE("ui.audio_presets.invalid_name").data());
}

/// @brief 对齐类别、百分数轨道和静音，窄屏才切换为两行布局。
/// @param settings 当前方案对应的软件混音字段，倍率与静音语义保持不变。
/// @return 仅实际编辑字段时返回真，语言和布局变化不算编辑。
/// @warning 每帧只绘制七个固定类别，不查询设备、不读取文件、不发布音频图。
/// @details 七个类别仍指向同一组软件字段，混音表格不是新的预设数据模型。
/// 滑条保留原有整数百分数及步进规则，静音列不改变倍率的保存精度。
/// 子窗口先扣除滚动条后才计算可用宽度，旋转和字体改变无需重建配置。
bool drawPresetMixer(Config::EditorSettings& settings)
{
    const std::array labels{
        TR_CACHE("ui.audio_manager.global_volume").data(),
        TR_CACHE("ui.audio_presets.music").data(),
        TR_CACHE("ui.audio_presets.effects").data(),
        TR_CACHE("ui.audio_manager.interaction_sfx_volume").data(),
        TR_CACHE("ui.audio_presets.unbound_key").data(),
        TR_CACHE("ui.audio_presets.bound_key").data(),
        TR_CACHE("ui.audio_presets.metronome").data()
    };
    const auto& style      = ImGui::GetStyle();
    float       labelWidth = 0;
    // 按本帧译文和字体判断，而不是把中文字符数当作固定宽度。
    // 列宽不依赖上一次打开时的语言，也不持久化到表格 ini。
    for ( const auto* label : labels )
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(label).x);
    const float muteWidth =
        std::max(
            ImGui::GetFrameHeight(),
            ImGui::CalcTextSize(TR_CACHE("ui.audio_manager.mute").data()).x) +
        2 * style.CellPadding.x;
    // 最小轨道按字号估计，不能让英文名称增长后只剩下百分数本身。
    // 静音列还要容纳表头，不能只按复选框尺寸计算而裁掉 Mute。
    // 列间留白使用当前主题 CellPadding，不另画固定宽度分隔条。
    const bool wide = ImGui::GetContentRegionAvail().x >=
                      labelWidth + 8 * ImGui::GetFontSize() + muteWidth +
                          8 * style.CellPadding.x;
    // 三列只用于可完整容纳名称、轨道与静音的情况，不依赖横向滚动补救。
    // 表头和滑条共用同一表格几何，长类别不会把静音推到相邻列之外。
    // NoSavedSettings 保证旧窗口列宽不覆盖当前语言与字号的重新测量。
    const bool table =
        wide && ImGui::BeginTable("MixerRows",
                                  3,
                                  ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_NoSavedSettings);
    // BeginTable 成功才建立三列；失败时仍由下方的单列路径提供可用控件。
    // 只对已开始的表格调用 EndTable，避免窄屏分支破坏 ImGui 栈。
    if ( table ) {
        // 类别按最长名称固定占位，轨道吸收剩余宽度，静音使用独立方形触点。
        // 表头只出现一次，避免每条音量重复一遍 Mute 挤占轨道。
        ImGui::TableSetupColumn(TR_CACHE("ui.audio_presets.channel").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                labelWidth);
        ImGui::TableSetupColumn(TR_CACHE("ui.audio_manager.volume").data(),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(TR_CACHE("ui.audio_manager.mute").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                muteWidth);
        ImGui::TableHeadersRow();
    }
    // 修改标志用按位合并，前一行已变化也必须继续提交剩余行的控件。
    // 页面浏览、滚动和分类切换都不会经过上层的保存与应用分支。
    bool changed = false;
    /// @brief 只提交一条原有混音字段，不保存配置或调度效果器。
    /// @warning 固定行数的绘制回调；ID 来自字段身份，不来自翻译标签。
    const auto mix = [&](const char* id,
                         const char* label,
                         float&      gain,
                         bool&       muted) {
        // 字段 ID 与数组序号和译文分离，换语言后各行仍对应原激活身份。
        // 同行滑条与静音拥有不同子 ID，拖动不能把点击转交给相邻控件。
        ImGui::PushID(id);
        if ( table ) {
            // 表格由 ImGui 统一计算列边界，滑条宽度始终使用实际当前列。
            // 标签先按框体基线对齐，再让换行文本增加所需高度。
            // 不手工改绘制坐标，因此放大字号后文字和输入轨道仍属于同一行。
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextWrapped("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            changed |= FeedbackVolumeSlider("##Gain", &gain);
            ImGui::TableSetColumnIndex(2);
            // 无重复标签的复选框由同列表头解释；静音状态与倍率始终独立。
            changed |= FeedbackCheckbox("##Muted", &muted);
        } else {
            // 竖屏优先保留滑条宽度；行间由主题间距分隔，不硬编码圆角或色块。
            // 直接显示完整静音文字，离开表头后也不依赖悬浮提示猜测用途。
            ImGui::TextWrapped("%s", label);
            // 右侧静音完整保留复选框、文字和内部间距，左侧轨道拿剩余宽度。
            // 即使极窄窗口也不生成负轨道尺寸；换行标签的高度由 ImGui 计算。
            const float muteItemWidth =
                ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                ImGui::CalcTextSize(TR_CACHE("ui.audio_manager.mute").data()).x;
            ImGui::SetNextItemWidth(std::max(1.0f,
                                             ImGui::GetContentRegionAvail().x -
                                                 muteItemWidth -
                                                 style.ItemSpacing.x));
            changed |= FeedbackVolumeSlider("##Gain", &gain);
            ImGui::SameLine();
            changed |= FeedbackCheckbox(
                TR_CACHE("ui.audio_manager.mute").data(), &muted);
            ImGui::Spacing();
        }
        ImGui::PopID();
    };
    // 以下映射与原界面保持一致，总线与键音类别不相互复制倍率。
    // 接口接收引用，用户拖动会在本帧直接写回原字段，不等待分类关闭。
    mix("Global", labels[0], settings.globalVolume, settings.globalMuted);
    mix("Music", labels[1], settings.bgmGain, settings.bgmGainMuted);
    mix("Effects", labels[2], settings.sfxGain, settings.sfxGainMuted);
    mix("Interaction",
        labels[3],
        settings.interactionSfxGain,
        settings.interactionSfxGainMuted);
    /// @brief 将旧配置的 enable 字段映射为界面静音，转换范围仅限这一行。
    /// @warning 每帧只用布尔草稿，不改变底层字段的持久化含义。
    // 这些字段的历史语义是是否启用，和四个总线的 muted 布尔值相反。
    // 转换放在边界，后续表格绘制不需要感知不同持久格式。
    const auto enabledMix =
        [&](const char* id, const char* label, float& gain, bool& enabled) {
            // 旧 enable 字段以取反草稿显示，不能把勾选静音直接写成
            // enable=true。
            // 只回写同一布尔字段，倍数字段仍由标准音量控件负责修改。
            bool muted = !enabled;
            mix(id, label, gain, muted);
            enabled = !muted;
        };
    enabledMix("Unbound",
               labels[4],
               settings.sfxConfig.unboundHitSfxGain,
               settings.sfxConfig.enableUnboundHitSfx);
    enabledMix("Bound",
               labels[5],
               settings.sfxConfig.boundHitSfxGain,
               settings.sfxConfig.enableBoundHitSfx);
    enabledMix("Metronome",
               labels[6],
               settings.sfxConfig.editorMetronomeGain,
               settings.sfxConfig.enableEditorMetronome);
    if ( table ) ImGui::EndTable();
    // 返回修改标志给上层统一捕获与发布，不在每条音量里反复保存整个配置。
    return changed;
}

/// @brief 将常用效果参数分列呈现，详细 EQ 频段独立展开。
/// @return 只有真实控件编辑才返回真，继续由调用方统一应用与保存。
/// @warning 常态只读取已有参数，频段数组仅在用户选择新 EQ 类型时重建。
/// @param preset 当前库项的独占 UI 借用；调用期间不得增删预设库项。
/// @details 页面只重排相同的四个常用参数，质量选项序号与旧格式保持一致。
/// 一帧内多个参数变化合并返回，由调用方一次提交到音频引擎。
bool drawPresetEffects(Config::AudioPreset& preset)
{
    bool changed = false;
    // 两列仅在每列都具有可读字段宽度时启用，否则四个字段按正常顺序纵排。
    // 断点使用字号而非固定设备型号，用户放大文字时会自动退回单列。
    const bool columns =
        ImGui::GetContentRegionAvail().x > 32 * ImGui::GetFontSize() &&
        ImGui::BeginTable("EffectFields",
                          2,
                          ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_NoSavedSettings);
    /// @brief 移到下一字段列，窄屏自然沿用普通纵向布局。
    // 不在正文外层保存列号；原生表格负责从第二列自动换到下一行。
    // 单列路径没有表格栈，因此不额外调用 NextColumn 或 EndTable。
    /// @param label 已本地化字段名称，隐藏输入 ID 由紧随其后的控件提供。
    /// @warning 只推进当前表格游标，不分配字段列表或改变 DSP 状态。
    const auto field = [&](const char* label) {
        if ( columns ) {
            ImGui::TableNextColumn();
            // 各列使用相同框体基线，右列不能继承左列输入框的基线而单独下移。
            ImGui::AlignTextToFramePadding();
        }
        presetFieldLabel(label);
    };
    // 译文数组只保存稳定指针，既有 EQ 与质量序号不随语言排序改变。
    // 禁用 EQ 对应零个频段；切换新布局时继续按原有规则重建平直增益。
    const char* eqItems[]{ TR_CACHE("ui.audio_manager.eq_none").data(),
                           TR_CACHE("ui.audio_manager.eq_10_band").data(),
                           TR_CACHE("ui.audio_manager.eq_15_band").data() };
    const char* qualityItems[]{
        TR_CACHE("ui.audio_manager.quality_fast").data(),
        TR_CACHE("ui.audio_manager.quality_balanced").data(),
        TR_CACHE("ui.audio_manager.quality_finer").data(),
        TR_CACHE("ui.audio_manager.quality_best").data()
    };
    field(TR_CACHE("ui.audio_manager.eq_control").data());
    if ( FeedbackCombo("##EqPreset", &preset.m_eqPreset, eqItems, 3) ) {
        // 保留原有序号与 DSP 段数映射，不按译文排序，也不转换物理单位。
        const int count =
            preset.m_eqPreset == 1 ? 10 : (preset.m_eqPreset == 2 ? 15 : 0);
        preset.m_eqGains.assign(count, 0.0F);
        preset.m_eqQs.assign(count, 1.414F);
        changed = true;
    }
    field(TR_CACHE("ui.audio_manager.stretch_quality").data());
    changed |= FeedbackCombo("##Quality", &preset.m_quality, qualityItems, 4);
    // UI 草稿使用 float，只有实际改动才回写 double，空闲展示不丢失旧值精度。
    // 先取草稿，再绘制两个字段，避免仅打开效果器页就把 double 截为 float。
    // 发生真实编辑后的回写条件保留旧入口行为，UI 不修改谱面时间轴。
    float speed = static_cast<float>(preset.m_speed);
    float pitch = static_cast<float>(preset.m_pitch);
    field(TR_CACHE("ui.audio_manager.speed_value").data());
    changed |= FeedbackSliderFloat("##Speed", &speed, 0.1F, 4, "%.2fx");
    field(TR_CACHE("ui.audio_presets.pitch").data());
    changed |= FeedbackSliderFloat("##Pitch", &pitch, -24, 24, "%.1f");
    // 常用字段结束后先离开两列表格，频段详情才具有完整正文宽度。
    // 表格开合只影响排布，不把每列的视觉宽度当作参数范围。
    if ( columns ) ImGui::EndTable();
    // 频段不挤占常用参数，两种屏宽都使用同一展开入口。
    // 关闭 EQ 后没有参数可编辑，不提供空白的大段折叠区域。
    if ( !preset.m_eqGains.empty() &&
         ImGui::CollapsingHeader(TR_CACHE("ui.audio_manager.bands").data()) ) {
        // 频段详情属于同一正文滚动区，不让展开十五段 EQ 把关闭按钮卷走。
        // 频段序号作为作用域，多个增益与 Q 控件不能共享交互身份。
        for ( size_t i = 0; i < preset.m_eqGains.size(); ++i ) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(
                TR_FMT("ui.audio_presets.band_fmt", i + 1).c_str());
            presetFieldLabel(TR_CACHE("ui.audio_manager.gain_db").data());
            changed |= FeedbackSliderFloat(
                "##EqGain", &preset.m_eqGains[i], -24, 24, "%.1f");
            presetFieldLabel("Q");
            changed |= FeedbackSliderFloat(
                "##EqQ", &preset.m_eqQs[i], 0.1F, 10, "%.2f");
            ImGui::PopID();
        }
    }
    if ( changed ) {
        // 与原入口一样在整组编辑结束后写回，随后只应用一次音频图。
        preset.m_speed = speed;
        preset.m_pitch = pitch;
    }
    return changed;
}
}  // namespace

/// @brief 绘制方案入口和居中宽面板，固定导航与关闭按钮，仅正文可以滚动。
/// @details 三个分类共享原有配置与应用入口，不创建第二套预设或混音状态。
/// 弹层保留普通 Popup 的外部点击和系统返回关闭行为，窗口身份不依赖译文。
/// @warning 每帧只处理固定 UI；保存与音频图更新仅发生在明确编辑边沿。
void drawAudioPresetControls()
{
    // 本状态只由 UI 线程访问，音频线程仍从既有应用步骤读取正式参数。
    // 重新打开不重置名称草稿或音量，只恢复默认分类和正文起始滚动位置。
    static PresetUiState state;
    auto&                app     = Config::AppConfig::instance();
    auto&                library = app.getAudioPresets();
    if ( FeedbackButton(TR_CACHE("ui.audio_presets.button").data()) ) {
        state.m_page = 0;
        ImGui::OpenPopup("AudioPresetSettings");
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(presetDisplayName(library.m_selected));
    // 弹层不再受音轨抽屉的宽度限制；两轴始终依据当前安全区重新定位。
    // 固定尺寸配合内部滚动，不能靠自动高度把页脚挤出屏幕。
    // 使用主视口的 WorkPos/WorkSize，系统安全区与桌面窗口原点变化都能被包含。
    // 首选上限防止大显示器上面板无限拉长，小屏则先满足视口边界。
    const auto* viewport = ImGui::GetMainViewport();
    const float scale    = app.getWindowContentScale();
    // 外层音轨页脚会临时取消内边距，弹层必须重新使用用户配置的窗口样式。
    // 样式作用域仅包围本面板，关闭或未打开时都自动恢复父窗口状态。
    Utils::CenteredModalPopupScope popupStyle(scale);
    ImGui::SetNextWindowPos(
        { viewport->WorkPos.x + viewport->WorkSize.x * .5f,
          viewport->WorkPos.y + viewport->WorkSize.y * .5f },
        ImGuiCond_Always,
        { .5f, .5f });
    ImGui::SetNextWindowSize(
        { std::min(viewport->WorkSize.x * .94f, 1100 * scale),
          std::min(viewport->WorkSize.y * .94f, 760 * scale) });
    if ( !ImGui::BeginPopup("AudioPresetSettings",
                            ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse) )
        return;
    // 外层窗口只承载固定框架，滚动能力留在正文子窗口中。
    // 不添加原生标题栏关闭按钮，移动端与桌面使用同一个明确关闭入口。
    const bool appearing = ImGui::IsWindowAppearing();
    // 平台差异仅通过外壳能力查询，外接键鼠和桌面路径不依赖 Android 宏。
    // UI 状态继续由同一线程处理，跨线程消费者只看到 applySelection 的结果。
    const bool touch = !workspaceAllowsHoverFeedback();
    // 只扩大当前面板中控件的触摸高度，不覆盖皮肤圆角、颜色或窗口美化参数。
    const auto  padding = ImGui::GetStyle().FramePadding;
    const float rowHeight =
        std::max(ImGui::GetFrameHeight(), touch ? 48 * scale : 0.0f);
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2{
            padding.x,
            std::max(padding.y, (rowHeight - ImGui::GetFontSize()) * .5f) });
    // 标题与自动保存提示常驻，正文滚动时仍能确认正在编辑哪个功能。
    // 当前方案另由组合框显示，标题不承担用户方案名的显示或查找职责。
    ImGui::TextUnformatted(TR_CACHE("ui.audio_presets.button").data());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR_CACHE("ui.audio_presets.auto_save").data());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if ( FeedbackBeginCombo("##CurrentPreset",
                            presetDisplayName(library.m_selected)) ) {
        for ( const auto& item : library.m_items ) {
            // 选项按真实名称取作用域，显示默认别名不会改变库项身份。
            ImGui::PushID(item.m_name.c_str());
            if ( FeedbackSelectable(presetDisplayName(item.m_name),
                                    item.m_name == library.m_selected) ) {
                library.select(item.m_name, app.getEditorSettings());
                applySelection();
            }
            ImGui::PopID();
        }
        FeedbackEndCombo();
    }
    // 三类入口常驻，切到管理页不必先滚过七条音量和全部效果器。
    // 标签在每帧按语言版本更新，数组索引才是分类状态的稳定身份。
    const std::array pages{ TR_CACHE("ui.audio_presets.tab_mixer").data(),
                            TR_CACHE("ui.audio_presets.tab_effects").data(),
                            TR_CACHE("ui.audio_presets.tab_manage").data() };
    const std::array icons{ ICON_MMM_VOLUME_HIGH,
                            ICON_MMM_SLIDERS,
                            ICON_MMM_COG };
    const float      gap    = ImGui::GetStyle().ItemSpacing.x;
    const float tabWidth    = (ImGui::GetContentRegionAvail().x - 2 * gap) / 3;
    float       minTabWidth = 0;
    for ( const auto* label : pages )
        minTabWidth = std::max(minTabWidth,
                               ImGui::CalcTextSize(label).x +
                                   (touch ? 56 * scale : 2 * padding.x));
    // 短边实在无法容纳图文时改为纵排，不能截掉某个分类的入口。
    // 判断包含图标占位，实际按钮不会在提交时再偷偷撑宽最后一列。
    // 横向和纵向导航共享相同页索引，断点变化不会跳到另一类配置。
    // 选中状态使用当前主题激活色，不硬编码亮蓝或固定圆角参数。
    const bool horizontal = tabWidth >= minTabWidth;
    // 分类点击只更新显示状态，不代表选中另一套听音预设。
    // 当前方案始终由上方组合框管理，两个选择维度不能互相覆盖。
    bool pageChanged = false;
    for ( int page = 0; page < 3; ++page ) {
        if ( page && horizontal ) ImGui::SameLine();
        ImGui::PushID(page);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(
                                  state.m_page == page ? ImGuiCol_ButtonActive
                                                       : ImGuiCol_Button));
        if ( workspaceActionButton(
                 pages[page],
                 icons[page],
                 { horizontal ? tabWidth : -1, rowHeight }) ) {
            pageChanged  = state.m_page != page;
            state.m_page = page;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    // 预留固定关闭行后，剩余空间全部交给正文，长内容不带着标题一起滚动。
    // 每个分类拥有独立子窗口身份，新打开和切换分类都从顶部开始。
    // rowHeight 已包含当前正文采用的
    // FramePadding，关闭按钮不会在绘制时额外增高。 同时扣除一次父级间距，使
    // EndChild 的游标推进不会把页脚挤出面板。
    const float bodyHeight =
        std::max(1.0f, ImGui::GetContentRegionAvail().y - rowHeight - gap);
    // 身份中含分类编号，各页滚动和折叠状态不串用同一个子窗口。
    // 重置滚动在 BeginChild 前提交，避免正文先闪出旧页面的尾部一帧。
    ImGui::PushID(state.m_page);
    if ( appearing || pageChanged ) ImGui::SetNextWindowScroll({ 0, 0 });
    if ( ImGui::BeginChild(
             "AudioPresetBody", { 0, bodyHeight }, ImGuiChildFlags_Borders) ) {
        if ( state.m_page == 0 ) {
            if ( drawPresetMixer(app.getEditorSettings()) ) {
                // 混音与旧入口一样先捕获当前项，再统一通知全部消费者。
                Config::captureAudioPreset(library.current(),
                                           app.getEditorSettings());
                applySelection();
            }
        } else if ( state.m_page == 1 ) {
            // 此时预设组合框已完成选择，重新借用当前项，避免沿用切换前引用。
            // 读取字段和滚动本身都不能触发音频图更新。
            if ( drawPresetEffects(library.current()) ) applySelection();
        } else {
            // 管理操作可能使库项地址变化，但本帧之后不再使用旧项字段引用。
            drawPresetManager(app, state, rowHeight);
        }
    }
    // BeginChild 即使不可见也要结束，固定关闭行必须回到弹层父窗口。
    ImGui::EndChild();
    ImGui::PopID();
    // 关闭只结束弹层，所有编辑已沿用原有自动保存入口即时提交。
    // 它不关闭父级音轨抽屉，也不清空当前预设或管理名称草稿。
    if ( workspaceActionButton(TR_CACHE("ui.window.close").data(),
                               ICON_MMM_CLOSE,
                               { -1, rowHeight }) )
        ImGui::CloseCurrentPopup();
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}
}  // namespace MMM::UI
