#pragma once

#include "event/ui/UISettingsTabEvent.h"
#include "graphic/imguivk/VKTexture.h"
#include "ui/ISubView.h"
#include "ui/ITextureLoader.h"
#include "ui/layout/box/CLayBox.h"
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::UI
{


class SettingsView : public ISubView
{
public:
    SettingsView(const std::string& subViewName);
    SettingsView(SettingsView&&)                 = default;
    SettingsView(const SettingsView&)            = default;
    SettingsView& operator=(SettingsView&&)      = delete;
    SettingsView& operator=(const SettingsView&) = delete;
    ~SettingsView() override;

    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

private:
    Event::SettingsTab m_currentTab = Event::SettingsTab::Software;
    uint64_t           m_tabSubId   = 0;

    // --- 布局池 (用于避免热路径堆分配) ---
    CLayHBox             m_rootHBox;
    CLayVBox             m_contentVBox;
    std::deque<CLayHBox> m_settingRows;
    std::deque<CLayVBox> m_sectionBoxes;

    void drawSoftwareSettings();
    void drawVisualSettings();
    void drawProjectSettings();
    void drawBeatmapSettings();
    void drawEditorSettings();

    // 辅助方法：获取或创建一个行布局
    CLayHBox& getRow(size_t index);

    /// @brief 获取或创建一个段落容器布局
    CLayVBox& getSection(size_t index);

    /// @brief 测量标签文本的像素宽度
    float measureLabelWidth(const char* label);

    /// @brief 添加一个设置项行（标签 + 控件）
    void addSettingItem(CLayVBox& parent, size_t& rowIndex, const char* label,
                        float labelWidth, CLayBox::DrawFunc widget);
};

}  // namespace MMM::UI
