/// @file BmsMyO2Export.h
/// @brief 从源谱面构建符合 MyO2 基础规范的离线导出数据。
/// @details 原模型以毫秒定位音符与效果；扩展事件和 WAV 编号只在导出期间编码。
/// 实际的检查与放置以小节分数为准，同时覆盖变长小节和向下一小节的进位。
/// 此模块只观察保存快照，允许多个不同难度独立生成，不共享项目写入状态。
#pragma once

#include "BmsFormat.hpp"
#include "BmsMyO2Grid.h"
#include "common/platform/DocumentStorage.h"

#include <array>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <system_error>

namespace MMM::Bms
{
/// @brief 量化时先保留控制事件，之后再分配给未占用的普通键位。
/// @details 内部编号不出现在文件中，量化完成后才知道标记的同拍空槽。
/// 将事件暂存于公共通道，允许同拍多个控制标记而不提前占用玩家轨。
inline constexpr int MYO2_CUE_CHANNEL = 0x102;
/// @brief MyO2 两种键数都按从左到右的同一轨序写出。
/// @note 通道 16 是首个可操作键，18 为 7K 的最右键；17 没有玩家语义。
inline constexpr std::array MYO2_LANES{
    0x16, 0x11, 0x12, 0x13, 0x14, 0x15, 0x18
};

/// @brief 谱面内部时刻和对应的 MyO2 控制码。
/// @details 显示标记和视频触发均为全局事件；通道仅用于选择空槽。
struct MyO2Cue {
    double m_time{};  ///< 未经过 STOP 累加的谱面毫秒时间。
    int    m_code{};  ///< 两字符标记的三十六进制编号。
};

/// @brief 一次导出使用的小节拍位和格式专用控制事件。
struct MyO2ExportData {
    std::array<double, 1001> m_starts{};  ///< 拍位起点，末项为允许的拍位上界。
    std::array<double, 1000> m_ratios{};  ///< 小节长度比例，标准小节为 1。
    std::vector<MyO2Cue>     m_cues;      ///< 含有顺序语义的控制事件。
};

/// @brief 从持久化属性还原非默认小节比例及控制事件。
/// @return 数据无效时拒绝整次导出，避免意外修改播放或视觉时序。
/// @param map 固定的导出快照，原始资源路径和对象均只读。
/// @details 属性和位移以文本持久化，避免引入仅某种格式使用的运行时状态。
/// 控制事件以谱面毫秒保存，后续编辑 BPM 再导出时可重新定位实际拍位。
/// 小节比例只影响定位小节，与曲线或 STOP 持续时长独立。
/// 普通小节默认比例为 1；比例数组覆盖三位小节号的完整范围。
inline std::optional<MyO2ExportData> myO2ExportData(const BeatMap& map)
{
    MyO2ExportData data;
    data.m_ratios.fill(1);
    // 首拍对齐在包服务中统一计算；此处只在构建离线标记时应用一次。
    // 一个共同位移能保持多次视频触发之间的精确间隔。
    const auto shiftValue = bmsProperty(map.m_metadata, MYO2_CUE_SHIFT);
    const auto shift =
        shiftValue.empty() ? std::optional<double>{ 0 } : decimal(shiftValue);
    if ( !shift ) return std::nullopt;

    const auto found = map.m_metadata.map_properties.find(MapMetadataType::BMS);
    if ( found != map.m_metadata.map_properties.end() ) {
        // 原生属性采用哈希表，按原导入次序整理同拍控制事件。
        std::map<int, MyO2Cue> cues;
        // 只接受明确的属性前缀。相似的原生元数据不会被写成任意 BMS 指令。
        for ( const auto& [key, value] : found->second ) {
            const std::string_view name(key);
            if ( name.starts_with(MYO2_MEASURE_PREFIX) ) {
                const int measure =
                    integer(name.substr(MYO2_MEASURE_PREFIX.size()), 10);
                const double ratio = positive(value);
                if ( measure < 0 || measure >= 1000 || ratio <= 0 ||
                     ratio > 1000 )
                    return std::nullopt;
                data.m_ratios[measure] = ratio;
            } else if ( name.starts_with(MYO2_CUE_PREFIX) &&
                        name != MYO2_CUE_SHIFT ) {
                const int index =
                    integer(name.substr(MYO2_CUE_PREFIX.size()), 10);
                // 下标控制同拍标记的声明顺序，控制码仅允许协议的四种视觉效果。
                // WW 由主音轨触发，不能用视觉属性指定第二个起播点。
                const auto code = upper(std::string_view(value).substr(0, 2));
                const auto time =
                    value.size() > 3 && value[2] == ' '
                        ? decimal(std::string_view(value).substr(3))
                        : std::nullopt;
                if ( index < 0 || !time || !std::isfinite(*time + *shift) ||
                     code == "WW" || !myO2ControlCode(code) )
                    return std::nullopt;
                cues.emplace(index,
                             MyO2Cue{ *time + *shift, integer(code, 36) });
            }
        }
        for ( const auto& [index, cue] : cues ) data.m_cues.push_back(cue);
    }
    // 由原生视频背景生成一次起播；来源已有 FF 序列时事先保留精确时刻。
    if ( map.m_baseMapMetadata.cover_type == CoverType::VIDEO &&
         !map.m_baseMapMetadata.main_cover_path.empty() &&
         std::none_of(
             data.m_cues.begin(), data.m_cues.end(), [](const auto& cue) {
                 return cue.m_code == integer("FF", 36);
             }) )
        data.m_cues.push_back({ map.m_baseMapMetadata.video_starttime + *shift,
                                integer("FF", 36) });
    // 前缀和给二分定位和量化校验提供同一拍位基准。
    // 使用半开区间，恰好到小节尾的事件由下一小节承接。
    for ( std::size_t i = 0; i < data.m_ratios.size(); ++i ) {
        data.m_starts[i + 1] = data.m_starts[i] + 4 * data.m_ratios[i];
        // 极小的小节不能折叠为同一双精度拍位，否则无法反查所在小节。
        if ( data.m_starts[i + 1] <= data.m_starts[i] ) return std::nullopt;
    }
    return data;
}

/// @brief 把控制码安排在同一拍位的空普通通道槽，不吞掉原有物件。
/// @details FF、WW 及显示控制不占用长条端点的编码空间。
/// @details 满键时使用两条解析路径均能识别的 01 控制槽，保留原拍位。
/// 01 逐事件写独立行，同时触发的自动音源仍可使用相同的时间。
/// @param rows 按目标网格处理的事件表，普通通道受单行约束。
/// @param tracks 已验证为 4 或 7。
/// @details 轨道显示控制作用于全局，物理通道只承载标记所在位置。
/// 需要同时检查 Tap 和之前放入的标记；Hold 独立使用 5x 通道。
/// 同时有 FF 和 HE 时二者分属不同空槽，避免转换器覆盖重复普通行。
inline void placeMyO2Cues(ExportRows& rows, int tracks)
{
    for ( auto it = rows.begin(); it != rows.end(); ) {
        if ( it->first.second != MYO2_CUE_CHANNEL ) {
            ++it;
            continue;
        }
        // 有序 map 的插入不废弃迭代器；完成一行后才移除内部分配通道。
        const int measure = it->first.first;
        for ( const auto& cue : it->second ) {
            // 固定从左至右选择，重复导出同一快照时结果稳定。
            const auto lane = std::find_if(
                MYO2_LANES.begin(), MYO2_LANES.begin() + tracks, [&](int ch) {
                    const auto notes = rows.find({ measure, ch });
                    return notes == rows.end() ||
                           std::none_of(notes->second.begin(),
                                        notes->second.end(),
                                        [&](const auto& note) {
                                            return std::abs(note.first -
                                                            cue.first) < 1e-12;
                                        });
                });
            const int channel = lane == MYO2_LANES.begin() + tracks ? 1 : *lane;
            rows[{ measure, channel }].push_back(cue);
        }
        it = rows.erase(it);
    }
}

/// @brief 在量化前验证原长条与点按，确保成对端点表示的是原始长条。
/// @details 两段重叠长条即使能按顺序两两配对，也会被改成另一种谱面。
/// 允许点按接在尾部，同轨的头与头、头与长条主体不可重合。
/// @param map 尚未转成成对通道端点的原始谱面快照。
/// @return 每轨的时间区间可被同一个有序端点流完整表达时为真。
inline bool validateMyO2Notes(const BeatMap& map)
{
    // 只排序时间对，避免排序拥有型 Note 导致原模型观察引用失效。
    // 同一结构用零长区间表示点按，起始时刻依然要遵守同轨唯一。
    std::array<std::vector<std::pair<double, double>>, 7> lanes;
    const auto append = [&](const Note& note, double duration) {
        if ( note.m_track >= lanes.size() || note.m_isSubNote ||
             !std::isfinite(note.m_timestamp) ||
             !std::isfinite(note.m_timestamp + duration) )
            return false;
        lanes[note.m_track].emplace_back(note.m_timestamp,
                                         note.m_timestamp + duration);
        return true;
    };
    for ( const auto& note : map.m_noteData.notes )
        if ( !append(note, 0) ) return false;
    for ( const auto& hold : map.m_noteData.holds )
        if ( hold.m_duration <= 0 || !append(hold, hold.m_duration) )
            return false;
    // 校验发生在量化前，原来重叠的极短物件不能因吸附变形而通过。
    for ( auto& lane : lanes ) {
        std::sort(lane.begin(), lane.end());
        double start = -std::numeric_limits<double>::infinity(), end = start;
        for ( const auto& [head, tail] : lane ) {
            // 先比较上一头，再比较上一尾；普通键可位于尾上的共同拍位。
            if ( head == start || head < end ) return false;
            start = head;
            end   = tail;
        }
    }
    return true;
}

/// @brief 用量化后的真实位置验证 MyO2 播放器要求的基本数据约束。
/// @details 时间积分包含每拍累计 STOP，音符窗口以 WW 为基准。
/// 允许普通键落在同轨长条尾部；同拍两个头或头尾合并则拒绝。
/// 只读校验量化结果，不能改写拍位以消除冲突。
/// @param rows 写出前按实际网格编码的小节事件。
/// @param grid 将小节分数换算成绝对拍位的变长小节布局。
/// @param tempos 08 定义，键是 BPM 数值，值是序列化编号。
/// @param stops 09 定义，键是 1/48 拍等待单位，值是序列化编号。
/// @param initial 文件头的初始 BPM。
/// @param wholeMusic 整曲模式要求唯一 WW，STANDARD 的基准是第零小节。
/// @return 实际编码结果满足音符冲突、时间范围和端点配对要求时为真。
/// @details 原始重叠由前置检查处理，本任务检查吸附及跨小节进位新产生的碰撞。
/// 必须把小节比例、BPM 和 STOP 同时应用，不能用音乐坐标估算判定时间窗。
inline bool validateMyO2Rows(const ExportRows& rows, const MyO2ExportData& grid,
                             const std::map<double, int>& tempos,
                             const std::map<double, int>& stops, double initial,
                             bool wholeMusic)
{
    // 声明由 writer 先验证编号范围，反向表避免对每个变速和 STOP 线性查找。
    // 03 的值已经是 BPM，读 08 才需要访问此表。
    // 36² 项覆盖全部两位编号；WAV 定义由各自的命名空间管理。
    // 边界上保留同拍原始数值，比较必须发生在积分前才能报告二义性。
    std::array<double, 1296> bpmById{}, stopById{};
    for ( const auto& [value, id] : tempos ) bpmById[id] = value;
    for ( const auto& [value, id] : stops ) stopById[id] = value;

    // 01 与 SC 不改变判定时间，排除它们后只扫描时钟锚点和待验证音符。
    // 小节起始和分数共用同一布局，跨通道的相同拍位仍可按组处理。
    std::vector<Event> events;
    for ( const auto& [key, row] : rows ) {
        if ( key.second == 1 || key.second == SCROLL_CHANNEL ) continue;
        for ( const auto& [fraction, value] : row ) {
            const double beat = grid.m_starts[key.first] +
                                4 * grid.m_ratios[key.first] * fraction;
            events.push_back({ beat, key.second, value });
        }
    }
    // 通道行的序列化顺序与判定顺序独立，不能按小节内的通道号直接积分。
    // 先建立同拍事件组，随后按进入事件组之前的 BPM 计算拍间耗时。
    std::stable_sort(
        events.begin(), events.end(), [](const auto& a, const auto& b) {
            return a.m_beat < b.m_beat;
        });

    // Held 标志跨小节保留，不能在遇到小节头时把还未结束的长条判为缺尾。
    std::array<bool, 7> held{};
    double              bpm = initial, beat = 0, time = 0, origin = 0;
    double first = std::numeric_limits<double>::infinity(), last = -first;
    // first/last 只记录点按和长条头，用于校验站点的音符起始时间限制。
    // 长条尾不参与此窗口，但仍受有效时间和同轨配对规则约束。
    bool seenOrigin = false;
    for ( auto it = events.begin(); it != events.end(); ) {
        const auto   begin    = it;
        const double nextBeat = begin->m_beat;
        while ( it != events.end() && it->m_beat == nextBeat ) ++it;
        // 先计算抵达时刻，先应用新 BPM，再登记该拍结束之后的 STOP。
        time += (nextBeat - beat) * 60 / bpm;
        beat         = nextBeat;
        double pause = 0;
        // 同拍冲突 BPM 的结果取决于通道更新顺序，因此只接受一个有效数值。
        // STOP 先累计刻度，全部同拍音符和标记都使用尚未暂停的抵达时刻。
        std::optional<double> change;
        // 临时标记按拍重置，只有 held 延续至下一拍。
        // 这样同拍点按的合法性不依赖点按行和长条行哪个先展开。
        std::array<bool, 7> started{}, taps{}, holdEndpoints{};
        for ( auto e = begin; e != it; ++e ) {
            if ( e->m_channel == 3 || e->m_channel == 8 ) {
                const double value =
                    e->m_channel == 3 ? e->m_value : bpmById[e->m_value];
                if ( change && *change != value ) return false;
                change = value;
            } else if ( e->m_channel == 9 ) {
                pause += stopById[e->m_value];
            } else if ( e->m_channel == MYO2_CUE_CHANNEL ) {
                // 整曲起播时间在实际停顿之后可能变化，保存它以计算最终判定范围。
                if ( e->m_value == integer("WW", 36) ) {
                    if ( seenOrigin ) return false;
                    seenOrigin = true;
                    origin     = time;
                }
            } else {
                const bool isHold = e->m_channel >= 0x50;
                const auto track =
                    std::find(MYO2_LANES.begin(),
                              MYO2_LANES.end(),
                              e->m_channel - (isHold ? 0x40 : 0));
                if ( track == MYO2_LANES.end() ) return false;
                const auto laneIndex =
                    static_cast<std::size_t>(track - MYO2_LANES.begin());
                if ( isHold ) {
                    // 两个同轨长条端点无法挤入同一编码槽，即使 WAV 编号相同。
                    if ( holdEndpoints[laneIndex] ) return false;
                    holdEndpoints[laneIndex] = true;
                    held[laneIndex]          = !held[laneIndex];
                    started[laneIndex]       = held[laneIndex];
                } else {
                    if ( taps[laneIndex] ) return false;
                    taps[laneIndex] = true;
                }
                if ( !isHold || started[laneIndex] ) {
                    first = std::min(first, time);
                    last  = std::max(last, time);
                }
            }
        }
        // 同拍全部端点完成配对后再查点按。已关闭的头尾允许接一个 Tap。
        // 仍处于 held 的轨道表示这拍已开始或延续一条长条，不能再触发点按。
        for ( std::size_t laneIndex = 0; laneIndex < held.size(); ++laneIndex )
            if ( taps[laneIndex] && held[laneIndex] ) return false;
        bpm = change.value_or(bpm);
        // 本拍更新后的 BPM 决定暂停长度，消耗的时间只影响后续拍位。
        time += pause / 48 * 60 / bpm;
        if ( !std::isfinite(time) ) return false;
    }
    // 整曲模式允许负前导；检查起始时刻的最小与最大值即可覆盖所有物件。
    // 空谱和未配对的尾端均无法进入游戏，保存前由此统一拒绝。
    return seenOrigin == wholeMusic && std::isfinite(first) &&
           first - origin >= -3600 && last - origin <= 3600 &&
           std::none_of(
               held.begin(), held.end(), [](bool active) { return active; });
}

/// @brief 将标准 BMS 中区分通道行的规则应用到 MyO2 的资源码。
/// @note 03 为十六进制 BPM，其余资源编号为三十六进制。
/// @param channel 两字符通道的内部编号。
/// @param value 已经过边界检查的 BPM、采样编号或保留码。
/// @return 可直接替换 00 格位的两字符 ASCII 文本。
inline std::string encodeMyO2Cell(int channel, int value)
{
    if ( channel == 3 ) return fmt::format("{:02X}", value);
    auto code = base36Code(value);
    // 控制码使用小写示例写法；其他通道可能合法复用同一数字作为声明编号。
    if ( (channel == 1 || (channel >= 0x11 && channel <= 0x18)) &&
         myO2ControlCode(code) )
        for ( char& c : code ) c += 'a' - 'A';
    return code;
}

/// @brief 写出或复用一份真实 PCM 静音采样。
/// @details STANDARD 的松散音源加载要求所有使用中的 WAV 码有可解码文件。
/// 同名非本程序生成的文件保留，避免静音采样覆盖用户已有资源。
/// @warning 只有完整谱面验证通过后的低频导出路径调用。
/// @param path 当前目标谱面同目录的预留文件路径。
/// @return 静音有效并已提交到目标系统文档时为真。
/// @details 标准松散模式会预解码所有使用中的编号；空 WAV
/// 声明会导致整个谱面加载失败。
/// 文件供默认静音判定占位，键音和已开始的音乐继续按自身的声道播放。
inline bool ensureMyO2Silence(const std::filesystem::path& path)
{
    // 48 kHz 单声道 16 位 PCM，五毫秒静音；数组尾部以零初始化。
    // RIFF 长度包含文件大小减去开头八字节；data 长度是 240 个 16 位采样。
    // 文件不依赖运行时编解码器，所有平台生成完全相同的内容。
    static constexpr std::array<unsigned char, 524> wav{
        'R', 'I', 'F', 'F', 4,  2, 0,   0,   'W', 'A', 'V', 'E', 'f', 'm', 't',
        ' ', 16,  0,   0,   0,  1, 0,   1,   0,   128, 187, 0,   0,   0,   119,
        1,   0,   2,   0,   16, 0, 'd', 'a', 't', 'a', 224, 1,   0,   0
    };
    std::error_code ec;
    // 复用前必须比较内容；目标路径存在不等于它是本程序可使用的静音文件。
    // 同名用户资源若内容不同会保留，由导出失败提示重新选择目录。
    if ( std::filesystem::exists(path, ec) ) {
        if ( std::filesystem::file_size(path, ec) != wav.size() || ec )
            return false;
        std::array<unsigned char, wav.size()> saved{};
        std::ifstream                         input(path, std::ios::binary);
        if ( !input.read(reinterpret_cast<char*>(saved.data()), saved.size()) ||
             saved != wav )
            return false;
    } else {
        if ( ec ) return false;
        // 不覆盖模式处理检查后并发创建的情况；流关闭后才确认磁盘写入成功。
        std::ofstream output(path, std::ios::binary | std::ios::noreplace);
        output.write(reinterpret_cast<const char*>(wav.data()), wav.size());
        output.close();
        if ( !output ) return false;
    }
    // 必须等本地写入完成，文件提供器才可同步完整的 RIFF 内容。
    // 复用已有相同文件也重新提交，以覆盖先前同步未完成的场景。
    std::string error;
    // Android
    // 的文件系统工作副本可能仍需经提供器发布，普通目录由平台层直接确认。
    return Common::publishDocument(path, error);
}
}  // namespace MMM::Bms
