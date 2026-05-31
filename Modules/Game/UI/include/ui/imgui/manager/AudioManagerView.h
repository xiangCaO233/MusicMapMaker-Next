#pragma once

#include "mmm/project/AudioResource.h"
#include "ui/ISubView.h"
#include "ui/layout/box/CLayBox.h"
#include <deque>

namespace MMM::UI
{
class AudioManagerView : public ISubView
{
public:
    AudioManagerView(const std::string& subViewName) : ISubView(subViewName) {}
    AudioManagerView(AudioManagerView&&)                 = default;
    AudioManagerView(const AudioManagerView&)            = default;
    AudioManagerView& operator=(AudioManagerView&&)      = delete;
    AudioManagerView& operator=(const AudioManagerView&) = delete;
    ~AudioManagerView() override                         = default;

    /// @brief 内部绘制逻辑 (Clay/ImGui)
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

    /// @brief 获取音频管理器中不可再换行控件所需的最小内容尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 音频管理器的动态最小内容尺寸。
    ImVec2 getMinContentSize(float dpiScale) const override;

private:
    bool m_showGlobalSettings = true;
    bool m_showPermanentSFX   = true;
    bool m_showMainTracks     = true;
    bool m_showProjectSFX     = true;

    // --- 音轨管理相关 ---
    std::string    m_manageTrackId;
    AudioTrackType m_manageTrackType;
    bool           m_openManageModal{ false };

    // --- 布局池 (用于避免热路径堆分配) ---
    std::deque<CLayHBox> m_settingRows;
    std::deque<CLayHBox> m_subHBoxes;
    std::deque<CLayVBox> m_subVBoxes;

    // 辅助方法：获取或创建一个行布局
    CLayHBox& getRow(size_t index)
    {
        while ( m_settingRows.size() <= index ) {
            m_settingRows.emplace_back();
        }
        return m_settingRows[index];
    }

    CLayHBox& getSubHBox(size_t index)
    {
        while ( m_subHBoxes.size() <= index ) {
            m_subHBoxes.emplace_back();
        }
        return m_subHBoxes[index];
    }

    CLayVBox& getSubVBox(size_t index)
    {
        while ( m_subVBoxes.size() <= index ) {
            m_subVBoxes.emplace_back();
        }
        return m_subVBoxes[index];
    }
};

}  // namespace MMM::UI
