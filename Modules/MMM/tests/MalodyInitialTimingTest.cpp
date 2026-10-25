#include "mmm/beatmap/BeatMap.h"

#include "log/colorful-log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
/// @file MalodyInitialTimingTest.cpp
/// @brief 不依赖真实媒体的首拍时序矩阵，音频编码另由 Logic 集成测试负责。
///
/// 每项表格给出首红线绝对时间、参考 BPM、同刻用户绿线和预期静音长度。
/// 120 BPM 首红线配合 100 BPM 参考值，可暴露误用首红线拍长的错误。
/// 1000ms 的原红线恰好回卷到零，1100ms 则保留非零相位。
/// 100ms 的首红线位于首拍前半段，不触发旧的向前回卷。
/// 常规用户绿线采用 2.5 倍，另加一倍用户绿线检查初始化身份不会吞掉原事件。
/// 后续再切换到 150 BPM，确保新参考段没有改变后续 BPM 积分。
/// 所有输出均位于构建树，源树媒体不参与文件写入。
/// @brief 首红线条件矩阵的独立期望，时间均为毫秒。
struct Scenario {
    double m_firstTime;       ///< 用户首红线位置，不是序列化后的包装相位。
    double m_referenceBpm;    ///< 参考 BPM，用户首红线固定为 120 BPM。
    bool   m_userScroll;      ///< 用户是否在首红线同刻放置自定义绿线。
    double m_expectedLeadIn;  ///< 必须与实际音频补静音一致的期望。
    /// @brief 一倍用户绿线也必须保留原始时间，不能冒充初始化。
    double m_userScrollValue{ 2.5 };
    /// @brief 不补头时保留同一 Main 文件的额外触发。
    bool m_repeatMain{ false };
    /// @brief 规范化后参考红绿线的绝对时刻，正锚点不能无条件放到零。
    double m_referenceAnchorTime{ 0 };
    /// @brief 无主音轨场景不能依赖 SOUND offset 反推首红线相位。
    bool m_withMain{ true };
};

/// @brief 用来源标签创建红线或绿线，导出与回读后仍能识别用户原始事件。
/// @param time 用户定义的绝对时间。
/// @param effect 事件种类，测试只构造 BPM 和 SCROLL。
/// @param value BPM 或滚速，不把绿线参数当成节拍频率。
/// @param origin 标签文本，合成首拍不具有该标签。
MMM::Timing makeTiming(double time, MMM::TimingEffect effect, double value,
                       std::string_view origin)
{
    MMM::Timing timing;
    timing.m_timestamp = time;
    timing.m_bpm       = effect == MMM::TimingEffect::BPM ? value : 120;
    timing.m_beat_length =
        effect == MMM::TimingEffect::BPM ? 60000 / value : value;
    timing.m_timingEffect          = effect;
    timing.m_timingEffectParameter = value;
    // 标签只用于区分原始与新增事件，不参与待测的 BPM 或时间计算。
    timing.m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["origin"] =
        nlohmann::json(origin).dump();
    return timing;
}

