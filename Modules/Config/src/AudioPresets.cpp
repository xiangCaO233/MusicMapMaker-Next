#include "config/AudioPresets.h"
#include "config/EditorSettings.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace MMM::Config
{
namespace
{
/// @brief 非有限值和旧版越界值不得进入实际混音。
/// 有限性校验先于范围夹取，NaN 不得传播到音频图。
/// 旧版本允许增益超过一，新控件统一限制为百分之百。
float volume(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
}
/// @brief 将听音分类音量规范化到界面的零到一百范围。
/// @details 不处理 Flick 宽度倍率等算法参数，它们不是直接音量控制。
/// 旧版本允许大于一的分类增益，迁移时同步实际值而不只改变显示。
void normalizeVolumes(SfxConfig& sfx)
{
    // 三类听音控制和常驻池均使用相同的有限线性倍率约束。
    sfx.unboundHitSfxGain   = volume(sfx.unboundHitSfxGain);
    sfx.boundHitSfxGain     = volume(sfx.boundHitSfxGain);
    sfx.editorMetronomeGain = volume(sfx.editorMetronomeGain);
    // 配置迁移属于低频路径，不在实际播放回调中遍历这些映射。
    for ( auto& [key, gain] : sfx.permanentSfxVolumes ) gain = volume(gain);
}
/// @brief 仅接收真实 JSON 数字，避免损坏配置触发类型异常。
/// @param j 当前预设的 JSON 对象，调用方已确认是 object。
/// @param key 需要读取的标量字段。
/// @param fallback 缺失或类型错误时保持兼容的默认值。
/// @param low 物理参数允许的下界。
/// @param high 物理参数允许的上界。
/// @return 经过类型、有限性和范围检查的值。
double number(const nlohmann::json& j, const char* key, double fallback,
              double low, double high)
{
    // find 不创建缺失字段，读取旧配置不会顺便修改输入 JSON。
    const auto it = j.find(key);
    if ( it == j.end() || !it->is_number() ) return fallback;
    // 先确认数字类型再转换，拒绝字符串数字而非隐式猜测。
    const double value = it->get<double>();
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
/// @brief 按类型读取布尔字段，旧配置缺失时使用默认值。
/// @param fallback 旧文件缺少字段时的兼容状态。
/// @return 仅布尔类型参与覆盖，数字和字符串不做隐式转换。
bool flag(const nlohmann::json& j, const char* key, bool fallback)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
}  // namespace
/// @brief 把当前软件混音参数写回指定预设。
/// @param p 被编辑方案，仅此方案发生变化。
/// @param s 当前生效的软件设置。
/// @details 不复制设备选择、编辑器布局等与听音方案无关的字段。
/// EQ 和拉伸直接由专用控件维护，不能被旧 EditorSettings 快照重置。
void captureAudioPreset(AudioPreset& p, const EditorSettings& s)
{
    // 总线和 SFX 已由现有入口更新；只捕获这些值，EQ 由音频管理器维护。
    p.m_volume           = volume(s.globalVolume);
    p.m_muted            = s.globalMuted;
    p.m_music            = volume(s.bgmGain);
    p.m_musicMuted       = s.bgmGainMuted;
    p.m_effect           = volume(s.sfxGain);
    p.m_effectMuted      = s.sfxGainMuted;
    p.m_interaction      = volume(s.interactionSfxGain);
    p.m_interactionMuted = s.interactionSfxGainMuted;
    // 按值复制嵌套 map，方案之间不共享可写音效状态。
    p.m_sfx = s.sfxConfig;
    normalizeVolumes(p.m_sfx);
}
/// @brief 将已保存的听音方案恢复到配置快照。
/// @param p 待恢复的方案，音量仍经过边界检查。
/// @param s 保留非音频字段的目标设置。
/// @details 本函数不操作音频设备；调用方随后统一发布到运行时图。
void applyAudioPreset(const AudioPreset& p, EditorSettings& s)
{
    // 不替换整份 EditorSettings，否则切换耳机预设会改变编辑行为和界面布局。
    s.globalVolume            = volume(p.m_volume);
    s.globalMuted             = p.m_muted;
    s.bgmGain                 = volume(p.m_music);
    s.bgmGainMuted            = p.m_musicMuted;
    s.sfxGain                 = volume(p.m_effect);
    s.sfxGainMuted            = p.m_effectMuted;
    s.interactionSfxGain      = volume(p.m_interaction);
    s.interactionSfxGainMuted = p.m_interactionMuted;
    // 应用完整 SFX 策略，避免只恢复音量却遗留旧方案的静音开关。
    s.sfxConfig = p.m_sfx;
    normalizeVolumes(s.sfxConfig);
}
/// @brief 在旧引擎快照提交前保留直接由混音面板维护的总线。
/// @param target 待写回的局部配置快照。
/// @param source 最新全局配置。
/// @details SFX 本身通过引擎配置接口修改，不在这里无条件覆盖。
/// 因此工具栏正常调整 Key 音量时仍能提交自己的新值。
void preserveAudioMixerSettings(EditorSettings&       target,
                                const EditorSettings& source)
{
    // 音频管理器直接写入的总线配置不能被其他设置面板的旧快照覆盖。
    target.globalVolume            = source.globalVolume;
    target.globalMuted             = source.globalMuted;
    target.bgmGain                 = source.bgmGain;
    target.bgmGainMuted            = source.bgmGainMuted;
    target.sfxGain                 = source.sfxGain;
    target.sfxGainMuted            = source.sfxGainMuted;
    target.interactionSfxGain      = source.interactionSfxGain;
    target.interactionSfxGainMuted = source.interactionSfxGainMuted;
}
/// @brief 查找所选方案，失效名称回退到首项。
/// @return 当前可编辑方案的引用。
/// @pre 库非空；加载和删除入口共同维持此不变量。
/// @warning 低频配置查找，不应在音频回调中访问可变配置库。
AudioPreset& AudioPresets::current()
{
    // 加载时保证非空；按名称定位避免删除前项导致索引指向其他预设。
    const auto it =
        std::find_if(m_items.begin(), m_items.end(), [&](const auto& p) {
            return p.m_name == m_selected;
        });
    return it == m_items.end() ? m_items.front() : *it;
}
/// @brief 在不修改选择的情况下取得当前方案。
/// @return 已选项或首项的只读引用。
/// @pre 库至少有一个记录，调用方不能手动清空公开容器。
/// @warning 仅在配置读取路径调用，不能作为跨线程发布机制。
const AudioPreset& AudioPresets::current() const
{
    const auto it =
        std::find_if(m_items.begin(), m_items.end(), [&](const auto& p) {
            return p.m_name == m_selected;
        });
    return it == m_items.end() ? m_items.front() : *it;
}
/// @brief 保存旧方案的混音草稿并恢复目标方案。
/// @return 名称存在时为真，失败时原状态不变。
/// @details 不插入或删除记录，调用方可在绘制列表期间安全切换。
bool AudioPresets::select(const std::string& name, EditorSettings& settings)
{
    // 无效选择不提交当前草稿，也不改变正在播放的配置。
    auto it = std::find_if(m_items.begin(), m_items.end(), [&](const auto& p) {
        return p.m_name == name;
    });
    if ( it == m_items.end() ) return false;
    captureAudioPreset(current(), settings);
    m_selected = name;
    applyAudioPreset(*it, settings);
    return true;
}
/// @brief 复制当前完整方案并使副本成为活动方案。
/// @return 新名称有效且插入成功时为真。
/// @details 深拷贝 EQ 数组与 SFX 映射，副本修改不影响原项。
bool AudioPresets::duplicate(const std::string& name, EditorSettings& settings)
{
    if ( name.empty() ||
         std::any_of(m_items.begin(), m_items.end(), [&](const auto& p) {
             return p.m_name == name;
         }) )
        return false;
    // 先收取旧项，复制完整效果器参数；插入后不再使用可能失效的 vector 引用。
    captureAudioPreset(current(), settings);
    auto copy   = current();
    copy.m_name = name;
    m_items.push_back(std::move(copy));
    m_selected = name;
    return true;
}
/// @brief 只改变当前方案的名称与选择键。
/// @return 新名称非空且未被使用时为真。
/// @details 效果器参数、混音状态与其他方案均不变。
bool AudioPresets::rename(const std::string& name)
{
    if ( name.empty() ||
         std::any_of(m_items.begin(), m_items.end(), [&](const auto& p) {
             return p.m_name == name;
         }) )
        return false;
    // 先校验唯一性再更新两个名称字段，不产生中间的失效选择。
    current().m_name = name;
    m_selected       = name;
    return true;
}
/// @brief 删除选中方案并恢复剩余首项的混音。
/// @return 删除成功时为真，最后一项禁止删除。
/// @details 调用方负责发布运行时状态及保存删除后的库。
bool AudioPresets::remove(EditorSettings& settings)
{
    // 最后一项始终可编辑，删除操作不会使后续音量调节失去保存目标。
    if ( m_items.size() <= 1 ) return false;
    // 删除与回退在一次低频操作内完成，调用方随后统一保存。
    std::erase_if(m_items,
                  [&](const auto& p) { return p.m_name == m_selected; });
    m_selected = m_items.front().m_name;
    applyAudioPreset(current(), settings);
    return true;
}
/// @brief 输出稳定的客户端预设格式。
/// @param j 输出 JSON，先替换为新的根对象。
/// @param presets 待保存库，调用方已捕获当前活动混音值。
/// @details 各项完整保存，未选中记录不会与当前设置合并。
void to_json(nlohmann::json& j, const AudioPresets& presets)
{
    // 保存完整记录而非差分，使新版本增加默认字段时仍可独立迁移。
    j = { { "selected", presets.m_selected },
          { "items", nlohmann::json::array() } };
    for ( const auto& p : presets.m_items ) {
        // 类型与单位保持稳定，音量显示方式不影响磁盘上的线性倍率。
        j["items"].push_back({ { "name", p.m_name },
                               { "volume", p.m_volume },
                               { "muted", p.m_muted },
                               { "music", p.m_music },
                               { "musicMuted", p.m_musicMuted },
                               { "effect", p.m_effect },
                               { "effectMuted", p.m_effectMuted },
                               { "interaction", p.m_interaction },
                               { "interactionMuted", p.m_interactionMuted },
                               { "sfx", p.m_sfx },
                               { "eqPreset", p.m_eqPreset },
                               { "eqGains", p.m_eqGains },
                               { "eqQs", p.m_eqQs },
                               { "speed", p.m_speed },
                               { "pitch", p.m_pitch },
                               { "quality", p.m_quality } });
    }
}
/// @brief 恢复个人预设库并清洗外部配置数据。
/// @param j 允许传入非对象或缺失字段的历史数据。
/// @param presets 输出库，失败时保留可编辑默认方案。
/// @details 先在局部数组恢复有效记录，最后一次性替换完整库。
/// 未知字段忽略，重复名只保留首项，避免选择的语义不确定。
void from_json(const nlohmann::json& j, AudioPresets& presets)
{
    // 先建立非空默认库，任何提前返回都能继续接受用户编辑。
    presets = {};
    if ( !j.is_object() ) return;
    const auto items = j.find("items");
    if ( items == j.end() || !items->is_array() ) return;
    // 暂存容器与默认库分离，空输入最终保留默认项。
    std::vector<AudioPreset> parsed;
    for ( const auto& item : *items ) {
        // 跳过坏记录和重复名字，保留其他可恢复的预设。
        if ( !item.is_object() || !item.contains("name") ||
             !item["name"].is_string() )
            continue;
        // 每条记录从默认值开始，不能沿用上一条解析出的可选字段。
        AudioPreset p;
        p.m_name = item["name"].get<std::string>();
        if ( p.m_name.empty() ||
             std::any_of(parsed.begin(), parsed.end(), [&](const auto& q) {
                 return q.m_name == p.m_name;
             }) )
            continue;
        p.m_volume           = number(item, "volume", 0.25, 0, 1);
        p.m_muted            = flag(item, "muted", false);
        p.m_music            = number(item, "music", 1, 0, 1);
        p.m_musicMuted       = flag(item, "musicMuted", false);
        p.m_effect           = number(item, "effect", 1, 0, 1);
        p.m_effectMuted      = flag(item, "effectMuted", false);
        p.m_interaction      = number(item, "interaction", 1, 0, 1);
        p.m_interactionMuted = flag(item, "interactionMuted", false);
        // SFX 沿用现有配置迁移规则，缺失字段保留原本默认音效策略。
        if ( item.contains("sfx") && item["sfx"].is_object() )
            p.m_sfx = item["sfx"].get<SfxConfig>();
        // 枚举值只允许三个已知布局，避免把任意整数转成 DSP 类型。
        p.m_eqPreset = static_cast<int>(number(item, "eqPreset", 0, 0, 2));
        p.m_speed    = number(item, "speed", 1, 0.1, 4);
        p.m_pitch    = number(item, "pitch", 0, -24, 24);
        p.m_quality  = static_cast<int>(number(item, "quality", 2, 0, 3));
        // EQ 段数由类型决定，额外或损坏数组成员不能扩大 DSP 图。
        const int count = p.m_eqPreset == 1 ? 10 : (p.m_eqPreset == 2 ? 15 : 0);
        // 按实际 EQ 布局读取有限个元素，不信任外部数组长度。
        for ( int i = 0; i < count; ++i ) {
            /// @brief 按索引恢复一个 EQ 参数，类型错误和缺失均回退。
            /// @return 有限且处于指定物理范围内的值。
            const auto read =
                [&](const char* key, float fallback, float low, float high) {
                    const auto values = item.find(key);
                    if ( values == item.end() || !values->is_array() ||
                         values->size() <= static_cast<size_t>(i) ||
                         !(*values)[i].is_number() )
                        return fallback;
                    // 有限性检查独立于夹取，NaN 不能由 clamp 自动修复。
                    const float value = (*values)[i].get<float>();
                    return std::isfinite(value) ? std::clamp(value, low, high)
                                                : fallback;
                };
            p.m_eqGains.push_back(read("eqGains", 0, -24, 24));
            p.m_eqQs.push_back(read("eqQs", 1, 0.1F, 10));
        }
        // 只有完整校验完的记录才进入有效列表，保持逐项隔离。
        parsed.push_back(std::move(p));
    }
    // 没有可恢复记录时不清空输出，current() 仍有安全的默认目标。
    if ( parsed.empty() ) return;
    // 所有记录校验完成后再发布，外层不会看到解析一半的库。
    presets.m_items = std::move(parsed);
    // 名称仅接受字符串，数值和空对象不会转成不可见的选择键。
    if ( j.contains("selected") && j["selected"].is_string() )
        presets.m_selected = j["selected"].get<std::string>();
    // 将失效选择规范化写回，使后续保存不继续携带悬空名称。
    presets.m_selected = presets.current().m_name;
}
}  // namespace MMM::Config
