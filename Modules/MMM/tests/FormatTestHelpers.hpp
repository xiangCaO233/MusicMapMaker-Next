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

// ---------------------------------------------------------------------------
// │ IMD 二进制分块对比工具
// ---------------------------------------------------------------------------

/// @brief 从文件中读取全部字节
inline std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return {};
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

/// @brief 小端读取 int32
inline int32_t readLEInt32(const uint8_t* p)
{
    int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

/// @brief 小端读取 int16
inline int16_t readLEInt16(const uint8_t* p)
{
    int16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

/// @brief 比较原始字节块，返回 true 表示完全一致
inline bool compareRawBytes(const uint8_t* a, const uint8_t* b, size_t len,
                            int& firstDiffOffset)
{
    firstDiffOffset = -1;
    for ( size_t i = 0; i < len; ++i ) {
        if ( a[i] != b[i] ) {
            firstDiffOffset = static_cast<int>(i);
            return false;
        }
    }
    return true;
}

/// @brief 比较原始字节块（无差异位置版）
inline bool compareRawBytes(const uint8_t* a, const uint8_t* b, size_t len)
{
    int dummy;
    return compareRawBytes(a, b, len, dummy);
}

/// @brief IMD 单条物件记录（11字节）的结构化解析
struct IMDParsedRecord {
    int8_t  typeInfo;
    uint8_t reserved;
    int32_t timestamp;
    uint8_t track;
    int32_t parameter;

    bool operator==(const IMDParsedRecord& o) const
    {
        return typeInfo == o.typeInfo && timestamp == o.timestamp &&
               track == o.track && parameter == o.parameter;
    }
};

/// @brief 从二进制指针解析一条 IMD 记录
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
inline int compareIMDChunks(const std::filesystem::path& origPath,
                            const std::filesystem::path& exportPath)
{
    auto orig = readBinaryFile(origPath);
    auto expo = readBinaryFile(exportPath);
    if ( orig.empty() || expo.empty() ) {
        XERROR("无法读取IMD文件进行二进制对比");
        return -1;
    }

    int passed = 0;

    // ---- Chunk 1: Header (8 bytes): int32 map_length + int32 timing_count ----
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

    // ---- Chunk 2: Timing Data (timing_count × 12 bytes) ----
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

    // ---- Chunk 3: Magic Marker (2 bytes): 0x0303 ----
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

    // ---- Chunk 4: Table Row Count (4 bytes): int32 ----
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

    // ---- Chunk 5: Note Records (table_rows × 11 bytes) ----
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

// ---------------------------------------------------------------------------
// │ Malody JSON 分段落对比工具
// ---------------------------------------------------------------------------

/// @brief 对两个 .mc JSON 文件进行分段落对比
/// 返回通过的段数，-1 表示读取失败
inline int compareMalodySections(const std::filesystem::path& origPath,
                                 const std::filesystem::path& exportPath)
{
    using json = nlohmann::json;

    auto loadJson = [](const std::filesystem::path& p) -> json {
        std::ifstream fs(p);
        if ( !fs.is_open() ) return {};
        json j;
        try {
            fs >> j;
        } catch ( ... ) {
            return {};
        }
        return j;
    };

    json orig = loadJson(origPath);
    json expo = loadJson(exportPath);
    if ( orig.is_null() || expo.is_null() ) {
        XERROR("无法解析Malody JSON文件进行分段落对比");
        return -1;
    }

    int passed = 0;

    // ---- Section 1: meta ----
    {
        bool hasOrig = orig.contains("meta");
        bool hasExpo = expo.contains("meta");
        bool ok      = false;
        if ( hasOrig && hasExpo ) {
            const auto& om = orig["meta"];
            const auto& em = expo["meta"];
            // 比较核心字段（忽略 id/preview/mode 等 saver 自动补全的键）
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
            auto sortByBeat = [](json arr) {
                std::sort(
                    arr.begin(), arr.end(), [](const json& a, const json& b) {
                        return a.value("beat", json::array()) <
                               b.value("beat", json::array());
                    });
                return arr;
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
            ok = (!hasOrig && !hasExpo);
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

    // ---- Section 4: note (物件段) ----
    {
        bool hasOrig = orig.contains("note");
        bool hasExpo = expo.contains("note");
        bool ok      = false;
        if ( hasOrig && hasExpo ) {
            // 过滤掉 type=1 的声音物件（加载器会跳过它们）
            auto filterNotes = [](const json& arr) -> json {
                json filtered = json::array();
                for ( const auto& n : arr ) {
                    if ( n.value("type", 0) != 1 ) {
                        filtered.push_back(n);
                    }
                }
                return filtered;
            };
            auto sortNotes = [](json arr) {
                std::sort(
                    arr.begin(), arr.end(), [](const json& a, const json& b) {
                        auto getBeatVal = [](const json& beatArr) {
                            if ( beatArr.is_array() && beatArr.size() == 3 ) {
                                return beatArr[0].get<double>() +
                                       beatArr[1].get<double>() /
                                           beatArr[2].get<double>();
                            }
                            return 0.0;
                        };
                        double av = getBeatVal(a.value("beat", json::array()));
                        double bv = getBeatVal(b.value("beat", json::array()));
                        if ( std::abs(av - bv) > 1e-5 ) return av < bv;

                        int colA = a.value(
                            "column",
                            a.contains("x")
                                ? (int)std::round(a["x"].get<int>() / 43.0)
                                : 0);
                        int colB = b.value(
                            "column",
                            b.contains("x")
                                ? (int)std::round(b["x"].get<int>() / 43.0)
                                : 0);

                        return colA < colB;
                    });
                return arr;
            };
            json oArr = sortNotes(filterNotes(orig["note"]));
            json eArr = sortNotes(filterNotes(expo["note"]));
            if ( oArr.size() == eArr.size() ) {
                ok = true;
                for ( size_t i = 0; i < oArr.size(); ++i ) {
                    const auto& o          = oArr[i];
                    const auto& e          = eArr[i];
                    auto        getBeatVal = [](const json& b) {
                        if ( b.is_array() && b.size() == 3 ) {
                            return b[0].get<double>() +
                                   b[1].get<double>() / b[2].get<double>();
                        }
                        return 0.0;
                    };
                    if ( std::abs(getBeatVal(o.value("beat", json::array())) -
                                  getBeatVal(e.value("beat", json::array()))) >
                             1e-3 ||
                         std::abs(
                             getBeatVal(o.value("endbeat", json::array())) -
                             getBeatVal(e.value("endbeat", json::array()))) >
                             1e-3 ) {
                        XERROR("Mismatch at index {}: beat diff > 1e-3", i);
                        XERROR("O: {}", o.dump());
                        XERROR("E: {}", e.dump());
                        ok = false;
                        break;
                    }
                    if ( o.contains("column") && e.contains("column") &&
                         o.value("column", 0) != e.value("column", 0) ) {
                        XERROR("Mismatch at index {}: column mismatch", i);
                        XERROR("O: {}", o.dump());
                        XERROR("E: {}", e.dump());
                        ok = false;
                        break;
                    }
                    if ( o.contains("seg") && e.contains("seg") ) {
                        if ( o["seg"].size() != e["seg"].size() ) {
                            XERROR("Mismatch at index {}: seg size mismatch",
                                   i);
                            XERROR("O: {}", o.dump());
                            XERROR("E: {}", e.dump());
                            ok = false;
                            break;
                        }
                    } else if ( o.contains("seg") != e.contains("seg") ) {
                        XERROR("Mismatch at index {}: seg presence mismatch",
                               i);
                        XERROR("O: {}", o.dump());
                        XERROR("E: {}", e.dump());
                        ok = false;
                        break;
                    }
                }
            }
        }
        if ( ok ) {
            size_t totalCnt = orig.contains("note") ? orig["note"].size() : 0;
            size_t type1Cnt = 0;
            if ( orig.contains("note") ) {
                for ( const auto& n : orig["note"] ) {
                    if ( n.value("type", 0) == 1 ) ++type1Cnt;
                }
            }
            XINFO(
                "[Malody Section 4 - note] 物件段: PASS "
                "({} notes, {} sound objects filtered)",
                totalCnt - type1Cnt,
                type1Cnt);
            ++passed;
        } else {
            size_t oCnt = orig.contains("note") ? orig["note"].size() : 0;
            size_t eCnt = expo.contains("note") ? expo["note"].size() : 0;
            XERROR(
                "[Malody Section 4 - note] 物件段: FAIL (orig_cnt={} "
                "expo_cnt={})",
                oCnt,
                eCnt);
        }
    }

    return passed;
}

// ---------------------------------------------------------------------------
// │ OSU 文本分章节对比工具
// ---------------------------------------------------------------------------

/// @brief 将 osu 文件文本按 [SectionName] 拆分
inline std::map<std::string, std::string> splitOSUSections(
    const std::string& content)
{
    std::map<std::string, std::string> sections;
    std::istringstream                 iss(content);
    std::string                        line;
    std::string                        currentSection;
    std::ostringstream                 currentContent;

    while ( std::getline(iss, line) ) {
        if ( !line.empty() && line.back() == '\r' ) line.pop_back();
        if ( line.empty() ) continue;

        if ( line[0] == '[' && line.back() == ']' ) {
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
    if ( !currentSection.empty() ) {
        sections[currentSection] = currentContent.str();
    }
    return sections;
}

/// @brief 提取 osu 章节中的键值对映射
inline void extractOSUKeyValues(const std::string& sectionContent,
                                std::map<std::string, std::string>& out)
{
    std::istringstream iss(sectionContent);
    std::string        line;
    while ( std::getline(iss, line) ) {
        if ( line.empty() ) continue;
        size_t colonPos = line.find(':');
        if ( colonPos != std::string::npos && colonPos > 0 && line[0] != '/' &&
             line[0] != ';' ) {
            std::string key   = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            out[key] = value;
        }
    }
}

/// @brief 统计 osu 章节中的行数（过滤注释和空行）
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
inline int checkOSUKVSuperset(const std::map<std::string, std::string>& origKV,
                              const std::map<std::string, std::string>& expoKV)
{
    int matched = 0;
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
inline int compareOSUSections(const std::filesystem::path& origPath,
                              const std::filesystem::path& exportPath)
{
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

    const std::vector<std::string> expectedSections = {
        "General", "Editor",       "Metadata",  "Difficulty",
        "Events",  "TimingPoints", "HitObjects"
    };

    for ( const auto& secName : expectedSections ) {
        auto oIt     = origSec.find(secName);
        auto eIt     = expoSec.find(secName);
        bool hasOrig = (oIt != origSec.end());
        bool hasExpo = (eIt != expoSec.end());

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
            // 键值对章节：超集检查（导出可包含额外默认键）
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