/// @brief 验证一组首拍策略及所有原始 timing、物件和采样的相对时间。
/// @param scenario 来自条件矩阵的独立测试输入。
/// @param output 用于真实加载器回读的构建树临时 MC。
/// @param mode Key 或 Slide，覆盖绝对尾拍与相对 seg 两种长条编码。
/// @return 初始红绿线与用户数据均符合独立期望时返回真。
/// @note 不复制首拍实现公式，后移量由表格明确给定。
/// @note 通过正式加载器测量绝对毫秒，不能只比较 JSON 数组数量。
/// @details 标签区分原始用户事件和合成锚点，不依赖时间相同节点的排序。
/// 验证目标包括原红线、自定义 Scroll、后续 BPM、Note 和 Hold 两端。
/// 自动采样另行验证有效播放时间，确保 offset 没被当成整谱偏移。
/// @note 来源模型保持原首红线和主音轨位置，可重复导出。
/// @note 重复 Main 的场景不补音频头，只验证额外触发仍保持普通采样语义。
/// @note 主采样的资源身份与触发事件身份分别检查，不能按文件名对事件去重。
bool checkScenario(const Scenario&              scenario,
                   const std::filesystem::path& output, int mode)
{
    MMM::BeatMap source;
    // 四条玩家轨和一条 BGM 轨足以覆盖 Note、Key Hold 及多个自动采样。
    // 模型没有来源 beat 缓存，迫使保存器依据当前毫秒时间生成拍位。
    source.m_baseMapMetadata.track_count     = 4;
    source.m_baseMapMetadata.bgm_track_count = 1;
    source.m_baseMapMetadata.preference_bpm  = scenario.m_referenceBpm;
    // 无 Main 场景也不留下歌曲提示，避免同名效果音被误当作主音频。
    source.m_baseMapMetadata.song_file_hint =
        scenario.m_withMain ? "music.wav" : "";
    // 模式单独切换，时间线输入和表格预期保持相同，不能分别使用两套相位规则。
    source.m_metadata.map_properties[MMM::MapMetadataType::MALODY]["mode"] =
        std::to_string(mode);
    // 原红线与后续红线使用不同 BPM，能发现新增参考段意外改变后半段积分。
    source.m_timings.push_back(
        makeTiming(scenario.m_firstTime, MMM::TimingEffect::BPM, 120, "first"));
    source.m_timings.push_back(makeTiming(
        scenario.m_firstTime + 2000, MMM::TimingEffect::BPM, 150, "later"));
    if ( scenario.m_userScroll )
        // 保留用户值和 origin 标签；初始化绿线不能覆盖或吞掉这个节点。
        source.m_timings.push_back(makeTiming(scenario.m_firstTime,
                                              MMM::TimingEffect::SCROLL,
                                              scenario.m_userScrollValue,
                                              "user-scroll"));
    source.m_timings.push_back(makeTiming(scenario.m_firstTime + 500,
                                          MMM::TimingEffect::SCROLL,
                                          3.5,
                                          "later-scroll"));
    // 主音轨配对零点；普通效果采样具有局部 offset，必须仍随内容一起移动。
    // 效果音特意与 Main 同名但目录不同，完整路径必须比文件名回退优先。
    // 有无配对 Main 都必须经过同一构建器，不允许用空文档绕过无音频测试。
    if ( scenario.m_withMain )
        source.m_audioSamples.push_back(MMM::AudioSampleEvent{
            .m_timestamp = 0, .m_track = 4, .m_audioResourceId = "music.wav" });
    if ( scenario.m_repeatMain ) {
        // 额外触发同一文件是合法普通采样，不能覆盖负责首拍配对的零点 Main。
        // 本场景不需要编码补头，应完整保留两个触发点而非拒绝整个导出。
        source.m_audioSamples.push_back(
            MMM::AudioSampleEvent{ .m_timestamp       = 1500,
                                   .m_track           = 4,
                                   .m_audioResourceId = "music.wav" });
    }
    source.m_audioSamples.push_back(
        MMM::AudioSampleEvent{ .m_timestamp       = scenario.m_firstTime + 750,
                               .m_offsetMs        = -13,
                               .m_track           = 4,
                               .m_audioResourceId = "effects/music.wav" });
    // 普通效果音早于首红线时会落在新参考段，验证分段拍间距而非只测后半段。
    source.m_audioSamples.push_back(
        MMM::AudioSampleEvent{ .m_timestamp       = 50,
                               .m_offsetMs        = 7,
                               .m_track           = 4,
                               .m_audioResourceId = "early.wav" });
    auto& note       = source.m_noteData.notes.emplace_back();
    note.m_timestamp = scenario.m_firstTime + 1000;
    note.m_track     = 1;
    auto& hold       = source.m_noteData.holds.emplace_back();
    // Hold 横跨两个绝对拍位，只有移动起点会改变持续时间并被后面的断言捕获。
    hold.m_timestamp = scenario.m_firstTime + 500;
    hold.m_duration  = 500;
    hold.m_track     = 2;
    source.sync();
    // 先建立统一物件索引，模型保存器按正式入口读取 Note 与 Hold。

    nlohmann::json document;
    // 静音长度是外部计划接口的一部分，独立于 JSON 时间线的重载校验。
    // 预期来自场景表，不能用当前实现计算结果作为自己的期望值。
    double leadIn = -1;
    if ( !source.buildMalodyExportDocument(document, leadIn) ||
         std::abs(leadIn - scenario.m_expectedLeadIn) > 1.0e-6 )
        return false;
    // 同 BPM 保留原始效果数量与滚速；不同 BPM 必须新增同拍单位绿线。
    // 是否补参考段只取决于实际 BPM，相同 BPM 的用户单位绿线仍是普通事件。
    const bool differentBpm = std::abs(scenario.m_referenceBpm - 120) > 1e-6;
    // 始终保留夹具中的后续红线与绿线，空输出不能以无需初始化作为成功。
    if ( document["time"].empty() || document["effect"].empty() ) return false;
    // 不同 BPM 同时检查值和拍位相等，防止红线与绿线各自落入不同网格。
    if ( differentBpm ) {
        if ( std::abs(document["time"][0]["bpm"].get<double>() -
                      scenario.m_referenceBpm) > 1e-6 ||
             document["effect"][0]["scroll"] != 1.0 ||
             document["time"][0]["beat"] != document["effect"][0]["beat"] )
            return false;
        // 相同 BPM 不得新增一个隐含 1.0 滚速，无论原来是否有同刻用户绿线。
    } else if ( document["effect"].size() !=
                (scenario.m_userScroll ? 2U : 1U) ) {
        return false;
    }
    // 真正写入并回读，覆盖 delay 与主 SOUND 的成对包装，避免只在构建器内自证。
    std::ofstream stream(output);
    // 文档来自本次构建，不从保存输出中读取参数再修改期望。
    stream << document.dump();
    stream.close();
    // 关闭后检查流状态，避免磁盘失败被旧文件或空文件掩盖。
    if ( !stream ) return false;
    auto loaded = MMM::BeatMap::loadFromFile(output);
    // 所有带来源标签的事件按源毫秒比较，不能从新拍号反推期望。
    const auto expectedTime = [&](std::string_view origin) {
        // 标签到原始时间的映射属于夹具定义，不读取输出中的 beat 或 delay。
        // 来源时刻加表格给出的静音长度，构成对绝对毫秒的独立期望。
        if ( origin == "first" || origin == "user-scroll" )
            return scenario.m_firstTime + leadIn;
        return scenario.m_firstTime + (origin == "later" ? 2000 : 500) + leadIn;
    };
    if ( differentBpm ) {
        // 检查加载后的真实时间，而不是只检查看似合法的三元组。
        // 初始化绿线的位置锁定参考段起点，而不是错误地锁定在原红线上。
        const auto green = std::find_if(
            loaded.m_timings.begin(),
            loaded.m_timings.end(),
            [](const auto& timing) {
                return timing.m_timingEffect == MMM::TimingEffect::SCROLL &&
                       timing.m_timingEffectParameter == 1.0;
            });
        if ( green == loaded.m_timings.end() ||
             std::abs(green->m_timestamp - scenario.m_referenceAnchorTime) >
                 .1 )
            return false;
    }
    // 每个带标签事件恰好出现一次；不能通过删掉原首红线或用户绿线通过校验。
    std::size_t originalCount = 0;
    // 原始事件数量由夹具决定，新增单位 Scroll 不计入用户事件。
    for ( const auto& timing : loaded.m_timings ) {
        // 新增的初始化节点没有来源标签，既不计作用户事件，也不作为丢失借口。
        const auto properties = timing.m_metadata.timing_properties.find(
            MMM::TimingMetadataType::MALODY);
        if ( properties == timing.m_metadata.timing_properties.end() ) continue;
        const auto origin = properties->second.find("origin");
        if ( origin == properties->second.end() ) continue;
        const auto text = nlohmann::json::parse(origin->second, nullptr, false);
        // 测试扩展字段与正式 MC 结构一起序列化，解析失败必须使本项失败。
        if ( !text.is_string() ) return false;
        const auto name = text.get<std::string>();
        if ( std::abs(timing.m_timestamp - expectedTime(name)) > .1 ) {
            XERROR("Initial MC timing moved: {} actual={} expected={}",
                   name,
                   timing.m_timestamp,
                   expectedTime(name));
            return false;
        }
        if ( name == "user-scroll" &&
             timing.m_timingEffectParameter != scenario.m_userScrollValue )
            return false;
        ++originalCount;
        // 每个原始事件都贡献计数，新增初始化不能抵扣遗漏的用户事件。
    }
    // 普通 Note 与 Key Hold 的起止拍都应平移，时长和自定义滚速保持不变。
    // .1ms 只容纳 MC 分拍拟合误差，不能掩盖一拍或整数毫秒级错误。
    if ( originalCount != (scenario.m_userScroll ? 4U : 3U) ||
         loaded.m_noteData.notes.size() != 1 ||
         loaded.m_noteData.holds.size() != 1 ||
         std::abs(loaded.m_noteData.notes[0].m_timestamp -
                  (scenario.m_firstTime + 1000 + leadIn)) > .1 ||
         std::abs(loaded.m_noteData.holds[0].m_timestamp -
                  (scenario.m_firstTime + 500 + leadIn)) > .1 ||
         std::abs(loaded.m_noteData.holds[0].m_duration - 500) > .1 )
        return false;
    // 无 Main 的场景仍包含普通采样，避免通过空音频数组绕过拍位换算。
    std::size_t mainCount = 0;
    for ( const auto& sample : loaded.m_audioSamples ) {
        // 前导段内的早期效果音能暴露“所有拍位只加同一个常数”的错误。
        // 后半段音效则与 BPM 切换后的普通物件共用原有时间映射。
        // 同一自动采样轨可以同时有 Main 和效果音，不能按轨道整体覆盖时间。
        // offset 在局部播放时刻才生效，不得被拍轴变换丢弃或重复累加。
        if ( sample.m_audioResourceId == "music.wav" ) {
            // 同资源身份不等于同一个事件：只把零点那个作为包装锚点。
            ++mainCount;
            if ( std::abs(sample.effectiveTimestamp()) > .2 &&
                 (!scenario.m_repeatMain ||
                  std::abs(sample.effectiveTimestamp() - 1500) > .2) )
                return false;
        }
        // SOUND offset 为整数毫秒；非整数首拍相位允许半毫秒的协议舍入。
        if ( sample.m_audioResourceId == "early.wav" &&
             std::abs(sample.effectiveTimestamp() - (57 + leadIn)) > .51 )
            return false;
        if ( sample.m_audioResourceId == "effects/music.wav" &&
             std::abs(sample.effectiveTimestamp() -
                      (scenario.m_firstTime + 737 + leadIn)) > .2 )
            return false;
    }
    // 数量与时间同时约束，防止通过丢弃重复触发来掩盖拍位错误。
    // 检查精确事件数，不能丢掉零点 Main 或悄悄复制为额外 BGM 触发。
    if ( mainCount !=
         (scenario.m_withMain ? (scenario.m_repeatMain ? 2U : 1U) : 0U) )
        return false;
    // 源模型保持原样，重复导出同一编辑谱面不能叠加前导时间。
    // 序列化只能改写输出 JSON，不能在来源容器内永久加入合成红绿线。
    // 重复导出同一来源时仍应得到相同前导长度，而不是逐次累加。
    return source.m_timings.front().m_timestamp == scenario.m_firstTime &&
           (!scenario.m_withMain ||
            source.m_audioSamples.front().effectiveTimestamp() == 0);
}
}  // namespace

