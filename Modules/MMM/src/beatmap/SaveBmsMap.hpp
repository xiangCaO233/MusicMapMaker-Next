#pragma once

#include "BmsExportTimeline.h"
#include "BmsFormat.hpp"
#include "BmsMyO2Export.h"
#include "mmm/beatmap/BeatmapOutputFile.h"
#include <array>
#include <iomanip>
#include <locale>
#include <map>
#include <numeric>

namespace MMM::Bms
{
/// @brief 写出标准通道文本，先在内存验证，再打开目标文件。
/// @details BMS 的 STOP 单位为四分音符的 1/48，MMM 的参数为秒。
/// 兼容模式保留 MyO2 基础协议，并在实际量化后的拍位验证可击打物件。
/// 普通 BMS 输出维持原有通道、叠加和精度语义，不附带专用控制码。
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
    const bool  myO2         = usesMyO2Format(map.m_metadata);
    const bool  myO2Grid     = bmsProperty(map.m_metadata, "myo2_grid") == "1";
    // 不同变体允许的键数和物件集合不同，三者分别校验，避免先依靠通用键路由产生非法编码。
    // MyO2 的判定通道由站点固定映射，PMS 不能靠改后缀复用同一档谱面。
    if ( myO2 &&
         (pms || (tracks != 4 && tracks != 7) ||
          map.m_noteData.notes.size() + map.m_noteData.holds.size() == 0 ||
          map.m_noteData.notes.size() + map.m_noteData.holds.size() >
              100000) ) {
        XERROR("MyO2 兼容导出需要 4K 或 7K，且可击打物件数量须为 1～100000");
        return false;
    }
    if ( myO2 && !validateMyO2Notes(map) ) {
        XERROR("MyO2 兼容导出需要有效且同轨不重叠的普通音符和长条");
        return false;
    }
    // MyO2 的视频入口只读取 MP4，不能用 FF 声明其他容器的文件。
    if ( myO2 && meta.cover_type == CoverType::VIDEO &&
         !meta.main_cover_path.empty() &&
         upper(Config::pathToUtf8(meta.main_cover_path.extension())) !=
             ".MP4" ) {
        XERROR("MyO2 视频 BGA 需要 MP4 文件");
        return false;
    }
    // 兼容声明解析出小节前缀和控制事件。只需要标准 BMS 时不应要求来源的 MyO2
    // 属性有效。
    // 保存会借用这些预处理结果到行序列化结束，过程中不再解释原始属性文本。
    const auto myO2Data = myO2 ? myO2ExportData(map) : std::nullopt;
    if ( myO2 && !myO2Data ) {
        XERROR("MyO2 小节比例或控制标记无效");
        return false;
    }
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
    // 传入的内部轨道号始终从零开始。调用者已检查档位，映射仅负责选择协议通道。
    // 自动采样绕过此映射直接走 01，不能把它们算入玩家键数。
    const auto channel = [&](int track) {
        if ( myO2 ) return MYO2_LANES[track];
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
    // 排序向量保存原事件地址以标识真正的首红线，避免同值 BPM
    // 与首红线在后续比较中混淆。
    std::vector<const Timing*> timings;
    for ( const auto& t : map.m_timings ) {
        // 外部协议没有负小节；非法或无限时间不能参与拍位积分。
        // 视觉效果不进入音乐拍位积分，可表达事件仍需验证数值。
        if ( !std::isfinite(t.m_timestamp) || (!myO2 && t.m_timestamp < 0) )
            return false;
        if ( t.m_timingEffect != TimingEffect::BPM &&
             t.m_timingEffect != TimingEffect::STOP &&
             !(myO2 && t.m_timingEffect == TimingEffect::SCROLL) ) {
            // 标准通道不携带视觉滚速；保留 BPM/STOP 的实际音乐时间。
            // 界面预先列出降级差异，独立保存调用也留下明确日志。
            XWARN("BMS 导出：忽略不支持的 timing 类型 {}，位置 {} ms",
                  static_cast<int>(t.m_timingEffect),
                  t.m_timestamp);
            continue;
        }
        // SCROLL 可正、零、负；兼容协议也允许保留时长为零的 STOP。
        if ( !std::isfinite(t.m_timingEffectParameter) || t.m_interpolation ||
             (t.m_timingEffect != TimingEffect::SCROLL &&
              (myO2 && t.m_timingEffect == TimingEffect::STOP
                   ? t.m_timingEffectParameter < 0
                   : t.m_timingEffectParameter <= 0)) )
            return false;
        // 插值段必须先由用户转换，不在此暗中改变其采样定义。
        // 保留同刻事件的稳定顺序，BPM 查询会单独处理优先级。
        timings.push_back(&t);
    }
    // 同刻多个声明仍按输入顺序参与判断。稳定排序不会因名称或类型改变最后生效的速度。
    // 无关视觉事件已经被过滤，积分锚点只来自时间模型可表示的记录。
    std::stable_sort(
        timings.begin(), timings.end(), [](const Timing* a, const Timing* b) {
            return a->m_timestamp < b->m_timestamp;
        });
    // 初始 BPM 与导入器相同，第一条红线之前也有确定的积分速度。
    // 没有红线时才回退参考 BPM，避免把参考值当作所有段的实际 BPM。
    double initial = meta.preference_bpm > 0 ? meta.preference_bpm : 120;
    // 只记录提供头部 BPM 的红线身份，不能按数值过滤后续重复红线。
    const Timing* initialTiming = nullptr;
    for ( const auto* t : timings )
        if ( t->m_timingEffect == TimingEffect::BPM ) {
            initial       = t->m_timingEffectParameter;
            initialTiming = t;
            break;
        }
    // 导出专用前导段只平移谱面起点，MYO2 的 WW 仍跟随音乐事件。
    // 采用统一原点可在没有负小节号的前提下保留起播前的物件与事件。
    double chartOrigin = 0;
    if ( myO2 ) {
        for ( const auto* t : timings )
            chartOrigin = std::min(chartOrigin, t->m_timestamp);
        for ( const auto& note : map.m_noteData.notes )
            chartOrigin = std::min(chartOrigin, note.m_timestamp);
        for ( const auto& hold : map.m_noteData.holds )
            chartOrigin = std::min(chartOrigin, hold.m_timestamp);
        for ( const auto& sample : map.m_audioSamples )
            chartOrigin = std::min(chartOrigin, sample.effectiveTimestamp());
        for ( const auto& cue : myO2Data->m_cues )
            chartOrigin = std::min(chartOrigin, cue.m_time);
    }
    // 拍位映射只积分 BPM；STOP 是同一拍位的等待。预建红线锚点使密集谱面按
    // 对数时间定位每个事件，不必为十万颗音符重复遍历全谱红线。
    struct TempoAnchor {
        double m_time{};  ///< 红线毫秒时间。
        double m_beat{};  ///< 红线不含 STOP 的绝对拍位。
        double m_bpm{};   ///< 从该位置开始生效的 BPM。
    };
    // 首个哨兵保证在可导出的最早事件之前已有速度状态，时间查询不需要为每个来点回退到默认
    // BPM。 仅兼容分支保留理想锚点；标准分支在量化时会更新另一套已编码时间线。
    std::vector<TempoAnchor> idealTempos{ { chartOrigin, 0, initial } };
    if ( myO2 ) {
        for ( const auto* t : timings ) {
            if ( t->m_timingEffect != TimingEffect::BPM ) continue;
            const auto&  previous = idealTempos.back();
            const double beat =
                previous.m_beat +
                (t->m_timestamp - previous.m_time) * previous.m_bpm / 60000;
            idealTempos.push_back(
                { t->m_timestamp, beat, t->m_timingEffectParameter });
        }
    }
    // upper_bound 允许同刻多个锚点，返回的是同刻最后一个已声明速度。下游 STOP
    // 同样使用它。 调用前由统一时间域保证 ms 不早于 chartOrigin，因而 prev
    // 不会越过哨兵。
    const auto idealTempoAt = [&](double ms) -> const TempoAnchor& {
        return *std::prev(std::upper_bound(
            idealTempos.begin(),
            idealTempos.end(),
            ms,
            [](double time, const auto& t) { return time < t.m_time; }));
    };
    const auto idealBeatAt = [&](double ms) {
        const auto& t = idealTempoAt(ms);
        return t.m_beat + (ms - t.m_time) * t.m_bpm / 60000;
    };
    // 使用全谱最低 BPM 将拍位误差约束在 0.01 毫秒内。
    // 毫秒量化的外部谱面通常不落在简单分数上，机器精度会制造巨型零串。
    double minimumBpm = initial;
    for ( const auto* t : timings )
        if ( t->m_timingEffect == TimingEffect::BPM )
            minimumBpm = std::min(minimumBpm, t->m_timingEffectParameter);
    // 一小节四拍，每拍 60000/BPM
    // 毫秒。用最慢速度得出的比例误差保证快段也满足同一时间预算。
    // 变长小节编码时还要按实际小节比例换算；这里以标准四拍长度作为基准。
    const double fractionTolerance = minimumBpm * 0.01 / 240000.0;
    // MyO2 的普通/长条和 BPM 通道只能写入单行；标准 BMS 仍可用稀疏行。
    const bool compactRows =
        !myO2 && bmsProperty(map.m_metadata, "compact_rows") != "0";
    ExportTimeline timeline(initial, fractionTolerance);
    // 目标文件直到全部对象通过验证才打开，失败不截断已有文件。
    // 声明与通道分开收集，最终先输出完整资源表。
    std::ostringstream header, body;
    // 声明和通道使用经典数值语法，运行机器设置不能让 BPM
    // 或小节比例出现本地化分隔符。
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
    // 站点转 XML 的扩展 BPM 定义只接受非指数十进制文本。
    const auto bpmText = [myO2](double value) {
        if ( !myO2 ) return fmt::format("{:.12g}", value);
        auto text = fmt::format("{:.12f}", value);
        text.erase(text.find_last_not_of('0') + 1);
        if ( text.back() == '.' ) text.pop_back();
        return text;
    };
    // 兼容格式的十进制精度固定到 12 位，过小的正初始速度会被文本表示舍成零。
    if ( !std::isfinite(initial) || (myO2 && initial < 1e-12) ) return false;
    const auto player = bmsProperty(map.m_metadata, "myo2_player");
    header << "#PLAYER ";
    // 兼容来源的有效 PLAYER
    // 声明具有优先权。普通导出按单边或双边轨道推导，不受旧兼容字段影响。
    if ( myO2 && integer(player, 10) > 0 )
        header << player;
    else
        header << (!pms && tracks > 8 ? 3 : 1);
    header << "\n#TITLE " << field(meta.title) << "\n#ARTIST "
           << field(meta.artist) << "\n#MAKER " << field(meta.author)
           << "\n#PLAYLEVEL " << field(meta.version) << "\n#BPM "
           << bpmText(initial) << "\n#LNTYPE 1\n";
    const auto bank     = bmsProperty(map.m_metadata, MYO2_KEYSOUND);
    const auto mode     = upper(bmsProperty(map.m_metadata, MYO2_AUDIO_MODE));
    const auto mainPath = meta.song_file_hint.lexically_normal();
    const AudioSampleEvent* main            = nullptr;
    std::size_t             nonemptySamples = 0;
    for ( const auto& sample : map.m_audioSamples ) {
        if ( sample.m_audioResourceId.empty() ) continue;
        ++nonemptySamples;
        if ( !main && !mainPath.empty() &&
             Config::utf8ToPath(sample.m_audioResourceId).lexically_normal() ==
                 mainPath )
            main = &sample;
    }
    // 只计算非空采样引用。空绑定用于静音判定，不应凭它要求提供额外 KEY 音包。
    const auto hasBinding = [](const auto& notes) {
        return std::any_of(notes.begin(), notes.end(), [](const auto& note) {
            return note.m_sampleBinding &&
                   !note.m_sampleBinding->m_audioResourceId.empty();
        });
    };
    const bool hasKeys =
        hasBinding(map.m_noteData.notes) || hasBinding(map.m_noteData.holds);
    // 整曲与附加音源共存时，MYO2 模式要求已有 ZIP；其余使用松散 STANDARD。
    const bool wholeMusic =
        myO2 && (mode == "MYO2" ||
                 (mode.empty() && !mainPath.empty() &&
                  (!bank.empty() || (!hasKeys && nonemptySamples <= 1))));
    // 显式声明不能被自动模式覆盖。对于整曲模式，没有音包只能容纳主音乐自己的触发。
    // 兼容音包内的名字与散装文件是两个命名空间，推导结果决定后续资源声明。
    if ( myO2 ) {
        if ( (!mode.empty() && mode != "MYO2" && mode != "STANDARD") ||
             (wholeMusic &&
              (mainPath.empty() ||
               (bank.empty() &&
                (hasKeys || nonemptySamples > (main ? 1U : 0U))))) ) {
            XERROR("MyO2 整曲模式需要主音乐，额外 Key 音或自动音需 KEY 音包");
            return false;
        }
        header << "#AUDIO_MODE " << (wholeMusic ? "MYO2" : "STANDARD") << '\n';
        if ( wholeMusic )
            header << "#BGM " << field(Config::pathToUtf8Generic(mainPath))
                   << '\n';
        if ( !bank.empty() )
            header << "#KEYSOUND " << field(std::string(bank)) << '\n';
        // RANK/TOTAL 保留来源字段，缺失时采用站点示例的保守默认值。
        const auto rank =
            decimal(bmsProperty(map.m_metadata, "myo2_rank")).value_or(3);
        const auto total =
            decimal(bmsProperty(map.m_metadata, "myo2_total")).value_or(4);
        header << "#RANK " << rank << "\n#TOTAL " << total << '\n';
        bool bga     = bmsProperty(map.m_metadata, "myo2_bga") == "1";
        bool display = bmsProperty(map.m_metadata, "myo2_sp_note") == "1";
        // FF
        // 视频与其余展示事件独立推导开关。谱面显式标记开启时，即使此批没有对应
        // cue 也保留。
        for ( const auto& cue : myO2Data->m_cues ) {
            bga |= cue.m_code == integer("FF", 36);
            display |= cue.m_code != integer("FF", 36);
        }
        if ( bga ) header << "#BGA_MODE 1\n";
        if ( display ) header << "#SP_NOTE 1\n";
        // 变长小节用单独的比例行；它们不参与事件网格压缩。
        for ( std::size_t i = 0; i < myO2Data->m_ratios.size(); ++i )
            if ( myO2Data->m_ratios[i] != 1 )
                body << fmt::format(
                    "#{:03}02:{:.17g}\n", i, myO2Data->m_ratios[i]);
    }
    // 资源号与实际路径去重。保留码即使未启用对应效果也不会发给普通音符。
    std::map<std::string, int> resources;
    int                        nextResource = 0;
    bool                       needsSilence = false;
    // 按完整引用字符串复用编号；编号还承担判定身份，所以静音也需要一个非零字元。
    // 资源分配先检查是否已有编号，已到编号上限时仍能继续引用已登记资源。
    const auto resource = [&](const std::string& name) {
        if ( const auto it = resources.find(name); it != resources.end() )
            return it->second;
        // 空绑定仍需非零判定码；STANDARD 松散音源路径必须指向真实采样。
        // 保留标识在兼容播放器中会触发控制行为，不能发给普通键或自动音。
        // 两位 base36 的最大值为 1295；零保留给空槽，耗尽时由 emit
        // 路径拒绝条目。
        do {
            if ( ++nextResource > 1295 ) return 0;
        } while ( myO2 && (myO2ControlCode(base36Code(nextResource)) ||
                           base36Code(nextResource) == "BB") );
        resources[name] = nextResource;
        if ( !myO2 || !name.empty() )
            header << "#WAV" << base36Code(nextResource) << ' ' << field(name)
                   << '\n';
        // 散装资源引用必须有对应磁盘文件。整曲或有音包时则不创建额外散装静音，避免改变部署内容。
        else if ( !wholeMusic && bank.empty() ) {
            needsSilence = true;
            header << "#WAV" << base36Code(nextResource) << ' '
                   << Config::pathToUtf8(myO2SilencePath(path).filename())
                   << '\n';
        }
        return nextResource;
    };
    // 限制总输出规模，防止高分辨率事件组合产生巨型文本。
    // 此计数在写磁盘之前执行，失败不会留下半份谱面。
    std::size_t outputBytes = 0;
    // MyO2 的转换器对同小节同通道只保留最后一行。
    // 因此普通键、长条端点与 BPM 先聚合；01、09 独立行保留复音和停顿叠加。
    ExportRows rows;
    const auto emitBeat = [&](double beat, int ch, int value) {
        if ( value <= 0 ) return false;
        // 所有协议位置来自统一 BPM 积分，不能把毫秒直接当拍号。
        // 三位小节号只允许 000–999，越界时拒绝而非截断。
        if ( !std::isfinite(beat) || beat < 0 ||
             beat >= (myO2 ? myO2Data->m_starts.back() : 4000) )
            return false;
        // 兼容小节的前缀数组含末尾边界，先用总范围检查保证查找结果是实际小节而非哨兵。
        // 标准小节每四拍推进一次，保持 000 起点与头部 BPM 统一。
        int measure =
            myO2 ? static_cast<int>(std::upper_bound(myO2Data->m_starts.begin(),
                                                     myO2Data->m_starts.end(),
                                                     beat) -
                                    myO2Data->m_starts.begin()) -
                       1
                 : static_cast<int>(beat / 4);
        // 变长小节改变分母对应的真实拍数，不能再按四拍分割同一绝对拍位。
        double fraction = myO2 ? (beat - myO2Data->m_starts[measure]) /
                                     (4 * myO2Data->m_ratios[measure])
                               : beat / 4 - measure;
        // 允许精度范围内的整小节尾差进入下一小节，不生成越界槽位。
        // 两种行写出路径共用规范化，避免聚合与独立事件行边界不同。
        // 标准扩展 BPM 和 STOP
        // 的位置由已编码时轴决定，不能在这里额外把极近尾部的时间点移动。
        // 其他通道的局部近似允许在误差范围内跨到下一小节第一槽。
        if ( (myO2 || (ch != 8 && ch != 9)) &&
             1.0 - fraction <= fractionTolerance ) {
            ++measure;
            fraction = 0;
        }
        // 小节尾部也可能被舍入到哨兵之后，因此需要在规范化之后再做一次上界检查。
        if ( measure >= 1000 ) return false;
        // 键位和 BPM 通道一次写出整小节；保留原拍位供公共分辨率搜索。
        // 不先逐事件量化，防止各自的近似误差在合并时再次累积。
        // 同通道事件共享小节键但仍逐个保留，避免聚合阶段吞掉复音或同刻速度声明。
        // 能否压成单行由最终协议和格点冲突共同决定，此处不消解碰撞。
        rows[{ measure, ch }].emplace_back(fraction, value);
        return true;
    };
    // 判定和 BGM 共用校正后的音乐时轴，不能分别使用两套拍位积分。
    // 兼容模式保留既有理想拍位，再按明确选择的有损网格处理。
    const auto emit = [&](double ms, int ch, int value) {
        if ( !std::isfinite(ms) || ms < chartOrigin ) return false;
        return emitBeat(
            myO2 ? idealBeatAt(ms) : timeline.beatAt(ms, initial), ch, value);
    };
    // 扩展 BPM 与 STOP 分属独立编号空间，不与 WAV 资源编号混用。
    // 数值精度与头部一致，避免倒数运算产生的伪小数进入协议声明。
    // 编号引用的是速度定义，而非事件身份；多次切回同一 BPM 可共用定义。
    // 精确比较已验证有限的正值，不通过近似舍入合并不同速度。
    // 保留变速的原时序与编号，只让文件头承担已存在于零点的首条 BPM。
    // 下游定义编号只在该谱面范围内有意义，不能将另一张谱面的编号缓存复用。
    std::map<double, int> bpmDefinitions;
    std::map<double, int> scrollDefinitions;
    std::map<double, int> stopDefinitions;
    int                   stopId = 0;
    double                bpm    = initial;
    for ( const auto* t : timings ) {
        // 先确定实际编码位置，再据此校正后续区间，禁止误差逐段累积。
        // STOP 只登记音乐位置，不把等待时长加进判定物件的拍位。
        const auto beat =
            myO2 ? std::optional<double>{ idealBeatAt(t->m_timestamp) }
                 : timeline.append(
                       t->m_timestamp,
                       t->m_timingEffect == TimingEffect::BPM
                           ? std::optional<double>{ t->m_timingEffectParameter }
                           : std::nullopt);
        // 标准映射可能拒绝无法精确编码的边界；不能改用理想拍位继续，否则后续事件积分仍依据另一套锚点。
        if ( !beat ) return false;
        if ( t->m_timingEffect == TimingEffect::SCROLL ) {
            const auto rate = t->m_timingEffectParameter;
            // 滚速影响显示位置，独立的定义编号允许它与同一拍的音乐 BPM 或 STOP
            // 共存。 首次出现才写定义行，后续相同速率只写通道事件。
            const auto definition = scrollDefinitions.try_emplace(
                rate, static_cast<int>(scrollDefinitions.size()) + 1);
            const int id = definition.first->second;
            if ( id > 1295 ) return false;
            if ( definition.second )
                header << "#SCROLL" << base36Code(id) << ' ' << rate << '\n';
            if ( !emitBeat(*beat, SCROLL_CHANNEL, id) ) return false;
        } else if ( t->m_timingEffect == TimingEffect::BPM ) {
            bpm = t->m_timingEffectParameter;
            if ( myO2 ) {
                // 初始红线已由头部承担；整数变速可用原生十六进制 03。
                if ( t == initialTiming && t->m_timestamp == chartOrigin )
                    continue;
                if ( bpm < 1e-12 ) return false;
                // 直接速度占用两个十六进制字符，边界 255
                // 仍可表达。小数或更快速度转用扩展通道。
                // 进入直写分支后不能再登记 08，否则读取端会收到两次变速。
                if ( bpm <= 255 && bpm == std::floor(bpm) ) {
                    if ( !emitBeat(*beat, 3, static_cast<int>(bpm)) )
                        return false;
                    continue;
                }
            }

            auto definition = bpmDefinitions.find(bpm);
            if ( definition == bpmDefinitions.end() ) {
                // 只有首次遇到的速度消耗编号；满表后仍允许引用已有定义。
                // 容量确实不足时在打开目标前拒绝，并留下可定位的原因。
                if ( bpmDefinitions.size() >= 1295U ) {
                    XERROR(
                        "BMS 导出：不同 BPM 定义超过 {} 种，位置 {} ms，BPM {}",
                        1295,
                        t->m_timestamp,
                        bpm);
                    return false;
                }
                const int  number = static_cast<int>(bpmDefinitions.size()) + 1;
                const auto code   = base36Code(number);
                definition =
                    bpmDefinitions.emplace(bpm, integer(code, 36)).first;
                header << "#BPM" << code << ' ' << bpmText(bpm) << '\n';
            }
            // 零点首红线已写入 #BPM，重复写 08 会在编辑器里叠出第二条红线。
            // 仍建立积分锚点与原定义编号，后续 BPM、STOP 和音符编码不变。
            if ( t == initialTiming && t->m_timestamp == 0 ) continue;
            if ( !emitBeat(*beat, 8, definition->second) ) return false;
        } else {
            // 同刻红线决定 STOP 长度，与导入器及常见 BMS 播放器保持一致。
            // 即使输入 Timing 的同刻顺序不同，先寻找该拍最后的 BPM。
            // 输出标准 1/48 拍单位，而不是写入 MMM 内部等待秒数。
            double stopBpm = myO2 ? idealTempoAt(t->m_timestamp).m_bpm : bpm;
            if ( !myO2 ) {
                for ( const auto* other : timings )
                    if ( other->m_timestamp == t->m_timestamp &&
                         other->m_timingEffect == TimingEffect::BPM )
                        stopBpm = other->m_timingEffectParameter;
            }
            const double units =
                t->m_timingEffectParameter * (stopBpm / 60) * 48;
            // 内部等待时间乘以当前速度后可能溢出，拒绝非有限协议值，避免生成读取端无法解释的定义。
            if ( !std::isfinite(units) ) return false;
            // STOP 值以刻度去重，重复 09 行仍累计等待时长。
            const auto definition =
                stopDefinitions.try_emplace(units, stopId + 1);
            if ( definition.second ) {
                if ( ++stopId > 1295 ) return false;
                header << "#STOP" << base36Code(stopId) << ' ' << units << '\n';
            }
            if ( !emitBeat(*beat, 9, definition.first->second) ) return false;
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
        // 长条头尾在同一玩家键的长条通道写成一对，尾部换用普通键通道会变成独立判定。
        // 单键由 duration
        // 为零选择普通通道；原始长条时长在外层已做严格正值检查。
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
    // WW 是整曲唯一的起播标记，绝不能同时再写一条同文件的 01。
    // 已有主曲事件时保留独立触发位置。旧谱面只有歌曲提示时，整曲起播才回退到零点。
    if ( wholeMusic && !emit(main ? main->effectiveTimestamp() : 0,
                             MYO2_CUE_CHANNEL,
                             integer("WW", 36)) )
        return false;
    if ( myO2 ) {
        for ( const auto& cue : myO2Data->m_cues )
            if ( !emit(cue.m_time, MYO2_CUE_CHANNEL, cue.m_code) ) return false;
        // 歌曲提示可以来自仅登记主音乐、尚未显式放置自动采样的旧谱面。
        if ( !wholeMusic && !mainPath.empty() && !main && bank.empty() &&
             !emit(0, 1, resource(Config::pathToUtf8Generic(mainPath))) )
            return false;
    }
    // 整曲模式先登记 WW，循环跳过同一主事件避免双播；同名的其他触发仍按独立 BGM
    // 表达。 兼容分支没有音源名称的事件无需写入
    // 01，玩家键的空采样则由判定资源号保留。
    for ( const auto& sample : map.m_audioSamples ) {
        if ( myO2 && (sample.m_audioResourceId.empty() ||
                      (wholeMusic && &sample == main)) )
            continue;
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
    // 有损兼容必须明确选择，不能靠删除物件消除吸附冲突。
    if ( myO2Grid &&
         !quantizeMyO2Rows(rows, SCROLL_CHANNEL, MYO2_CUE_CHANNEL) ) {
        XERROR(
            "MyO2 "
            "兼容导出失败：吸附后物件或长条端点重叠，或超出小节范围；请调整冲突"
            "物件后重试");
        return false;
    }
    if ( myO2 ) {
        if ( !validateMyO2Rows(rows,
                               *myO2Data,
                               bpmDefinitions,
                               stopDefinitions,
                               initial,
                               wholeMusic) ) {
            XERROR(
                "MyO2 兼容导出失败：音符重叠、变速位置冲突或演奏时间超出规范");
            return false;
        }
        // 先验证键和变速行再布置控制事件，控制码不参与玩家物件重叠检查。
        // 控制事件最终与普通网格一起序列化，原始 cue 的绝对毫秒不直接写进通道。
        placeMyO2Cues(rows, tracks);
        // 第七轨空着时声明一行空格，MMM 可识别键数，站点仍按选定模式解析。
        if ( tracks == 7 &&
             std::none_of(rows.begin(), rows.end(), [](const auto& row) {
                 return row.first.second == 0x18 || row.first.second == 0x58;
             }) )
            body << "#00018:00\n";
    }
    // SC 为扩展文字通道，其他通道采用固定两位表示，不能直接使用十进制轨道号。
    const auto rowChannel = [](int ch) {
        return ch == SCROLL_CHANNEL ? std::string("SC")
                                    : fmt::format("{:02X}", ch);
    };
    // 单元编码与通道一起确定。MyO2
    // 的直接速度是十六进制，其他资源编号仍须使用相应形式。
    const auto encodeCell = [myO2](int ch, int value) {
        return myO2 ? encodeMyO2Cell(ch, value) : base36Code(value);
    };
    // 兼容位置已严格量化；序列化不能再用毫秒预算近似为别的分母。
    // 否则网格上的点也可能被编码成带亚 tick 残差的离拍位置。
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
        // 音乐时轴已确定实际拍位，序列化不能再次近似这些 BPM/STOP。
        // 判定与 BGM 仍使用局部预算，整体误差由前面的逐段校正约束。
        // 公共行和拆分行共享这一误差限。变长小节比例越大，相同相对偏差造成的时间误差越大。
        // 已确定的音乐时间点使用更严格界限，以免读回后后续小节的积分原点移动。
        const double rowTolerance =
            myO2Grid ||
                    (!myO2 && (position.second == 8 || position.second == 9))
                ? 1e-12
                : fractionTolerance /
                      (myO2 ? myO2Data->m_ratios[position.first] : 1);
        // 从最小可能行宽开始测试所有同通道事件。第一个可行值使常见拍点无需大量填充。
        // 上限控制搜索时间，未命中时保留分母状态供后续判断能否改写为多行。
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
        if ( compactRows || position.second == 1 || position.second == 9 ||
             position.second == SCROLL_CHANNEL ) {
            // 比较成本前仅存整数位置，不提前为每个候选分配大段零串。
            // 选择公共行或独立行只改变文本表示，不改变通道和采样身份。
            // 同一行内真实离拍事件仍按既有误差预算表达，不强制取整拍。
            // 每个候选记录槽、分辨率和资源号，延迟分配直到行策略确定。
            // 字节预算包括固定的行前缀与结尾；即便单槽靠近零点也要计入整行文本。
            std::vector<std::array<int, 3>> sparse;
            std::size_t                     sparseBytes = 0;
            for ( const auto& [fraction, value] : events ) {
                const auto approximation =
                    approximateExportFraction(fraction, rowTolerance);
                // 分母近似失败表示该事件无法在当前预算内表达，不能省略它来满足其他槽。
                if ( !approximation ) return false;
                const int slot       = (*approximation)[0],
                          resolution = (*approximation)[1];
                sparse.push_back({ slot, resolution, value });
                sparseBytes += resolution * 2U + 9U;
            }
            // 同刻红线也保留为独立事件，不能因复用同一编号而在公共行中覆盖。
            // 默认保真允许叠加行；显式兼容模式仍遵循既有单行限制。
            // 相邻检查基于此前稳定时间排序，等拍的速度事件连续存放。
            // 序列化的同拍声明顺序仍供读取器确定最终速度。
            const bool repeatedTiming =
                position.second == 8 &&
                std::adjacent_find(events.begin(),
                                   events.end(),
                                   [](const auto& a, const auto& b) {
                                       return a.first == b.first;
                                   }) != events.end();
            if ( position.second == 1 || position.second == 9 ||
                 position.second == SCROLL_CHANNEL || repeatedTiming ||
                 denominator > 65536 || sparseBytes < denominator * 2U + 9U ) {
                outputBytes += sparseBytes;
                if ( outputBytes > 64U * 1024U * 1024U ) return false;
                // 每个复音或停顿独占一行，制造两个相同位置的记录时仍保留原来的声明顺序。
                // 每槽用两个字符，置换发生在该行已确定的总宽度之内。
                for ( const auto& event : sparse ) {
                    std::string data(event[1] * 2U, '0');
                    data.replace(event[0] * 2U,
                                 2,
                                 encodeCell(position.second, event[2]));
                    body << '#' << std::setfill('0') << std::setw(3) << std::dec
                         << position.first << rowChannel(position.second) << ':'
                         << data << '\n';
                }
                continue;
            }
        }
        if ( denominator > 65536 ) {
            // 首拍平移可能使一组离拍音符无法共用短分母，但仍能按原精度表达。
            // 四舍五入到 D 格时误差至多为 0.5/D，直接求保证精度的公共格点。
            // 不放宽 0.01ms 预算，也不改用多行，保持 MyO2 的单行读取兼容性。
            // 额外加一格避开浮点边界；计算量固定，不扩大逐个分母搜索范围。
            // 该分支求的是共有格点的数学上界，不是继续穷举到高分母。
            // 额外容量检查覆盖固定行开销，后面的格式单行限制仍独立适用。
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
        // 与读取器保持相同行长边界，不能生成只能写出却无法重开的文件。
        // 单行上限是格式回读约束，即使整个谱面尚有文本预算也不能超出。
        if ( denominator > 1'000'000 ) return false;
        outputBytes += denominator * 2U + 9U;
        if ( outputBytes > 64U * 1024U * 1024U ) return false;
        // 先构造空槽串再写有效位置，读取器可根据总字符数解释分辨率，不能删掉行尾空槽。
        // 总预算在此之前已计入本行，防止边界检查发生在大分配之后。
        std::string data(denominator * 2U, '0');
        for ( const auto& [fraction, value] : events ) {
            const int slot =
                static_cast<int>(std::round(fraction * denominator));
            // 边界量化不能生成越过本行的写入；冲突音符也不能被静默覆盖。
            // 同拍 BPM 沿用声明顺序，最后一条定义决定后续积分速度。
            // 即使比例 <
            // 1，乘以分母后的四舍五入仍可能落到第一个越界格；检查实际格点再编码。
            if ( slot < 0 || slot >= denominator ) return false;
            const auto code = encodeCell(position.second, value);
            const auto old  = data.substr(slot * 2U, 2);
            // 相同资源的重复声明可以合并；不同资源争用同一键位无法无损表示。
            // BGM 复音已由独立 01 行表达，不适用玩家键位的冲突处理。
            if ( old != "00" && old != code && position.second != 8 )
                return false;
            // 冲突规则先于覆盖执行。同刻 BPM
            // 允许使用最后的声明，其余通道需已判定等价。
            data.replace(slot * 2U, 2, code);
        }
        body << '#' << std::setfill('0') << std::setw(3) << std::dec
             << position.first << rowChannel(position.second) << ':' << data
             << '\n';
    }
    // 最后一次提交检查文本写入与关闭结果后才替换目标文件。
    // 不声称资源已打包，此入口只写 BMS 及其相对音频引用。
    // 输出流失败通过返回值传播，禁止在无异常构建中引入异常处理。
    // 二进制打开避免平台自动插入换行，结果在各平台一致。
    // 数值流使用经典 locale，系统小数逗号不会污染协议。
    // 静音是已选散装模式中的真实依赖，提交谱面之前确认资源存在。
    // 已有同名的非预期文件必须保留并报告失败，以免修改其他谱面引用的资源。
    if ( needsSilence && !ensureMyO2Silence(myO2SilencePath(path)) ) {
        XERROR("MyO2 静音音源无法写入或目标文件与已有资源冲突");
        return false;
    }
    // 使用谱面共用暂存提交接口，关闭失败要保留原文件。
    // 正文不交给文本模式转换，避免固定协议行分隔被平台再次翻译。
    Internal::BeatmapOutputFile staged(path, std::ios::binary);
    auto&                       output = staged.stream();
    // 分隔行也是部分旧播放器定位主数据区的入口，不能只输出空行。
    output << header.str() << "\n*---------------------- MAIN DATA FIELD\n"
           << body.str();
    // 返回提交结果而非流打开状态，覆盖延迟写出错误和最终替换失败。
    return staged.commit();
}
}  // namespace MMM::Bms
