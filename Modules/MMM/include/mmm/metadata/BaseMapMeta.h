#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace MMM
{
/// @brief 标识背景资源类型，保持历史枚举数值。
enum class CoverType {
    /// @brief 静态图片背景。
    IMAGE,
    /// @brief 带起播偏移的视频背景。
    VIDEO,
};

/// @brief 跨格式共享的谱面基本信息。
struct BaseMapMeta {
    /// @brief 谱面名称
    std::string name;
    /// @brief 谱面歌曲标题
    std::string title;
    /// @brief 谱面歌曲标题(unicode)
    std::string title_unicode;
    /// @brief 谱面歌曲艺术家
    std::string artist;
    /// @brief 谱面歌曲艺术家(unicode)
    std::string artist_unicode;
    /// @brief 歌曲所属专辑；音频没有该标签时保持为空。
    std::string album;
    /// @brief 谱面文件路径
    std::filesystem::path map_path;
    /// @brief 旧版单主音频路径；仅供兼容尚未迁移的调用方，不再决定播放事件。
    std::filesystem::path main_audio_path;
    /// @brief 歌曲文件提示，用于格式元数据显示和资源选择。
    std::filesystem::path song_file_hint;
    /// @brief 主背景文件路径
    std::filesystem::path main_cover_path;
    /// @brief 封面图片文件路径
    std::filesystem::path cover_path;
    /// @brief 主背景类型
    CoverType cover_type{ CoverType::IMAGE };
    /// @brief 主背景类型为视频时的开始时间
    int32_t video_starttime{ 0 };
    /// @brief 背景的位置x偏移
    int32_t bgxoffset{ 0 };
    /// @brief 背景的位置y偏移
    int32_t bgyoffset{ 0 };

    /// @brief 谱面版本名
    std::string version;
    /// @brief 谱面作者
    std::string author;

    /// @brief 谱面参考bpm
    double preference_bpm{ -1. };
    /// @brief 玩家可操作的主轨道数量。
    int32_t track_count{ -1 };
    /// @brief 主轨道区之后用于自动采样对象的 BGM 轨道数量。
    int32_t bgm_track_count{ 0 };
    /// @brief 谱面总时长
    double map_length{ -1. };
};

}  // namespace MMM
