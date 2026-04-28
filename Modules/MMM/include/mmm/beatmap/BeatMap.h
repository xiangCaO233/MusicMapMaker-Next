#pragma once

#include "mmm/Metadata.h"
#include "mmm/note/Flick.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Note.h"
#include "mmm/note/Polyline.h"
#include "mmm/timing/Timing.h"
#include <deque>
#include <filesystem>
#include <functional>
#include <vector>

namespace MMM
{

struct NoteData {
    /// @brief 所有普通物件
    std::deque<Note> notes;

    /// @brief 所有长条物件
    std::deque<Hold> holds;

    /// @brief 所有滑键物件
    std::deque<Flick> flicks;

    /// @brief 所有折线物件
    std::deque<Polyline> polylines;
};

class BeatMap
{
public:
    BeatMap();
    BeatMap(BeatMap&&)                 = default;
    BeatMap(const BeatMap&)            = delete;
    BeatMap& operator=(BeatMap&&)      = default;
    BeatMap& operator=(const BeatMap&) = delete;
    ~BeatMap();

    /**
     * @brief 从文件加载谱面
     * @param mapFilePath 谱面文件路径
     */
    static BeatMap loadFromFile(std::filesystem::path mapFilePath);

    /**
     * @brief 保存谱面到文件
     * @param mapFilePath 保存的目标路径
     * @return 是否保存成功
     */
    bool saveToFile(std::filesystem::path mapFilePath) const;

    /// @brief 同步物件引用表 (m_allNotes)
    void sync();

    /// @brief 所有物件引用
    std::vector<std::reference_wrapper<Note>> m_allNotes;

    /// @brief 所有物件数据
    NoteData m_noteData;

    /// @brief 所有时间线
    std::vector<Timing> m_timings;

    /// @brief 谱面基本元数据
    BaseMapMeta m_baseMapMetadata;

    /// @brief 所有谱面元数据
    MapMetadata m_metadata;
};

}  // namespace MMM
