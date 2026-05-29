#pragma once

#include "mmm/beatmap/BeatMap.h"
#include "ui/IUIView.h"
#include <filesystem>
#include <string>

namespace MMM::UI
{

class NewBeatmapWizard : public IUIView
{
public:
    /// @brief 构造新建谱面向导。
    NewBeatmapWizard();

    /// @brief 销毁新建谱面向导。
    virtual ~NewBeatmapWizard() = default;

    /// @brief 更新并绘制新建谱面向导弹窗。
    /// @param sourceManager 当前 UI 管理器。
    void update(UIManager* sourceManager) override;

    /// @brief 打开向导并重置输入状态。
    void open();

    /// @brief 关闭向导弹窗。
    void close();

private:
    /// @brief 重置向导字段到默认值。
    void reset();

    /// @brief 处理音频资源选择并读取音频元数据。
    /// @param path 项目内音频相对路径。
    void onAudioSelected(const std::filesystem::path& path);

    /// @brief 当前向导弹窗是否打开。
    bool m_isOpen = false;
    /// @brief 下一帧是否需要打开弹窗。
    bool m_shouldOpen = false;
    /// @brief 待创建谱面的基础元数据。
    MMM::BaseMapMeta m_meta;


    /// @brief 谱面内部名称输入缓冲区。
    char m_nameBuf[256] = { 0 };
    /// @brief 标题输入缓冲区。
    char m_titleBuf[256] = { 0 };
    /// @brief Unicode 标题输入缓冲区。
    char m_titleUnicodeBuf[256] = { 0 };
    /// @brief 艺术家输入缓冲区。
    char m_artistBuf[256] = { 0 };
    /// @brief Unicode 艺术家输入缓冲区。
    char m_artistUnicodeBuf[256] = { 0 };
    /// @brief 谱师输入缓冲区。
    char m_authorBuf[256] = { 0 };
    /// @brief 难度名输入缓冲区。
    char m_versionBuf[256] = { 0 };

    /// @brief 谱面默认 BPM。
    double m_bpm = 120.0;
    /// @brief 谱面轨道数。
    int m_trackCount = 4;
    /// @brief 选中的主音频相对路径。
    std::filesystem::path m_selectedAudioPath;
    /// @brief 选中的主音频资源 ID。
    std::string m_selectedAudioTrackId;
    /// @brief 选中的背景资源相对路径。
    std::filesystem::path m_selectedCoverPath;
    /// @brief 选中的封面图片路径
    std::filesystem::path m_selectedCoverImgPath;
    /// @brief 当前选中音频时长，单位为秒。
    double m_audioDuration = 0.0;
};

}  // namespace MMM::UI
