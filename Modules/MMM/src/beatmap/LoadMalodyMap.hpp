#pragma once

#include "MalodyVolume.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <vector>

using json = nlohmann::json;

namespace MMM
{

/// @file LoadMalodyMap.hpp
/// @brief Malody `.mc` JSON 到统一 BeatMap 模型的兼容加载器。
///
/// Malody 的时间轴以三元拍位 `[整数拍, 分子, 分母]` 表达，物件形状则依赖
/// mode、column、x、endbeat、dir 和 seg 等组合字段。加载器先建立 BPM 拍时
/// 映射，再把每个拍位转换为绝对毫秒，最后构造统一 Timing、Note 与采样对象。
///
/// 顶层数据职责：
/// - `meta` 保存作者、难度、模式、歌曲和轨道信息；
/// - `time` 保存 BPM 红线及可选 delay；
/// - `effect` 保存 scroll、jump 与 hs 效果；
/// - `note` 同时保存玩家物件和 SOUND 自动采样；
/// - `extra` 保存未进入公共模型的顶层扩展值。
///
/// meta 公共字段映射：
/// - `creator` 映射谱师；
/// - `version` 映射难度名称；
/// - `background` 映射主背景资源；
/// - `cover` 映射静态封面资源；
/// - `song.title` 与 `song.titleorg` 映射标题；
/// - `song.artist` 与 `song.artistorg` 映射艺术家；
/// - `song.file` 映射主音频提示；
/// - `song.bpm` 映射偏好 BPM；
/// - `mode_ext.column` 提供 Key 模式声明轨道数。
///
/// Malody 私有元数据保留：
/// - `id`、`preview` 与 `mode` 保存为字符串属性；
/// - `free` 保留整数、字符串或其他 JSON 表示；
/// - mode 7 缺少 free 时按自由坐标模式补 1；
/// - mode 0 缺少 free 时按 Key 模式补 0；
/// - `$ver`、`aimode` 与完整 mode_ext 保存供再次导出；
/// - 顶层 extra 以 JSON 文本保存，不猜测内部结构。
///
/// 拍位三元组转换规则：
/// - 第一项是整数拍索引；
/// - 第二项是当前拍内分子；
/// - 第三项是细分分母；
/// - 绝对拍数为整数拍加分子除以分母；
/// - 数组缺项或分母近零时回退到零拍；
/// - 数字字符串与 JSON number 均可读取；
/// - NaN、无穷与解析失败值使用字段默认值。
///
/// 时间事件先收集为两种中间对象：
/// - RawEvent 保留 BPM 或效果的拍位、参数、类型与原 JSON；
/// - BpmEvent 只保存构造拍时映射所需的 BPM、delay 与来源顺序；
/// - 同拍位时 BPM 排在效果之前；
/// - stable_sort 保留完全同键事件的来源顺序；
/// - 原 JSON 中未被公共字段消费的键写入 Timing 私有元数据。
///
/// time.delay 与主 SOUND 共同编码歌曲零点相位。规范文件可能把主音频节点放在
/// 首条 BPM 的同一拍，并使用零 offset 或与 delay 配对的旧式 offset。加载器
/// 只有在资源身份、拍位和 offset 形态同时匹配时才把该节点识别为主音频包装。
///
/// 主音频包装逆变换：
/// - song.file 与 SOUND.sound 必须路径或文件名一致；
/// - SOUND 拍位必须与首 BPM 拍位一致；
/// - offset 必须近似零或近似首 BPM delay；
/// - 首 delay 为负时不进入该配对路径；
/// - 相位按首拍长度取模并规范到 `[0, beatLength)`；
/// - 接近零或完整一拍的相位归零；
/// - 正相位包装会从后续普通内容拍位移除一拍进位；
/// - 首 BPM 自身保持原拍位，其他 BPM 与效果统一减去进位。
///
/// 轨道数推断同时处理 Key 模式和自由坐标模式：
/// - column 最大值加一提供可玩物件下界；
/// - Key 模式的 mode_ext.column 提供声明下界；
/// - 无 column 时回退到声明轨道数或四轨；
/// - 存在 x 时在 4K 至 9K 候选网格间计算加权拟合误差；
/// - 每个 x 的出现频率作为误差权重；
/// - 只有平均误差小于阈值的候选才替换轨道配置；
/// - 4K、5K、6K 使用历史客户端的特殊网格中心与间距；
/// - 其他键数按 256 宽画布平均分配；
/// - 最终 x 轨道索引夹取到有效玩家轨范围。
///
/// BPM 拍时映射规则：
/// - 首事件以前使用偏好 BPM 或 120 BPM；
/// - 每个 BpmEvent 计算绝对毫秒锚点；
/// - delay 只作用于其对应 BPM 锚点；
/// - 后续区间按前一 BPM 的拍长积分；
/// - 效果事件使用所在拍位的当前 BPM；
/// - 缺少全部 Timing 时在零点生成一个默认 BPM；
/// - 偏好 BPM 缺失时取首个有效 BPM Timing。
///
/// SOUND 自动采样识别兼容两种 type：
/// - 字符串 `"SOUND"`；
/// - 旧版数值 `1`；
/// - SOUND 不参与玩家轨数量统计；
/// - sound 缺失、类型错误或空字符串时跳过；
/// - vol 通过 Malody 百分比约定转换为统一线性音量；
/// - offset 保存为有符号 64 位毫秒；
/// - 主音频包装在统一模型中恢复为时间零、偏移零。
///
/// 自动采样轨道 x 规则：
/// - 整数 x 且不小于玩家轨数时直接作为绝对 BGM 轨；
/// - x 位于玩家区、非整数或越界时迁移到首条 BGM 轨；
/// - 非法原值保存到 Malody 私有 original_x；
/// - 缺少 x 的旧文件从 max(10, 玩家轨数) 开始自动分轨；
/// - 同一实际触发时刻的无 x SOUND 依次占用不同轨；
/// - 触发时刻变化后重新从旧版起始轨展开；
/// - 每次迁移都补足 bgm_track_count；
/// - 非法显式 x 和自动分轨分别产生可读诊断。
///
/// 玩家物件形状映射：
/// - 只有 beat、没有派生字段时构造普通 Note；
/// - endbeat 构造 Hold，持续时间由两拍位的绝对时间差得到；
/// - dir 与 w 构造 Flick，方向 8 为左、2 为右；
/// - seg 描述连续长条与横移组合；
/// - 单 seg 同轨时可直接退化为 Hold；
/// - 单 seg 同时间但跨轨时可直接退化为 Flick；
/// - 其他 seg 组合构造 Polyline。
///
/// Flick 宽度兼容：
/// - w 的个位差值表示跨轨距离；
/// - 基数随 4K、5K、6K、7K、8K 和其他键数变化；
/// - 旧 7K 保存器使用 37 作为基数；
/// - 旧 8K 保存器使用 32 作为基数；
/// - 兼容基数只用于解码距离，不改变最终轨道数；
/// - 负距离夹取为零，方向再决定 dtrack 符号。
///
/// seg 到 Polyline 的转换：
/// - root beat 与每个 seg 相对 beat 相加得到节点拍位；
/// - root x 与 seg.x 相加得到节点绝对横坐标；
/// - 时间前进时先在当前轨生成 Hold；
/// - 同一节点同时跨轨时再在段尾生成 Flick；
/// - 时间未前进但跨轨时只生成瞬时 Flick；
/// - 每个子对象标记 m_isSubNote；
/// - Hold 同时进入 m_subNotes 与 m_subHolds；
/// - Flick 同时进入 m_subNotes 与 m_subFlicks；
/// - Polyline 自身锚定根物件时间与轨道。
///
/// 玩家 note.sound 与 SOUND 自动采样不同。前者附着于可玩物件，加载为
/// AudioSampleBinding；后者加载到 BeatMap::m_audioSamples。两类数据不得因为
/// 字段名相同而合并。玩家物件的 vol 只控制命中采样音量。
///
/// 解析器维持以下不变量：
/// - 文件与 JSON 无效时返回空谱面；
/// - 路径转换失败时保留调用者路径；
/// - 所有动态数值必须有限；
/// - 所有物件轨道最终位于玩家轨范围；
/// - 自动采样轨道最终位于 BGM 区；
/// - 折线节点由 NoteData 类型容器拥有；
/// - Polyline 只保存对子节点的引用；
/// - m_allNotes 只由最终 sync 统一构建；
/// - map_length 覆盖物件尾点与自动采样有效触发位置；
/// - 未消费的来源键保存在对应 Malody Metadata 中。
///
/// 数值默认与边界速查：
/// - song.bpm 非法时先使用 0，随后由 Timing 补足；
/// - time.bpm 非法时使用 120；
/// - time.delay 非法时使用 0 ms；
/// - effect 参数非法时使用 0；
/// - 拍位分母非法时整条拍位回退到零；
/// - mode 缺失时按 Key 模式 0；
/// - mode_ext.column 缺失时由物件推断；
/// - 玩家轨最终至少按回退规则得到四轨；
/// - 玩家物件 column 越界时夹取到最近有效轨；
/// - 自由坐标 x 越界时映射到最近有效轨；
/// - SOUND offset 越界或非法时使用 0；
/// - SOUND vol 非法时按 Malody 零增益解释；
/// - Hold 结束时间早于起点时由现有模型保留差值；
/// - seg 时间倒退不产生负长度 Hold；
/// - Flick 负距离先归零再应用方向；
/// - map_length 只向更晚的有效结束点扩展。
///
/// 字段消费与扩展保留边界：
/// - meta 的公共曲目信息进入 BaseMapMeta；
/// - meta 私有字段进入 MapMetadataType::MALODY；
/// - 顶层 extra 整体保存在谱面私有属性；
/// - time 的 bpm 与 delay 用于时间映射；
/// - effect 的 scroll、jump、hs 用于 Timing；
/// - Timing 其余键进入 TimingMetadataType::MALODY；
/// - SOUND 的 type、sound、offset、x、vol 被公共采样消费；
/// - SOUND 其余键进入 SampleMetadataType::MALODY；
/// - 玩家 note 的形状、时间、轨道、sound、vol 被公共物件消费；
/// - 玩家 note 其余键进入 NoteMetadataType::MALODY；
/// - 原始 beat 会随私有元数据保留以辅助精确写回；
/// - 未知 JSON 值按 dump 文本保存，不转换为错误标量类型。
///
/// 同拍事件顺序约束：
/// - BPM 在同拍效果之前进入 RawEvent；
/// - 多个 BPM 保持 time 数组来源顺序；
/// - 多个效果保持 effect 数组展开顺序；
/// - scroll、jump、hs 按固定调用顺序展开；
/// - stable_sort 不重排完全相同的键；
/// - 首 BPM 的 sourceOrder 在相位逆变换中保持可追踪；
/// - 内容移拍后重新稳定排序 RawEvent；
/// - BpmEvent 的相对顺序只由拍位决定；
/// - 最终 Timing 次序由 BeatMap::sync 规范化。
///
/// 主音频相位识别的拒绝条件：
/// - 没有 BPM 事件；
/// - 没有 note 数组；
/// - 首 BPM delay 为显著负值；
/// - note 不是 SOUND；
/// - sound 不是字符串或为空；
/// - sound 与 song.file 身份不一致；
/// - SOUND 缺少 beat；
/// - SOUND 与首 BPM 不在同一拍；
/// - offset 为负；
/// - offset 既不近似零也不近似 delay；
/// - 拍长无法由正 BPM 计算。
///
/// 只有全部识别条件同时满足才执行包装逆变换。这样普通同名采样不会因为靠近
/// 首拍而被误当作歌曲零点，用户显式编排的 offset 也不会被静默清零。
///
/// 类型容器所有权速查：
/// - NoteData::notes 拥有普通点击及折线普通节点；
/// - NoteData::holds 拥有独立 Hold 及折线 Hold 节点；
/// - NoteData::flicks 拥有独立 Flick 及折线 Flick 节点；
/// - NoteData::polylines 拥有折线父对象；
/// - Polyline::m_subNotes 保留完整节点顺序引用；
/// - Polyline::m_subHolds 提供 Hold 分类引用；
/// - Polyline::m_subFlicks 提供 Flick 分类引用；
/// - m_isSubNote 区分节点与顶层基础物件；
/// - BeatMap::m_allNotes 由 sync 建立跨类型观察视图；
/// - 自动采样由 m_audioSamples 独立拥有，不进入玩家物件容器。
///
/// 加载诊断边界：
/// - 非法显式 SOUND.x 逐项记录迁移原因和原值；
/// - 缺失 x 的旧 SOUND 使用汇总诊断；
/// - 诊断关联当前谱面绝对路径；
/// - 诊断不改变保存路径或自动修复源文件；
/// - 可恢复字段错误不升级为整个文件加载失败；
/// - JSON 根损坏和文件打开失败才返回空谱面；
/// - 调用方负责向用户展示并确认诊断。
///
/// 维护本加载器时，应同步核对 SaveMalodyMap 的拍位拟合、相位包装、坐标网格、
/// SOUND 分轨和 seg 生成规则，并使用普通谱面与折线谱面同时做往返验证。
///
/// 最小回归验证矩阵：
/// - 普通 Key 模式 column 谱面；
/// - 自由坐标 x 谱面；
/// - 同拍 BPM 与 scroll 效果；
/// - 首 BPM 正 delay 与主 SOUND 配对；
/// - 首 BPM 无包装的普通负时间内容；
/// - 单段同轨 Hold 退化；
/// - 单段跨轨 Flick 退化；
/// - 多段 Hold/Flick 折线；
/// - 玩家物件独立命中采样；
/// - 带合法显式 x 的自动采样；
/// - 带非法显式 x 的自动采样；
/// - 多个同时间且缺失 x 的旧自动采样；
/// - 数字字符串形式的 BPM、delay 与 offset；
/// - 7K 和 8K 旧 Flick 宽度基数；
/// - 缺少 time 数组的默认 BPM；
/// - meta.free 的不同 JSON 类型；
/// - 未消费字段的再次写出；
/// - 主音频包装不会吞并普通同名 SOUND；
/// - map_length 覆盖折线最终 Hold 尾点；
/// - bgm_track_count 覆盖最高自动采样轨。
///
/// 这些场景分别由一致性夹具、边界测试和绑定音效测试承担；单一普通谱面通过
/// 不能证明相位、自由坐标或旧 SOUND 分轨路径正确。

/// @brief 无异常读取 Malody JSON 数值，兼容数字与数字字符串。
/// @param value 待读取的 JSON 值。
/// @param defaultValue 类型不兼容或解析失败时使用的默认值。
/// @return 有限的解析结果，失败时返回默认值。
inline double parseMalodyJsonDouble(const json& value, double defaultValue)
{
    // JSON number 仍可能承载非有限浮点，必须在进入时间换算前过滤。
    if ( value.is_number() ) {
        const double number = value.get<double>();
        return std::isfinite(number) ? number : defaultValue;
    }
    // 数字字符串是历史编辑器常见写法，使用无异常解析器兼容。
    if ( value.is_string() ) {
        const double number = Internal::safeStod(
            value.get_ref<const std::string&>(), defaultValue);
        return std::isfinite(number) ? number : defaultValue;
    }
    return defaultValue;
}

/// @brief 无异常读取 Malody JSON 对象中的数值字段。
/// @param object 待读取的 JSON 对象。
/// @param key 字段名称。
/// @param defaultValue 字段缺失或解析失败时使用的默认值。
/// @return 有限的解析结果，失败时返回默认值。
inline double readMalodyJsonDouble(const json& object, const char* key,
                                   double defaultValue)
{
    // 容器或字段缺失时直接回退，不通过 json::value 触发类型转换异常。
    if ( !object.is_object() ) return defaultValue;
    const auto value = object.find(key);
    if ( value == object.end() ) return defaultValue;
    return parseMalodyJsonDouble(*value, defaultValue);
}

/// @brief 无异常读取 Malody JSON 对象中的 64 位整数字段。
/// @param object 待读取的 JSON 对象。
/// @param key 字段名称。
/// @param defaultValue 字段缺失、越界或解析失败时使用的默认值。
/// @return 四舍五入后的 64 位整数或默认值。
inline std::int64_t readMalodyJsonInt64(const json& object, const char* key,
                                        std::int64_t defaultValue)
{
    // 先以浮点兼容历史 JSON，再在明确范围内取最近的整数毫秒。
    const long double value = static_cast<long double>(readMalodyJsonDouble(
        object, key, std::numeric_limits<double>::quiet_NaN()));
    if ( !std::isfinite(value) ||
         value < static_cast<long double>(
                     std::numeric_limits<std::int64_t>::min()) ||
         value > static_cast<long double>(
                     std::numeric_limits<std::int64_t>::max()) ) {
        return defaultValue;
    }
    return static_cast<std::int64_t>(std::llround(value));
}

/// @brief 从 Malody `.mc` JSON 文件加载谱面。
/// @param path 待加载的谱面路径。
/// @return 加载后的谱面；文件或 JSON 无效时返回空谱面。
inline BeatMap loadMalodyMap(std::filesystem::path path)
{
    // 返回对象先保持默认状态，只有确认字段后才逐层覆盖。
    BeatMap beatMap;

    // 获取谱面基本元数据
    BaseMapMeta& basemeta = beatMap.m_baseMapMetadata;
    basemeta.map_path     = path;
    // 绝对路径使伴随资源解析不依赖当前工作目录；失败时保留原路径。
    std::error_code ec;
    if ( basemeta.map_path.is_relative() ) {
        auto abs_path = std::filesystem::absolute(basemeta.map_path, ec);
        if ( !ec ) {
            basemeta.map_path = abs_path;
        }
    }

    XINFO("加载malody谱面路径:{}", Config::pathToUtf8(basemeta.map_path));

    std::ifstream fs{ path };
    if ( !fs.is_open() ) {
        XERROR("无法打开 malody 谱面文件: {}", Config::pathToUtf8(path));
        return {};
    }

    // 先读完整文本再解析，允许 JSON 解析器忽略 UTF-8 BOM。
    json        fileData;
    std::string fileContent((std::istreambuf_iterator<char>(fs)),
                            std::istreambuf_iterator<char>());
    // allow_exceptions=false 将语法错误表示为 discarded，避免异常控制流。
    fileData = json::parse(fileContent, nullptr, false, true);

    if ( fileData.is_discarded() ) {
        XERROR("解析 malody 谱面 JSON 失败，可能存在严重的编码错误: {}",
               Config::pathToUtf8(path));
        return {};
    }

    // 第一阶段提取公共元数据，并同时保留 Malody 专属字段。
    if ( fileData.contains("meta") ) {
        const auto& meta = fileData["meta"];
        basemeta.author  = meta.value("creator", "");
        basemeta.version = meta.value("version", "");
        basemeta.main_cover_path =
            Config::utf8ToPath(meta.value("background", ""));
        basemeta.cover_path = Config::utf8ToPath(meta.value("cover", ""));

        // song 子对象提供资源身份与曲目信息，缺失时保持默认空值。
        if ( meta.contains("song") ) {
            const auto& song        = meta["song"];
            basemeta.title          = song.value("title", "");
            basemeta.title_unicode  = song.value("titleorg", "");
            basemeta.artist         = song.value("artist", "");
            basemeta.artist_unicode = song.value("artistorg", "");
            basemeta.song_file_hint =
                Config::utf8ToPath(song.value("file", ""));
            basemeta.main_audio_path = basemeta.song_file_hint;
            basemeta.preference_bpm  = readMalodyJsonDouble(song, "bpm", 0.0);
        }

        // mode_ext.column 是初始声明，后续仍会结合实际物件校正。
        if ( meta.contains("mode_ext") ) {
            basemeta.track_count = meta["mode_ext"].value("column", 4);
        }

        // 来源专属值以字符串或 JSON 文本保留，供保存器无损恢复。
        auto& malody_props =
            beatMap.m_metadata.map_properties[MapMetadataType::MALODY];
        malody_props["id"]      = std::to_string(meta.value("id", 0));
        malody_props["preview"] = std::to_string(meta.value("preview", 0));
        const int malodyMode    = meta.value("mode", 0);
        malody_props["mode"]    = std::to_string(malodyMode);
        // free 的历史类型不稳定，分别保留整数、字符串和其他 JSON 表示。
        if ( meta.contains("free") ) {
            const auto& free = meta["free"];
            if ( free.is_number_integer() ) {
                malody_props["free"] = std::to_string(free.get<int>());
            } else if ( free.is_string() ) {
                malody_props["free"] = free.get<std::string>();
            } else {
                malody_props["free"] = free.dump();
            }
        } else if ( malodyMode == 7 ) {
            malody_props["free"] = "1";
        } else if ( malodyMode == 0 ) {
            malody_props["free"] = "0";
        }
        if ( meta.contains("$ver") ) {
            malody_props["$ver"] = std::to_string(meta["$ver"].get<int>());
        }
        if ( meta.contains("aimode") ) {
            malody_props["aimode"] = meta["aimode"].get<std::string>();
        }
        if ( meta.contains("mode_ext") ) {
            malody_props["mode_ext"] = meta["mode_ext"].dump();
        }
    }

    // 顶层 extra 不属于公共模型，整体序列化到 Malody 属性中。
    if ( fileData.contains("extra") ) {
        beatMap.m_metadata.map_properties[MapMetadataType::MALODY]["extra"] =
            fileData["extra"].dump();
    }

    /// @brief 把 Malody 三元拍位转换为连续绝对拍数。
    /// @return 数组损坏或分母近零时返回零拍。
    auto beatToDouble = [](const json& b) {
        if ( !b.is_array() || b.size() < 3 ) return 0.0;
        // 分母必须先验证，防止无效拍位把后续整个时间积分污染为无穷。
        const double denominator = parseMalodyJsonDouble(b[2], 1.0);
        if ( std::abs(denominator) <= 1e-9 ) return 0.0;
        return parseMalodyJsonDouble(b[0], 0.0) +
               (parseMalodyJsonDouble(b[1], 0.0) / denominator);
    };

    // 第二阶段先收集值对象；排序与时间积分在所有来源事件齐备后进行。
    struct RawEvent {
        /// @brief Malody 拍号位置。
        double beat = 0.0;
        /// @brief Timing 事件使用的 BPM 值。
        double bpm = -1.0;
        /// @brief 非 BPM 事件使用的效果参数。
        double value = 0.0;
        /// @brief BPM 事件在原始 time 数组中的顺序。
        std::size_t bpmSourceOrder = 0;
        /// @brief 用于往返保存的原始 JSON 对象。
        json raw;
        /// @brief 内部 Timing 效果类型。
        TimingEffect effect{ TimingEffect::BPM };
        /// @brief 是否来自 Malody 的 time 段。
        bool isBpm = false;
    };
    struct BpmEvent {
        /// @brief Malody 拍号位置。
        double beat = 0.0;
        /// @brief 从该拍号开始生效的 BPM 值。
        double bpm = 120.0;
        /// @brief 相对纯 beat 时间追加的局部延迟，单位为毫秒。
        double delayMs = 0.0;
        /// @brief 应用当前 delay 后的绝对时间锚点，单位为毫秒。
        double timestamp = 0.0;
        /// @brief 对应原始 time 数组的稳定顺序。
        std::size_t sourceOrder = 0;
    };
    std::vector<RawEvent> rawEvents;
    std::vector<BpmEvent> bpmEvents;

    // time 数组中的每项同时进入完整事件流和 BPM 专用积分表。
    if ( fileData.contains("time") ) {
        for ( const auto& t : fileData["time"] ) {
            RawEvent ev;
            ev.beat           = beatToDouble(t.value("beat", json::array()));
            ev.bpm            = readMalodyJsonDouble(t, "bpm", 120.0);
            ev.bpmSourceOrder = bpmEvents.size();
            ev.isBpm          = true;
            ev.raw            = t;

            // sourceOrder 关联两份中间表中的同一个原始红线。
            rawEvents.push_back(ev);
            bpmEvents.push_back(
                { .beat        = ev.beat,
                  .bpm         = ev.bpm,
                  .delayMs     = readMalodyJsonDouble(t, "delay", 0.0),
                  .sourceOrder = ev.bpmSourceOrder });
        }
    }
    // 一个 effect 对象可能同时携带多个效果键，需要分别展开为 Timing。
    if ( fileData.contains("effect") ) {
        for ( const auto& e : fileData["effect"] ) {
            /// @brief 将当前 JSON 对象的一个已知效果键展开为 RawEvent。
            auto pushEffect = [&](const char* key, TimingEffect effect) {
                if ( !e.contains(key) ) return;
                RawEvent ev;
                ev.beat   = beatToDouble(e.value("beat", json::array()));
                ev.value  = readMalodyJsonDouble(e, key, 0.0);
                ev.effect = effect;
                ev.isBpm  = false;
                ev.raw    = e;
                rawEvents.push_back(ev);
            };
            pushEffect("scroll", TimingEffect::SCROLL);
            pushEffect("jump", TimingEffect::JUMP);
            pushEffect("hs", TimingEffect::HS);
        }
    }
    // 同拍 BPM 必须先于效果生效，使效果读取到该拍新 BPM。
    std::stable_sort(rawEvents.begin(),
                     rawEvents.end(),
                     [](const RawEvent& a, const RawEvent& b) {
                         if ( a.beat != b.beat ) {
                             return a.beat < b.beat;
                         }
                         return a.isBpm && !b.isBpm;
                     });
    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const BpmEvent& a, const BpmEvent& b) { return a.beat < b.beat; });

    // 模式决定 column 声明和自由坐标拟合的优先关系。
    int malodyMode = 0;
    if ( fileData.contains("meta") ) {
        malodyMode = fileData["meta"].value("mode", 0);
    }

    /// @brief 判断 note 条目是否为音效或 BGM 采样。
    /// type 字段为字符串 ("SOUND") 或旧版整数 (1) 时不参与 key 数推断。
    auto isSoundNote = [](const json& n) -> bool {
        // 字符串比较保持大小写契约；数值 1 只服务旧文件。
        if ( n.contains("type") ) {
            if ( n["type"].is_string() )
                return n["type"].get<std::string>() == "SOUND";
            if ( n["type"].is_number() ) {
                return std::abs(parseMalodyJsonDouble(n["type"], 0.0) - 1.0) <=
                       std::numeric_limits<double>::epsilon();
            }
        }
        return false;
    };

    /// @brief 判断自动采样是否引用 meta.song.file 指定的主音频。
    /// @param node 待判断的 Malody note 节点。
    /// @return sound 字段与主音频路径或文件名一致时返回 true。
    auto isMainSongSample = [&](const json& node) {
        // 资源可按完整相对路径或仅文件名匹配，兼容目录前缀差异。
        const auto sound = node.find("sound");
        if ( sound == node.end() || !sound->is_string() ||
             basemeta.song_file_hint.empty() ) {
            return false;
        }
        const std::string& resourceId = sound->get_ref<const std::string&>();
        const std::string  songFileValue =
            Config::pathToUtf8(basemeta.song_file_hint);
        if ( resourceId == songFileValue ) return true;
        return Config::utf8ToPath(resourceId).filename() ==
               basemeta.song_file_hint.filename();
    };

    /// @brief 与首红线对应并锚定歌曲时间零点的主 SOUND 节点。
    const json* wrappedMainSoundNode = nullptr;
    /// @brief 首红线相对主音频零点的规范化相位，单位为毫秒。
    double wrappedFirstTimingPhaseMs = 0.0;
    // 只有首 BPM 和 note 数组同时存在时才可能识别成对相位编码。
    if ( !bpmEvents.empty() && fileData.contains("note") ) {
        const auto&  firstBpmEvent = bpmEvents.front();
        const double firstBpm =
            firstBpmEvent.bpm > 0.0 ? firstBpmEvent.bpm : 120.0;
        const double firstBeatLengthMs = 60000.0 / firstBpm;
        // 负 delay 没有该回卷形态，不尝试以模运算猜测主 SOUND。
        if ( firstBpmEvent.delayMs >= -1e-6 ) {
            for ( const auto& node : fileData["note"] ) {
                if ( !isSoundNote(node) || !isMainSongSample(node) ||
                     !node.contains("beat") ) {
                    continue;
                }
                const double sampleBeat = beatToDouble(node["beat"]);
                const double sampleOffset =
                    readMalodyJsonDouble(node, "offset", 0.0);
                // 规范文件会按首红线所在半拍选择零 offset 或与 delay
                // 相同的回卷 offset；旧文件也可能始终使用后一种形态。
                // 两种值都只在资源与首 timing 拍号匹配时按主音轨逆变换。
                // 同时接受规范零偏移与旧保存器的 delay 配对偏移。
                const bool hasCanonicalMainOffset =
                    std::abs(sampleOffset) <= 0.51;
                const bool hasLegacyPairedOffset =
                    std::abs(sampleOffset - firstBpmEvent.delayMs) <= 0.51;
                if ( std::abs(sampleBeat - firstBpmEvent.beat) <= 1e-6 &&
                     sampleOffset >= -1e-6 &&
                     (hasCanonicalMainOffset || hasLegacyPairedOffset) ) {
                    wrappedMainSoundNode = &node;
                    // 将负 delay 映射到首拍内正相位，消除完整拍进位。
                    wrappedFirstTimingPhaseMs =
                        std::fmod(-firstBpmEvent.delayMs, firstBeatLengthMs);
                    if ( wrappedFirstTimingPhaseMs < 0.0 ) {
                        wrappedFirstTimingPhaseMs += firstBeatLengthMs;
                    }
                    if ( std::abs(wrappedFirstTimingPhaseMs) <= 1e-6 ||
                         std::abs(wrappedFirstTimingPhaseMs -
                                  firstBeatLengthMs) <= 1e-6 ) {
                        wrappedFirstTimingPhaseMs = 0.0;
                    }
                    break;
                }
            }
        }
    }

    /// @brief 成对首拍的正相位进位在导入时从所有普通内容拍号中移除。
    const double malodyContentBeatShift =
        wrappedMainSoundNode != nullptr && wrappedFirstTimingPhaseMs > 1e-6
            ? 1.0
            : 0.0;
    // 正相位包装会让普通内容整体多一拍，导入时从首 BPM 之外的内容移除。
    if ( malodyContentBeatShift != 0.0 && !bpmEvents.empty() ) {
        const std::size_t wrappedFirstBpmSourceOrder =
            bpmEvents.front().sourceOrder;
        for ( std::size_t index = 1; index < bpmEvents.size(); ++index ) {
            bpmEvents[index].beat -= malodyContentBeatShift;
        }
        // RawEvent 中的首 BPM 用来源序号识别，避免浮点拍位比较误判。
        for ( auto& event : rawEvents ) {
            const bool isWrappedFirstBpm =
                event.isBpm &&
                event.bpmSourceOrder == wrappedFirstBpmSourceOrder;
            if ( !isWrappedFirstBpm ) {
                event.beat -= malodyContentBeatShift;
            }
        }
        std::stable_sort(rawEvents.begin(),
                         rawEvents.end(),
                         [](const RawEvent& a, const RawEvent& b) {
                             if ( a.beat != b.beat ) {
                                 return a.beat < b.beat;
                             }
                             return a.isBpm && !b.isBpm;
                         });
    }

    // 第三阶段预扫描可玩物件，建立 column 下界与 x 坐标频率分布。
    std::map<int, int> xFreq;
    int                maxColumnField    = -1;
    bool               hasX              = false;
    int                playableNoteCount = 0;
    int                metadataTrackCount =
        basemeta.track_count > 0 ? basemeta.track_count : -1;

    if ( fileData.contains("note") ) {
        // SOUND 使用 BGM 轨语义，不能参与玩家轨拟合。
        for ( const auto& n : fileData["note"] ) {
            if ( isSoundNote(n) ) continue;
            ++playableNoteCount;
            if ( n.contains("column") ) {
                maxColumnField = std::max(maxColumnField, n.value("column", 0));
            } else if ( n.contains("x") ) {
                xFreq[n["x"].get<int>()]++;
                hasX = true;
            }
        }
    }

    // column 是零基索引，因此最大值加一才是所需轨道数。
    int finalK = std::max(0, maxColumnField + 1);

    // Key 模式的 mode_ext.column 是谱面声明的列数；可玩物件只能在此基础上扩展。
    if ( malodyMode == 0 && metadataTrackCount > 0 ) {
        finalK = std::max(finalK, metadataTrackCount);
    }

    // 无可玩 column 时按声明轨数回退，二者皆缺失才使用四轨。
    if ( finalK <= 0 ) {
        finalK = metadataTrackCount > 0 ? metadataTrackCount : 4;
    }

    // 默认网格覆盖 256 宽虚拟画布；特殊键数再使用历史中心值。
    float bestW = 256.0f / (float)finalK;
    float bestS = bestW / 2.0f;

    // 4K 至 6K 的中心与间距来自 Malody 实际坐标约定。
    if ( finalK == 4 ) {
        bestW = 64.0f;
        bestS = 31.0f;
    } else if ( finalK == 5 ) {
        bestW = 51.0f;
        bestS = 25.0f;
    } else if ( finalK == 6 ) {
        bestW = 43.0f;
        bestS = 21.0f;
    } else {
        bestW = 256.0f / (float)finalK;
        bestS = bestW / 2.0f;
    }

    // 自由坐标输入在常用 4K 至 9K 网格中选择加权误差最小者。
    if ( hasX ) {
        double minTotalError = 1e18;
        bool   foundBetter   = false;

        for ( int k : { 4, 5, 6, 7, 8, 9 } ) {
            float w = 256.0f / k;
            float s = w / 2.0f;
            if ( k == 4 ) {
                w = 64.0f;
                s = 31.0f;
            } else if ( k == 5 ) {
                w = 51.0f;
                s = 25.0f;
            } else if ( k == 6 ) {
                w = 43.0f;
                s = 21.0f;
            } else {
                w = 256.0f / k;
                s = w / 2.0f;
            }

            // 高频坐标对拟合结果贡献更高，离群点不应主导键数。
            double error = 0;
            for ( auto const& [x, freq] : xFreq ) {
                float target = std::round((x - s) / w) * w + s;
                error += (double)freq * std::abs(x - target);
            }
            if ( error < minTotalError ) {
                minTotalError = error;
                // 平均误差阈值防止任意自由坐标被强行解释为键盘网格。
                if ( error < (double)playableNoteCount * 5.0 ) {
                    finalK      = k;
                    bestW       = w;
                    bestS       = s;
                    foundBetter = true;
                }
            }
        }
    }

    // finalK 是后续玩家轨夹取与 BGM 轨起点的统一权威值。
    basemeta.track_count = finalK;
    XINFO("MC Map track count:{}", basemeta.track_count);

    /// @brief 将 Malody 虚拟横坐标映射为最近玩家轨。
    auto getTrackIndexFromX = [&](int x_val) -> uint32_t {
        if ( finalK <= 0 ) return 0;
        // 四舍五入到最近网格中心，再夹取防止离群坐标越界。
        int idx = static_cast<int>(
            std::round((static_cast<float>(x_val) - bestS) / bestW));
        return static_cast<uint32_t>(std::clamp(idx, 0, finalK - 1));
    };

    /// @brief 优先读取 column，否则把 x 映射为玩家轨。
    auto getNoteTrackIndex = [&](const json& n) -> uint32_t {
        if ( n.contains("column") ) return n.value("column", 0);
        if ( n.contains("x") ) {
            return getTrackIndexFromX(n["x"].get<int>());
        }
        return 0;
    };

    // 3. 辅助函数：计算拍号锚点的绝对时间 (ms)
    auto getInitialBpm = [&]() {
        double initialBpm =
            basemeta.preference_bpm > 0 ? basemeta.preference_bpm : 120.0;
        if ( !bpmEvents.empty() && bpmEvents.front().beat <= 0.0 ) {
            initialBpm = bpmEvents.front().bpm;
        }
        return initialBpm;
    };
    std::vector<double> bpmTimestampsBySourceOrder(bpmEvents.size(), 0.0);
    double              anchorBeat = 0.0;
    double              anchorTime = 0.0;
    double              anchorBpm  = getInitialBpm();
    for ( std::size_t index = 0; index < bpmEvents.size(); ++index ) {
        auto& ev = bpmEvents[index];
        if ( index == 0 ) {
            const double firstBpm   = ev.bpm > 0.0 ? ev.bpm : getInitialBpm();
            const double beatLength = 60000.0 / firstBpm;
            if ( wrappedMainSoundNode != nullptr ) {
                // 成对编码的首 timing 只保留一拍内相位；beat 上的整拍
                // 前导由后续内容承担，避免首红线停留在数拍之后。
                ev.timestamp = wrappedFirstTimingPhaseMs;
            } else {
                ev.timestamp = ev.beat * beatLength + ev.delayMs;
            }
            bpmTimestampsBySourceOrder[ev.sourceOrder] = ev.timestamp;
            anchorBeat                                 = ev.beat;
            anchorTime                                 = ev.timestamp;
            anchorBpm                                  = ev.bpm;
            continue;
        }
        ev.timestamp = anchorTime +
                       (ev.beat - anchorBeat) * (60000.0 / anchorBpm) +
                       ev.delayMs;
        bpmTimestampsBySourceOrder[ev.sourceOrder] = ev.timestamp;
        anchorBeat                                 = ev.beat;
        anchorTime                                 = ev.timestamp;
        anchorBpm                                  = ev.bpm;
    }

    auto getBpmAtBeat = [&](double beat) {
        double curBpm =
            bpmEvents.empty() ? getInitialBpm() : bpmEvents.front().bpm;
        for ( const auto& ev : bpmEvents ) {
            if ( ev.beat > beat + 1e-9 ) break;
            curBpm = ev.bpm;
        }
        return curBpm;
    };
    auto getAbsTime = [&](double beat) {
        const BpmEvent* relative = nullptr;
        for ( const auto& ev : bpmEvents ) {
            if ( ev.beat > beat + 1e-9 ) break;
            relative = &ev;
        }
        if ( relative == nullptr ) {
            if ( bpmEvents.empty() ) {
                return beat * (60000.0 / getInitialBpm());
            }
            const auto& first = bpmEvents.front();
            return first.timestamp +
                   (beat - first.beat) * (60000.0 / first.bpm);
        }
        return relative->timestamp +
               (beat - relative->beat) * (60000.0 / relative->bpm);
    };

    // 4. 处理时间线点 (Timing Points)
    double currentBpm = getInitialBpm();

    for ( auto& ev : rawEvents ) {
        Timing timing;
        timing.m_timestamp = ev.isBpm
                                 ? bpmTimestampsBySourceOrder[ev.bpmSourceOrder]
                                 : getAbsTime(ev.beat);

        if ( ev.isBpm ) {
            currentBpm                     = ev.bpm;
            timing.m_timingEffect          = TimingEffect::BPM;
            timing.m_bpm                   = currentBpm;
            timing.m_beat_length           = 60000.0 / currentBpm;
            timing.m_timingEffectParameter = currentBpm;
        } else {
            currentBpm                     = getBpmAtBeat(ev.beat);
            timing.m_timingEffect          = ev.effect;
            timing.m_bpm                   = currentBpm;
            timing.m_timingEffectParameter = ev.value;
            timing.m_beat_length           = timing.m_timingEffectParameter;
        }

        auto& malody_timing_props =
            timing.m_metadata.timing_properties[TimingMetadataType::MALODY];
        for ( auto it = ev.raw.begin(); it != ev.raw.end(); ++it ) {
            if ( it.key() != "bpm" && it.key() != "scroll" &&
                 it.key() != "jump" && it.key() != "hs" ) {
                malody_timing_props[it.key()] = it.value().dump();
            }
        }
        if ( !ev.isBpm ) {
            malody_timing_props["effect"] =
                "\"" + timingEffectToString(ev.effect) + "\"";
        }
        if ( beatMap.m_baseMapMetadata.preference_bpm <= 0.0 &&
             timing.m_timingEffect == TimingEffect::BPM ) {
            beatMap.m_baseMapMetadata.preference_bpm = timing.m_bpm;
        }
        beatMap.m_timings.push_back(timing);
    }

    if ( beatMap.m_timings.empty() ) {
        Timing t;
        t.m_timestamp             = 0.0;
        t.m_bpm                   = currentBpm;
        t.m_beat_length           = 60000.0 / currentBpm;
        t.m_timingEffect          = TimingEffect::BPM;
        t.m_timingEffectParameter = currentBpm;
        beatMap.m_timings.push_back(t);

        if ( basemeta.preference_bpm <= 0.0 ) {
            basemeta.preference_bpm = currentBpm;
        }
    } else {
        if ( basemeta.preference_bpm <= 0.0 ) {
            for ( const auto& t : beatMap.m_timings ) {
                if ( t.m_timingEffect == TimingEffect::BPM ) {
                    basemeta.preference_bpm = t.m_bpm;
                    break;
                }
            }
        }
    }

    // 5. 处理物件 (Notes)
    constexpr std::uint32_t     MALODY_LEGACY_SAMPLE_TRACK_BEGIN = 10U;
    std::optional<std::int64_t> legacySampleEffectiveTimestampMs;
    std::uint32_t               nextLegacySampleTrack =
        std::max(MALODY_LEGACY_SAMPLE_TRACK_BEGIN,
                 static_cast<std::uint32_t>(std::max(0, finalK)));
    std::size_t legacyAutoPositionedSampleCount = 0;

    if ( fileData.contains("note") ) {
        for ( const auto& n : fileData["note"] ) {
            if ( !n.contains("beat") ) continue;

            const bool isAutomaticSample = isSoundNote(n);
            const bool isWrappedMainSample =
                isAutomaticSample && &n == wrappedMainSoundNode;
            double startBeat = beatToDouble(n["beat"]);
            if ( !isWrappedMainSample ) {
                startBeat -= malodyContentBeatShift;
            }
            double startTime = getAbsTime(startBeat);

            if ( isAutomaticSample ) {
                const auto soundIt = n.find("sound");
                if ( soundIt == n.end() || !soundIt->is_string() ||
                     soundIt->get_ref<const std::string&>().empty() ) {
                    continue;
                }

                AudioSampleEvent& sample =
                    beatMap.m_audioSamples.emplace_back();
                if ( isWrappedMainSample ) {
                    // 成对字段只描述歌曲相位；MMM 内部将主音频物化在
                    // 时间零点，避免把 Malody 的相位编码误当成局部 offset。
                    sample.m_timestamp = 0.0;
                    sample.m_offsetMs  = 0;
                } else {
                    sample.m_timestamp = startTime;
                    sample.m_offsetMs  = readMalodyJsonInt64(n, "offset", 0);
                }
                sample.m_audioResourceId =
                    soundIt->get_ref<const std::string&>();
                sample.m_volume = Internal::malodyGainPercentToVolume(
                    readMalodyJsonDouble(n, "vol", 0.0));

                auto& props =
                    sample.m_metadata
                        .sample_properties[SampleMetadataType::MALODY];
                for ( auto it = n.begin(); it != n.end(); ++it ) {
                    if ( it.key() != "type" && it.key() != "sound" &&
                         it.key() != "offset" && it.key() != "x" &&
                         it.key() != "vol" ) {
                        props[it.key()] = it.value().dump();
                    }
                }
                const auto   xIt = n.find("x");
                const double parsedX =
                    xIt == n.end()
                        ? std::numeric_limits<double>::quiet_NaN()
                        : parseMalodyJsonDouble(
                              *xIt, std::numeric_limits<double>::quiet_NaN());
                const double roundedX = std::round(parsedX);
                const bool   validBgmTrack =
                    std::isfinite(parsedX) &&
                    std::abs(parsedX - roundedX) <= 1e-6 &&
                    roundedX >= static_cast<double>(finalK) &&
                    roundedX <=
                        static_cast<double>(std::numeric_limits<int>::max());
                if ( xIt == n.end() ) {
                    const std::int64_t effectiveTimestampMs =
                        static_cast<std::int64_t>(
                            std::llround(sample.effectiveTimestamp()));
                    if ( !legacySampleEffectiveTimestampMs.has_value() ||
                         *legacySampleEffectiveTimestampMs !=
                             effectiveTimestampMs ) {
                        legacySampleEffectiveTimestampMs = effectiveTimestampMs;
                        nextLegacySampleTrack            = std::max(
                            MALODY_LEGACY_SAMPLE_TRACK_BEGIN,
                            static_cast<std::uint32_t>(std::max(0, finalK)));
                    }
                    sample.m_track = nextLegacySampleTrack;
                    if ( nextLegacySampleTrack <
                         std::numeric_limits<std::uint32_t>::max() ) {
                        ++nextLegacySampleTrack;
                    }
                    props["original_x"] = "null";
                    ++legacyAutoPositionedSampleCount;
                } else if ( validBgmTrack ) {
                    sample.m_track =
                        static_cast<uint32_t>(static_cast<int>(roundedX));
                } else {
                    sample.m_track      = static_cast<uint32_t>(finalK);
                    props["original_x"] = xIt == n.end() ? "null" : xIt->dump();
                    beatMap.m_loadDiagnostics.push_back(
                        { .m_code = BeatmapLoadDiagnosticCode::
                              AUDIO_SAMPLE_TRACK_RELOCATED,
                          .m_severity = BeatmapLoadDiagnosticSeverity::
                              BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
                          .m_message = fmt::format(
                              "Malody 自动采样 '{}' 的轨道 x={} 不属于 "
                              "BGM 区，已迁移到首条 BGM 轨 {}",
                              sample.m_audioResourceId,
                              props["original_x"],
                              finalK),
                          .m_relatedPath = basemeta.map_path });
                    XWARN(
                        "Malody 自动采样 '{}' 的 BGM 轨道 x={} "
                        "非法，已归入首个 "
                        "BGM 轨道 {}",
                        sample.m_audioResourceId,
                        props["original_x"],
                        finalK);
                }

                const std::uint64_t requiredBgmTrackCount64 =
                    static_cast<std::uint64_t>(sample.m_track) -
                    static_cast<std::uint64_t>(finalK) + 1;
                const int requiredBgmTrackCount =
                    static_cast<int>(std::min<std::uint64_t>(
                        requiredBgmTrackCount64,
                        static_cast<std::uint64_t>(
                            std::numeric_limits<int>::max())));
                basemeta.bgm_track_count =
                    std::max(basemeta.bgm_track_count, requiredBgmTrackCount);
                basemeta.map_length =
                    std::max(basemeta.map_length, sample.effectiveTimestamp());
                continue;
            }

            uint32_t track =
                std::clamp(getNoteTrackIndex(n),
                           0u,
                           (uint32_t)std::max(0, basemeta.track_count - 1));

            Note* notePtr = nullptr;

            if ( n.contains("seg") ) {
                auto   segs         = n["seg"];
                double rootBeatRaw  = startBeat;
                double firstSegBeat = rootBeatRaw + beatToDouble(segs[0].value(
                                                        "beat", json::array()));
                double firstTime    = getAbsTime(firstSegBeat);

                int      rootX   = n.value("x", 0);
                int      xOffset = segs[0].value("x", 0);
                int      firstX  = rootX + xOffset;
                uint32_t firstSegTrack =
                    std::clamp(getTrackIndexFromX(firstX),
                               0u,
                               (uint32_t)std::max(0, basemeta.track_count - 1));

                if ( segs.size() == 1 ) {
                    if ( firstSegTrack == track ) {
                        Hold& h       = beatMap.m_noteData.holds.emplace_back();
                        h.m_type      = NoteType::HOLD;
                        h.m_timestamp = startTime;
                        h.m_track     = track;
                        h.m_duration  = std::max(0.0, firstTime - startTime);
                        notePtr       = &h;
                    } else if ( firstTime == startTime ) {
                        Flick& f = beatMap.m_noteData.flicks.emplace_back();
                        f.m_type = NoteType::FLICK;
                        f.m_timestamp = startTime;
                        f.m_track     = track;
                        f.m_dtrack    = (int32_t)firstSegTrack - (int32_t)track;
                        notePtr       = &f;
                    }
                }

                if ( !notePtr ) {
                    Polyline& poly =
                        beatMap.m_noteData.polylines.emplace_back();
                    poly.m_type      = NoteType::POLYLINE;
                    poly.m_timestamp = startTime;
                    poly.m_track     = track;

                    uint32_t runningTrack = track;
                    double   runningTime  = startTime;

                    for ( size_t i = 0; i < segs.size(); ++i ) {
                        const auto& s = segs[i];
                        double      stepBeatValue =
                            rootBeatRaw +
                            beatToDouble(s.value("beat", json::array()));
                        double stepTime = getAbsTime(stepBeatValue);

                        int      stepAbsX  = rootX + s.value("x", 0);
                        uint32_t stepTrack = std::clamp(
                            getTrackIndexFromX(stepAbsX),
                            0u,
                            (uint32_t)std::max(0, basemeta.track_count - 1));

                        if ( stepTime > runningTime + 1e-7 ) {
                            // 长按段。
                            Hold& h  = beatMap.m_noteData.holds.emplace_back();
                            h.m_type = NoteType::HOLD;
                            h.m_timestamp = runningTime;
                            h.m_track     = runningTrack;
                            h.m_duration =
                                std::max(0.0, stepTime - runningTime);
                            h.m_isSubNote = true;

                            poly.m_subNotes.push_back(h);
                            poly.m_subHolds.push_back(h);

                            if ( stepTrack != runningTrack ) {
                                Flick& f =
                                    beatMap.m_noteData.flicks.emplace_back();
                                f.m_type      = NoteType::FLICK;
                                f.m_timestamp = stepTime;
                                f.m_track     = runningTrack;
                                f.m_dtrack =
                                    (int32_t)stepTrack - (int32_t)runningTrack;
                                f.m_isSubNote = true;
                                poly.m_subNotes.push_back(f);
                                poly.m_subFlicks.push_back(f);
                            }
                        } else if ( stepTrack != runningTrack ) {
                            // 瞬时 Flick，仅在轨道发生变化时创建。
                            Flick& f = beatMap.m_noteData.flicks.emplace_back();
                            f.m_type = NoteType::FLICK;
                            f.m_timestamp = runningTime;
                            f.m_track     = runningTrack;
                            f.m_dtrack =
                                (int32_t)stepTrack - (int32_t)runningTrack;
                            f.m_isSubNote = true;
                            poly.m_subNotes.push_back(f);
                            poly.m_subFlicks.push_back(f);
                        }
                        runningTime  = stepTime;
                        runningTrack = stepTrack;
                    }
                    notePtr = &poly;
                }
            } else if ( n.contains("endbeat") ) {
                // 处理长条 Hold
                double endBeat =
                    beatToDouble(n["endbeat"]) - malodyContentBeatShift;
                double endTime   = getAbsTime(endBeat);
                Hold&  hold      = beatMap.m_noteData.holds.emplace_back();
                hold.m_type      = NoteType::HOLD;
                hold.m_timestamp = startTime;
                hold.m_track     = track;
                hold.m_duration  = endTime - startTime;
                notePtr          = &hold;
            } else if ( n.contains("dir") ) {
                int trackCount = basemeta.track_count;
                int flickWidthBase =
                    trackCount == 4   ? 60
                    : trackCount == 5 ? 50
                    : trackCount == 6 ? 40
                    : trackCount == 7 ? 30
                    : trackCount == 8
                        ? 20
                        : static_cast<int>(std::round(256.0 / trackCount));

                // Flick 的 w
                // 个位表示跨轨数；十位基数同时落在皮肤的键数识别区间。
                Flick& flick      = beatMap.m_noteData.flicks.emplace_back();
                flick.m_type      = NoteType::FLICK;
                flick.m_timestamp = startTime;
                flick.m_track     = track;

                int wVal = n.value("w", flickWidthBase);
                int distance;
                if ( trackCount == 7 && wVal >= 37 ) {
                    // 兼容旧写出器使用 37 作为 7K Flick 基数的文件。
                    distance = wVal - 37;
                } else if ( trackCount == 8 && wVal >= 32 ) {
                    // 兼容旧写出器使用 32 作为 8K Flick 基数的文件。
                    distance = wVal - 32;
                } else {
                    distance = wVal - flickWidthBase;
                }
                distance = std::max(0, distance);

                int direction = n.value("dir", 0);
                // 8 为左 (-)，2 为右 (+)
                flick.m_dtrack = (direction == 8) ? -distance : distance;
                notePtr        = &flick;
            } else {
                // 普通点点击 Note
                Note& note       = beatMap.m_noteData.notes.emplace_back();
                note.m_type      = NoteType::NOTE;
                note.m_timestamp = startTime;
                note.m_track     = track;
                notePtr          = &note;
            }

            // 物件元数据存储
            if ( notePtr ) {
                if ( auto sound = n.find("sound");
                     sound != n.end() && sound->is_string() ) {
                    notePtr->setSampleBinding(
                        { sound->get_ref<const std::string&>(),
                          Internal::malodyGainPercentToVolume(
                              readMalodyJsonDouble(n, "vol", 0.0)) });
                }

                auto& props = notePtr->m_metadata
                                  .note_properties[NoteMetadataType::MALODY];

                for ( auto it = n.begin(); it != n.end(); ++it ) {
                    if ( it.key() != "beat" && it.key() != "column" &&
                         it.key() != "x" && it.key() != "w" &&
                         it.key() != "type" && it.key() != "dir" &&
                         it.key() != "endbeat" && it.key() != "seg" &&
                         it.key() != "sound" && it.key() != "vol" ) {
                        props[it.key()] = it.value().dump();
                    }
                }
                // beatMap.m_allNotes.push_back(*notePtr); // 统一由 sync() 处理

                // 更新谱面最大长度
                double noteEnd = notePtr->m_timestamp;
                if ( notePtr->m_type == NoteType::HOLD ) {
                    noteEnd += static_cast<Hold*>(notePtr)->m_duration;
                } else if ( notePtr->m_type == NoteType::POLYLINE ) {
                    Polyline& p = *static_cast<Polyline*>(notePtr);
                    if ( !p.m_subNotes.empty() ) {
                        Note& finalSub = p.m_subNotes.back();
                        noteEnd        = finalSub.m_timestamp;
                        if ( finalSub.m_type == NoteType::HOLD ) {
                            noteEnd += static_cast<Hold&>(finalSub).m_duration;
                        }
                    }
                }
                if ( noteEnd > basemeta.map_length ) {
                    basemeta.map_length = noteEnd;
                }
            }
        }
    }

    if ( legacyAutoPositionedSampleCount > 0 ) {
        const auto legacyTrackBegin =
            std::max(MALODY_LEGACY_SAMPLE_TRACK_BEGIN,
                     static_cast<std::uint32_t>(std::max(0, finalK)));
        beatMap.m_loadDiagnostics.push_back(
            { .m_code = BeatmapLoadDiagnosticCode::AUDIO_SAMPLE_TRACK_RELOCATED,
              .m_severity = BeatmapLoadDiagnosticSeverity::
                  BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
              .m_message = fmt::format(
                  "{} 个缺少 x 的旧版 Malody 自动采样已从绝对轨道 {} "
                  "开始自动分轨；同一实际触发时刻的采样会依次展开",
                  legacyAutoPositionedSampleCount,
                  legacyTrackBegin),
              .m_relatedPath = basemeta.map_path });
        XINFO(
            "已按 Malody Pro Editor 规则为 {} 个缺少 x 的自动采样分轨，"
            "起始绝对轨道为 {}",
            legacyAutoPositionedSampleCount,
            legacyTrackBegin);
    }

    // 更新谱面元数据
    basemeta.name             = fmt::format("[mc] {} [{}] {}",
                                basemeta.title,
                                basemeta.track_count,
                                basemeta.version);
    beatMap.m_baseMapMetadata = basemeta;

    // 最终同步引用
    beatMap.sync();

    XINFO("Successfully loaded Malody map with {} notes and {} timings.",
          beatMap.m_allNotes.size(),
          beatMap.m_timings.size());

    return beatMap;
}

}  // namespace MMM
