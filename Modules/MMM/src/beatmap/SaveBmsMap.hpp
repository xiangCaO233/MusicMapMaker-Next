#pragma once
#include "BmsFormat.hpp"
#include "BmsMyO2Grid.h"
#include <array>
#include <iomanip>
#include <locale>
#include <numeric>

namespace MMM::Bms
{
/// @brief 写出标准通道文本，先在内存验证，再打开目标文件。
/// @details BMS 的 STOP 单位为四分音符的 1/48，MMM 的参数为秒。
/// 四至八轨按 MyO2 顺序从 16 开始排列；忽略的滚速和增益由界面提示。
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
    // 四至八轨沿用 MyO2 的顺序，第一轨占用 16，不额外插入空转盘。
    const auto& meta   = map.m_baseMapMetadata;
    const bool  pms    = upper(Config::pathToUtf8(path.extension())) == ".PMS";
    const int   tracks = meta.track_count;
    const bool  keyboardOnly = !pms && tracks >= 1 && tracks <= 3;
    // Flick 和折线没有这里实现的 BMS 通道表示。
    // 提前拒绝可避免只保存可转换子集、丢失其余物件。
    if ( (pms && tracks != 9) ||
         (!pms && !keyboardOnly && (tracks < 4 || tracks > 8) && tracks != 12 &&
          tracks != 16) ||
         !map.m_noteData.flicks.empty() || !map.m_noteData.polylines.empty() )
        return false;
    const bool seven    = tracks == 7 || tracks == 8 || tracks == 16;
    const int  sideSize = seven ? 8 : 6;
    // 四至八轨使用 16、11、12、13、14、15、18、19 的前缀。
    // 轨道数已经在入口限制，下面只处理标准布局。
    // 七键的后两键跳过通道 17，不能简单把轨道号加到 11。
    // 第二玩家的通道十位与第一玩家不同，键序保持一致。
    // PMS 使用独立九键映射，不包含转盘。
    const auto channel = [&](int track) {
        if ( pms ) return track < 5 ? 0x11 + track : 0x22 + track - 5;
        // 一至三轨保留既有普通键补位行为，不扩大本次四至八轨调整范围。
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
    // 十二位有效数字覆盖毫秒级谱面时长的既有误差预算，同时消除
    // 139.99999999999986 一类来源倒数运算残差，不把它当作真实小数 BPM。
    header << std::setprecision(12);
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
    // MyO2 的转换器对同小节同通道只保留最后一行。
    // 因此普通键、长条端点与 BPM 先聚合；01、09 独立行保留复音和停顿叠加。
    ExportRows rows;
    const auto emit = [&](double ms, int ch, int value) {
        if ( !std::isfinite(ms) || ms < 0 || value <= 0 ) return false;
        // 所有协议位置来自统一 BPM 积分，不能把毫秒直接当拍号。
        // 三位小节号只允许 000–999，越界时拒绝而非截断。
        double beat = beatAt(ms);
        if ( !std::isfinite(beat) || beat < 0 || beat >= 4000 ) return false;
        int measure = static_cast<int>(beat / 4);
        // 每小节默认四拍；原变长小节已经展平到相同绝对拍位。
        // 分数用于生成本行自己的格点，不强制全谱使用同一分辨率。
        double fraction = beat / 4 - measure;
        // 允许精度范围内的整小节尾差进入下一小节，不生成越界槽位。
        // 两种行写出路径共用规范化，避免聚合与独立事件行边界不同。
        if ( 1.0 - fraction <= fractionTolerance ) {
            ++measure;
            fraction = 0;
        }
        if ( measure >= 1000 ) return false;
        // 键位和 BPM 通道一次写出整小节；保留原拍位供公共分辨率搜索。
        // 不先逐事件量化，防止各自的近似误差在合并时再次累积。
        rows[{ measure, ch }].emplace_back(fraction, value);
        return true;
    };
    // 扩展 BPM 与 STOP 分属独立编号空间，不与 WAV 资源编号混用。
    // 数值精度与头部一致，避免倒数运算产生的伪小数进入协议声明。
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
    // 标准 BMS 允许同小节同通道多行叠加。资源包可以选择更短的稀疏行，
    // 避免保留离拍物件时强迫整小节共享巨大的分母。普通另存为仍保留单行
    // 兼容路径，供只识别同通道最后一行的网页转换器使用。
    const auto exportProperties =
        map.m_metadata.map_properties.find(MapMetadataType::BMS);
    const bool compactRows =
        exportProperties != map.m_metadata.map_properties.end() &&
        exportProperties->second.contains("compact_rows") &&
        exportProperties->second.at("compact_rows") == "1";
    // 兼容模式仅作用于本次序列化副本，默认关闭并保留真实毫秒位置。
    // 明确拒绝吸附冲突，不能靠删除物件令检查器归零。
    const bool myO2Grid =
        exportProperties != map.m_metadata.map_properties.end() &&
        exportProperties->second.contains("myo2_grid") &&
        exportProperties->second.at("myo2_grid") == "1";
    if ( myO2Grid && !quantizeMyO2Rows(rows) ) {
        XERROR(
            "MyO2 "
            "兼容导出失败：吸附后物件或长条端点重叠，或超出小节范围；请关闭兼容"
            "网格后重试");
        return false;
    }
    // 兼容位置已严格量化；序列化不能再用毫秒预算近似为别的分母。
    // 否则网格上的点也可能被编码成带亚 tick 残差的离拍位置。
    const double rowTolerance = myO2Grid ? 1e-12 : fractionTolerance;
    // 同小节同通道寻找能够同时表达所有事件的最短格点。
    // 长条头尾共享一行，避免只保留末端导致无法配对。
    // 短格点搜索有固定上限，高精度回退仍受总文本预算约束。
    // 公共格点不能取各事件最简分母的最大值，互质分母会落在不同位置。
    // 也不能直接取它们的最小公倍数，逐事件近似会制造过大的分母。
    // 这里直接针对原始拍位检查候选，所有事件共同满足既有时间误差。
    // 常见二分、三连音小节会在很小的分母结束，不固定输出长零串。
    // 密集外部毫秒谱面超过精度预算时返回失败，保持原有禁止静默吸附的约束。
    // 预算按编码后的每行计算，防止少量高精度位置撑爆最终文本。
    // 容量检查发生在分配零串之前，且目标文件此时尚未打开。
    // 空行不会进入 rows，分母至少为一，避免空格点和除零分支。
    for ( const auto& [position, events] : rows ) {
        int denominator = 1;
        for ( ; denominator <= 65536; ++denominator ) {
            const bool fits = std::all_of(
                events.begin(), events.end(), [&](const auto& event) {
                    return std::abs(event.first -
                                    std::round(event.first * denominator) /
                                        denominator) <= rowTolerance;
                });
            if ( fits ) break;
        }
        // 独立事件选择最短的有效分母，不把它吸附到用户未选择的拍线。
        // 比较真实输出字节数，只有严格更短或公共格点不存在时才拆行。
        // 数值误差预算仍是原有 0.01ms，不通过放宽精度换取更小文件。
        if ( compactRows || position.second == 1 || position.second == 9 ) {
            // 比较成本前仅存整数位置，不提前为每个候选分配大段零串。
            // 选择公共行或独立行只改变文本表示，不改变通道和采样身份。
            // 同一行内真实离拍事件仍按既有误差预算表达，不强制取整拍。
            std::vector<std::array<int, 3>> sparse;
            std::size_t                     sparseBytes = 0;
            for ( const auto& [fraction, value] : events ) {
                int resolution = 1, slot = 0;
                for ( ; resolution <= 65536; ++resolution ) {
                    slot = static_cast<int>(std::round(fraction * resolution));
                    if ( std::abs(fraction - static_cast<double>(slot) /
                                                 resolution) <= rowTolerance )
                        break;
                }
                if ( resolution > 65536 || slot < 0 || slot >= resolution )
                    return false;
                sparse.push_back({ slot, resolution, value });
                sparseBytes += resolution * 2U + 9U;
            }
            if ( position.second == 1 || position.second == 9 ||
                 denominator > 65536 || sparseBytes < denominator * 2U + 9U ) {
                outputBytes += sparseBytes;
                if ( outputBytes > 64U * 1024U * 1024U ) return false;
                for ( const auto& event : sparse ) {
                    std::string data(event[1] * 2U, '0');
                    data.replace(event[0] * 2U, 2, encode(event[2]));
                    body << '#' << std::setfill('0') << std::setw(3) << std::dec
                         << position.first << std::setw(2) << std::hex
                         << std::uppercase << position.second << ':' << data
                         << '\n';
                }
                continue;
            }
        }
        if ( denominator > 65536 ) {
            // 首拍平移可能使一组离拍音符无法共用短分母，但仍能按原精度表达。
            // 四舍五入到 D 格时误差至多为 0.5/D，直接求保证精度的公共格点。
            // 不放宽 0.01ms 预算，也不改用多行，保持 MyO2 的单行读取兼容性。
            // 额外加一格避开浮点边界；计算量固定，不扩大逐个分母搜索范围。
            const double resolution = std::ceil(0.5 / rowTolerance) + 1;
            // 仅分配剩余文本预算能容纳的格点，极低 BPM 不能制造无界零串。
            // 容量检查先于整数转换，防止无限值或超大分母越界。
            constexpr std::size_t limit = 64U * 1024U * 1024U;
            if ( outputBytes > limit - 11U || !std::isfinite(resolution) ||
                 resolution >
                     static_cast<double>((limit - outputBytes - 9U) / 2U) )
                return false;
            denominator = static_cast<int>(resolution);
        }
        outputBytes += denominator * 2U + 9U;
        if ( outputBytes > 64U * 1024U * 1024U ) return false;
        std::string data(denominator * 2U, '0');
        for ( const auto& [fraction, value] : events ) {
            const int slot =
                static_cast<int>(std::round(fraction * denominator));
            // 边界量化不能生成越过本行的写入；冲突音符也不能被静默覆盖。
            // 同拍 BPM 沿用声明顺序，最后一条定义决定后续积分速度。
            if ( slot < 0 || slot >= denominator ) return false;
            const auto code = encode(value);
            const auto old  = data.substr(slot * 2U, 2);
            // 相同资源的重复声明可以合并；不同资源争用同一键位无法无损表示。
            // BGM 复音已由独立 01 行表达，不适用玩家键位的冲突处理。
            if ( old != "00" && old != code && position.second != 8 )
                return false;
            data.replace(slot * 2U, 2, code);
        }
        body << '#' << std::setfill('0') << std::setw(3) << std::dec
             << position.first << std::setw(2) << std::hex << std::uppercase
             << position.second << ':' << data << '\n';
    }
    // 最后一次提交只进行文本写入；good 同时检查打开和写入状态。
    // 不声称资源已打包，此入口只写 BMS 及其相对音频引用。
    // 输出流失败通过返回值传播，禁止在无异常构建中引入异常处理。
    // 二进制打开避免平台自动插入换行，结果在各平台一致。
    // 数值流使用经典 locale，系统小数逗号不会污染协议。
    std::ofstream output(path, std::ios::binary);
    // 分隔行也是部分旧播放器定位主数据区的入口，不能只输出空行。
    output << header.str() << "\n*---------------------- MAIN DATA FIELD\n"
           << body.str();
    return output.good();
}
}  // namespace MMM::Bms