/// @brief 运行参考 BPM 相等性、回卷、负时间和无 Main 的首拍导出组合。
/// @param argc 必须传入构建树输出目录。
/// @param argv 输出路径，不访问源码测试资源。
/// @return 所有真实序列化与回读检查通过时返回零。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    // 参数缺失直接失败，避免默认落到工作目录并污染工程根目录。
    // CTest 提供隔离位置，不读取或修改源码测试资源。
    const std::filesystem::path directory(argv[1]);
    // 文件目录来自 CTest，重复运行只覆盖本测试自己的输出，不清理共享资源。
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if ( error ) return 1;
    // 目录创建失败不会继续写谱面，防止读取上一次测试残留结果。
    // 120 BPM 一拍 500ms，参考 100 BPM 一拍 600ms，能区分错误采用首拍 BPM。
    // 相同 BPM 的零点按是否已有用户绿线拆分，再补负时间单位绿线的边界。
    // 其余场景保留用户绿线，直接检查新增配对不会覆盖 2.5 倍滚速。
    // 预期毫秒值独立列出，不调用导出器的回卷或规范化函数。
    const std::array scenarios{
        Scenario{ 1100, 120, true, 0 },
        Scenario{ 1000, 120, true, 0 },
        Scenario{ 100, 120, true, 0 },
        Scenario{ 0, 120, false, 0 },
        Scenario{ 0, 120, true, 0 },
        Scenario{ 1100, 100, true, 500 },
        Scenario{ 1000, 100, true, 600 },
        Scenario{ 100, 100, true, 500 },
        Scenario{ 0, 100, true, 600 },
        Scenario{ -100, 120, true, 0, 1.0 },
        Scenario{ -100, 100, true, 700 },
        Scenario{ 400, 240, true, 0, 2.5, false, 150 },
        // 无 Main、整数拍回卷及非整数参考拍长不能依赖旧音频包装。
        Scenario{ 1100, 100, true, 500, 2.5, false, 0, false },
        Scenario{ 173.3, 123, true, 314.50487804878 },
        Scenario{ 399.9, 234, true, 0, 2.5, false, 143.489743589744 },
        Scenario{ 100, 120, true, 0, 2.5, true }
    };
    for ( std::size_t index = 0; index < scenarios.size(); ++index ) {
        // 失败日志保留矩阵索引，便于对应到明确的输入条件而非 JSON 排序。
        for ( const int mode : { 0, 7 } ) {
            // 产物按模式隔离，可在失败后直接核对 Key endbeat 与 Slide seg。
            // 同一个参考 BPM 拍长对两个模式都生效，不随皮肤或轨道坐标改变。
            if ( !checkScenario(scenarios[index],
                                directory / (std::to_string(index) + "_" +
                                             std::to_string(mode) + ".mc"),
                                mode) ) {
                XERROR("Initial MC timing scenario {} mode {} failed",
                       index,
                       mode);
                return 1;
            }
        }
    }
    return 0;
}
