// 预设切换构造播放速度等具体命令，完整变体只在此调用端实例化。
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
#include "ui/utils/UIWidgetUtils.h"
#include <array>
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 提交客户端方案并通知键音、节拍器等读取引擎配置的消费者。
/// @warning 用户操作入口，会调整音频图与保存配置，不在空闲帧执行。
/// @details 顺序先音频、后引擎配置、最后磁盘，保证后续面板读到相同方案。
/// Engine 的软件级字段保护允许这里发布全局音量而不被旧快照覆盖。
/// 调用方已完成名称和范围校验，此处不进行文件导入或项目修改。
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
}  // namespace
/// @brief 将个人听音方案管理集中在一个弹层，避免资源列表堆叠控件。
/// @warning 每帧绘制仅借用配置；重建路由和保存仅由明确编辑事件触发。
/// @details 此入口既负责预设管理，也提供没有活动谱面时的听音调整。
/// 资源管理表仍负责创作数据，两者不共用项目持久化命令。
/// 弹层使用稳定 ID，标题和当前方案名称不参与窗口身份。
/// 改动标志区分混音与效果器，空闲帧只展示不会重新发布音频图。
void drawAudioPresetControls()
{
    // 库属于客户端，界面只保存名称草稿，不创建第二份可变配置。
    // 未打开弹层时只显示所选名称，避免每帧复制整套 SFX 映射。
    // 本入口由桌面侧栏和移动抽屉共用，文案必须从同一翻译器读取。
    // 翻译缓存按语言版本失效，不把首次显示的中文保存到静态标签数组。
    // 音频分类与质量选项复用原有翻译键，使皮肤覆写在各入口含义一致。
    auto& app     = Config::AppConfig::instance();
    auto& library = app.getAudioPresets();
    // 控件使用固定 ID，重命名不会导致焦点丢失或弹层重新创建。
    if ( FeedbackButton(TR_CACHE("ui.audio_presets.button").data()) )
        ImGui::OpenPopup("AudioPresetSettings");
    ImGui::SameLine();
    // 旧版默认项只转换显示名，用户命名和磁盘选择身份不随语言改写。
    ImGui::TextUnformatted(library.m_selected == "默认"
                               ? TR_CACHE("ui.audio_presets.default").data()
                               : library.m_selected.c_str());
    // 限制弹层尺寸，十五段 EQ 展开后使用滚动而不是越过屏幕边界。
    ImGui::SetNextWindowSizeConstraints(
        { 400, 0 }, { 650, ImGui::GetIO().DisplaySize.y * 0.85F });
    if ( !ImGui::BeginPopup("AudioPresetSettings") ) return;
    // 弹层内部 ID 不随语言变化，保留已打开状态与已有滚动位置。
    // 说明经格式占位转发，翻译中的百分号不能作为 ImGui 格式说明符。
    ImGui::TextUnformatted(TR_CACHE("ui.audio_presets.title").data());
    ImGui::TextWrapped("%s", TR_CACHE("ui.audio_presets.description").data());
    // 选择项使用库中名字作为身份，删除前项不会导致序号错配。
    // 每次选中先捕获旧方案，再把目标方案完整应用到音频图。
    if ( FeedbackBeginCombo(TR_CACHE("ui.audio_presets.current").data(),
                            library.m_selected == "默认"
                                ? TR_CACHE("ui.audio_presets.default").data()
                                : library.m_selected.c_str()) ) {
        for ( const auto& item : library.m_items ) {
            // 显示别名不能用于库项查找，磁盘与选择动作仍以用户原名称匹配。
            // 列表同时提供实际名称作用域，译文变化不造成不同项身份冲突。
            ImGui::PushID(item.m_name.c_str());
            if ( FeedbackSelectable(
                     item.m_name == "默认"
                         ? TR_CACHE("ui.audio_presets.default").data()
                         : item.m_name.c_str(),
                     item.m_name == library.m_selected) ) {
                // select 不改变容器大小，迭代中的记录引用保持有效。
                library.select(item.m_name, app.getEditorSettings());
                applySelection();
            }
            ImGui::PopID();
        }
        FeedbackEndCombo();
    }
    // 名称草稿仅存在于当前客户端窗口，不属于预设内容。
    // 草稿容量固定，逐帧输入不会为字符缓冲反复扩容。
    // 不自动使用当前名字，避免复制时误以为正在覆盖已有预设。
    static std::array<char, 128> name{};
    static bool                  invalidName = false;
    ImGui::InputText(
        TR_CACHE("ui.audio_presets.name").data(), name.data(), name.size());
    // 复制包含 EQ 与拉伸参数，适合从当前耳机方案调整出外放方案。
    // 深拷贝确保后续拖动副本不会改变原方案。
    if ( FeedbackButton(TR_CACHE("ui.audio_presets.duplicate").data()) ) {
        invalidName = !library.duplicate(name.data(), app.getEditorSettings());
        if ( !invalidName ) app.save();
    }
    ImGui::SameLine();
    // 重命名只改变库内身份，音频图保持原状态，无需重新应用。
    // 错误提示保留至下一次命名操作成功，输入内容不会丢弃。
    // 重命名失败时不能替换当前项或重建播放路由，名称草稿继续保留。
    // 用户输入不参与翻译，中文方案名在英文界面中仍属于用户内容。
    if ( FeedbackButton(TR_CACHE("ui.audio_presets.rename").data()) ) {
        invalidName = !library.rename(name.data());
        if ( !invalidName ) app.save();
    }
    ImGui::SameLine();
    // 库不能为空，最后一项仍允许改名和编辑。
    // 删除成功立即恢复剩余项，不能只让下拉框换名字而声音未变化。
    // current() 要求库非空，因此最后一项可以编辑与重命名，但不能删除。
    ImGui::BeginDisabled(library.m_items.size() <= 1);
    if ( FeedbackButton(TR_CACHE("ui.audio_presets.delete").data()) &&
         library.remove(app.getEditorSettings()) )
        applySelection();
    ImGui::EndDisabled();
    // 失败文案按当前语言读取，不缓存第一次操作时的提示字符串。
    if ( invalidName )
        ImGui::TextUnformatted(
            TR_CACHE("ui.audio_presets.invalid_name").data());
    ImGui::Separator();
    // 混音界面直接编辑同一份 AppConfig；其他面板提交时有覆盖保护。
    // 全部修改标志合并到本帧末尾一次发布，避免反复更新音频图。
    auto& settings   = app.getEditorSettings();
    bool  mixChanged = false;
    // 总线与分类共用同一百分数控件，整数输入不暴露内部倍率。
    /// @brief 绘制一条总线的音量和独立静音，合并配置修改标志。
    /// @warning 每帧调用，只借用字段，不保存文件或触发音频图重建。
    const auto mix = [&](const char* label, float& gain, bool& muted) {
        // 所有类别沿用线性倍率，语言变化不能量化尚未编辑的原始精度。
        // 静音与倍率独立，取消静音后仍恢复先前调节的位置。
        // 标签作为 ID 作用域，使各行同名静音控件互不干扰。
        ImGui::PushID(label);
        mixChanged |= FeedbackVolumeSlider(label, &gain);
        ImGui::SameLine();
        mixChanged |=
            ImGui::Checkbox(TR_CACHE("ui.audio_manager.mute").data(), &muted);
        ImGui::PopID();
    };
    mix(TR_CACHE("ui.audio_manager.global_volume").data(),
        settings.globalVolume,
        settings.globalMuted);
    mix(TR_CACHE("ui.audio_presets.music").data(),
        settings.bgmGain,
        settings.bgmGainMuted);
    mix(TR_CACHE("ui.audio_presets.effects").data(),
        settings.sfxGain,
        settings.sfxGainMuted);
    mix(TR_CACHE("ui.audio_manager.interaction_sfx_volume").data(),
        settings.interactionSfxGain,
        settings.interactionSfxGainMuted);
    // Key 与节拍器原有字段为启用语义，转换仅发生在 UI 边界。
    /// @brief 适配旧配置的 enable 语义，保持静音按钮外观一致。
    /// @warning 每帧调用；转换使用局部布尔量，不改动非音量字段。
    const auto enabledMix = [&](const char* label, float& gain, bool& enabled) {
        // 只翻转界面布尔草稿，底层 enable 字段的持久化含义保持不变。
        bool muted = !enabled;
        mix(label, gain, muted);
        enabled = !muted;
    };
    enabledMix(TR_CACHE("ui.audio_presets.unbound_key").data(),
               settings.sfxConfig.unboundHitSfxGain,
               settings.sfxConfig.enableUnboundHitSfx);
    enabledMix(TR_CACHE("ui.audio_presets.bound_key").data(),
               settings.sfxConfig.boundHitSfxGain,
               settings.sfxConfig.enableBoundHitSfx);
    enabledMix(TR_CACHE("ui.audio_presets.metronome").data(),
               settings.sfxConfig.editorMetronomeGain,
               settings.sfxConfig.enableEditorMetronome);
    // 先捕获再应用，运行时和库内当前项在本次交互结束时保持一致。
    // 其他方案没有参与本次引用，不会被音量调节联动修改。
    // 这一步同时更新节拍器配置消费者，不能只改 AudioManager 的总线。
    if ( mixChanged ) {
        Config::captureAudioPreset(library.current(), settings);
        applySelection();
    }
    ImGui::Separator();
    // 所有容器大小变化已经结束，现在取得引用不会被复制/删除操作失效。
    auto& preset = library.current();
    // 当前项的数组与标量在同一编辑边沿提交，避免存储不完整效果器状态。
    // 主轨控制器和此弹层读取相同字段，两个入口不维护独立预设副本。
    // 没有参数变化时只展示数值，不执行磁盘保存或重新发布滤波器。
    // 选择别的方案后这里重新取得引用，不复用旧方案的字段地址。
    bool effectChanged = false;
    // EQ 与拉伸保留物理单位，不把 dB、Q 和半音误显示为百分数。
    // 切换频段布局不按数组索引搬运旧增益，不同布局的中心频率不同。
    // 新布局以平直曲线开始，防止用户切换类型后得到意外峰值。
    // 翻译顺序必须对应原有 DSP 布局序号，不能按译文重新排序。
    // 未改变选项时不重建频段数组，避免语言切换本身清空已调整的增益。
    const char* eqItems[]{ TR_CACHE("ui.audio_manager.eq_none").data(),
                           TR_CACHE("ui.audio_manager.eq_10_band").data(),
                           TR_CACHE("ui.audio_manager.eq_15_band").data() };
    if ( FeedbackCombo(TR_CACHE("ui.audio_manager.eq_control").data(),
                       &preset.m_eqPreset,
                       eqItems,
                       3) ) {
        const int count =
            preset.m_eqPreset == 1 ? 10 : (preset.m_eqPreset == 2 ? 15 : 0);
        preset.m_eqGains.assign(count, 0.0F);
        preset.m_eqQs.assign(count, 1.414F);
        // 布局变化即使增益仍为零也需要发布，实际节点段数必须同步。
        effectChanged = true;
    }
    // 数组仅在切换 EQ 类型时重建；常态绘制不分配 DSP 数据。
    // 展开时才绘制详细参数，常用混音控件始终放在上方。
    if ( ImGui::CollapsingHeader(TR_CACHE("ui.audio_manager.bands").data()) )
        for ( size_t i = 0; i < preset.m_eqGains.size(); ++i ) {
            // 每个频段独立作用域，多个增益滑块不会共享激活状态。
            // 显示频段从一开始，数组仍从零索引，模板仅接收显示编号。
            // Q 是通用物理符号，不另做单位转换或音频参数映射。
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(
                TR_FMT("ui.audio_presets.band_fmt", i + 1).c_str());
            effectChanged |=
                FeedbackSliderFloat(TR_CACHE("ui.audio_manager.gain_db").data(),
                                    &preset.m_eqGains[i],
                                    -24,
                                    24,
                                    "%.1f");
            effectChanged |=
                FeedbackSliderFloat("Q", &preset.m_eqQs[i], 0.1F, 10, "%.2f");
            ImGui::PopID();
        }
    // UI 使用 float 草稿，持久格式仍保留 double 的速度和半音值。
    // 保存只发生在用户改变参数时，空闲显示不降低旧数据精度。
    float speed = static_cast<float>(preset.m_speed);
    float pitch = static_cast<float>(preset.m_pitch);
    effectChanged |=
        FeedbackSliderFloat(TR_CACHE("ui.audio_manager.speed_value").data(),
                            &speed,
                            0.1F,
                            4,
                            "%.2fx");
    effectChanged |= FeedbackSliderFloat(
        TR_CACHE("ui.audio_presets.pitch").data(), &pitch, -24, 24, "%.1f");
    // 选项数组只借用翻译器稳定指针，不复制文本，也不修改质量序号。
    const char* qualityItems[]{
        TR_CACHE("ui.audio_manager.quality_fast").data(),
        TR_CACHE("ui.audio_manager.quality_balanced").data(),
        TR_CACHE("ui.audio_manager.quality_finer").data(),
        TR_CACHE("ui.audio_manager.quality_best").data()
    };
    effectChanged |=
        FeedbackCombo(TR_CACHE("ui.audio_manager.stretch_quality").data(),
                      &preset.m_quality,
                      qualityItems,
                      4);
    // 效果器参数与音量使用同一方案身份，不更新项目的资源缓存。
    // 既有主音轨控制面板也写入此项，两个入口展示的是同一状态。
    // 译文更新本身不算编辑，不能触发参数写回、音频图发布或磁盘保存。
    if ( effectChanged ) {
        preset.m_speed = speed;
        preset.m_pitch = pitch;
        applySelection();
    }
    // Popup 自己管理滚动与焦点，结束后恢复音频资源列表的绘制窗口。
    // 保证所有成功 BeginPopup 的分支成对结束，失败分支已提前返回。
    ImGui::EndPopup();
}
}  // namespace MMM::UI
