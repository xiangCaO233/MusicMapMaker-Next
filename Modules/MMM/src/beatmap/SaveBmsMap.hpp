#pragma once
#include "BmsFormat.hpp"
#include <iomanip>
#include <locale>
#include <numeric>

namespace MMM::Bms
{
/// @brief 写出标准通道文本，先在内存验证，再打开目标文件。
/// @details BMS 的 STOP 单位为四分音符的 1/48，MMM 的参数为秒。
/// 普通键布局补齐标准空轨；忽略的滚速和增益由导出界面事先提示。
/// @param map 已同步完成的导出快照，音频引用必须为实际路径。
/// @param path 目标 BMS/BME/BML/PMS 文件名。
/// @return 所有类型可表达且文件写入成功时返回 true。
/// @note 不创建资源包，不复制音频；上层负责解析相对文件引用。
/// @note 仅输出 LNTYPE 1，避免两套长条编码混用。
/// @note STOP 参数换算采用事件所在拍的有效 BPM。
/// @note 原模型不排序、不改写，失败也不改变编辑状态。
/// @warning 用户导出低频路径，构造完整文本后才打开目标文件。
inline bool save(const BeatMap& map, const std::filesystem::path& path)
{
    // 目标后缀决定 PMS 键位协议，不能单凭九轨猜测其他格式。
    // 转盘计入标准键数；普通键谱面在左侧补空转盘，不移动原有键序。
    const auto& meta   = map.m_baseMapMetadata;
    const bool  pms    = upper(Config::pathToUtf8(path.extension())) == ".PMS";
    const int   tracks = meta.track_count;
    const bool  keyboardOnly =
        !pms && ((tracks >= 1 && tracks <= 5) || tracks == 7);
    // Flick 和折线没有这里实现的 BMS 通道表示。
    // 提前拒绝可避免只保存可转换子集、丢失其余物件。
    if ( (pms && tracks != 9) ||
         (!pms && !keyboardOnly && tracks != 6 && tracks != 8 && tracks != 12 &&
          tracks != 16) ||
         !map.m_noteData.flicks.empty() || !map.m_noteData.polylines.empty() )
        return false;
    const bool seven    = tracks == 7 || tracks == 8 || tracks == 16;
    const int  sideSize = seven ? 8 : 6;
    // 内部轨道保留转盘位置，PMS 使用自己的九键通道映射。
    // 轨道数已经在入口限制，下面只处理标准布局。
    // 七键的后两键跳过通道 17，不能简单把轨道号加到 11。
    // 第二玩家的通道十位与第一玩家不同，键序保持一致。
    // PMS 使用独立九键映射，不包含转盘。
    const auto channel = [&](int track) {
        if ( pms ) return track < 5 ? 0x11 + track : 0x22 + track - 5;
        // 无转盘来源的第一颗普通键写入 11，而不是转盘通道 16。
        if ( keyboardOnly ) ++track;
        const int key = track % sideSize;
        return 0x10 * (track / sideSize + 1) + (key == 0   ? 6
                                                : key <= 5 ? key
                                                           : key + 2);
    };
    // 指针只观察调用期间固定的谱面，排序不改变活动编辑模型。
    // 原始 Timing 顺序不一定按时间排列，所以导出单独建立有序视图。
    std::vector<const Timing*> timings;
    for ( const auto& t : map.m_timings ) {
        // 外部协议没有负小节；非法或无限时间不能参与拍位积分。
        // 视觉效果不进入音乐拍位积分，可表达事件仍需验证数值。
        if ( !std::isfinite(t.m_timestamp) || t.m_timestamp < 0 ) return false;
        if ( t.m_timingEffect != TimingEffect::BPM &&
             t.m_timingEffect != TimingEffect::STOP ) {
            // 标准通道不携带视觉滚速；保留 BPM/STOP 的实际音乐时间。
            // 界面预先列出降级差异，独立保存调用也留下明确日志。
            XWARN("BMS 导出：忽略不支持的 timing 类型 {}，位置 {} ms",
                  static_cast<int>(t.m_timingEffect),
                  t.m_timestamp);
            continue;
        }
        if ( !std::isfinite(t.m_timingEffectParameter) ||
             t.m_timingEffectParameter <= 0 || t.m_interpolation )
            return false;
        // 插值段必须先由用户转换，不在此暗中改变其采样定义。
        // 保留同刻事件的稳定顺序，BPM 查询会单独处理优先级。
        timings.push_back(&t);
    }
    std::stable_sort(
        timings.begin(), timings.end(), [](const Timing* a, const Timing* b) {
            return a->m_timestamp < b->m_timestamp;
        });
    // 初始 BPM 与导入器相同，第一条红线之前也有确定的积分速度。
    // 没有红线时才回退参考 BPM，避免把参考值当作所有段的实际 BPM。
    double initial = meta.preference_bpm > 0 ? meta.preference_bpm : 120;
    for ( const auto* t : timings )
        if ( t->m_timingEffect == TimingEffect::BPM ) {
            initial = t->m_timingEffectParameter;
            break;
        }
    // 映射只积分 BPM；STOP 是同一音乐位置的等待，不改变拍位。
    const auto beatAt = [&](double ms) {
        // 所有协议位置来自统一 BPM 积分，不能把毫秒直接当拍号。
        // 三位小节号只允许 000–999，越界时拒绝而非截断。
        double beat = 0, last = 0, bpm = initial;
        for ( const auto* t : timings ) {
            if ( t->m_timestamp > ms ) break;
            if ( t->m_timingEffect != TimingEffect::BPM ) continue;
            beat += (t->m_timestamp - last) * bpm / 60000;
            last = t->m_timestamp;
            bpm  = t->m_timingEffectParameter;
        }
        return beat + (ms - last) * bpm / 60000;
    };
    // 使用全谱最低 BPM 将拍位误差约束在 0.01 毫秒内。
    // 毫秒量化的外部谱面通常不落在简单分数上，机器精度会制造巨型零串。
    double minimumBpm = initial;
    for ( const auto* t : timings )
        if ( t->m_timingEffect == TimingEffect::BPM )
            minimumBpm = std::min(minimumBpm, t->m_timingEffectParameter);
    const double fractionTolerance = minimumBpm * 0.01 / 240000.0;
    // 固定两位资源编号容量为 36²，零编号不代表有效事件。
    // 所有调用先限制为 1–1295，避免字符串表越界。
    const auto encode = [](int value) {
        constexpr std::string_view digits =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        return std::string{ digits[value / 36], digits[value % 36] };
    };
    // 目标文件直到全部对象通过验证才打开，失败不截断已有文件。
    // 声明与通道分开收集，最终先输出完整资源表。
    std::ostringstream header, body;
    header.imbue(std::locale::classic());
    body.imbue(std::locale::classic());
    header << std::setprecision(17);
    // 防止元数据中的换行成为额外的 BMS 指令。
    const auto field = [](std::string text) {
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');
        return text;
    };
    header << "#PLAYER " << (!pms && tracks > 8 ? 3 : 1) << "\n#TITLE "
           << field(meta.title) << "\n#ARTIST " << field(meta.artist)
           << "\n#MAKER " << field(meta.author) << "\n#PLAYLEVEL "
           << field(meta.version) << "\n#BPM " << initial << "\n#LNTYPE 1\n";
    // 相同音频文件只占用一个资源编号，Key 与 BGM 可以共享定义。
    // 调用层须先把项目资源 ID 转成目标文件旁的相对路径。
    std::map<std::string, int> resources;
    const auto                 resource = [&](const std::string& name) {
        if ( const auto it = resources.find(name); it != resources.end() )
            return it->second;
        // 编号耗尽时返回无效零值，由 emit 统一使事务失败。
        // 不回绕编号，否则后续定义会覆盖此前音效。
        if ( resources.size() >= 1295 ) return 0;
        const int id    = static_cast<int>(resources.size()) + 1;
        resources[name] = id;
        // 未绑定的 Note 仍需要非零通道编号，空 WAV 定义表示静音。
        header << "#WAV" << encode(id) << ' ' << field(name) << '\n';
        return id;
    };
    // 限制总输出规模，防止高分辨率事件组合产生巨型文本。
    // 此计数在写磁盘之前执行，失败不会留下半份谱面。
    std::size_t outputBytes = 0;
    const auto  emit        = [&](double ms, int ch, int value) {
        if ( !std::isfinite(ms) || ms < 0 || value <= 0 ) return false;
        // 所有协议位置来自统一 BPM 积分，不能把毫秒直接当拍号。
        // 三位小节号只允许 000–999，越界时拒绝而非截断。
        double beat = beatAt(ms);
        if ( !std::isfinite(beat) || beat < 0 || beat >= 4000 ) return false;
        int measure = static_cast<int>(beat / 4);
        // 每小节默认四拍；原变长小节已经展平到相同绝对拍位。
        // 分数用于生成本行自己的格点，不强制全谱使用同一分辨率。
        const double fraction = beat / 4 - measure;
        // 优先最小分母，避免 1/4 拍物件被写成庞大且无意义的零串。
        int numerator = 0, denominator = 1;
        // 有界有理数逼近保证常见节拍短文本且任意编辑不无限搜索。
        // 超过精度预算的时间返回失败，不能悄悄吸附到错误拍位。
        for ( ; denominator <= 65536; ++denominator ) {
            numerator = static_cast<int>(std::round(fraction * denominator));
            if ( std::abs(fraction - static_cast<double>(numerator) /
                                         denominator) <= fractionTolerance )
                break;
        }
        if ( denominator > 65536 ) return false;
        // 浮点尾差接近整小节时规范到下一个小节的起点。
        // 随后再次检查小节上界，避免生成四位小节号。
        if ( numerator == denominator ) {
            ++measure;
            numerator   = 0;
            denominator = 1;
        }
        if ( measure >= 1000 ) return false;
        outputBytes += denominator * 2U + 9U;
        if ( outputBytes > 64U * 1024U * 1024U ) return false;
        // 每条事件使用独立行，00 表示该格没有事件而不是覆盖其他行。
        // 尤其 BGM 的多行语义用于保留同拍复音。
        std::string data(denominator * 2U, '0');
        data.replace(numerator * 2U, 2, encode(value));
        body << '#' << std::setfill('0') << std::setw(3) << std::dec << measure
             << std::setw(2) << std::hex << std::uppercase << ch << ':' << data
             << '\n';
        return true;
    };
    // 扩展 BPM 与 STOP 分属独立编号空间，不与 WAV 资源编号混用。
    // 数值使用十七位精度，避免 MMM 往返时积累拍位误差。
    int    bpmId = 0, stopId = 0;
    double bpm = initial;
    for ( const auto* t : timings ) {
        if ( t->m_timingEffect == TimingEffect::BPM ) {
            bpm = t->m_timingEffectParameter;
            if ( ++bpmId > 1295 ) return false;
            header << "#BPM" << encode(bpmId) << ' ' << bpm << '\n';
            if ( !emit(t->m_timestamp, 8, bpmId) ) return false;
        } else {
            // 同刻红线决定 STOP 长度，与导入器及常见 BMS 播放器保持一致。
            // 即使输入 Timing 的同刻顺序不同，先寻找该拍最后的 BPM。
            // 输出标准 1/48 拍单位，而不是写入 MMM 内部等待秒数。
            double stopBpm = bpm;
            for ( const auto* other : timings )
                if ( other->m_timestamp == t->m_timestamp &&
                     other->m_timingEffect == TimingEffect::BPM )
                    stopBpm = other->m_timingEffectParameter;
            if ( ++stopId > 1295 ) return false;
            header << "#STOP" << encode(stopId) << ' '
                   << t->m_timingEffectParameter * stopBpm * 48 / 60 << '\n';
            if ( !emit(t->m_timestamp, 9, stopId) ) return false;
        }
    }
    // 普通音符使用 1x/2x，长条使用 5x/6x 的一对端点。
    // 子物件属于折线，不能作为独立 BMS 头重复输出。
    // 长条起止映射使用同一条 BPM 积分，跨变速段不会被误算成固定时长。
    // 尾部和头部共享 WAV 编号，仅在开始处触发采样。
    // 键位检查在输出前执行，禁止生成不存在的侧别通道。
    const auto writeNote = [&](const Note& note, double duration) {
        if ( note.m_track >= static_cast<unsigned>(tracks) || note.m_isSubNote )
            return false;
        if ( note.m_sampleBinding && note.m_sampleBinding->m_volume != 1.0F )
            XWARN("BMS 导出：物件采样音量恢复为 100，位置 {} ms",
                  note.m_timestamp);
        // 空采样仍分配非零编号，使静音 Note 的判定信息得以保留。
        // 共享 WAV 无法携带事件增益，导出确认中已说明恢复为默认音量。
        const int sound = resource(note.m_sampleBinding
                                       ? note.m_sampleBinding->m_audioResourceId
                                       : "");
        int       ch    = channel(note.m_track);
        if ( duration > 0 ) ch += 0x40;
        if ( !emit(note.m_timestamp, ch, sound) ) return false;
        return duration == 0 || emit(note.m_timestamp + duration, ch, sound);
    };
    for ( const auto& note : map.m_noteData.notes )
        if ( !writeNote(note, 0) ) return false;
    for ( const auto& hold : map.m_noteData.holds )
        if ( !std::isfinite(hold.m_duration) || hold.m_duration <= 0 ||
             !writeNote(hold, hold.m_duration) )
            return false;
    // 自动采样统一写入 01，不把 BGM 错当玩家键位。
    // effectiveTimestamp 包含事件自身偏移，导出位置应与实际触发一致。
    for ( const auto& sample : map.m_audioSamples ) {
        if ( sample.m_volume != 1.0F )
            XWARN("BMS 导出：自动采样音量恢复为 100，位置 {} ms",
                  sample.m_timestamp);
        if ( !emit(sample.effectiveTimestamp(),
                   1,
                   resource(sample.m_audioResourceId)) )
            return false;
    }
    // 最后一次提交只进行文本写入；good 同时检查打开和写入状态。
    // 不声称资源已打包，此入口只写 BMS 及其相对音频引用。
    // 输出流失败通过返回值传播，禁止在无异常构建中引入异常处理。
    // 二进制打开避免平台自动插入换行，结果在各平台一致。
    // 数值流使用经典 locale，系统小数逗号不会污染协议。
    std::ofstream output(path, std::ios::binary);
    output << header.str() << '\n' << body.str();
    return output.good();
}
}  // namespace MMM::Bms
