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
    NewBeatmapWizard();
    virtual ~NewBeatmapWizard() = default;

    void update(UIManager* sourceManager) override;

    void open();
    void close();

private:
    void reset();
    void onAudioSelected(const std::filesystem::path& path);

    bool m_isOpen = false;
    bool m_shouldOpen = false;
    MMM::BaseMapMeta m_meta;


    // UI 状态
    char m_nameBuf[256]          = { 0 };
    char m_titleBuf[256]         = { 0 };
    char m_titleUnicodeBuf[256]  = { 0 };
    char m_artistBuf[256]        = { 0 };
    char m_artistUnicodeBuf[256] = { 0 };
    char m_authorBuf[256]        = { 0 };
    char m_versionBuf[256]       = { 0 };

    double m_bpm = 120.0;
    int    m_trackCount = 4;
    std::filesystem::path m_selectedAudioPath;
    std::filesystem::path m_selectedCoverPath;
    double m_audioDuration = 0.0;
};

}  // namespace MMM::UI
