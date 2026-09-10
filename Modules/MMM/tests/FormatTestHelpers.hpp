#pragma once

#include "log/colorful-log.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace MMM::Test
{

/// @file FormatTestHelpers.hpp
/// @brief 为 IMD、Malody 和 osu! 往返测试提供格式层比较器。
///
/// 本文件检查磁盘表示层，而 TestHelper.hpp 检查统一 BeatMap 语义层。二者必须
/// 分开：格式文本可以规范化但模型等价，也可能读写器产生相互抵消的同向错误，
/// 只有同时比较磁盘结构与重载模型才能区分这些情况。
///
/// IMD 比较分为五个区块：
/// - 8 字节头部：map_length 与 timing_count；
/// - timing_count 乘 12 字节的 Timing 表；
/// - 2 字节固定标记 0x0303；
/// - 4 字节 table_rows；
/// - table_rows 乘 11 字节的物件记录。
///
/// IMD 标量由小端定宽字段组成。辅助函数使用 memcpy 读取，避免未对齐指针
/// 解引用；调用方负责在读取前确认缓冲区拥有足够字节。
///
/// IMD 物件记录字段：
/// - typeInfo 高四位描述折线位置、低四位描述基础类型；
/// - reserved 是保留字节，不参与结构化等价；
/// - timestamp 是 int32 毫秒；
/// - track 是 uint8 玩家轨；
/// - parameter 依 Note、Flick、Hold 类型解释。
///
/// IMD 物件区先尝试原始字节比较。若只因保存器重排而失败，则解析记录并按
/// timestamp、track、typeInfo 排序后比较结构。reserved 有意不参与 operator==，
/// 因为保存器会把历史非规范保留值统一写为零。
///
/// IMD 比较返回通过区块数，而不是单一布尔值：
/// - 5 表示所有磁盘区块一致；
/// - 3 以上且模型一致可由调用测试按历史规范化策略接受；
/// - -1 表示文件无法读取；
/// - 每个失败区块记录原值、导出值或结构匹配数量；
/// - 比较器本身不决定整个测试是否通过。
///
/// Malody 比较分为四个逻辑分区：
/// - meta 比较作者、难度、背景、歌曲和声明列数；
/// - time 按 beat 排序后比较 BPM 数量和值；
/// - effect 只比较 scroll 子集，并允许缺失键与空数组等价；
/// - note 分开比较玩家物件与 SOUND 自动采样。
///
/// Malody meta 不要求完整 JSON 相等。id、preview、mode 等字段可能由保存器补全，
/// 测试只检查来源谱面承诺保留的核心公共字段。song 子对象存在时再比较标题、
/// 艺术家、音频文件和 BPM；mode_ext 同时存在时比较 column。
///
/// Malody time 比较先复制数组再按 beat 排序：
/// - 不修改原始 JSON；
/// - 数量必须一致；
/// - 对应 BPM 必须一致；
/// - 拍位排序消除等价数组顺序差异；
/// - 更细的绝对时间语义由 BeatMap 比较器承担。
///
/// Malody effect 当前只以 scroll 为格式往返哨兵：
/// - 过滤不含 scroll 的 jump、hs 等条目；
/// - 按 beat 排序；
/// - 数量和值必须一致；
/// - 两边均无有效 scroll 时视为通过；
/// - 其他效果的模型语义由逻辑比较覆盖。
///
/// Malody note 节点分类兼容：
/// - 字符串 type=`SOUND` 是 Slide 自动采样；
/// - 数值 type=1 是 Key 自动采样；
/// - 其他节点均作为玩家物件；
/// - 玩家物件和自动采样分别排序、计数与比较；
/// - 一类通过不能抵扣另一类失败。
///
/// 玩家物件排序键：
/// - 三元 beat 转换后的连续拍数；
/// - column，或由 x 近似得到的轨道；
/// - sound 资源名；
/// - vol；
/// - 排序只服务匹配，不改变输出文件。
///
/// 玩家物件比较字段：
/// - beat；
/// - endbeat；
/// - 双方均有 column 时的轨道；
/// - seg 数量与存在性；
/// - 玩家命中 sound；
/// - 玩家命中 vol；
/// - 失败时打印原始与导出 JSON 条目。
///
/// 自动采样排序键：
/// - beat；
/// - sound；
/// - x；
/// - offset；
/// - vol。
///
/// 自动采样规范表示检查：
/// - Slide 输出必须使用字符串 SOUND；
/// - Slide 输出不得携带 x；
/// - Key 输出必须使用整数 1；
/// - Key 输出必须携带数值 x；
/// - 两种模式都不得写 column；
/// - 来源有显式 x 时 Key 输出必须保持相同轨道；
/// - beat、sound、offset 与 vol 必须在容差内一致。
///
/// osu! 比较先把文本按 `[SectionName]` 拆分。解析器：
/// - 去除 CRLF 中的尾随 CR；
/// - 忽略空行；
/// - 忽略以 `/` 或 `;` 开头的注释；
/// - 保存每个章节的非注释正文；
/// - 最后一个章节在文件结束时显式提交。
///
/// osu! 固定检查七个章节：
/// - General；
/// - Editor；
/// - Metadata；
/// - Difficulty；
/// - Events；
/// - TimingPoints；
/// - HitObjects。
///
/// General、Editor、Metadata、Difficulty、Events 按键值对超集比较：
/// - 来源中的每个键必须在输出中存在；
/// - 对应字符串值必须一致；
/// - 输出可以增加保存器生成的默认键；
/// - 冒号后的首尾空白不参与值比较；
/// - 首个冒号分隔键和值，值内其余冒号保留。
///
/// TimingPoints 与 HitObjects 是逗号列表，不按键值解析。当前格式层只比较过滤
/// 注释后的行数，更细的字段正确性由加载后 BeatMap 逻辑比较承担。
///
/// 比较器共同约束：
/// - 输入文件只读打开；
/// - 解析失败返回 -1 或空结果，不抛出异常；
/// - 临时排序只作用于值副本或观察数据；
/// - 第一处细项差异记录足够定位信息；
/// - 容差只用于拍位和浮点音量等表示误差；
/// - 数量检查先于逐项访问；
/// - 格式层通过不替代统一模型层测试；
/// - 本文件不写入任何测试资源或输出。
///
/// IMD 区块失败解释：
/// - 头部失败可能来自 map_length 或 timing_count 变化；
/// - timing_count 差异常见于加载器去除连续重复 BPM；
/// - Timing 数量相同但字节不同表示时间或 BPM 值改变；
/// - 固定标记失败表示字段偏移或写出常量回归；
/// - table_rows 失败表示物件计数与扁平化结果改变；
/// - 物件原始字节失败后需查看结构化比较；
/// - 结构化通过表示仅记录次序或保留字节规范化；
/// - 结构化失败日志给出排序后相同前缀数量；
/// - 文件不足最小字段长度时只返回此前已通过区块；
/// - 负计数不是有效夹具输入，本辅助函数不负责修复损坏文件。
///
/// Malody meta 失败解释：
/// - orig_has 与 expo_has 区分整个分区丢失；
/// - creator/version/background 任一变化会失败；
/// - 双方存在 song 时检查六个核心字段；
/// - 双方存在 mode_ext 时检查 column；
/// - 一方缺少可选子对象时由当前核心比较边界决定；
/// - 自动补充 id、preview、mode 不构成失败；
/// - 来源私有键由边界测试另行覆盖。
///
/// Malody time 失败解释：
/// - 分区一侧缺失；
/// - BPM 条目数量不同；
/// - 按 beat 排序后的 BPM 值不同；
/// - 当前辅助层不直接比较 delay；
/// - 拍位绝对时间一致性由模型层验证；
/// - 相位包装边界由 MalodyEdgeCaseTest 验证。
///
/// Malody effect 失败解释：
/// - 有效 scroll 数量不同；
/// - 对应 scroll 值不同；
/// - 一侧缺失但另一侧含有效条目；
/// - 两侧均无有效条目时通过；
/// - jump 与 hs 不在本格式哨兵比较范围；
/// - 同时间排序问题仍会由逻辑 Timing 比较暴露。
///
/// Malody 玩家物件失败解释：
/// - 数量变化通常表示折线展开或重复去重差异；
/// - beat 差异表示拍时投影不一致；
/// - endbeat 差异表示 Hold 尾点改变；
/// - column 差异表示 Key 轨道改变；
/// - seg 存在性或数量差异表示折线结构改变；
/// - sound 或 vol 差异表示玩家绑定丢失；
/// - 日志中的 O/E 分别是排序后的原始和导出条目。
///
/// Malody 自动采样失败解释：
/// - 数量变化表示 SOUND 丢失或玩家物件误分类；
/// - type 非规范表示模式写出规则错误；
/// - Slide 携带 x 表示绝对 BGM 轨泄漏到自由模式；
/// - Key 缺少 x 表示分轨身份丢失；
/// - column 出现表示 SOUND 被错误当成玩家物件；
/// - beat 差异表示触发锚点改变；
/// - offset 差异表示有效播放时间改变；
/// - sound 或 vol 差异表示资源或音量改变。
///
/// osu! 章节失败解释：
/// - 两侧均缺少章节视为等价；
/// - 仅一侧存在表示章节被新增或丢失；
/// - 列表章节失败只表示有效行数不同；
/// - 键值章节失败日志指出首个缺失或不同键；
/// - 输出额外默认键允许存在并在通过日志中计数；
/// - 注释和空行差异不会影响结果；
/// - 章节之外的文件头版本行不参与当前比较。
///
/// 修改比较口径时必须同步对应 ConsistencyTest 的总区块数或章节数，且不能用
/// 放宽格式层断言代替修复生产读写器。允许规范化的差异应有明确语义理由，并
/// 由统一模型比较证明内容没有丢失。
///
/// 最小回归覆盖要求：
/// - IMD 完全相同文件的五区块通过；
/// - IMD Timing 去重时的区块级诊断；
/// - IMD 仅记录重排时的结构化通过；
/// - IMD 物件字段真实变化时的结构化失败；
/// - Malody 普通 Key 谱面的四分区通过；
/// - Malody Slide 折线谱面的四分区通过；
/// - Malody 无 effect 与空 effect 等价；
/// - Malody 玩家绑定 sound/vol 保留；
/// - Malody Key SOUND type 与 x 规范；
/// - Malody Slide SOUND type 且无 x；
/// - Malody 同时间多个 SOUND 的确定匹配；
/// - osu! 普通谱面的七章节通过；
/// - osu! 大量 TimingPoints 的行数比较；
/// - osu! 大量 HitObjects 的行数比较；
/// - osu! 导出额外默认键的超集通过；
/// - osu! 来源键缺失或值变化的失败日志；
/// - CRLF 与 LF 文本都能正确拆分；
/// - 注释和空行不影响章节内容；
/// - 输入文件无法读取时返回 -1；
/// - JSON 解析失败时返回 -1；
/// - 各比较函数不修改输入文件。
///
/// 当前辅助器是回归定位工具，不是任意格式文件的安全解析器；测试夹具需先满足
/// 对应格式最小结构，损坏输入的边界由生产加载器及专门边界测试覆盖。

/// @name IMD 二进制分块比较工具
/// @{

/// @brief 从文件中读取全部字节
/// @param path 输入文件路径。
/// @return 完整字节数组，打开失败时返回空数组。
inline std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return {};
    // ate 模式先取得文件大小，再回到起点进行一次性读取。
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

/// @brief 小端读取 int32
/// @param p 至少指向四个可读字节。
/// @return 主机中的 int32 值。
inline int32_t readLEInt32(const uint8_t* p)
{
    int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

/// @brief 小端读取 int16
/// @param p 至少指向两个可读字节。
/// @return 主机中的 int16 值。
inline int16_t readLEInt16(const uint8_t* p)
{
    int16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

/// @brief 比较原始字节块，返回 true 表示完全一致
/// @param firstDiffOffset 首个差异的相对偏移，无差异时为 -1。
inline bool compareRawBytes(const uint8_t* a, const uint8_t* b, size_t len,
                            int& firstDiffOffset)
{
    firstDiffOffset = -1;
    // 发现第一处差异立即返回，日志无需扫描剩余大型物件区。
    for ( size_t i = 0; i < len; ++i ) {
        if ( a[i] != b[i] ) {
            firstDiffOffset = static_cast<int>(i);
            return false;
        }
    }
    return true;
}

/// @brief 比较原始字节块（无差异位置版）
/// @return 指定长度内全部字节一致时返回 true。
inline bool compareRawBytes(const uint8_t* a, const uint8_t* b, size_t len)
{
    int dummy;
    return compareRawBytes(a, b, len, dummy);
}

/// @brief IMD 单条物件记录（11字节）的结构化解析
struct IMDParsedRecord {
    /// @brief 类型与折线位置复合字段。
    int8_t typeInfo;
    /// @brief 历史保留字节，不参与结构等价。
    uint8_t reserved;
    /// @brief 毫秒时间戳。
    int32_t timestamp;
    /// @brief 玩家轨道。
    uint8_t track;
    /// @brief 类型相关参数。
    int32_t parameter;

    /// @brief 比较具有业务含义的字段，忽略 reserved 规范化。
    bool operator==(const IMDParsedRecord& o) const
    {
        return typeInfo == o.typeInfo && timestamp == o.timestamp &&
               track == o.track && parameter == o.parameter;
    }
};

/// @brief 从二进制指针解析一条 IMD 记录
/// @param p 至少指向 11 个可读字节。
/// @return 结构化记录。
inline IMDParsedRecord parseIMDRecord(const uint8_t* p)
{
    IMDParsedRecord r;
    r.typeInfo  = static_cast<int8_t>(p[0]);
    r.reserved  = p[1];
    r.timestamp = readLEInt32(p + 2);
    r.track     = p[6];
    r.parameter = readLEInt32(p + 7);
    return r;
}

/// @brief 从二进制数据解析所有 IMD 记录
/// @param data 首条记录地址。
/// @param count 记录数量，调用方已验证缓冲区长度。
/// @return 保持文件次序的结构化记录数组。
inline std::vector<IMDParsedRecord> parseAllIMDRecords(const uint8_t* data,
                                                       int32_t        count)
{
    std::vector<IMDParsedRecord> records;
    records.reserve(count);
    for ( int32_t i = 0; i < count; ++i ) {
        records.push_back(parseIMDRecord(data + i * 11));
    }
    return records;
}

/// @brief 按 timestamp, track, typeInfo 排序 IMD 记录（用于去重后对比）
/// @param records 原地排序的记录数组。
inline void sortIMDRecords(std::vector<IMDParsedRecord>& records)
{
    std::sort(records.begin(),
              records.end(),
              [](const IMDParsedRecord& a, const IMDParsedRecord& b) {
                  if ( a.timestamp != b.timestamp )
                      return a.timestamp < b.timestamp;
                  if ( a.track != b.track ) return a.track < b.track;
                  return a.typeInfo < b.typeInfo;
              });
}

/// @brief 对两个 .imd 文件进行分块二进制对比
/// 返回通过的块数，-1 表示读取失败
/// @param origPath 来源基准文件。
/// @param exportPath 往返导出文件。
inline int compareIMDChunks(const std::filesystem::path& origPath,
                            const std::filesystem::path& exportPath)
{
    auto orig = readBinaryFile(origPath);
    auto expo = readBinaryFile(exportPath);
    if ( orig.empty() || expo.empty() ) {
        XERROR("无法读取IMD文件进行二进制对比");
        return -1;
    }

    // 每个独立布局区块通过时累加，调用测试决定接受阈值。
    int passed = 0;

    // ---- 分块 1：文件头（8 字节）：int32 map_length + int32 timing_count ----
    {
        bool ok = (orig.size() >= 8 && expo.size() >= 8 &&
                   compareRawBytes(orig.data(), expo.data(), 8));
        if ( ok ) {
            int32_t ml = readLEInt32(orig.data());
            int32_t tc = readLEInt32(orig.data() + 4);
            XINFO("[IMD Chunk 1 - Header] map_length={} timing_count={}: PASS",
                  ml,
                  tc);
            ++passed;
        } else {
            int32_t oML = readLEInt32(orig.data());
            int32_t eML = readLEInt32(expo.data());
            int32_t oTC = readLEInt32(orig.data() + 4);
            int32_t eTC = readLEInt32(expo.data() + 4);
            XERROR(
                "[IMD Chunk 1 - Header] 字节不一致: FAIL "
                "(orig: ml={} tc={} | expo: ml={} tc={})",
                oML,
                oTC,
                eML,
                eTC);
            if ( oTC != eTC ) {
                XWARN("  → timing_count 变化由加载时的BPM去重引起 (560→1 等)");
            }
        }
    }

    if ( orig.size() < 8 || expo.size() < 8 ) return passed;
    int32_t origTC         = readLEInt32(orig.data() + 4);
    int32_t expoTC         = readLEInt32(expo.data() + 4);
    size_t  origTimingSize = static_cast<size_t>(origTC) * 12;
    size_t  expoTimingSize = static_cast<size_t>(expoTC) * 12;

    // ---- 分块 2：Timing 数据（timing_count × 12 字节） ----
    {
        bool rawOk =
            (orig.size() >= 8 + origTimingSize &&
             expo.size() >= 8 + expoTimingSize &&
             origTimingSize == expoTimingSize &&
             compareRawBytes(orig.data() + 8, expo.data() + 8, origTimingSize));

        if ( rawOk ) {
            XINFO("[IMD Chunk 2 - Timing Data] {} timing(s) × 12 bytes: PASS",
                  origTC);
            ++passed;
        } else if ( origTC != expoTC ) {
            XERROR(
                "[IMD Chunk 2 - Timing Data] 数量不匹配: FAIL "
                "(orig_count={} expo_count={}, BPM去重导致)",
                origTC,
                expoTC);
        } else {
            XERROR("[IMD Chunk 2 - Timing Data] 同数量但字节不一致: FAIL");
        }
    }

    size_t origMagicOff = 8 + origTimingSize;
    size_t expoMagicOff = 8 + expoTimingSize;

    // ---- 分块 3：魔数标记（2 字节）：0x0303 ----
    {
        bool ok =
            (orig.size() >= origMagicOff + 2 &&
             expo.size() >= expoMagicOff + 2 &&
             compareRawBytes(
                 orig.data() + origMagicOff, expo.data() + expoMagicOff, 2));
        if ( ok ) {
            int16_t m = readLEInt16(orig.data() + origMagicOff);
            XINFO("[IMD Chunk 3 - Magic Marker] 0x{:04X}: PASS", m);
            ++passed;
        } else {
            XERROR("[IMD Chunk 3 - Magic Marker] 0x0303: FAIL");
        }
    }

    size_t origTRCOff = origMagicOff + 2;
    size_t expoTRCOff = expoMagicOff + 2;

    // ---- 分块 4：表格行数（4 字节）：int32 ----
    {
        bool ok =
            (orig.size() >= origTRCOff + 4 && expo.size() >= expoTRCOff + 4 &&
             compareRawBytes(
                 orig.data() + origTRCOff, expo.data() + expoTRCOff, 4));
        if ( ok ) {
            int32_t rc = readLEInt32(orig.data() + origTRCOff);
            XINFO("[IMD Chunk 4 - Table Row Count] {} rows: PASS", rc);
            ++passed;
        } else {
            int32_t oRC = readLEInt32(orig.data() + origTRCOff);
            int32_t eRC = readLEInt32(expo.data() + expoTRCOff);
            XERROR(
                "[IMD Chunk 4 - Table Row Count] 不一致: FAIL (orig={} "
                "expo={})",
                oRC,
                eRC);
        }
    }

    if ( orig.size() < origTRCOff + 4 || expo.size() < expoTRCOff + 4 )
        return passed;

    int32_t origRows     = readLEInt32(orig.data() + origTRCOff);
    int32_t expoRows     = readLEInt32(expo.data() + expoTRCOff);
    size_t  origNoteSize = static_cast<size_t>(origRows) * 11;
    size_t  expoNoteSize = static_cast<size_t>(expoRows) * 11;

    size_t origNoteOff = origTRCOff + 4;
    size_t expoNoteOff = expoTRCOff + 4;

    // ---- 分块 5：音符记录（table_rows × 11 字节） ----
    {
        bool rawOk = (orig.size() >= origNoteOff + origNoteSize &&
                      expo.size() >= expoNoteOff + expoNoteSize &&
                      origNoteSize == expoNoteSize &&
                      compareRawBytes(orig.data() + origNoteOff,
                                      expo.data() + expoNoteOff,
                                      origNoteSize));

        if ( rawOk ) {
            XINFO("[IMD Chunk 5 - Note Records] {} note(s) × 11 bytes: PASS",
                  origRows);
            ++passed;
        } else {
            // 原始字节不一致，执行结构化逐条对比
            auto oRecs =
                parseAllIMDRecords(orig.data() + origNoteOff, origRows);
            auto eRecs =
                parseAllIMDRecords(expo.data() + expoNoteOff, expoRows);
            sortIMDRecords(oRecs);
            sortIMDRecords(eRecs);

            if ( oRecs == eRecs ) {
                XINFO(
                    "[IMD Chunk 5 - Note Records] 字节序不同但结构化一致: PASS "
                    "({} notes reordered by saver)",
                    origRows);
                ++passed;
            } else {
                size_t matchCnt = 0;
                size_t minCnt   = std::min(oRecs.size(), eRecs.size());
                for ( size_t i = 0; i < minCnt; ++i ) {
                    if ( oRecs[i] == eRecs[i] ) ++matchCnt;
                }
                XERROR(
                    "[IMD Chunk 5 - Note Records] FAIL "
                    "(raw_bytes_differ, structured_match={}/{})",
                    matchCnt,
                    minCnt);
            }
        }
    }

    return passed;
}

/// @}
/// @name Malody JSON 分区比较工具
/// @{

/// @brief 对两个 .mc JSON 文件进行分段落对比
/// 返回通过的段数，-1 表示读取失败
/// @param origPath 来源 Malody 文件。
/// @param exportPath 往返导出文件。
/// @return meta、time、effect、note 中通过的分区数。
inline int compareMalodySections(const std::filesystem::path& origPath,
                                 const std::filesystem::path& exportPath)
{
    using json = nlohmann::json;

    /// @brief 使用无异常模式加载单个 JSON 文件。
    auto loadJson = [](const std::filesystem::path& p) -> json {
        std::ifstream fs(p);
        if ( !fs.is_open() ) return {};
        json j = json::parse(fs, nullptr, false);
        if ( j.is_discarded() ) return {};
        return j;
    };

    json orig = loadJson(origPath);
    json expo = loadJson(exportPath);
    if ( orig.is_null() || expo.is_null() ) {
        XERROR("无法解析Malody JSON文件进行分段落对比");
        return -1;
    }

    // 每个分区独立累加，失败后继续检查其余分区以提供完整诊断。
    int passed = 0;

    // ---- 分段 1：meta ----
    {
        bool hasOrig = orig.contains("meta");
        bool hasExpo = expo.contains("meta");
        bool ok      = false;
        if ( hasOrig && hasExpo ) {
            const auto& om = orig["meta"];
            const auto& em = expo["meta"];
            // 忽略保存器可补全字段，只比较来源承诺保留的公共 meta。
            ok = (om.value("creator", "") == em.value("creator", "") &&
                  om.value("version", "") == em.value("version", "") &&
                  om.value("background", "") == em.value("background", ""));
            if ( ok && om.contains("song") && em.contains("song") ) {
                const auto& os = om["song"];
                const auto& es = em["song"];
                ok = (os.value("title", "") == es.value("title", "") &&
                      os.value("titleorg", "") == es.value("titleorg", "") &&
                      os.value("artist", "") == es.value("artist", "") &&
                      os.value("artistorg", "") == es.value("artistorg", "") &&
                      os.value("file", "") == es.value("file", "") &&
                      os.value("bpm", 0.0) == es.value("bpm", 0.0));
            }
            if ( ok && om.contains("mode_ext") && em.contains("mode_ext") ) {
                ok = (om["mode_ext"].value("column", 0) ==
                      em["mode_ext"].value("column", 0));
            }
        }
        if ( ok ) {
            XINFO("[Malody Section 1 - meta] 元数据段 (core fields): PASS");
            ++passed;
        } else {
            XERROR(
                "[Malody Section 1 - meta] 元数据段: FAIL (orig_has={} "
                "expo_has={})",
                hasOrig,
                hasExpo);
        }
    }

    // ---- Section 2: time (BPM 时间线) ----
    {
        bool hasOrig = orig.contains("time");
        bool hasExpo = expo.contains("time");
        bool ok      = false;
        if ( hasOrig && hasExpo ) {
            // 接收值副本，排序不会改变后续其他分区使用的根 JSON。
            auto sortByBeat = [](json arr) {
                std::sort(
                    arr.begin(), arr.end(), [](const json& a, const json& b) {
                        return a.value("beat", json::array()) <
                               b.value("beat", json::array());
                    });
                return arr;
            };
            json oArr = sortByBeat(orig["time"]);
            json eArr = sortByBeat(expo["time"]);
            // 数量相同后才按索引比较，避免越界与错位日志。
            if ( oArr.size() == eArr.size() ) {
                ok = true;
                for ( size_t i = 0; i < oArr.size(); ++i ) {
                    if ( oArr[i].value("bpm", 0.0) !=
                         eArr[i].value("bpm", 0.0) ) {
                        ok = false;
                        break;
                    }
                }
            }
        }
        if ( ok ) {
            size_t cnt = orig["time"].size();
            XINFO("[Malody Section 2 - time] BPM时间线段: PASS ({} entries)",
                  cnt);
            ++passed;
        } else {
            XERROR(
                "[Malody Section 2 - time] BPM时间线段: FAIL (orig_cnt={} "
                "expo_cnt={})",
                hasOrig ? orig["time"].size() : 0,
                hasExpo ? expo["time"].size() : 0);
        }
    }

    // ---- Section 3: effect (滚动效果) ----
    {
        bool hasOrig = orig.contains("effect");
        bool hasExpo = expo.contains("effect");
        bool ok      = false;
        if ( hasOrig && hasExpo ) {
            // 只保留当前格式哨兵 scroll，再按拍位形成可比较序列。
            auto sortByBeat = [](json arr) {
                json filtered = json::array();
                for ( const auto& item : arr ) {
                    if ( item.contains("scroll") ) {
                        filtered.push_back(item);
                    }
                }
                std::sort(filtered.begin(),
                          filtered.end(),
                          [](const json& a, const json& b) {
                              return a.value("beat", json::array()) <
                                     b.value("beat", json::array());
                          });
                return filtered;
            };
            json oArr = sortByBeat(orig["effect"]);
            json eArr = sortByBeat(expo["effect"]);
            if ( oArr.size() == eArr.size() ) {
                ok = true;
                for ( size_t i = 0; i < oArr.size(); ++i ) {
                    if ( oArr[i].value("scroll", 1.0) !=
                         eArr[i].value("scroll", 1.0) ) {
                        ok = false;
                        break;
                    }
                }
            }
        } else {
            // 缺失 effect 与空数组都表示没有有效效果，允许规范化差异。
            auto getEffectSize = [](const json& j) -> size_t {
                if ( j.contains("effect") && j["effect"].is_array() )
                    return j["effect"].size();
                return 0;
            };
            ok = (getEffectSize(orig) == 0 && getEffectSize(expo) == 0);
        }
        if ( ok ) {
            size_t cnt = orig.contains("effect") ? orig["effect"].size() : 0;
            XINFO("[Malody Section 3 - effect] 滚动效果段: PASS ({} entries)",
                  cnt);
            ++passed;
        } else {
            size_t oCnt = orig.contains("effect") ? orig["effect"].size() : 0;
            size_t eCnt = expo.contains("effect") ? expo["effect"].size() : 0;
            XERROR(
                "[Malody Section 3 - effect] 滚动效果段: FAIL (orig_cnt={} "
                "expo_cnt={})",
                oCnt,
                eCnt);
        }
    }

    // ---- Section 4: note（玩家物件与自动采样对象）----
    {
        const bool hasOrig = orig.contains("note");
        const bool hasExpo = expo.contains("note");
        bool       ok      = false;
        size_t     playableCount{ 0 };
        size_t     sampleCount{ 0 };
        if ( hasOrig && hasExpo ) {
            /// @brief 判断 Malody note 节点是否为自动采样对象。
            auto isSound = [](const json& node) -> bool {
                // 字符串与整数两种 type 分别对应 Slide 与 Key 的规范表示。
                if ( !node.contains("type") ) return false;
                if ( node["type"].is_string() ) {
                    return node["type"].get<std::string>() == "SOUND";
                }
                return node["type"].is_number_integer() &&
                       node["type"].get<int>() == 1;
            };
            /// @brief 把 Malody 拍号数组换算为可比较的小数拍。
            auto getBeatValue = [](const json& beatArray) {
                // 损坏三元组回退零，仅用于测试排序和差异报告。
                if ( beatArray.is_array() && beatArray.size() == 3 ) {
                    const double denominator = beatArray[2].get<double>();
                    if ( std::abs(denominator) > 1e-9 ) {
                        return beatArray[0].get<double>() +
                               beatArray[1].get<double>() / denominator;
                    }
                }
                return 0.0;
            };
            /// @brief 按节点类别筛选 Malody note 数组。
            auto filterNodes = [&](const json& array, bool wantSound) {
                // 玩家物件与 SOUND 分流后独立验证，防止类型互换后数量抵消。
                json filtered = json::array();
                for ( const auto& node : array ) {
                    if ( isSound(node) == wantSound ) {
                        filtered.push_back(node);
                    }
                }
                return filtered;
            };
            /// @brief 对玩家物件按拍号和轨道排序。
            auto sortPlayableNotes = [&](json array) {
                // 拍位相同再使用轨道、采样和音量建立确定全序。
                std::sort(
                    array.begin(),
                    array.end(),
                    [&](const json& lhs, const json& rhs) {
                        const double lhsBeat =
                            getBeatValue(lhs.value("beat", json::array()));
                        const double rhsBeat =
                            getBeatValue(rhs.value("beat", json::array()));
                        if ( std::abs(lhsBeat - rhsBeat) > 1e-5 ) {
                            return lhsBeat < rhsBeat;
                        }

                        const int lhsTrack =
                            lhs.value("column",
                                      lhs.contains("x")
                                          ? static_cast<int>(std::round(
                                                lhs["x"].get<double>() / 43.0))
                                          : 0);
                        const int rhsTrack =
                            rhs.value("column",
                                      rhs.contains("x")
                                          ? static_cast<int>(std::round(
                                                rhs["x"].get<double>() / 43.0))
                                          : 0);
                        if ( lhsTrack != rhsTrack ) {
                            return lhsTrack < rhsTrack;
                        }
                        const std::string lhsSound =
                            lhs.value("sound", std::string{});
                        const std::string rhsSound =
                            rhs.value("sound", std::string{});
                        if ( lhsSound != rhsSound ) {
                            return lhsSound < rhsSound;
                        }
                        return lhs.value("vol", 0.0) < rhs.value("vol", 0.0);
                    });
                return array;
            };
            /// @brief 对自动采样对象按其完整语义字段排序。
            auto sortSamples = [&](json array) {
                // SOUND 排序覆盖其完整播放身份，不依赖来源数组顺序。
                std::sort(
                    array.begin(),
                    array.end(),
                    [&](const json& lhs, const json& rhs) {
                        const double lhsBeat =
                            getBeatValue(lhs.value("beat", json::array()));
                        const double rhsBeat =
                            getBeatValue(rhs.value("beat", json::array()));
                        if ( std::abs(lhsBeat - rhsBeat) > 1e-5 ) {
                            return lhsBeat < rhsBeat;
                        }
                        const std::string lhsSound =
                            lhs.value("sound", std::string{});
                        const std::string rhsSound =
                            rhs.value("sound", std::string{});
                        if ( lhsSound != rhsSound ) {
                            return lhsSound < rhsSound;
                        }
                        const int lhsTrack = lhs.value("x", -1);
                        const int rhsTrack = rhs.value("x", -1);
                        if ( lhsTrack != rhsTrack ) {
                            return lhsTrack < rhsTrack;
                        }
                        const double lhsOffset = lhs.value("offset", 0.0);
                        const double rhsOffset = rhs.value("offset", 0.0);
                        if ( std::abs(lhsOffset - rhsOffset) > 1e-5 ) {
                            return lhsOffset < rhsOffset;
                        }
                        return lhs.value("vol", 0.0) < rhs.value("vol", 0.0);
                    });
                return array;
            };

            // 玩家物件先比较数量，再逐项检查公共形状和绑定字段。
            const json originalPlayable =
                sortPlayableNotes(filterNodes(orig["note"], false));
            const json exportedPlayable =
                sortPlayableNotes(filterNodes(expo["note"], false));
            playableCount = originalPlayable.size();
            bool playableOk =
                originalPlayable.size() == exportedPlayable.size();
            for ( size_t i = 0; playableOk && i < originalPlayable.size();
                  ++i ) {
                const auto& original = originalPlayable[i];
                const auto& exported = exportedPlayable[i];
                if ( std::abs(
                         getBeatValue(original.value("beat", json::array())) -
                         getBeatValue(exported.value("beat", json::array()))) >
                         1e-3 ||
                     std::abs(getBeatValue(
                                  original.value("endbeat", json::array())) -
                              getBeatValue(exported.value(
                                  "endbeat", json::array()))) > 1e-3 ) {
                    XERROR("Playable note mismatch at index {}: beat differs",
                           i);
                    playableOk = false;
                } else if ( original.contains("column") &&
                            exported.contains("column") &&
                            original.value("column", 0) !=
                                exported.value("column", 0) ) {
                    XERROR("Playable note mismatch at index {}: column differs",
                           i);
                    playableOk = false;
                } else if ( original.contains("seg") &&
                            exported.contains("seg") &&
                            original["seg"].size() != exported["seg"].size() ) {
                    XERROR(
                        "Playable note mismatch at index {}: seg size "
                        "differs",
                        i);
                    playableOk = false;
                } else if ( original.contains("seg") !=
                            exported.contains("seg") ) {
                    XERROR(
                        "Playable note mismatch at index {}: seg presence "
                        "differs",
                        i);
                    playableOk = false;
                } else if ( original.value("sound", std::string{}) !=
                                exported.value("sound", std::string{}) ||
                            std::abs(original.value("vol", 0.0) -
                                     exported.value("vol", 0.0)) > 1e-3 ) {
                    XERROR(
                        "Playable note mismatch at index {}: sample binding "
                        "differs",
                        i);
                    playableOk = false;
                }
                if ( !playableOk ) {
                    XERROR("O: {}", original.dump());
                    XERROR("E: {}", exported.dump());
                }
            }

            // 自动采样另建序列，模式决定 type 与 x 的规范约束。
            const json originalSamples =
                sortSamples(filterNodes(orig["note"], true));
            const json exportedSamples =
                sortSamples(filterNodes(expo["note"], true));
            // 以导出 meta.mode 判断保存器选择的规范 SOUND 形态。
            const bool exportsSlideMode = expo.contains("meta") &&
                                          expo["meta"].is_object() &&
                                          expo["meta"].value("mode", -1) == 7;
            sampleCount   = originalSamples.size();
            bool sampleOk = originalSamples.size() == exportedSamples.size();
            for ( size_t i = 0; sampleOk && i < originalSamples.size(); ++i ) {
                const auto& original = originalSamples[i];
                const auto& exported = exportedSamples[i];
                const bool  canonicalType =
                    exported.contains("type") &&
                    ((exportsSlideMode && exported["type"].is_string() &&
                      exported["type"].get<std::string>() == "SOUND") ||
                     (!exportsSlideMode &&
                      exported["type"].is_number_integer() &&
                      exported["type"].get<int>() == 1));
                const bool canonicalTrack =
                    exportsSlideMode
                        ? !exported.contains("x")
                        : exported.contains("x") && exported["x"].is_number();
                const bool matchingExplicitTrack =
                    exportsSlideMode || !original.contains("x") ||
                    (exported.contains("x") &&
                     original.value("x", -1) == exported.value("x", -1));
                const bool matchingFields =
                    std::abs(
                        getBeatValue(original.value("beat", json::array())) -
                        getBeatValue(exported.value("beat", json::array()))) <=
                        1e-3 &&
                    original.value("sound", std::string{}) ==
                        exported.value("sound", std::string{}) &&
                    std::abs(original.value("offset", 0.0) -
                             exported.value("offset", 0.0)) <= 1e-3 &&
                    std::abs(original.value("vol", 0.0) -
                             exported.value("vol", 0.0)) <= 1e-3;
                if ( !canonicalType || !canonicalTrack ||
                     exported.contains("column") || !matchingExplicitTrack ||
                     !matchingFields ) {
                    XERROR("Audio sample mismatch at index {}", i);
                    XERROR("O: {}", original.dump());
                    XERROR("E: {}", exported.dump());
                    sampleOk = false;
                }
            }
            // 两类节点必须同时通过，数量或字段失败不能互相抵扣。
            ok = playableOk && sampleOk;
        }
        if ( ok ) {
            XINFO(
                "[Malody Section 4 - note] 物件段: PASS "
                "({} playable notes, {} audio samples)",
                playableCount,
                sampleCount);
            ++passed;
        } else {
            const size_t originalCount =
                hasOrig && orig["note"].is_array() ? orig["note"].size() : 0;
            const size_t exportedCount =
                hasExpo && expo["note"].is_array() ? expo["note"].size() : 0;
            XERROR(
                "[Malody Section 4 - note] 物件段: FAIL (orig_cnt={} "
                "expo_cnt={})",
                originalCount,
                exportedCount);
        }
    }

    return passed;
}

/// @}
/// @name osu! 文本章节比较工具
/// @{

/// @brief 将 osu 文件文本按 [SectionName] 拆分
/// @param content 完整 osu! 文本。
/// @return 章节名称到过滤后正文的映射。
inline std::map<std::string, std::string> splitOSUSections(
    const std::string& content)
{
    std::map<std::string, std::string> sections;
    std::istringstream                 iss(content);
    std::string                        line;
    std::string                        currentSection;
    std::ostringstream                 currentContent;

    // 逐行识别章节头，并把非注释正文累计到当前章节。
    while ( std::getline(iss, line) ) {
        if ( !line.empty() && line.back() == '\r' ) line.pop_back();
        if ( line.empty() ) continue;

        if ( line[0] == '[' && line.back() == ']' ) {
            // 遇到新章节前提交上一章节，随后清空内容缓冲。
            if ( !currentSection.empty() ) {
                sections[currentSection] = currentContent.str();
                currentContent.str("");
            }
            currentSection = line.substr(1, line.size() - 2);
        } else if ( !currentSection.empty() ) {
            if ( !line.empty() && line[0] != '/' && line[0] != ';' ) {
                currentContent << line << "\n";
            }
        }
    }
    // 文件结束没有下一章节头，需要显式提交最后章节。
    if ( !currentSection.empty() ) {
        sections[currentSection] = currentContent.str();
    }
    return sections;
}

/// @brief 提取 osu 章节中的键值对映射
/// @param sectionContent 单个章节正文。
/// @param out 接收键值的映射；同名键以后出现的值覆盖。
inline void extractOSUKeyValues(const std::string& sectionContent,
                                std::map<std::string, std::string>& out)
{
    std::istringstream iss(sectionContent);
    std::string        line;
    while ( std::getline(iss, line) ) {
        if ( line.empty() ) continue;
        // 只以首个冒号分隔，值中路径或时间格式的后续冒号继续保留。
        size_t colonPos = line.find(':');
        if ( colonPos != std::string::npos && colonPos > 0 && line[0] != '/' &&
             line[0] != ';' ) {
            std::string key   = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);
            // 键保持原文，值去除首尾水平空白后比较。
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            out[key] = value;
        }
    }
}

/// @brief 统计 osu 章节中的行数（过滤注释和空行）
/// @param sectionContent 单个章节正文。
/// @return 有效数据行数量。
inline size_t countOSULines(const std::string& sectionContent)
{
    std::istringstream iss(sectionContent);
    std::string        line;
    size_t             count = 0;
    while ( std::getline(iss, line) ) {
        if ( !line.empty() && line[0] != '/' && line[0] != ';' ) ++count;
    }
    return count;
}

/// @brief 超集检查：验证 orig_kv 的所有键都在 expo_kv 中且值相同
/// @return 匹配的键数，-1 表示存在缺失或值不匹配
/// @note 导出映射允许包含保存器补充的默认键。
inline int checkOSUKVSuperset(const std::map<std::string, std::string>& origKV,
                              const std::map<std::string, std::string>& expoKV)
{
    int matched = 0;
    // 来源键是必须保留的下界，发现第一处差异即可返回定位。
    for ( const auto& [key, val] : origKV ) {
        auto it = expoKV.find(key);
        if ( it == expoKV.end() ) {
            XWARN("  → 原始键 [{}] 在导出中缺失", key);
            return -1;
        }
        if ( it->second != val ) {
            XWARN("  → 键 [{}] 值不一致: orig='{}' vs expo='{}'",
                  key,
                  val,
                  it->second);
            return -1;
        }
        ++matched;
    }
    return matched;
}

/// @brief 对两个 .osu 文本文件进行分章节对比
/// 返回通过的章节数，-1 表示读取失败
/// @param origPath 来源 osu! 文件。
/// @param exportPath 往返导出文件。
/// @return 七个预期章节中通过的数量。
inline int compareOSUSections(const std::filesystem::path& origPath,
                              const std::filesystem::path& exportPath)
{
    /// @brief 读取完整文本，打开失败时结果为空。
    auto readText = [](const std::filesystem::path& p) -> std::string {
        std::ifstream      fs(p);
        std::ostringstream oss;
        oss << fs.rdbuf();
        return oss.str();
    };

    std::string origText = readText(origPath);
    std::string expoText = readText(exportPath);
    if ( origText.empty() || expoText.empty() ) {
        XERROR("无法读取OSU文件进行分章节对比");
        return -1;
    }

    auto origSec = splitOSUSections(origText);
    auto expoSec = splitOSUSections(expoText);

    int passed = 0;

    // 固定章节列表让缺失章节也产生明确通过或失败结果。
    const std::vector<std::string> expectedSections = {
        "General", "Editor",       "Metadata",  "Difficulty",
        "Events",  "TimingPoints", "HitObjects"
    };

    for ( const auto& secName : expectedSections ) {
        auto oIt     = origSec.find(secName);
        auto eIt     = expoSec.find(secName);
        bool hasOrig = (oIt != origSec.end());
        bool hasExpo = (eIt != expoSec.end());

        // 两边都缺失表示该谱面不使用此可选章节。
        if ( !hasOrig && !hasExpo ) {
            XINFO("[OSU Section - {}] 章节均不存在: PASS", secName);
            ++passed;
            continue;
        }

        if ( !hasOrig || !hasExpo ) {
            XERROR(
                "[OSU Section - {}] 章节存在性不一致: FAIL (orig={} expo={})",
                secName,
                hasOrig,
                hasExpo);
            continue;
        }

        const std::string& oCont = oIt->second;
        const std::string& eCont = eIt->second;

        if ( secName == "TimingPoints" || secName == "HitObjects" ) {
            // 列表章节不含稳定键名，格式层只检查有效行数。
            // 逗号分隔列表章节：只比较行数
            size_t oLines = countOSULines(oCont);
            size_t eLines = countOSULines(eCont);
            if ( oLines == eLines ) {
                XINFO("[OSU Section - {}] {} 行: PASS", secName, oLines);
                ++passed;
            } else {
                XERROR("[OSU Section - {}] 行数不一致: FAIL (orig={} expo={})",
                       secName,
                       oLines,
                       eLines);
            }
        } else {
            // 键值章节要求来源集合为导出集合的子集。
            std::map<std::string, std::string> oKV, eKV;
            extractOSUKeyValues(oCont, oKV);
            extractOSUKeyValues(eCont, eKV);

            int matched = checkOSUKVSuperset(oKV, eKV);
            if ( matched >= 0 && matched == (int)oKV.size() ) {
                XINFO("[OSU Section - {}] {}/{} 键匹配 (导出额外{}键): PASS",
                      secName,
                      matched,
                      oKV.size(),
                      eKV.size() - oKV.size());
                ++passed;
            } else {
                XERROR(
                    "[OSU Section - {}] 键值对不一致: FAIL "
                    "(orig={} expo={} matched={})",
                    secName,
                    oKV.size(),
                    eKV.size(),
                    matched);
            }
        }
    }

    return passed;
}

}  // namespace MMM::Test
