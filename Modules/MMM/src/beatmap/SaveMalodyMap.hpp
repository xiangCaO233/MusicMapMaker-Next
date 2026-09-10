#pragma once

#include "MalodyVolume.h"

#include "mmm/beatmap/MalodyMode.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatFraction.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/Timing.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM
{

using json = nlohmann::json;

/// @file SaveMalodyMap.hpp
/// @brief 将统一 BeatMap 模型投影为 Malody `.mc` JSON。
///
/// 导出器支持 Malody Key 模式 0 与 Slide 模式 7。其他模式的物件语义尚无
/// 无损映射，因此在创建输出文件前明确失败，不能把未知模式静默改成键盘谱。
///
/// 顶层写出职责：
/// - `meta` 保存曲目信息、模式与来源扩展；
/// - `time` 保存 BPM 红线和 delay；
/// - `effect` 保存 scroll、jump 与 hs；
/// - `note` 同时保存 SOUND 自动采样和玩家物件；
/// - 原始顶层 `extra` 从 Malody 私有元数据恢复。
///
/// mode 选择规则：
/// - 默认按 Slide 模式 7；
/// - 优先恢复 MapMetadataType::MALODY 中的 mode；
/// - mode 文本必须是去除 ASCII 空白后的完整十进制整数；
/// - 解析失败直接返回 false；
/// - 仅 0 与 7 进入后续写出；
/// - Key 模式写 `free=0`；
/// - Slide 模式写 `free=1`；
/// - Key 模式确保 mode_ext.column 存在。
///
/// meta 公共字段映射：
/// - author 写入 creator；
/// - 空难度名回退为 `default`；
/// - 主背景与封面只写文件名；
/// - title、title_unicode 写入 title、titleorg；
/// - artist、artist_unicode 写入 artist、artistorg；
/// - song_file_hint 优先作为 song.file；
/// - 缺少显式提示时使用 main_audio_path 的文件名；
/// - preference_bpm 写入 song.bpm；
/// - 未提供来源 id 时写零。
///
/// Malody 私有谱面属性恢复：
/// - initialDelay 与 audioOffset 是旧内部兼容键，不直接写回 meta；
/// - mode_ext 按 JSON 恢复，失败时保留原字符串；
/// - id、preview、mode 优先恢复为整数；
/// - 无法解析的整数保留字符串；
/// - extra 恢复到顶层；
/// - 其他键按 JSON 或原字符串恢复；
/// - 最后重新覆盖 mode 与 free，防止陈旧元数据改变当前导出决策。
///
/// 轨道到 Slide x 坐标的映射与加载器互逆：
/// - Malody 虚拟画布宽度为 256；
/// - 4K 间距 64、中心 31；
/// - 5K 间距 51、中心 25；
/// - 6K 间距 43、中心 21；
/// - 7K 使用 36.5 的网格间距；
/// - 其他键数使用 256 除以轨道数；
/// - 最终坐标四舍五入为整数；
/// - 非正轨道数回退为四轨。
///
/// Flick 与长条宽度编码：
/// - 4K 默认宽度基数为 60；
/// - 5K 默认宽度基数为 50；
/// - 6K 默认宽度基数为 40；
/// - 7K Flick 基数为 30；
/// - 8K Flick 基数为 20；
/// - 其他键数使用网格宽度的最近整数；
/// - Slide Flick 的 w 为基数加绝对跨轨数；
/// - dir 8 表示向左，dir 2 表示向右；
/// - 7K/8K 长条宽度沿用皮肤识别基数。
///
/// Polyline 采样表达限制：
/// - Key 模式会展开折线，无法保留折线根采样绑定；
/// - Slide seg 没有节点级 sound 语义；
/// - 任一折线子节点带采样绑定时拒绝导出；
/// - 拒绝发生在文件创建之前；
/// - 普通 Note、Hold、Flick 的命中绑定可以写为 sound 与 vol；
/// - 自动采样始终写为独立 SOUND note。
///
/// Timing 元数据中的 beat、delay 等值保存为 JSON 文本。读取时同时接受 number
/// 与数字字符串，只返回有限值。这样既能恢复来源格式的精确拍位提示，也不会
/// 让损坏私有字段通过异常或非有限数污染导出时间轴。
///
/// BPM 收集与排序：
/// - 只收集 TimingEffect::BPM；
/// - 按绝对时间稳定排序；
/// - 非 Malody 来源的首 BPM 作为生成拍轴原点；
/// - 有 Malody beat 或 delay 元数据的首 BPM 尝试恢复来源拍轴；
/// - 空 BPM 表由后续转换逻辑使用偏好 BPM 建立安全基线；
/// - 同时间 BPM 的既有顺序保持稳定。
///
/// 主音频采样识别：
/// - sample.audioResourceId 与 song.file 完整值一致时匹配；
/// - 否则比较两者文件名；
/// - 资源提示为空时不识别主音频；
/// - 多个候选时优先统一模型中的规范零点形态；
/// - 同类候选选择有效触发时间绝对值更接近零者；
/// - 普通同名采样不满足时间形态时不会被吞并。
///
/// 规范主音频形态要求 timestamp 近似零且 offsetMs 精确为零。为兼容旧 MMM
/// 加载器，还识别“首 Timing 锚点加回卷整数 offset”的有限形态。旧形态必须：
/// - 首 BPM 保留原 Malody delay；
/// - delay 位于半拍到一拍区间；
/// - sample.timestamp 与首 BPM 时间一致；
/// - effectiveTimestamp 与整数拍边界在 0.51 ms 内一致；
/// - offset 与 delay 减一拍后的整数值一致；
/// - 不满足全部条件时按普通自动采样写出。
///
/// 首 BPM 与主 SOUND 相位编码：
/// - 主采样有效时间减首 BPM 时间得到有符号差；
/// - 差值按首拍长度回卷到非负相位；
/// - 接近零或完整一拍的相位归零；
/// - 可兼容的原 delay 与当前相位一致时优先恢复；
/// - 正相位可能需要为普通内容增加一拍补偿；
/// - 首 BPM 晚于生成拍轴首拍时可插入合成首 BPM；
/// - 主 SOUND offset 与首红线 delay 成对生成；
/// - 非主采样保持自身 timestamp 与 offset 语义。
///
/// 时间到 Malody 拍位的转换：
/// - BPM 时间点分割绝对时间区间；
/// - 每段按对应 beat_length 积分；
/// - 先得到连续拍数，再由 fitMalodyBeatFraction 拟合三元分数；
/// - 普通内容按 malodyContentBeatShift 增加包装进位；
/// - 首 BPM 使用自身规范化原点；
/// - 位于生成首 BPM 之前的采样锚定到原点并把时间差折入 offset；
/// - int64 offset 计算在 long double 中检查上下界；
/// - 溢出时夹取到 int64 边界。
///
/// time 数组写出规则：
/// - 每个 BPM 写 beat 与 bpm；
/// - 首 BPM 按相位决策写 delay；
/// - 来源 Malody Timing 的其他键恢复；
/// - beat 与 bpm 不能被私有元数据覆盖；
/// - 必要时在首项前插入合成 BPM 锚点；
/// - BPM 数组保持时间次序；
/// - 无意义的近零 delay 可省略或规范化。
///
/// effect 数组写出规则：
/// - 全部 Timing 按时间稳定排序；
/// - 同时间 BPM 排在效果之前；
/// - osu! 红线隐式重置 scroll 为 1；
/// - 当前 scroll 非 1 时才显式生成重置事件；
/// - SCROLL 写 scroll；
/// - JUMP 写 jump；
/// - HS 写 hs；
/// - Malody 私有效果字段在排除核心键后恢复；
/// - 空效果数组不写顶层 effect。
///
/// 玩家物件模式差异：
/// - Key 模式使用 column；
/// - Slide 模式使用 x 与 w；
/// - Key Hold 使用绝对 endbeat；
/// - Slide Hold 使用单项相对 seg；
/// - Key Flick 退化为普通 column note；
/// - Slide Flick 使用 dir 与 w；
/// - Key Polyline 展开其中的 Hold 子节点；
/// - Slide Polyline 转换为相对 seg 数组。
///
/// Slide Polyline 清洗：
/// - 先忽略零长度 Hold；
/// - 收集 Hold 与 Flick 为轻量 CleanSeg；
/// - 迭代删除零段；
/// - 只合并时间连续的相邻同类段；
/// - Hold 合并持续时间；
/// - Flick 合并轨道位移；
/// - 删除紧邻同时间 Flick 之前的冗余 Hold；
/// - 合并与删除反复执行到固定点；
/// - 唯一根时刻 Flick 可直接导出 dir；
/// - 无剩余段时退化为普通点击；
/// - 其他情况逐段生成相对 beat 与 x 偏移。
///
/// seg 的拍位相对根物件计算。轨道位置则以折线根 x 为基准，只在偏移非零时
/// 写 seg.x。节点 Malody 私有字段可恢复，但 beat、x 和结构哨兵不能覆盖新计算
/// 结果，保证编辑后的几何而非陈旧来源文本成为权威。
///
/// 自动采样 SOUND 写出：
/// - 恢复未消费的 Malody 私有字段；
/// - beat 始终由当前 timestamp 投影；
/// - Slide 模式 type 写字符串 `SOUND`；
/// - Key 模式 type 写旧兼容数值 1；
/// - sound 写资源标识；
/// - offset 写有符号毫秒；
/// - Key 模式 x 写绝对 BGM 轨道；
/// - Slide 模式不写 x，遵循其 SOUND 语义；
/// - vol 从统一线性音量转换为 Malody 增益百分比；
/// - 空资源采样不进入输出数组。
///
/// note 数组确定性顺序：
/// - 自动采样先按时间、轨道、资源和 offset 稳定排序；
/// - 玩家物件从各类型拥有容器收集；
/// - 折线子节点地址集合用于排除顶层重复；
/// - Key 模式折线只展开 Hold 子节点；
/// - Slide 模式保留折线父对象；
/// - 玩家物件按时间和轨道排序；
/// - SOUND 先写入 note 数组，随后写玩家物件；
/// - 排序视图只保存观察指针，不修改 BeatMap。
///
/// 导出器维持以下不变量：
/// - 不支持的模式和不可表达采样在打开文件前失败；
/// - 来源私有字段不能覆盖当前公共模型计算的核心键；
/// - 所有 JSON 解析都使用无异常路径；
/// - 所有拍位通过统一拟合函数生成；
/// - 主音频相位只在严格识别后成对编码；
/// - 玩家绑定与自动采样保持独立；
/// - 折线子节点不会作为独立顶层对象重复写出；
/// - 输出路径日志使用 UTF-8；
/// - 文件无法打开时返回 false。
///
/// 最小回归验证矩阵：
/// - Key 模式普通 Note 与 Hold；
/// - Slide 模式普通 Note、Hold 与 Flick；
/// - Slide 多段 Polyline；
/// - Key 模式 Polyline 展开；
/// - Polyline 根采样拒绝路径；
/// - Polyline 子节点采样拒绝路径；
/// - 玩家物件 sound/vol 往返；
/// - 多个同时间自动 SOUND 的稳定次序；
/// - Key SOUND 的数值 type 与显式 x；
/// - Slide SOUND 的字符串 type；
/// - 主音频规范零点包装；
/// - 旧版主音频整数 offset 包装；
/// - 普通同名采样不被错误配对；
/// - 首 BPM 正相位内容拍补偿；
/// - 首 BPM 负时间的合成锚点；
/// - 位于生成拍轴以前的自动采样；
/// - 同时间 BPM 与效果排序；
/// - osu! 红线 scroll 重置；
/// - scroll、jump、hs 私有字段恢复；
/// - mode_ext.column 的 Key 模式补全；
/// - meta.free 与 mode 的最终一致性；
/// - 数字字符串 Timing 元数据；
/// - 无效 mode 元数据拒绝路径；
/// - 不支持 mode 拒绝路径；
/// - 4K、5K、6K 坐标中心；
/// - 7K、8K Flick 历史宽度；
/// - 零长度 Hold 与零位移 Flick 清理；
/// - 连续同类段合并；
/// - 唯一 Flick 折线退化为 dir；
/// - 空折线退化为点击；
/// - seg 相对 beat 与 x 偏移；
/// - 自动采样 int64 offset 边界；
/// - 空资源自动采样过滤；
/// - 来源额外 JSON 类型恢复；
/// - 当前核心字段不被旧元数据覆盖；
/// - 写出失败不报告成功。
///
/// 普通与折线真实夹具验证整体往返，MalodyEdgeCaseTest 验证相位、模式、坐标、
/// SOUND 和退化边界，BoundNoteSoundTest 单独验证玩家绑定与自动采样职责隔离。
/// 三类测试必须共同通过，才能覆盖本保存器的主要格式契约。
/// 任何新增 Malody 核心键都应同时定义加载、写出、私有元数据排除和测试路径。

/// @brief 去除 ASCII 空白，用于解析 Malody mode 元数据。
/// @param text 原始字符串视图。
/// @return 去除首尾空白后的视图。
inline std::string_view trimMalodyAsciiWhitespace(std::string_view text)
{
    // 只处理格式允许的 ASCII 空白，不改变元数据中间内容。
    while ( !text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                              text.front() == '\n' || text.front() == '\r') ) {
        text.remove_prefix(1);
    }
    while ( !text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                              text.back() == '\n' || text.back() == '\r') ) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 无异常解析 Malody mode 字符串。
