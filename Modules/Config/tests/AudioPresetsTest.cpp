#include "config/AudioPresets.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace
{
/// @brief 验证预设切换与删除不把其他方案的混音或 DSP 参数覆盖。
/// @return 整个操作链保持方案隔离时为真。
/// @details 测试依次覆盖捕获、复制、切换、重命名、删除的状态转换。
/// 初始混音值刻意不同于默认值，避免只验证对象构造默认值。
/// EQ 增益使用正负相反数，便于发现 vector 未独立拷贝的问题。
/// 最终删除副本后应恢复原方案的音量，而不是继承已删除方案。
bool isolation()
{
    MMM::Config::AudioPresets   presets;
    MMM::Config::EditorSettings settings;
    // 第一项模拟外放，第二项模拟耳机，差值使交叉覆盖可被直接发现。
    settings.globalVolume = 0.35F;
    // 模拟用户已经调好的十段 EQ，复制操作应保留全部频段。
    presets.current().m_eqPreset = 1;
    presets.current().m_eqGains.assign(10, 3.0F);
    presets.current().m_eqQs.assign(10, 1.414F);
    presets.current().m_speed = 0.75;
    // 复制从当前实值捕获，不能从上次保存前的缓存复制。
    if ( !presets.duplicate("耳机", settings) ) return false;
    settings.globalVolume                  = 0.8F;
    settings.sfxConfig.editorMetronomeGain = 0.2F;
    presets.current().m_eqGains[0]         = -6;
    // 离开时收取改动，再次进入必须恢复，包括内嵌的 SFX 选项。
    if ( !presets.select("默认", settings) || settings.globalVolume != 0.35F ||
         presets.current().m_eqGains[0] != 3 )
        return false;
    if ( !presets.select("耳机", settings) || settings.globalVolume != 0.8F ||
         settings.sfxConfig.editorMetronomeGain != 0.2F )
        return false;
    // 重名与空名失败不改变当前方案；最后一项禁止删除。
    if ( presets.rename("默认") || presets.duplicate("", settings) )
        return false;
    // 连续删除验证最后一项保护没有把已经恢复的音量再次重置。
    if ( !presets.rename("外放") || !presets.remove(settings) ||
         presets.remove(settings) )
        return false;
    return settings.globalVolume == 0.35F;
}
/// @brief 模拟旧配置迁移、修改、重启加载，覆盖真实 AppConfig 保存路径。
/// @param root CTest 分配的输出目录，禁止使用真实用户配置目录。
/// @return 旧音量迁移且新选择和效果器均成功恢复时为真。
/// @details 使用真实磁盘往返，覆盖 AppConfig 与预设序列化的集成。
/// 第一次加载故意不带新字段，模拟从旧版本升级。
/// 第二次加载前 reset 清除进程内缓存，避免误把内存值当作持久化结果。
/// 两个方案使用不同音量，检验 save 是否正确捕获所选项。
/// EQ 类型与数组长度同时验证，不能只保存“已开启”标志。
/// 输出文件留在构建目录，失败时可检查实际 JSON 内容。
bool persistence(const std::filesystem::path& root)
{
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if ( ec ) return false;
    const auto file = root / "audio_presets.json";
    auto&      app  = MMM::Config::AppConfig::instance();
    // 记录外层与嵌套字段，防止仅外层指针稳定而子对象被重新分配。
    // 两类预设库也由同一私有状态拥有，必须共享这份生命周期保证。
    // 指针只用于相等比较，失效时不解引用，失败本身不会访问释放内存。
    // 保存各层对象地址，捕获整体替换私有存储导致的引用失效。
    const auto* configAddress   = &app.getEditorConfig();
    const auto* settingsAddress = &app.getEditorSettings();
    const auto* visualAddress   = &app.getVisualConfig();
    const auto* audioAddress    = &app.getAudioPresets();
    const auto* layoutAddress   = &app.getLayoutPresets();
    // 清空单例的旧状态，保证迁移用例从确定的默认起点开始。
    app.reset();
    auto legacy                               = app.getEditorConfig();
    legacy.settings.globalVolume              = 0.65F;
    legacy.settings.sfxConfig.boundHitSfxGain = 1.5F;
    // 写入没有 audioPresets 的历史格式，证明迁移不重置用户已有音量。
    {
        std::ofstream out(file);
        out << nlohmann::json(legacy);
    }
    if ( !app.load(file) || app.getAudioPresets().current().m_volume != 0.65F )
        return false;
    // 接下来所有修改都针对新副本，原方案作为恢复基线保留。
    // 不调用 select 前的显式捕获，复制函数必须自行收取最新混音。
    // 两次 load 间清空内存，因此成功来自磁盘保存而非单例残留。
    // 旧的放大增益迁移为新控件上界，实际播放值与显示保持一致。
    if ( app.getEditorSettings().sfxConfig.boundHitSfxGain != 1.0F )
        return false;
    auto& presets = app.getAudioPresets();
    if ( !presets.duplicate("耳机", app.getEditorSettings()) ) return false;
    // 仅改活动设置，故意不手动捕获，验证保存入口确实负责收取。
    app.getEditorSettings().globalVolume = 0.4F;
    presets.current().m_eqPreset         = 2;
    presets.current().m_eqGains.assign(15, -2);
    presets.current().m_eqQs.assign(15, 2);
    presets.current().m_pitch   = -5;
    presets.current().m_quality = 3;
    // 保存必须收取实时混音字段，而非预设中尚未刷新的旧缓存。
    if ( !app.save(file) ) return false;
    app.reset();
    if ( !app.load(file) ) return false;
    // 加载和重置只能更新既有对象内容；UI 缓存的引用必须继续指向这些对象。
    if ( configAddress != &app.getEditorConfig() ||
         settingsAddress != &app.getEditorSettings() ||
         visualAddress != &app.getVisualConfig() ||
         audioAddress != &app.getAudioPresets() ||
         layoutAddress != &app.getLayoutPresets() )
        return false;
    // 重置和读取均在显式临时文件上验证，不把真实用户配置当作夹具。
    // 常量 1.5 与 1.25 可被二进制浮点精确表示，避免容差掩盖读取旧值。
    // const 入口必须返回同一个对象，不能在查询时复制出临时配置。
    // 两个缩放 getter 都要读取实时倍率，不能一个委托新状态而另一个留旧缓存。
    // 缩放 getter 从私有配置读取最新倍率，不能在搬移时缓存旧值。
    app.setUIScale(1.5F);
    app.getEditorSettings().uiScaleMultiplier = 1.25F;
    const auto& constApp                      = app;
    if ( constApp.getUIScale() != 1.875F ||
         constApp.getWindowContentScale() != 1.875F ||
         &constApp.getEditorConfig() != configAddress ||
         &constApp.getEditorSettings() != settingsAddress ||
         &constApp.getVisualConfig() != visualAddress )
        return false;
    // 同时检查名称、音量、EQ、音高和质量，覆盖标量与数组字段。
    const auto& restored = app.getAudioPresets().current();
    if ( restored.m_name != "耳机" ||
         app.getEditorSettings().globalVolume != 0.4F ||
         restored.m_eqGains.size() != 15 || restored.m_eqGains[3] != -2 ||
         restored.m_pitch != -5 || restored.m_quality != 3 )
        return false;
    // 无关编辑器字段仍由项目/软件设置管理，听音方案不改变布局行为。
    auto stale         = app.getEditorSettings();
    stale.globalVolume = 0.1F;
    stale.bgmGain      = 0.2F;
    MMM::Config::preserveAudioMixerSettings(stale, app.getEditorSettings());
    return stale.globalVolume == 0.4F &&
           stale.bgmGain == app.getEditorSettings().bgmGain;
}
/// @brief 验证损坏选择、空库和异常 EQ 数组不会留下无效索引。
/// @return 坏数据清洗后仍有可编辑方案时为真。
/// @details 手工构造 JSON，不依赖生产序列化器生成正常输入。
/// 一个无效名称、一个重复名称和一个有效方案混在同一数组中。
/// 测试要求保留有效数据，不能因为局部损坏丢弃整个库。
/// 增益的字符串成员回退默认值，越界数字则夹取到有效上限。
/// 失效选择与空库分别验证，避免两种故障只覆盖同一回退分支。
bool malformed()
{
    const nlohmann::json encoded = { { "selected", "missing" },
                                     { "items",
                                       { { { "name", "a" },
                                           { "volume", 9 },
                                           { "eqPreset", 1 },
                                           { "eqGains", { "bad", 80 } } },
                                         { { "name", "a" } },
                                         { { "name", "" } } } } };
    const auto           parsed  = encoded.get<MMM::Config::AudioPresets>();
    // 重复名称不能让切换含义不确定，未知选择回退首个有效项。
    if ( parsed.m_items.size() != 1 || parsed.m_selected != "a" ) return false;
    // 段数恢复是类型推导结果，不是输入数组只有两个成员的长度。
    const auto& preset = parsed.current();
    if ( preset.m_volume != 1 || preset.m_eqGains.size() != 10 ||
         preset.m_eqGains[0] != 0 || preset.m_eqGains[1] != 24 )
        return false;
    // 空数组必须恢复默认项，而非让 current() 解引用空容器。
    const auto empty = nlohmann::json{
        { "items", nlohmann::json::array() }
    }.get<MMM::Config::AudioPresets>();
    // 最终库始终可编辑，默认选择不会指向不存在的元素。
    return empty.m_items.size() == 1;
}
}  // namespace
/// @brief 在隔离输出目录运行预设行为和持久化回归。
/// @details CTest 必须传入输出目录；缺少参数直接失败，不回退用户目录。
/// 各测试共用一个独立进程，AppConfig 单例不会污染正在运行的编辑器。
/// 不启动音频设备，不把操作系统设备差异混入配置行为回归。
int main(int argc, char** argv)
{
    if ( argc != 2 || !isolation() || !persistence(argv[1]) || !malformed() ) {
        XERROR("Audio presets regression failed");
        return 1;
    }
    return 0;
}
