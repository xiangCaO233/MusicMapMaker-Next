#include "mmm/beatmap/BeatMap.h"

#include "LoadMMMMap.hpp"
#include "LoadMalodyMap.hpp"
#include "LoadOSUMap.hpp"
#include "LoadRMMap.hpp"
#include "SaveMMMMap.hpp"
#include "SaveMalodyMap.hpp"
#include "SaveOSUMap.hpp"
#include "SaveRMMap.hpp"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <fstream>

namespace MMM
{

/**
 * @brief 从文件加载谱面
 * @param mapFilePath 谱面文件路径
 */
BeatMap BeatMap::loadFromFile(std::filesystem::path mapFilePath)
{
    std::error_code ec;
    if ( !std::filesystem::exists(mapFilePath, ec) ) {
        XWARN("Load Map Failed: File Not Exists or Access Denied: {}", Config::pathToUtf8(mapFilePath));
        return {};
    }

    if ( !mapFilePath.has_extension() ) {
        std::ifstream ifs(mapFilePath);
        if ( ifs.is_open() ) {
            std::string firstLine;
            if ( std::getline(ifs, firstLine) ) {
                if ( firstLine.find("osu file format") != std::string::npos ) {
                    return loadOSUMap(mapFilePath);
                }
            }
        }
        XWARN(
            "Load Map Failed: Unknown File extension and content check "
            "failed.");
        return {};
    }
    std::string mapFileExtention = Config::pathToUtf8(mapFilePath.extension());
    if ( mapFileExtention == ".osu" ) {
        return loadOSUMap(mapFilePath);
    }
    if ( mapFileExtention == ".mc" ) {
        return loadMalodyMap(mapFilePath);
    }
    if ( mapFileExtention == ".imd" ) {
        return loadRMMap(mapFilePath);
    }
    if ( mapFileExtention == ".mmm" ) {
        return loadMMMMap(mapFilePath);
    }
    XWARN("Unsupport map file type: {}", mapFileExtention);
    return {};
}

bool BeatMap::saveToFile(std::filesystem::path mapFilePath) const
{
    std::string mapFileExtention = Config::pathToUtf8(mapFilePath.extension());
    if ( mapFileExtention == ".osu" ) {
        return saveOSUMap(*this, mapFilePath);
    }
    if ( mapFileExtention == ".mc" ) {
        return saveMalodyMap(*this, mapFilePath);
    }
    if ( mapFileExtention == ".imd" ) {
        return saveRMMap(*this, mapFilePath);
    }
    if ( mapFileExtention == ".mmm" ) {
        return saveMMMMap(*this, mapFilePath);
    }
    XWARN("Unsupport save map file type: {}", mapFileExtention);
    return false;
}

void BeatMap::sync()
{
    m_allNotes.clear();
    // 添加所有普通物件
    for ( auto& note : m_noteData.notes ) {
        if ( !note.m_isSubNote ) m_allNotes.push_back(std::ref(note));
    }
    // 添加所有长条物件
    for ( auto& hold : m_noteData.holds ) {
        if ( !hold.m_isSubNote ) m_allNotes.push_back(std::ref(hold));
    }
    // 添加所有滑键物件
    for ( auto& flick : m_noteData.flicks ) {
        if ( !flick.m_isSubNote ) m_allNotes.push_back(std::ref(flick));
    }
    // 添加所有折线物件
    for ( auto& poly : m_noteData.polylines ) {
        m_allNotes.push_back(std::ref(poly));
    }

    // 确定性排序：时间戳为主键，轨道和类型为次键
    std::stable_sort(m_allNotes.begin(),
                     m_allNotes.end(),
                     [](const std::reference_wrapper<Note>& a_ref,
                        const std::reference_wrapper<Note>& b_ref) {
                         const Note& a = a_ref.get();
                         const Note& b = b_ref.get();
                         if ( std::abs(a.m_timestamp - b.m_timestamp) > 1e-4 )
                             return a.m_timestamp < b.m_timestamp;
                         if ( a.m_track != b.m_track )
                             return a.m_track < b.m_track;
                         return a.m_type < b.m_type;
                     });
}

BeatMap::BeatMap() {}

BeatMap::~BeatMap() {}
}  // namespace MMM
