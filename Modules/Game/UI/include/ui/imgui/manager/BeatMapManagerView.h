#pragma once

#include "ui/ISubView.h"
#include "ui/layout/box/CLayBox.h"
#include <deque>

namespace MMM::UI
{

class BeatMapManagerView : public ISubView
{
public:
    BeatMapManagerView(const std::string& subViewName) : ISubView(subViewName)
    {
    }
    BeatMapManagerView(BeatMapManagerView&&)                 = default;
    BeatMapManagerView(const BeatMapManagerView&)            = default;
    BeatMapManagerView& operator=(BeatMapManagerView&&)      = delete;
    BeatMapManagerView& operator=(const BeatMapManagerView&) = delete;
    ~BeatMapManagerView() override                           = default;

    /// @brief 内部绘制逻辑 (Clay/ImGui)
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

private:
    bool m_showBeatmapList = true;

    // --- 谱面管理相关 ---
    std::string m_manageBeatmapPath;
    bool        m_openManageModal{ false };

    // --- 布局池 (用于避免热路径堆分配) ---
    std::deque<CLayHBox> m_rows;
    std::deque<CLayVBox> m_vboxes;

    CLayHBox& getRow(size_t index)
    {
        while ( m_rows.size() <= index ) {
            m_rows.emplace_back();
        }
        return m_rows[index];
    }
};

}  // namespace MMM::UI
