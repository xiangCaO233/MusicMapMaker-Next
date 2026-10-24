// 预设切换构造播放速度等具体命令，完整变体只在此调用端实例化。
#include "ui/imgui/audio/AudioPresetControls.h"
#include "audio/AudioManager.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/AudioPresets.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
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
    auto& app     = Config::AppConfig::instance();
    auto& library = app.getAudioPresets();
    // 控件使用固定 ID，重命名不会导致焦点丢失或弹层重新创建。
    if ( FeedbackButton("音频预设") ) ImGui::OpenPopup("AudioPresetSettings");
    ImGui::SameLine();
    ImGui::TextUnformatted(library.m_selected.c_str());
    // 限制弹层尺寸，十五段 EQ 展开后使用滚动而不是越过屏幕边界。
    ImGui::SetNextWindowSizeConstraints(
        { 400, 0 }, { 650, ImGui::GetIO().DisplaySize.y * 0.85F });
    if ( !ImGui::BeginPopup("AudioPresetSettings") ) return;
    ImGui::TextUnformatted("个人音频预设（自动保存）");
    ImGui::TextWrapped(
        "打开项目沿用当前方案。调整只影响当前方案；谱面采样与物件本身的音量仍属"
        "于创作数据。");
    // 选择项使用库中名字作为身份，删除前项不会导致序号错配。
    // 每次选中先捕获旧方案，再把目标方案完整应用到音频图。
    if ( ImGui::BeginCombo("当前预设", library.m_selected.c_str()) ) {
        for ( const auto& item : library.m_items ) {
            if ( ImGui::Selectable(item.m_name.c_str(),
                                   item.m_name == library.m_selected) ) {
                // select 不改变容器大小，迭代中的记录引用保持有效。
                library.select(item.m_name, app.getEditorSettings());
                applySelection();
            }
        }
        ImGui::EndCombo();
    }
    // 名称草稿仅存在于当前客户端窗口，不属于预设内容。
    // 草稿容量固定，逐帧输入不会为字符缓冲反复扩容。
    // 不自动使用当前名字，避免复制时误以为正在覆盖已有预设。
    static std::array<char, 128> name{};
    static bool                  invalidName = false;
    ImGui::InputText("名称", name.data(), name.size());
    // 复制包含 EQ 与拉伸参数，适合从当前耳机方案调整出外放方案。
    // 深拷贝确保后续拖动副本不会改变原方案。
    if ( FeedbackButton("复制为新预设") ) {
        invalidName = !library.duplicate(name.data(), app.getEditorSettings());
        if ( !invalidName ) app.save();
    }
    ImGui::SameLine();
    // 重命名只改变库内身份，音频图保持原状态，无需重新应用。
    // 错误提示保留至下一次命名操作成功，输入内容不会丢弃。
    if ( FeedbackButton("重命名") ) {
        invalidName = !library.rename(name.data());
        if ( !invalidName ) app.save();
    }
    ImGui::SameLine();
    // 库不能为空，最后一项仍允许改名和编辑。
    // 删除成功立即恢复剩余项，不能只让下拉框换名字而声音未变化。
    ImGui::BeginDisabled(library.m_items.size() <= 1);
    if ( FeedbackButton("删除当前预设") &&
         library.remove(app.getEditorSettings()) )
        applySelection();
    ImGui::EndDisabled();
    if ( invalidName ) ImGui::TextUnformatted("名称不能为空或与已有预设重复。");
    ImGui::Separator();
    // 混音界面直接编辑同一份 AppConfig；其他面板提交时有覆盖保护。
    // 全部修改标志合并到本帧末尾一次发布，避免反复更新音频图。
    auto& settings   = app.getEditorSettings();
    bool  mixChanged = false;
    // 总线与分类共用同一百分数控件，整数输入不暴露内部倍率。
    /// @brief 绘制一条总线的音量和独立静音，合并配置修改标志。
    /// @warning 每帧调用，只借用字段，不保存文件或触发音频图重建。
    const auto mix = [&](const char* label, float& gain, bool& muted) {
        // 标签作为 ID 作用域，使各行同名静音控件互不干扰。
        ImGui::PushID(label);
        mixChanged |= FeedbackVolumeSlider(label, &gain);
        ImGui::SameLine();
        mixChanged |= ImGui::Checkbox("静音", &muted);
        ImGui::PopID();
    };
    mix("总音量", settings.globalVolume, settings.globalMuted);
    mix("音乐", settings.bgmGain, settings.bgmGainMuted);
    mix("效果音", settings.sfxGain, settings.sfxGainMuted);
    mix("界面音效",
        settings.interactionSfxGain,
        settings.interactionSfxGainMuted);
    // Key 与节拍器原有字段为启用语义，转换仅发生在 UI 边界。
    /// @brief 适配旧配置的 enable 语义，保持静音按钮外观一致。
    /// @warning 每帧调用；转换使用局部布尔量，不改动非音量字段。
    const auto enabledMix = [&](const char* label, float& gain, bool& enabled) {
        bool muted = !enabled;
        mix(label, gain, muted);
        enabled = !muted;
    };
    enabledMix("无绑定 Key 音",
               settings.sfxConfig.unboundHitSfxGain,
               settings.sfxConfig.enableUnboundHitSfx);
    enabledMix("绑定 Key 音",
               settings.sfxConfig.boundHitSfxGain,
               settings.sfxConfig.enableBoundHitSfx);
    enabledMix("节拍器",
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
    if ( ImGui::Combo("均衡器", &preset.m_eqPreset, "关闭\0十段\0十五段\0") ) {
        const int count =
            preset.m_eqPreset == 1 ? 10 : (preset.m_eqPreset == 2 ? 15 : 0);
        preset.m_eqGains.assign(count, 0.0F);
        preset.m_eqQs.assign(count, 1.414F);
        // 布局变化即使增益仍为零也需要发布，实际节点段数必须同步。
        effectChanged = true;
    }
    // 数组仅在切换 EQ 类型时重建；常态绘制不分配 DSP 数据。
    // 展开时才绘制详细参数，常用混音控件始终放在上方。
    if ( ImGui::CollapsingHeader("均衡器频段") )
        for ( size_t i = 0; i < preset.m_eqGains.size(); ++i ) {
            // 每个频段独立作用域，多个增益滑块不会共享激活状态。
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("频段 %d", static_cast<int>(i + 1));
            effectChanged |= FeedbackSliderFloat(
                "增益 (dB)", &preset.m_eqGains[i], -24, 24, "%.1f");
            effectChanged |=
                FeedbackSliderFloat("Q", &preset.m_eqQs[i], 0.1F, 10, "%.2f");
            ImGui::PopID();
        }
    // UI 使用 float 草稿，持久格式仍保留 double 的速度和半音值。
    // 保存只发生在用户改变参数时，空闲显示不降低旧数据精度。
    float speed = static_cast<float>(preset.m_speed);
    float pitch = static_cast<float>(preset.m_pitch);
    effectChanged |= FeedbackSliderFloat("播放速度", &speed, 0.1F, 4, "%.2fx");
    effectChanged |=
        FeedbackSliderFloat("音高 (半音)", &pitch, -24, 24, "%.1f");
    effectChanged |=
        ImGui::Combo("拉伸质量", &preset.m_quality, "快速\0平衡\0精细\0最佳\0");
    // 效果器参数与音量使用同一方案身份，不更新项目的资源缓存。
    // 既有主音轨控制面板也写入此项，两个入口展示的是同一状态。
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
