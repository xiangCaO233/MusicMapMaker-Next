#pragma once

#include "BmsTextEncoding.hpp"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Bms
{
/// @brief BMS 的一条已拆分通道事件，拍位不包含 STOP 等待时间。
struct Event {
    /// @brief 从零开始的四分音符拍位。
    double m_beat{};
    /// @brief 十六进制通道号，资源编号另按三十六进制解释。
    int m_channel{};
    /// @brief 已解码的资源索引或直接 BPM。
    int m_value{};
};

/// @brief 去除字段两侧的 ASCII 空白，不破坏文件名内部空格。
/// @param text 原始字段视图，调用方保持底层行缓冲存活。
/// @return 指向原输入的子视图，不分配新的字符串。
/// @note 本函数不删除字段内部字符，也不处理引号转义。
inline std::string_view trim(std::string_view text)
{
    // 不使用 locale 空白分类，避免高位字节被错误裁掉。
    // 空视图直接返回，不对 npos 做减法。
    const auto first = text.find_first_not_of(" \t\r\n");
    if ( first == std::string_view::npos ) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// @brief 严格解析完整整数；非法编号返回负值而非资源零。
/// @param text 固定宽度字段，空串不表示零。
/// @param base 小节为十进制，通道为十六进制，资源为三十六进制。
/// @return 完整消费时返回数值，否则返回 -1。
/// @note 错误和空事件的区别用于阻止损坏文件被静默接受。
inline int integer(std::string_view text, int base)
{
    // 通道字段固定宽度；允许前缀消费会把损坏指令解释成有效事件。
    // 零是空事件，而负值专门表示解析失败。
    int        value{};
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
               ? value
               : -1;
}

/// @brief 严格解析有限的正十进制数，禁止部分消费及十六进制扩展。
/// @param text 允许指数记法的十进制文本。
/// @return 有限正值；失败时以零表示无效。
/// @note 不允许不完整的指数、额外单位或其他尾随字符。
/// @note 先做字符约束，再做完整消费与范围检查。
inline double positive(std::string_view text)
{
    // macOS 低部署目标也通过项目统一浮点入口解析。
    // 字符白名单阻止平台 strtod 接受十六进制等非 BMS 数值。
    // NaN、Inf、零和负值都不允许进入 BPM 分母。
    text = trim(text);
    if ( text.find_first_not_of("0123456789.eE+-") != std::string_view::npos )
        return 0;
    const auto value = Internal::parseFloatingPrefix(text);
    return value.error == std::errc{} && value.parsedLength == text.size() &&
                   std::isfinite(value.value) && value.value > 0
               ? value.value
               : 0;
}

/// @brief 只规范 ASCII 指令部分，资源路径保留大小写。
/// @param text 指令或扩展名，不应传入整行资源声明。
/// @return 独立字符串，便于存入声明表并脱离 getline 缓冲。
/// @note 不使用依赖全局 locale 的大小写变换。
inline std::string upper(std::string_view text)
{
    // 大小写仅影响指令与扩展名，不能整体转换输入文本。
    // 同一目录中 WAV 文件可能区分大小写。
    std::string result(text);
    for ( auto& ch : result )
        if ( ch >= 'a' && ch <= 'z' ) ch -= 'a' - 'A';
    return result;
}

/// @brief 标准键盘通道到内部轨道；每侧顺序为转盘、五键或七键。
/// @param channel 已去掉长条偏移的通道号。
/// @param seven 是否预留每侧第六、第七颗普通键。
/// @param pms 是否采用九键 PMS 协议。
/// @return 零起始轨道；不属于玩家物件通道时返回 -1。
/// @note 布局由整份文件先行探测，不能随事件顺序改变。
inline int lane(int channel, bool seven, bool pms)
{
    // 通道十位标识玩家，个位标识键位；不是十进制的连续轨道号。
    // 转盘保留独立轨道，避免和第一颗键重叠。
    const int side = (channel >> 4) & 3;
    const int key  = channel & 15;
    // PMS 后四键使用第二侧 22–25，不能套用双人键盘的转盘规则。
    if ( pms ) {
        if ( side == 1 && key >= 1 && key <= 5 ) return key - 1;
        if ( side == 2 && key >= 2 && key <= 5 ) return key + 3;
        return -1;
    }
    // 不可见键、BGA、地雷等其他通道不当作普通可击打 Note。
    // 每侧预留六或八轨，双人第二侧在同一坐标系统追加。
    if ( side != 1 && side != 2 ) return -1;
    const int offset = (side - 1) * (seven ? 8 : 6);
    if ( key == 6 ) return offset;
    if ( key >= 1 && key <= 5 ) return offset + key;
    if ( seven && (key == 8 || key == 9) ) return offset + key - 2;
    return -1;
}

/// @brief 读取传统 BMS/BME/BML/PMS 通道文本。
/// @details STOP 保存等待秒数，不加到后续物件的音乐时间戳；播放层冻结统一时钟。
/// 不支持的控制流与长条编码明确失败，避免把随机分支叠加成错误谱面。
/// @param path 原文件路径，同时作为相对音频引用的来源。
/// @return 完整可索引的模型；打开、解析或配对失败时返回空模型。
/// @note 不读取 WAV 内容；音频由项目资源服务解析和加载。
/// @note 未定义 WAV 的物件保留判定，绑定为空表示静音。
/// @note 初始 BPM 必须存在；不根据音频长度猜测速度。
/// @note 原生时间使用毫秒，暂停参数单独保存为秒。
/// @note 目前只接受确定的通道序列，不执行 RANDOM 条件分支。
/// @warning 用户打开文件的低频路径，会分配、排序和访问磁盘。
inline BeatMap load(const std::filesystem::path& path)
{
    // 文件打开失败由公共加载入口转换为失败结果。
    // 解析过程只构造局部模型，不修改调用者正在编辑的谱面。
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return {};
    const auto size = file.tellg();
    if ( size < 0 || size > 64 * 1024 * 1024 ) return {};
    // 整份文件先统一编码，避免 CP932 文件名中的字节被当作路径分隔符。
    // 长度检查在分配之前执行，损坏或过大的输入不进入转换器。
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if ( !file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) )
        return {};
    // UTF-8 与旧 CP932 在同一入口规范，标题和资源引用使用相同编码。
    // 解码发生在指令识别之前，双字节字符不会被误认成 ASCII 分隔符。
    // 原始文件不重写，后续另存为 BMS 时才输出统一 UTF-8 文本。
    auto decoded = decodeText(std::move(bytes));
    if ( !decoded ) {
        XERROR("Invalid BMS text encoding: expected UTF-8 or CP932");
        return {};
    }
    std::istringstream input(std::move(*decoded));
    // 定义表采用最后声明胜出；资源编号和命令名都先转为 ASCII 大写。
    // 数据行单独保留原有顺序，后续稳定排序保留同拍事件顺序。
    std::map<std::string, std::string> headers;
    std::vector<std::string>           rows;
    std::array<double, 1000>           lengths;
    // 未声明的小节固定四个四分音符；02 通道覆盖的是整小节长度。
    // 长度必须在展开之前确定，否则先出现的数据行会算错位置。
    lengths.fill(4.0);
    bool        seven = false, dual = false;
    std::string line;
    // 先收集声明和小节长度，允许 WAV/BPM/STOP 定义位于数据行之后。
    std::size_t inputBytes = 0;
    while ( std::getline(input, line) ) {
        // 限制累计文本规模，避免合法小节数下塞入无限重复行。
        inputBytes += line.size();
        if ( inputBytes > 64U * 1024U * 1024U ) return {};
        auto text = trim(line);
        if ( text.starts_with("\xEF\xBB\xBF") ) text.remove_prefix(3);
        if ( text.empty() || text.front() != '#' ) continue;
        // 只有固定位置的冒号才是通道行；标题中的冒号仍属于元数据。
        // 小节号按十进制、通道号按十六进制独立解析。
        if ( text.size() >= 7 && text[6] == ':' ) {
            const int measure = integer(text.substr(1, 3), 10);
            const int channel = integer(text.substr(4, 2), 16);
            if ( measure < 0 || measure >= 1000 || channel < 0 ) return {};
            // 02 通道是单个比例文本，而非两字符资源编号序列。
            // 上限限制异常的小节拍位，不让时间积分无限放大。
            if ( channel == 2 ) {
                const double ratio = positive(text.substr(7));
                if ( ratio == 0 || ratio > 1000 ) return {};
                lengths[measure] = ratio * 4;
            } else {
                rows.emplace_back(text);
                // 长条通道减去 0x40 后与普通键共享键位布局。
                // 在展开前探测七键和双人，保证此前的轨道也使用相同偏移。
                const int normalized = channel & 0x3F;
                if ( normalized >= 0x11 && normalized <= 0x29 ) {
                    seven |= (normalized & 15) >= 8;
                    dual |= (normalized >> 4) == 2;
                }
            }
            continue;
        }
        // 指令与参数以首个空格分离，路径内部空格必须保留。
        // 无参数的结束指令不会被当作普通资源定义。
        const auto split = text.find_first_of(" \t");
        const auto key   = upper(text.substr(
            1, split == std::string_view::npos ? text.size() - 1 : split - 1));
        // 当前实现不执行随机条件树；把多个分支同时导入会改变谱面内容。
        // 显式失败使用户能够先在源编辑器导出确定分支。
        if ( key == "RANDOM" || key == "IF" || key == "SWITCH" ||
             key == "SETRANDOM" ) {
            XERROR(
                "BMS control flow requires resolving branches before import: "
                "{}",
                key);
            return {};
        }
        if ( split != std::string_view::npos )
            headers[key] = std::string(trim(text.substr(split + 1)));
    }
    // 基础 BPM 是所有未遇变速事件区间的积分起点。
    // LNTYPE 2 使用持续填充编码，不能按端点成对规则处理。
    const double initialBpm = positive(headers["BPM"]);
    if ( initialBpm == 0 ||
         (headers.contains("LNTYPE") && headers["LNTYPE"] != "1") )
        return {};
    const bool pms    = upper(Config::pathToUtf8(path.extension())) == ".PMS";
    const int  tracks = pms ? 9 : (seven ? 8 : 6) * (dual ? 2 : 1);
    // 前缀和只计算小节绝对拍位，暂不涉及 BPM 和 STOP。
    // 因此变长小节与时间线效果可以独立组合。
    std::array<double, 1001> starts{};
    for ( int i = 0; i < 1000; ++i ) starts[i + 1] = starts[i] + lengths[i];
    // 中间事件只携带拍位、通道、编号，尚未持有任何音频对象。
    // 声明表先完整读取，因此前向引用 WAV、BPM、STOP 都有效。
    // 稳定排序用于同拍事件，不能按资源编号打乱多声部 BGM。
    // BPM 优先级独立于输入行顺序，确保 STOP 单位换算一致。
    // 普通和长条键位使用同一映射，不能把尾部落到另一侧轨道。
    // 事件转换完成前不调用模型 sync，避免索引指向尚在变化的容器。
    // 所有时间积分保持 double，直到音频准备阶段才量化为采样帧。
    // 不把 STOP 等待展开到每个物件上，保留后续编辑该等待的能力。
    // 此结构只在加载期使用，播放时不重复解析文本。
    std::vector<Event> events;
    // 每行独立展开，重复的 01 通道代表复音 BGM，不能按通道覆盖。
    for ( const auto& row : rows ) {
        const auto text    = std::string_view(row);
        const int  measure = integer(text.substr(1, 3), 10);
        const int  channel = integer(text.substr(4, 2), 16);
        const auto data    = trim(text.substr(7));
        if ( data.size() % 2 || data.size() > 2'000'000 ) return {};
        for ( std::size_t i = 0; i < data.size(); i += 2 ) {
            const int value =
                integer(data.substr(i, 2), channel == 3 ? 16 : 36);
            if ( value < 0 ) return {};
            // 单事件数量同样有限，避免大量非零编号耗尽内存。
            if ( events.size() >= 2'000'000 ) return {};
            if ( value )
                events.push_back(
                    { starts[measure] + lengths[measure] * i / data.size(),
                      channel,
                      value });
        }
    }
    // 同拍先更新 BPM，再将 STOP 的 1/48 拍单位换算为秒。
    std::stable_sort(
        events.begin(), events.end(), [](const Event& a, const Event& b) {
            if ( a.m_beat != b.m_beat ) return a.m_beat < b.m_beat;
            return (a.m_channel == 3 || a.m_channel == 8) >
                   (b.m_channel == 3 || b.m_channel == 8);
        });
    // 到此仍未发布结果；任何后续配对错误都会丢弃整份临时模型。
    // 原文件不被改写，导入和原生保存是独立事务。
    BeatMap map;
    auto&   meta  = map.m_baseMapMetadata;
    meta.map_path = path;
    meta.title    = headers["TITLE"];
    meta.name     = meta.title;
    meta.artist   = headers["ARTIST"];
    meta.author   = headers["MAKER"];
    meta.version  = headers["PLAYLEVEL"];
    // 静态封面只记录路径；动态 BGA 不转换成音符或音频事件。
    meta.main_cover_path = Config::utf8ToPath(headers["STAGEFILE"]);
    meta.preference_bpm  = initialBpm;
    meta.track_count     = tracks;
    meta.bgm_track_count = 1;
    double bpm = initialBpm, previousBeat = 0, milliseconds = 0;
    // Timing 的毫秒时间与 BPM 派生字段一并初始化，供已有编辑器使用。
    // STOP 参数使用秒，其他类型仍遵守各自原有单位。
    const auto timing = [&](TimingEffect effect, double value) {
        Timing t;
        t.m_timestamp             = milliseconds;
        t.m_bpm                   = bpm;
        t.m_beat_length           = 60000 / bpm;
        t.m_timingEffect          = effect;
        t.m_timingEffectParameter = value;
        map.m_timings.push_back(std::move(t));
    };
    timing(TimingEffect::BPM, bpm);
    std::array<std::optional<Note>, 16>        longStarts;
    std::array<std::optional<std::size_t>, 16> lastNotes;
    const int  lnobj = integer(headers["LNOBJ"], 36);
    const auto id    = [](int value) {
        constexpr std::string_view digits =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::string result(2, '0');
        result[0] = digits[value / 36];
        result[1] = digits[value % 36];
        return result;
    };
    // 未定义资源保留空采样；时间和物件不能因缺少 WAV 声明而丢失。
    // Key 的绑定覆盖默认打击音，BGM 使用同一资源表但由自动采样触发。
    // 两者共享文件定义不意味着共用播放游标，每次触发仍是独立声音。
    // 保留原始相对路径，使重开 MMM 后仍可在源目录找到文件。
    const auto binding = [&](int value) {
        std::string filename = headers["WAV" + id(value)];
        // Windows 反斜杠路径规范为跨平台分隔符，保留文件名本身。
        // 空绑定仍代表静音物件，不能删除其判定。
        std::replace(filename.begin(), filename.end(), '\\', '/');
        return AudioSampleBinding{ filename, 1.0F };
    };
    for ( const auto& event : events ) {
        milliseconds += (event.m_beat - previousBeat) * 60000 / bpm;
        previousBeat = event.m_beat;
        // 极小 BPM 可能使积分溢出；拒绝非有限时间，不能传给播放调度。
        if ( !std::isfinite(milliseconds) ) return {};
        if ( event.m_channel == 3 || event.m_channel == 8 ) {
            bpm = event.m_channel == 3
                      ? event.m_value
                      : positive(headers["BPM" + id(event.m_value)]);
            if ( bpm <= 0 ) return {};
            timing(TimingEffect::BPM, bpm);
        } else if ( event.m_channel == 9 ) {
            // 同拍 BPM 已由排序优先处理，STOP 使用该拍开始时的有效 BPM。
            // 不增加 milliseconds：播放层另外消耗等待期间的输出帧。
            const double units = positive(headers["STOP" + id(event.m_value)]);
            if ( units <= 0 ) return {};
            const double duration = units * 60 / (48 * bpm);
            if ( !std::isfinite(duration) ) return {};
            timing(TimingEffect::STOP, duration);
        } else if ( event.m_channel == 1 ) {
            // 多条 BGM 即使同拍也全部保留，播放器负责混音。
            // 自动采样放在玩家轨道之后，与 Key 音显式绑定分开。
            AudioSampleEvent sample;
            sample.m_timestamp       = milliseconds;
            sample.m_track           = tracks;
            sample.m_audioResourceId = binding(event.m_value).m_audioResourceId;
            map.m_audioSamples.push_back(std::move(sample));
        } else {
            const bool longChannel =
                event.m_channel >= 0x51 && event.m_channel <= 0x69;
            const int track =
                lane(longChannel ? event.m_channel - 0x40 : event.m_channel,
                     seven,
                     pms);
            if ( track < 0 || track >= tracks ) continue;
            Note note;
            note.m_timestamp = milliseconds;
            note.m_track     = track;
            note.setSampleBinding(binding(event.m_value));
            // 长条端点复用开始点的音效，尾部资源编号不重新触发 Key。
            // 零长度或逆序端点不能转换成有效 Hold。
            // 成对模式在关闭后立即清空状态，下一对可重用相同 WAV 编号。
            // 尾部音效不单独触发；其职责仅是确定 Hold 的结束音乐时间。
            if ( longChannel ) {
                auto& start = longStarts[track];
                if ( !start ) {
                    start = std::move(note);
                    continue;
                }
                Hold hold;
                static_cast<Note&>(hold) = *start;
                hold.m_type              = NoteType::HOLD;
                hold.m_duration          = milliseconds - start->m_timestamp;
                if ( hold.m_duration <= 0 ) return {};
                map.m_noteData.holds.push_back(std::move(hold));
                start.reset();
            } else if ( event.m_value == lnobj ) {
                if ( !lastNotes[track] ) return {};
                // 只允许关闭该轨最近的普通头，不跨轨寻找候选。
                // 稍后统一清除旧头，保持此前记录的索引有效。
                auto& start = map.m_noteData.notes[*lastNotes[track]];
                Hold  hold;
                static_cast<Note&>(hold) = start;
                hold.m_type              = NoteType::HOLD;
                hold.m_duration          = milliseconds - start.m_timestamp;
                if ( hold.m_duration <= 0 ) return {};
                map.m_noteData.holds.push_back(std::move(hold));
                // 标记旧头，统一清除，避免 deque 索引在转换时失效。
                start.m_isSubNote = true;
                lastNotes[track].reset();
            } else {
                lastNotes[track] = map.m_noteData.notes.size();
                map.m_noteData.notes.push_back(std::move(note));
            }
        }
    }
    // 未闭合的长条不能猜测终点；整个导入失败避免静默截断。
    // sync 在所有拥有型容器稳定后建立统一索引和缓存。
    for ( const auto& start : longStarts )
        if ( start ) return {};
    std::erase_if(map.m_noteData.notes,
                  [](const Note& note) { return note.m_isSubNote; });
    // 长度只描述音乐坐标的末端，实际播放墙钟会额外包含 STOP。
    // 不把等待写进 map_length，防止播放器又增加一遍空白区间。
    // 下游音频调度还会扩展有效音源尾部长度。
    meta.map_length = milliseconds;
    map.sync();
    return map;
}
}  // namespace MMM::Bms
