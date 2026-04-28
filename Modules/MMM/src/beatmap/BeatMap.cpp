#include "mmm/beatmap/BeatMap.h"

#include "LoadMMMMap.hpp"
#include "LoadMalodyMap.hpp"
#include "LoadOSUMap.hpp"
#include "LoadRMMap.hpp"
#include "SaveMMMMap.hpp"
#include "SaveMalodyMap.hpp"
#include "SaveOSUMap.hpp"
#include "SaveRMMap.hpp"

#include "log/colorful-log.h"
#include <filesystem>

namespace MMM
{

/**
 * @brief 从文件加载谱面
 * @param mapFilePath 谱面文件路径
 */
BeatMap BeatMap::loadFromFile(std::filesystem::path mapFilePath)
{
    if ( !std::filesystem::exists(mapFilePath) ) {
        XWARN("Load Map Failed: File Not Exists.");
        return {};
    }
    if ( !mapFilePath.has_extension() ) {
        XWARN("Load Map Failed: Unknown File extension.");
        return {};
    }
    std::string mapFileExtention = mapFilePath.extension().generic_string();
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
    std::string mapFileExtention = mapFilePath.extension().generic_string();
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
        m_allNotes.push_back(std::ref(note));
    }
    // 添加所有长条物件
    for ( auto& hold : m_noteData.holds ) {
        m_allNotes.push_back(std::ref(hold));
    }
    // 添加所有滑键物件
    for ( auto& flick : m_noteData.flicks ) {
        m_allNotes.push_back(std::ref(flick));
    }
    // 添加所有折线物件
    for ( auto& poly : m_noteData.polylines ) {
        m_allNotes.push_back(std::ref(poly));
    }

    // 按时间戳排序
    std::stable_sort(m_allNotes.begin(),
                     m_allNotes.end(),
                     [](const std::reference_wrapper<Note>& a,
                        const std::reference_wrapper<Note>& b) {
                         return a.get().m_timestamp < b.get().m_timestamp;
                     });
}

BeatMap::BeatMap() {}

BeatMap::~BeatMap() {}
}  // namespace MMM