/// @param text mode 元数据文本。
/// @return 成功时返回 mode 整数，否则返回空。
inline std::optional<int> parseMalodyModeValue(std::string_view text)
{
    // from_chars 不依赖区域设置，也不会接受未消费的尾随字符。
    text = trimMalodyAsciiWhitespace(text);
    if ( text.empty() ) return std::nullopt;

    int  value = 0;
    auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 无异常解析 Malody 元数据整数。
/// @param text 元数据文本。
/// @return 成功时返回 64 位整数，否则返回空。
inline std::optional<int64_t> parseMalodyInt64Value(std::string_view text)
{
    // 完整消费约束防止把带单位或小数的文本误写为整数 JSON。
    text = trimMalodyAsciiWhitespace(text);
    if ( text.empty() ) return std::nullopt;

    int64_t value = 0;
    auto    result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 无异常解析 Malody 元数据 JSON。
/// @param text 元数据文本。
/// @return 成功时返回 JSON 值，否则返回空。
inline std::optional<json> parseMalodyJsonValue(std::string_view text)
{
    // allow_exceptions=false 使损坏扩展值转为可检查的 discarded。
    json parsed = json::parse(text.begin(), text.end(), nullptr, false);
    if ( parsed.is_discarded() ) {
        return std::nullopt;
    }
    return parsed;
}

/// @brief 解析 JSON 元数据，失败时保留原始字符串。
/// @param text 元数据文本。
/// @return JSON 值或原始字符串。
inline json parseMalodyJsonOrString(const std::string& text)
{
    // 合法 JSON 恢复原类型，其他文本保持字符串，避免静默丢值。
    if ( auto parsed = parseMalodyJsonValue(text) ) {
        return *parsed;
    }
    return text;
}

/// @brief 判断当前导出器是否支持指定 Malody mode。
/// @param mode Malody 模式编号。
/// @return 支持 key(0) 或 slide(7) 时返回 true。
inline bool isSupportedMalodyExportMode(int mode)
{
    return mode == malodyModeValue(MalodyMode::Key) ||
           mode == malodyModeValue(MalodyMode::Slide);
}

/// @brief 保存谱面为 Malody .mc JSON 文件。
/// @warning 低频导出路径：允许完整遍历谱面数据；位置换算必须以当前时间戳为准。
inline bool saveMalodyMap(const BeatMap& beatMap, std::filesystem::path path)
{
    // 先在内存完成兼容性验证和 JSON 构造，最后才创建目标文件。
    json fileData;

    // 轨道数决定 Slide 坐标、长条宽度和 Flick 距离基数。
    int trackCount = static_cast<int>(beatMap.m_baseMapMetadata.track_count);
    if ( trackCount <= 0 ) trackCount = 4;

    const double defaultXW        = trackCount == 4   ? 64.0
                                    : trackCount == 5 ? 51.0
                                    : trackCount == 6 ? 43.0
                                    : trackCount == 7
                                        ? 36.5
                                        : 256.0 / static_cast<double>(trackCount);
    const int    defaultWW        = trackCount == 4   ? 60
                                    : trackCount == 5 ? 50
                                    : trackCount == 6 ? 40
                                    : trackCount == 7 ? 30
                                    : trackCount == 8
                                        ? 20
                                        : static_cast<int>(std::round(defaultXW));
    const int    defaultLongNoteW = trackCount == 7 || trackCount == 8
                                        ? defaultWW
                                        : static_cast<int>(std::round(defaultXW));
    const int    defaultFlickW    = trackCount == 7   ? 30
                                    : trackCount == 8 ? 20
                                                      : defaultWW;

    /// @brief 将轨道索引转换为 mode 7 的 x 坐标。
    /// @return 256 宽虚拟画布中的最近整数中心坐标。
    auto columnToX = [&](int column) {
        double center = 0.0;
        if ( trackCount == 4 )
            center = 31.0;
        else if ( trackCount == 5 )
            center = 25.0;
        else if ( trackCount == 6 )
            center = 21.0;
        else
            center = defaultXW / 2.0;

        // 坐标取整与加载器最近网格拟合保持互逆。
        return static_cast<int>(
            std::round(static_cast<double>(column) * defaultXW + center));
    };

    // 第一阶段写公共 meta，并在后面恢复来源私有字段。
    const std::string malodyVersion = beatMap.m_baseMapMetadata.version.empty()
                                          ? "default"
                                          : beatMap.m_baseMapMetadata.version;
    auto&             meta          = fileData["meta"];
    meta["creator"]                 = beatMap.m_baseMapMetadata.author;
    meta["version"]                 = malodyVersion;
    meta["background"]              = Config::pathToUtf8(
        beatMap.m_baseMapMetadata.main_cover_path.filename());
    meta["cover"] =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.cover_path.filename());
    meta["id"] = 0;

    // 原始 mode 是格式语义，必须优先于默认 Slide 选择。
    int mode = 7;
    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        if ( it->second.contains("mode") ) {
            auto parsedMode = parseMalodyModeValue(it->second.at("mode"));
            if ( !parsedMode ) {
                XERROR("Failed to save Malody map: invalid mode metadata '{}'",
                       it->second.at("mode"));
                return false;
            }
            mode = *parsedMode;
        }
    }
    // 未知模式不尝试降级，避免生成可打开但玩法错误的谱面。
    if ( !isSupportedMalodyExportMode(mode) ) {
        XERROR(
            "Failed to save Malody map: unsupported mode {}. Only key(0) "
            "and slide(7) are supported.",
            mode);
        return false;
    }
    const bool saveAsKeyMode   = mode == malodyModeValue(MalodyMode::Key);
    const bool saveAsSlideMode = mode == malodyModeValue(MalodyMode::Slide);
    meta["mode"]               = mode;

    // 在构造 time/note 前统一检查 Polyline 采样表达能力。
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        if ( saveAsKeyMode && !polyline.m_subNotes.empty() &&
             polyline.getSampleBinding() ) {
            XERROR(
                "Malody 导出失败：Key 模式会展开 "
                "Polyline，无法保留其根节点采样绑定");
            return false;
        }
        // seg 没有节点级 sound，任一子绑定都会导致信息丢失。
        const auto boundSubNote = std::find_if(
            polyline.m_subNotes.begin(),
            polyline.m_subNotes.end(),
            [](const auto& noteRef) {
                return noteRef.get().getSampleBinding().has_value();
            });
        if ( boundSubNote != polyline.m_subNotes.end() ) {
            XERROR(
                "Malody 导出失败：Polyline 子节点的采样绑定无法由 seg "
                "字段无损表达");
            return false;
        }
    }

    auto& song        = meta["song"];
    song["title"]     = beatMap.m_baseMapMetadata.title;
    song["titleorg"]  = beatMap.m_baseMapMetadata.title_unicode;
    song["artist"]    = beatMap.m_baseMapMetadata.artist;
    song["artistorg"] = beatMap.m_baseMapMetadata.artist_unicode;
    // 显式项目提示保留相对路径；旧 main_audio_path 只取文件名。
    const bool hasExplicitSongFileHint =
        !beatMap.m_baseMapMetadata.song_file_hint.empty();
    const std::filesystem::path& songFileHint =
        hasExplicitSongFileHint ? beatMap.m_baseMapMetadata.song_file_hint
                                : beatMap.m_baseMapMetadata.main_audio_path;
    const std::string songFileValue =
        hasExplicitSongFileHint ? Config::pathToUtf8(songFileHint)
                                : Config::pathToUtf8(songFileHint.filename());
    const std::string songFileNameValue =
        Config::pathToUtf8(Config::utf8ToPath(songFileValue).filename());
    song["file"] = songFileValue;
    song["bpm"]  = beatMap.m_baseMapMetadata.preference_bpm;

    meta["mode_ext"] = json::object();

    // 来源属性先恢复，再由当前模式相关字段覆盖陈旧值。
    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        for ( const auto& [key, val] : it->second ) {
            // 旧内部键已由相位逻辑取代，不属于标准 Malody meta。
            if ( key == "initialDelay" || key == "audioOffset" ) {
                continue;
            }
            if ( key == "mode_ext" ) {
                meta["mode_ext"] = parseMalodyJsonOrString(val);
            } else if ( key == "id" || key == "preview" || key == "mode" ) {
                if ( auto parsedInteger = parseMalodyInt64Value(val) ) {
                    meta[key] = *parsedInteger;
                } else {
                    meta[key] = val;
                }
            } else if ( key == "extra" ) {
                fileData["extra"] = parseMalodyJsonOrString(val);
            } else {
                meta[key] = parseMalodyJsonOrString(val);
            }
        }
    }
    meta["mode"] = mode;
    meta["free"] = saveAsSlideMode ? 1 : 0;
    // Key 模式必须声明 column；已有来源对象中的显式值优先保留。
    if ( saveAsKeyMode ) {
        if ( !meta["mode_ext"].is_object() ) {
            meta["mode_ext"] = json::object();
        }
        if ( !meta["mode_ext"].contains("column") ) {
            meta["mode_ext"]["column"] = trackCount;
        }
    }

    /// @brief 从 Timing 的 Malody 元数据读取有限数值。
    /// @param timing 待读取的 Timing。
    /// @param key 元数据字段名。
    /// @return 字段存在且为有限 JSON 数值时返回其值。
    auto getMalodyTimingNumber =
        [](const Timing&    timing,
           std::string_view key) -> std::optional<double> {
        const auto source = timing.m_metadata.timing_properties.find(
            TimingMetadataType::MALODY);
        if ( source == timing.m_metadata.timing_properties.end() ) {
            return std::nullopt;
        }
        const auto value = source->second.find(key);
        if ( value == source->second.end() ) {
            return std::nullopt;
        }
        // 属性以 JSON 文本保存，读取时恢复 number 或数字字符串。
        const auto parsed = parseMalodyJsonValue(value->second);
        if ( !parsed ) {
            return std::nullopt;
        }
        double number = std::numeric_limits<double>::quiet_NaN();
        if ( parsed->is_number() ) {
            number = parsed->get<double>();
        } else if ( parsed->is_string() ) {
            number = Internal::safeStod(parsed->get_ref<const std::string&>(),
                                        number);
        }
        return std::isfinite(number) ? std::optional<double>{ number }
                                     : std::nullopt;
    };

    // 指针视图用于排序，保持 BeatMap 原 Timing 容器不变。
    std::vector<const Timing*> bpmTimings;
    bpmTimings.reserve(beatMap.m_timings.size());
    for ( const auto& timing : beatMap.m_timings ) {
        if ( timing.m_timingEffect == TimingEffect::BPM ) {
            bpmTimings.push_back(&timing);
        }
    }
    // stable_sort 保留完全同时间 BPM 的来源顺序。
    std::stable_sort(bpmTimings.begin(),
                     bpmTimings.end(),
                     [](const Timing* lhs, const Timing* rhs) {
                         return lhs->m_timestamp < rhs->m_timestamp;
                     });

    /// @brief 判断采样是否对应 Malody 的主音频提示。
    /// @param sample 待判断的自动采样。
    /// @return 资源标识或文件名与 meta.song.file 一致时返回 true。
    auto isMainSongSample = [&](const AudioSampleEvent& sample) {
        // 完整资源标识优先，文件名比较只用于目录前缀兼容。
        if ( songFileValue.empty() ) return false;
        if ( sample.m_audioResourceId == songFileValue ) return true;
        if ( songFileNameValue.empty() ) return false;
        const std::string sampleFileName = Config::pathToUtf8(
            Config::utf8ToPath(sample.m_audioResourceId).filename());
        return sampleFileName == songFileNameValue;
    };

    /// @brief 非 Malody 来源首次投影到 Malody 时使用的拍轴原点。
    const Timing* generatedFirstBpmOrigin = nullptr;
    // 首 BPM 没有 Malody 拍位提示时，当前时间戳成为新拍轴原点。
    if ( !bpmTimings.empty() ) {
        const Timing& firstBpm = *bpmTimings.front();
        const auto    source   = firstBpm.m_metadata.timing_properties.find(
            TimingMetadataType::MALODY);
        if ( source == firstBpm.m_metadata.timing_properties.end() ||
             (!source->second.contains("beat") &&
              !source->second.contains("delay")) ) {
            generatedFirstBpmOrigin = &firstBpm;
        }
    }

    /// @brief 与首红线对应且已规范化到时间零点的主音频采样。
    const AudioSampleEvent* wrappedMainSample = nullptr;
    /// @brief 当前配对采样是否来自旧版 MMM 的锚点加整数 offset 形态。
    bool wrappedMainSampleUsesLegacyShape = false;
    /// @brief 音频零点减首红线时间按首拍长回卷后的非负 Malody 值。
    double wrappedMainOffsetMs = 0.0;
    if ( !bpmTimings.empty() ) {
        const Timing& firstBpm = *bpmTimings.front();
        const double  firstBpmValue =
            firstBpm.m_bpm > 0.0 ? firstBpm.m_bpm : 120.0;
        const double firstBeatLength = 60000.0 / firstBpmValue;
        const auto   originalFirstDelay =
            getMalodyTimingNumber(firstBpm, "delay");

        // 在全部主音频候选中选择最符合规范零点语义的一项。
        for ( const auto& sample : beatMap.m_audioSamples ) {
            const double effectiveTimestamp = sample.effectiveTimestamp();
            if ( !isMainSongSample(sample) ) {
                continue;
            }

            // 新规范形态必须精确位于时间零点。旧版 Loader 曾保存为
            // “首 Timing 锚点 + 回卷后的整数 offset”，只对该可识别形态
            // 保留 0.51 ms 的整数化兼容窗口，避免吞掉用户的细微移动。
            // 规范形态精确表达统一模型歌曲零点，不需要宽松容差。
            const bool normalizedShape =
                std::abs(sample.m_timestamp) <= 1e-6 && sample.m_offsetMs == 0;
            bool legacyShape = false;
            // 旧形态仅在保留了可验证原 delay 时进入兼容判断。
            if ( !normalizedShape && originalFirstDelay &&
                 *originalFirstDelay > firstBeatLength * 0.5 &&
                 *originalFirstDelay <= firstBeatLength + 1e-6 &&
                 std::abs(sample.m_timestamp - firstBpm.m_timestamp) <= 1e-6 ) {
                double originalTimingPhase =
                    std::fmod(-*originalFirstDelay, firstBeatLength);
                if ( originalTimingPhase < 0.0 ) {
                    originalTimingPhase += firstBeatLength;
                }
                if ( std::abs(originalTimingPhase) <= 1e-6 ||
                     std::abs(originalTimingPhase - firstBeatLength) <= 1e-6 ) {
                    originalTimingPhase = 0.0;
                }
                const double legacyWholeBeat =
                    std::round((firstBpm.m_timestamp - originalTimingPhase) /
                               firstBeatLength);
                const bool legacyEffectiveTimeMatches =
                    std::abs(effectiveTimestamp -
                             legacyWholeBeat * firstBeatLength) <= 0.51;
                const std::int64_t roundedDelay = static_cast<std::int64_t>(
                    std::llround(*originalFirstDelay));
                // 检查最近整数附近三值，吸收旧 JSON 到整数 offset 的舍入。
                for ( std::int64_t delta = -1; delta <= 1; ++delta ) {
                    const std::int64_t sourceOffset = roundedDelay + delta;
                    if ( static_cast<double>(sourceOffset) <=
                             firstBeatLength * 0.5 ||
                         std::abs(static_cast<double>(sourceOffset) -
                                  *originalFirstDelay) > 0.51 ) {
                        continue;
                    }
                    const auto legacyOffset = static_cast<std::int64_t>(
                        std::llround(static_cast<double>(sourceOffset) -
                                     firstBeatLength));
                    if ( legacyEffectiveTimeMatches &&
                         sample.m_offsetMs == legacyOffset ) {
                        legacyShape = true;
                        break;
                    }
                }
            }
            if ( !normalizedShape && !legacyShape ) {
                continue;
            }

            const bool currentIsNormalized = wrappedMainSample != nullptr &&
                                             !wrappedMainSampleUsesLegacyShape;
            const bool sameShapeKind = wrappedMainSample != nullptr &&
                                       normalizedShape == currentIsNormalized;
            // 规范候选优先于旧候选；同类按有效时间绝对值选择。
            if ( wrappedMainSample == nullptr ||
                 (normalizedShape && !currentIsNormalized) ||
                 (sameShapeKind &&
                  std::abs(effectiveTimestamp) <
                      std::abs(wrappedMainSample->effectiveTimestamp())) ) {
                wrappedMainSample                = &sample;
                wrappedMainSampleUsesLegacyShape = legacyShape;
            }
        }

        // 配对后将有符号差回卷为 Malody 可写的非负拍内相位。
        if ( wrappedMainSample != nullptr && firstBeatLength > 0.0 ) {
            const double signedOffset =
                wrappedMainSample->effectiveTimestamp() - firstBpm.m_timestamp;
            wrappedMainOffsetMs = std::fmod(signedOffset, firstBeatLength);
            if ( wrappedMainOffsetMs < 0.0 ) {
                wrappedMainOffsetMs += firstBeatLength;
            }
            if ( std::abs(wrappedMainOffsetMs) <= 1e-6 ||
                 std::abs(wrappedMainOffsetMs - firstBeatLength) <= 1e-6 ) {
                wrappedMainOffsetMs = 0.0;
            }
        }
    }

    /// @brief 首个 BPM 在 Malody 拍轴上的规范化拍号。
    double firstBpmOriginBeat = 0.0;
    /// @brief 首 BPM 的非负回卷 delay。
    double firstBpmDelayMs = 0.0;
    /// @brief 配对主音轨在 Malody 中导出的非负 offset。
    std::int64_t wrappedMainExportOffsetMs = 0;
    /// @brief 首红线归一到首拍后，Malody 内容拍轴的整拍补偿。
    std::int64_t malodyContentBeatShift = 0;
    /// @brief 首红线晚于第一拍时是否需要额外生成首拍锚点。
    bool prependSyntheticFirstBpm = false;
    // 第二阶段根据配对结果确定首 BPM 拍号、delay 与内容整拍补偿。
    if ( !bpmTimings.empty() ) {
        const Timing& firstBpm = *bpmTimings.front();
        const double  firstBpmValue =
            firstBpm.m_bpm > 0.0 ? firstBpm.m_bpm : 120.0;
        const double firstBeatLength = 60000.0 / firstBpmValue;

        if ( wrappedMainSample != nullptr ) {
            // 来源 delay 相位一致时优先恢复，减少无意义文本变化。
            firstBpmDelayMs = wrappedMainOffsetMs;
            if ( const auto originalDelay =
                     getMalodyTimingNumber(firstBpm, "delay");
                 originalDelay && *originalDelay >= -1e-6 ) {
                double originalPhase =
                    std::fmod(*originalDelay, firstBeatLength);
                if ( originalPhase < 0.0 ) {
                    originalPhase += firstBeatLength;
                }
                const double phaseDifference =
                    std::abs(originalPhase - wrappedMainOffsetMs);
                const double circularPhaseDifference = std::min(
                    phaseDifference, firstBeatLength - phaseDifference);
                const double phaseTolerance =
                    wrappedMainSampleUsesLegacyShape ? 0.51 : 1e-6;
                if ( circularPhaseDifference <= phaseTolerance &&
                     *originalDelay <= firstBeatLength + 1e-6 ) {
                    firstBpmDelayMs = *originalDelay;
                }
            }
            double timingPhase = std::fmod(-firstBpmDelayMs, firstBeatLength);
            if ( timingPhase < 0.0 ) {
                timingPhase += firstBeatLength;
            }
            if ( std::abs(timingPhase) <= 1e-6 ||
                 std::abs(timingPhase - firstBeatLength) <= 1e-6 ) {
                timingPhase = 0.0;
            }
            // MMM 允许首红线位于负时间；导出 Malody 时，该负相位会
            // 转为非负 delay，配对主 SOUND 必须携带同一个值，不能因
            // 其小于半拍而被清零。非负首红线仍沿用既有半拍规则，
            // 避免游戏端重复应用相位。
            if ( firstBpm.m_timestamp < -1e-6 ||
                 firstBpmDelayMs > firstBeatLength * 0.5 + 1e-6 ) {
                wrappedMainExportOffsetMs =
                    static_cast<std::int64_t>(std::llround(firstBpmDelayMs));
            }
            firstBpmOriginBeat     = 0.0;
            malodyContentBeatShift = static_cast<std::int64_t>(std::llround(
                (firstBpm.m_timestamp + firstBpmDelayMs) / firstBeatLength));
            prependSyntheticFirstBpm =
                firstBpm.m_timestamp < -1e-6 ||
                firstBpm.m_timestamp >= firstBeatLength - 1e-6;
        } else {
            double wholeBeat =
                std::floor(firstBpm.m_timestamp / firstBeatLength);
            double delayMs = firstBpm.m_timestamp - wholeBeat * firstBeatLength;
            if ( std::abs(delayMs) <= 1e-6 ) {
                delayMs = 0.0;
            } else if ( std::abs(delayMs - firstBeatLength) <= 1e-6 ) {
                wholeBeat += 1.0;
                delayMs = 0.0;
            }
            firstBpmOriginBeat = wholeBeat;
            firstBpmDelayMs    = delayMs;
        }
    }

    /// @brief 按 Malody 的逐 Timing delay 锚点将毫秒时间转换为拍号。
    /// @param time 待转换的绝对时间，单位为毫秒。
    /// @return Malody beat 三元数组。
    /// @brief 把绝对毫秒映射到未加内容补偿的 Malody 三元拍位。
    auto timeToBeat = [&](double time) {
        double currentBpm = beatMap.m_baseMapMetadata.preference_bpm > 0
                                ? beatMap.m_baseMapMetadata.preference_bpm
                                : 120.0;
        double lastTime   = 0;
        double lastBeat   = 0;

        if ( !bpmTimings.empty() ) {
            const Timing& firstBpm = *bpmTimings.front();
            lastTime               = firstBpm.m_timestamp;
            lastBeat               = firstBpmOriginBeat;
            if ( firstBpm.m_bpm > 0.0 ) {
                currentBpm = firstBpm.m_bpm;
            }
        }

        for ( const Timing* timing : bpmTimings ) {
            const Timing& t = *timing;
            if ( timing == bpmTimings.front() ) continue;
            if ( t.m_timestamp > time + 1e-4 ) break;

            const double delayMs =
                getMalodyTimingNumber(t, "delay").value_or(0.0);
            lastBeat +=
                (t.m_timestamp - delayMs - lastTime) / (60000.0 / currentBpm);
            lastTime = t.m_timestamp;
            if ( t.m_bpm > 0.0 ) {
                currentBpm = t.m_bpm;
            }
        }
        lastBeat += (time - lastTime) / (60000.0 / currentBpm);

        // 连续拍数统一交给有限分母拟合器，避免各对象自行取整。
        const auto fit = fitMalodyBeatFraction(lastBeat);
        return json::array({ fit.beatIndex, fit.numerator, fit.denominator });
    };

    /// @brief 将普通谱面内容转换到带首拍相位补偿的 Malody 拍轴。
    /// @param time 物件绝对时间，单位为毫秒。
    /// @return 已应用整拍补偿的 Malody beat 三元数组。
    /// @brief 把普通内容时间映射到包含整拍包装补偿的拍位。
    auto timeToMalodyContentBeat = [&](double time) {
        json beat = timeToBeat(time);
        if ( malodyContentBeatShift != 0 ) {
            beat[0] = beat[0].get<std::int64_t>() + malodyContentBeatShift;
        }
        return beat;
    };

    // 计时与效果数据
    json timeArr = json::array();
    if ( prependSyntheticFirstBpm && !bpmTimings.empty() ) {
        const Timing& firstBpm = *bpmTimings.front();
        timeArr.push_back({ { "beat", json::array({ 0, 0, 1 }) },
                            { "bpm", firstBpm.m_bpm },
                            { "delay", firstBpmDelayMs } });
    }
    // 第三阶段分别生成 BPM time 数组和 effect 数组。
    for ( const auto& t : beatMap.m_timings ) {
        if ( t.m_timingEffect == TimingEffect::BPM ) {
            json tj;

            // beat 必须由当前内部时间线重新计算；导入元数据中的旧拍号
            // 只用于诊断，不能覆盖用户移动或变速后的实际位置。
            const bool isFirstBpm =
                !bpmTimings.empty() && &t == bpmTimings.front();
            tj["beat"] = isFirstBpm && !prependSyntheticFirstBpm
                             ? timeToBeat(t.m_timestamp)
                             : timeToMalodyContentBeat(t.m_timestamp);

            tj["bpm"] = t.m_bpm;

            // 恢复 Malody 特有字段
            // 来源私有字段在排除核心 beat/bpm 后恢复。
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                for ( const auto& [key, val] : it->second ) {
                    if ( key != "bpm" && key != "beat" ) {
                        tj[key] = parseMalodyJsonOrString(val);
                    }
                }
            }
            if ( isFirstBpm && !prependSyntheticFirstBpm ) {
                tj["beat"]  = timeToBeat(t.m_timestamp);
                tj["delay"] = firstBpmDelayMs;
            } else if ( isFirstBpm ) {
                // 额外首拍锚点承载歌曲相位；原首红线保留原时间，但不
                // 重复附加 delay。
                tj.erase("delay");
            }
            timeArr.push_back(tj);
        }
    }
    if ( !timeArr.empty() ) {
        fileData["time"] = timeArr;
    }

    bool isOsuSource =
        beatMap.m_metadata.map_properties.find(MapMetadataType::OSU) !=
        beatMap.m_metadata.map_properties.end();

    double currentScroll = -1.0;  // 哨兵值，确保首个 BPM 点必定输出

    // 对计时点排序：相同时间戳时红线(BPM)必须在绿线(SCROLL)之前
    // 确保 scroll=1.0 重置在绿线的 scroll=0.01 覆盖之前输出
    std::vector<const Timing*> sortedTimings;
    sortedTimings.reserve(beatMap.m_timings.size());
    for ( const auto& t : beatMap.m_timings ) {
        sortedTimings.push_back(&t);
    }
    // 同时刻红线先于效果，才能在必要时先生成 scroll 重置。
    std::stable_sort(sortedTimings.begin(),
                     sortedTimings.end(),
                     [](const Timing* a, const Timing* b) {
                         if ( std::abs(a->m_timestamp - b->m_timestamp) > 1e-4 )
                             return a->m_timestamp < b->m_timestamp;
                         // 同一时间：BPM（红线）排在效果之前
                         if ( a->m_timingEffect != b->m_timingEffect ) {
                             return a->m_timingEffect == TimingEffect::BPM;
                         }
                         return false;
                     });

    json effectArr = json::array();
    for ( const Timing* tp : sortedTimings ) {
        const auto& t = *tp;
        if ( t.m_timingEffect == TimingEffect::BPM && isOsuSource ) {
            // osu! 红线隐含 scroll=1，仅在状态变化时显式补事件。
            // OSU 红线隐式将滑条速度重置为 1.0
            // 仅当当前有效 scroll 不等于 1.0 时才需要显式输出
            if ( currentScroll != 1.0 ) {
                json resetEj;

                resetEj["beat"]   = timeToMalodyContentBeat(t.m_timestamp);
                resetEj["scroll"] = 1.0;
                effectArr.push_back(resetEj);
                currentScroll = 1.0;
            }
        }

        if ( t.m_timingEffect == TimingEffect::SCROLL ||
             t.m_timingEffect == TimingEffect::JUMP ||
             t.m_timingEffect == TimingEffect::HS ) {
            json ej;
            ej["beat"] = timeToMalodyContentBeat(t.m_timestamp);

            if ( t.m_timingEffect == TimingEffect::SCROLL ) {
                ej["scroll"] = t.m_timingEffectParameter;
            } else if ( t.m_timingEffect == TimingEffect::JUMP ) {
                ej["jump"] = t.m_timingEffectParameter;
            } else {
                ej["hs"] = t.m_timingEffectParameter;
            }

            if ( t.m_timingEffect == TimingEffect::SCROLL ) {
                currentScroll = ej["scroll"];
            }

            // 恢复 Malody 特有字段
            // 效果私有字段不能覆盖当前类型、参数和拍位。
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                for ( const auto& [key, val] : it->second ) {
                    if ( key != "scroll" && key != "jump" && key != "hs" &&
                         key != "effect" && key != "beat" ) {
                        ej[key] = parseMalodyJsonOrString(val);
                    }
                }
            }
            effectArr.push_back(ej);
        }
    }
    if ( !effectArr.empty() ) {
        fileData["effect"] = effectArr;
    }

    // 第四阶段收集折线子节点地址，避免随后作为顶层物件重复写出。
    std::set<const Note*> subNotePtrs;
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        for ( const auto& subNoteRef : poly.m_subNotes ) {
            subNotePtrs.insert(&subNoteRef.get());
        }
    }

    /// @brief 判断基础物件是否由某个 Polyline 引用为子节点。
    auto isSubNote = [&](const Note& note) {
        return subNotePtrs.count(&note) > 0;
    };

    /// @brief 将单个顶层玩家物件投影为当前 Malody 模式的 JSON。
    auto serializeToMalody = [&](const Note& note) {
        json nj;
        nj["beat"] = timeToMalodyContentBeat(note.m_timestamp);

        if ( saveAsSlideMode ) {
            nj["x"] = columnToX((int)note.m_track);
            // 4K 至 6K 的 Polyline 和 Hold 根节点沿用网格宽度；7K、8K
            // 按皮肤的十位宽度规则固定为 30、20，避免键数误判。
            nj["w"] = (note.m_type == NoteType::POLYLINE ||
                       note.m_type == NoteType::HOLD)
                          ? defaultLongNoteW
                          : defaultWW;
        } else {
            nj["column"] = (int)note.m_track;
        }

        /// @brief 计算 seg 相对根物件的三元拍位。
        auto getRelBeat = [&](double targetTime, const json& rootBeatArr) {
            double rootBeatVal =
                rootBeatArr[0].get<double>() +
                (rootBeatArr[1].get<double>() / rootBeatArr[2].get<double>());
            auto   relBeatArr = timeToMalodyContentBeat(targetTime);
            double relBeatVal =
                relBeatArr[0].get<double>() +
                (relBeatArr[1].get<double>() / relBeatArr[2].get<double>()) -
                rootBeatVal;

            // 相对差值再次拟合，防止两个分数直接相减扩大分母。
            const auto fit = fitMalodyBeatFraction(relBeatVal);
            return json::array(
                { fit.beatIndex, fit.numerator, fit.denominator });
        };

        if ( note.m_type == NoteType::HOLD ) {
            const auto& h = static_cast<const Hold&>(note);

            if ( saveAsSlideMode ) {
                // 普通 Hold 写成单 seg 模式，且 seg 内不包含 w 和 x
                nj["seg"] = json::array();
                json sj;
                sj["beat"] =
                    getRelBeat(h.m_timestamp + h.m_duration, nj["beat"]);
                nj["seg"].push_back(sj);
            } else {
                nj["endbeat"] =
                    timeToMalodyContentBeat(h.m_timestamp + h.m_duration);
            }
        } else if ( note.m_type == NoteType::FLICK ) {
            const auto& f = static_cast<const Flick&>(note);

            if ( saveAsSlideMode ) {
                // Slide 模式：Flick 导出为 dir + w
                nj["dir"] = (f.m_dtrack < 0) ? 8 : 2;
                int wVal  = defaultFlickW + std::abs(f.m_dtrack);
                nj["w"]   = wVal;
            }
            // Key 模式下 Flick 不产生额外字段，作为普通 column note 处理
        } else if ( note.m_type == NoteType::POLYLINE && saveAsSlideMode ) {
            const auto& p = static_cast<const Polyline&>(note);

            // 先投影为轻量段，原节点指针只用于恢复未消费的 seg 私有字段。
            struct CleanSeg {
                /// @brief 段类型，只使用 Hold 或 Flick。
                NoteType type;
                /// @brief 段起始时间。
                double timestamp;
                /// @brief Hold 时长。
                double duration;
                /// @brief 起始轨道。
                int track;
                /// @brief Flick 位移。
                int dtrack;
                /// @brief 来源节点观察指针，不拥有对象。
                const Note* original_sn;
            };
            std::vector<CleanSeg> cleanSubs;
            for ( const auto& subNoteRef : p.m_subNotes ) {
                const Note& sn = subNoteRef.get();
                if ( sn.m_type == NoteType::HOLD ) {
                    double dur = static_cast<const Hold&>(sn).m_duration;
                    if ( dur < 1e-4 ) continue;
                    cleanSubs.push_back({ NoteType::HOLD,
                                          sn.m_timestamp,
                                          dur,
                                          (int)sn.m_track,
                                          0,
                                          &sn });
                } else if ( sn.m_type == NoteType::FLICK ) {
                    cleanSubs.push_back(
                        { NoteType::FLICK,
                          sn.m_timestamp,
                          0.0,
                          (int)sn.m_track,
                          static_cast<const Flick&>(sn).m_dtrack,
                          &sn });
                }
            }

            // 合并可产生新零段或邻接项，因此迭代到固定点。
            bool changed = true;
            while ( changed ) {
                changed = false;

                // 1. 过滤零值
                auto it = std::remove_if(
                    cleanSubs.begin(), cleanSubs.end(), [](const auto& s) {
                        if ( s.type == NoteType::HOLD )
                            return s.duration < 1e-4;
                        if ( s.type == NoteType::FLICK ) return s.dtrack == 0;
                        return false;
                    });
                if ( it != cleanSubs.end() ) {
                    cleanSubs.erase(it, cleanSubs.end());
                    changed = true;
                }

                // 同类段只有首尾时间连续时才可合并。
                if ( cleanSubs.size() > 1 ) {
                    for ( size_t i = 0; i < cleanSubs.size() - 1; ) {
                        auto& curr = cleanSubs[i];
                        auto& next = cleanSubs[i + 1];
                        if ( curr.type == next.type &&
                             std::abs(curr.timestamp + curr.duration -
                                      next.timestamp) < 1e-5 ) {
                            if ( curr.type == NoteType::HOLD ) {
                                curr.duration += next.duration;
                                cleanSubs.erase(cleanSubs.begin() + i + 1);
                                changed = true;
                                continue;
                            } else if ( curr.type == NoteType::FLICK ) {
                                curr.dtrack += next.dtrack;
                                cleanSubs.erase(cleanSubs.begin() + i + 1);
                                changed = true;
                                continue;
                            }
                        }
                        i++;
                    }
                }

                // 紧邻 Flick 前的零偏移 Hold 不增加 Malody seg 几何，移除它。
                if ( cleanSubs.size() > 1 ) {
                    for ( size_t i = 0; i < cleanSubs.size() - 1; ) {
                        auto& curr = cleanSubs[i];
                        auto& next = cleanSubs[i + 1];
                        if ( curr.type == NoteType::HOLD &&
                             next.type == NoteType::FLICK &&
                             std::abs((curr.timestamp + curr.duration) -
                                      next.timestamp) < 1e-5 ) {
                            cleanSubs.erase(cleanSubs.begin() + i);
                            changed = true;
                            continue;
                        }
                        i++;
                    }
                }
            }

            // 唯一且位于根时间的 Flick 用标准 dir 表示，避免无意义 seg 包装。
            bool exportedAsDir = false;
            if ( cleanSubs.size() == 1 ) {
                const auto& s = cleanSubs[0];
                if ( s.type == NoteType::FLICK &&
                     std::abs(s.timestamp - p.m_timestamp) < 1e-5 ) {
                    nj["dir"]     = (s.dtrack < 0) ? 8 : 2;
                    nj["w"]       = defaultFlickW + std::abs(s.dtrack);
                    exportedAsDir = true;
                }
            }

            if ( !exportedAsDir && cleanSubs.empty() ) {
                // 所有子物件被清理后无剩余段，降级为普通点物件
                nj.erase("seg");
                nj.erase("w");
            } else if ( !exportedAsDir ) {
                nj["seg"] = json::array();

                for ( size_t i = 0; i < cleanSubs.size(); ++i ) {
                    const auto& s = cleanSubs[i];

                    double current_time  = s.timestamp;
                    int    current_track = s.track;
                    if ( s.type == NoteType::HOLD ) {
                        current_time += s.duration;
                    } else if ( s.type == NoteType::FLICK ) {
                        current_track += s.dtrack;
                    }

                    json sj;
                    sj["beat"] = getRelBeat(current_time, nj["beat"]);

                    int x_offset = columnToX(current_track) -
                                   columnToX(static_cast<int>(p.m_track));
                    if ( x_offset != 0 ) {
                        sj["x"] = x_offset;
                    }

                    // 恢复未消费 seg 字段，但结构核心键始终由当前模型计算。
                    if ( s.original_sn ) {
                        if ( auto it =
                                 s.original_sn->m_metadata.note_properties.find(
                                     NoteMetadataType::MALODY);
                             it !=
                             s.original_sn->m_metadata.note_properties.end() ) {
                            for ( const auto& [key, val] : it->second ) {
                                if ( key != "beat" && key != "x" &&
                                     key != "original_structure" &&
                                     key != "original_structure_flick" ) {
                                    sj[key] = parseMalodyJsonOrString(val);
                                }
                            }
                        }
                    }

                    nj["seg"].push_back(sj);
                }
            }
        }
        if ( auto it =
                 note.m_metadata.note_properties.find(NoteMetadataType::MALODY);
             it != note.m_metadata.note_properties.end() ) {
            for ( const auto& [key, val] : it->second ) {
                const bool shouldDropWidth = saveAsKeyMode ||
                                             note.m_type == NoteType::FLICK ||
                                             note.m_type == NoteType::NOTE;
                // 排除结构核心键，防止编辑前的私有快照覆盖当前几何。
                if ( key != "beat" && key != "column" && key != "x" &&
                     key != "endbeat" && key != "seg" && key != "dir" &&
                     key != "type" && key != "sound" && key != "vol" &&
                     key != "original_structure" &&
                     key != "original_structure_flick" &&
                     (!shouldDropWidth || key != "w") ) {
                    nj[key] = parseMalodyJsonOrString(val);
                }
            }
        }
        // 玩家绑定独立写为 sound/vol；清空后主动移除旧私有键。
        const auto binding = note.getSampleBinding();
        if ( !binding ) {
            nj.erase("sound");
            nj.erase("vol");
        } else {
            nj["sound"] = binding->m_audioResourceId;
            nj["vol"] = Internal::volumeToMalodyGainPercent(binding->m_volume);
        }
        return nj;
    };

    auto& noteArr = fileData["note"];
    noteArr       = json::array();

    /// @brief 按当前模式序列化 Malody 自动采样对象。
    auto serializeAudioSample = [&](const AudioSampleEvent& sample) {
        // 私有字段先恢复，再由当前播放语义覆盖 beat、sound、offset、x 与 vol。
        json sampleJson;

        if ( auto it = sample.m_metadata.sample_properties.find(
                 SampleMetadataType::MALODY);
             it != sample.m_metadata.sample_properties.end() ) {
            for ( const auto& [key, value] : it->second ) {
                if ( key != "beat" && key != "type" && key != "sound" &&
                     key != "offset" && key != "x" && key != "vol" &&
                     key != "original_x" ) {
                    sampleJson[key] = parseMalodyJsonOrString(value);
                }
            }
        }
        // beat 是 m_timestamp 的格式投影；不能让导入时保留的旧 beat
        // 覆盖编辑器已经移动过的锚点。位于生成拍轴之前的采样锚定到
        // 首 BPM 的规范化拍号，并把时间差折入自身
        // offset，以保持实际播放时刻不变。
        // 默认保持资源内偏移，只有主包装或拍轴前采样需要重新配对。
        std::int64_t exportedOffset = sample.m_offsetMs;
        if ( &sample == wrappedMainSample ) {
            sampleJson["beat"] = timeToBeat(bpmTimings.front()->m_timestamp);
            // 主 SOUND 的 offset 必须与首红线相位匹配：负时间首红线
            // 跟随转正后的 delay；非负首红线沿用既有半拍规则。
            exportedOffset = wrappedMainExportOffsetMs;
        } else if ( generatedFirstBpmOrigin != nullptr &&
                    sample.m_timestamp <
                        generatedFirstBpmOrigin->m_timestamp - 1e-4 ) {
            sampleJson["beat"] =
                timeToMalodyContentBeat(generatedFirstBpmOrigin->m_timestamp);
            const long double adjustedOffset =
                static_cast<long double>(sample.m_offsetMs) +
                static_cast<long double>(sample.m_timestamp) -
                static_cast<long double>(generatedFirstBpmOrigin->m_timestamp);
            const long double minimumOffset = static_cast<long double>(
                std::numeric_limits<std::int64_t>::min());
            const long double maximumOffset = static_cast<long double>(
                std::numeric_limits<std::int64_t>::max());
            if ( adjustedOffset <= minimumOffset ) {
                exportedOffset = std::numeric_limits<std::int64_t>::min();
            } else if ( adjustedOffset >= maximumOffset ) {
                exportedOffset = std::numeric_limits<std::int64_t>::max();
            } else {
                exportedOffset =
                    static_cast<std::int64_t>(std::llround(adjustedOffset));
            }
        } else {
            sampleJson["beat"] = timeToMalodyContentBeat(sample.m_timestamp);
        }

        // Malody Slide 游戏逻辑只识别字符串 SOUND；Key 模式保留数值 1，
        // 兼容 BMS 编辑与既有 Key 谱面。
        // type 形态由模式决定，兼容对应 Malody 编辑器的识别规则。
        if ( saveAsSlideMode ) {
            sampleJson["type"] = "SOUND";
        } else {
            sampleJson["type"] = 1;
        }
        sampleJson["sound"]  = sample.m_audioResourceId;
        sampleJson["offset"] = exportedOffset;
        if ( !saveAsSlideMode ) {
            sampleJson["x"] = sample.m_track;
        }
        sampleJson["vol"] =
            Internal::volumeToMalodyGainPercent(sample.m_volume);
        return sampleJson;
    };

    // 空资源没有可播放身份，过滤后再建立确定输出顺序。
    std::vector<const AudioSampleEvent*> sortedSamples;
    sortedSamples.reserve(beatMap.m_audioSamples.size());
    for ( const auto& sample : beatMap.m_audioSamples ) {
        if ( !sample.m_audioResourceId.empty() ) {
            sortedSamples.push_back(&sample);
        }
    }

    // 复合排序键让同时间多 SOUND 的写出结果可重复。
    std::stable_sort(
        sortedSamples.begin(),
        sortedSamples.end(),
        [](const AudioSampleEvent* lhs, const AudioSampleEvent* rhs) {
            if ( std::abs(lhs->m_timestamp - rhs->m_timestamp) > 1e-6 ) {
                return lhs->m_timestamp < rhs->m_timestamp;
            }
            if ( lhs->m_track != rhs->m_track ) {
                return lhs->m_track < rhs->m_track;
            }
            if ( lhs->m_audioResourceId != rhs->m_audioResourceId ) {
                return lhs->m_audioResourceId < rhs->m_audioResourceId;
            }
            return lhs->m_offsetMs < rhs->m_offsetMs;
        });
    for ( const AudioSampleEvent* sample : sortedSamples ) {
        noteArr.push_back(serializeAudioSample(*sample));
    }

    // 玩家物件同样使用观察指针视图，不复制或修改模型。
    std::vector<const Note*> sortedNotes;
    for ( const auto& n : beatMap.m_noteData.notes )
        if ( !isSubNote(n) ) sortedNotes.push_back(&n);
    for ( const auto& n : beatMap.m_noteData.holds )
        if ( !isSubNote(n) ) sortedNotes.push_back(&n);
    for ( const auto& n : beatMap.m_noteData.flicks )
        if ( !isSubNote(n) ) sortedNotes.push_back(&n);
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        if ( isSubNote(poly) ) continue;
        // Key 模式不能表达 seg，仅展开其中具有时长语义的 Hold 节点。
        if ( saveAsKeyMode && !poly.m_subNotes.empty() ) {
            for ( const auto& subNoteRef : poly.m_subNotes ) {
                const Note& subNote = subNoteRef.get();
                if ( subNote.m_type == NoteType::HOLD ) {
                    sortedNotes.push_back(&subNote);
                }
            }
        } else {
            sortedNotes.push_back(&poly);
        }
    }

    // 时间优先、轨道决胜，保证跨类型容器收集后输出顺序稳定。
    std::sort(sortedNotes.begin(),
              sortedNotes.end(),
              [](const auto& a, const auto& b) {
                  if ( std::abs(a->m_timestamp - b->m_timestamp) > 1e-6 )
                      return a->m_timestamp < b->m_timestamp;
                  return a->m_track < b->m_track;
              });

    for ( const Note* note : sortedNotes ) {
        noteArr.push_back(serializeToMalody(*note));
    }

    // 所有兼容检查和 JSON 构造成功后才打开目标路径。
    std::ofstream ofs(path);
    if ( !ofs.is_open() ) {
        XERROR("Failed to open file [{}] for Malody map write",
               Config::pathToUtf8(path));
        return false;
    }
    // 四空格缩进便于版本控制审阅，语义不依赖排版。
    ofs << fileData.dump(4);
    XINFO("Successfully saved map to {}", Config::pathToUtf8(path));
    return true;
}

}  // namespace MMM
