#pragma once

#include "event/ui/UISettingsTabEvent.h"
#include "graphic/imguivk/VKTexture.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/ITextureLoader.h"
#include "ui/IUIView.h"
#include "ui/layout/box/CLayBox.h"
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::UI
{

/// @brief 编辑器设置面板视图，负责设置分类导航和各设置页渲染。
class SettingsView : virtual public IUIView
{
public:
    /// @brief 构造设置面板视图。
    /// @param viewName 视图名称。
    SettingsView(const std::string& viewName);

    /// @brief 禁止移动构造，避免 UI 布局缓存和事件订阅状态被搬移。
    SettingsView(SettingsView&&) = delete;

    /// @brief 禁止拷贝构造，避免 UI 布局缓存和事件订阅状态被复制。
    SettingsView(const SettingsView&) = delete;

    /// @brief 禁止移动赋值，避免 UI 布局缓存和事件订阅状态被覆盖。
    SettingsView& operator=(SettingsView&&) = delete;

    /// @brief 禁止拷贝赋值，避免 UI 布局缓存和事件订阅状态被覆盖。
    SettingsView& operator=(const SettingsView&) = delete;

    /// @brief 析构设置面板视图并取消事件订阅。
    ~SettingsView() override;

    /// @brief 打开设置窗口并切换到指定设置页。
    /// @param tab 需要激活的设置页。
    void open(Event::SettingsTab tab);

    /// @brief 请求下一帧将设置窗口停靠到主编辑区中心标签页。
    void requestDockToCenter();

    /// @brief 请求下一帧聚焦设置窗口。
    void requestFocus();

    /// @brief 更新并渲染独立设置窗口。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning 热路径：每帧 UI
    /// 更新时执行；禁止加入文件系统扫描或重型资源重建。
    void update(UIManager* sourceManager) override;

    /// @brief 获取实际实例指针，用于禁用 RTTI 时安全下行转换。
    /// @return 当前设置视图实例。
    void* getActualInstance() override { return this; }

private:
    /// @brief 当前激活的设置分类页。
    Event::SettingsTab m_currentTab = Event::SettingsTab::Software;

    /// @brief 设置页切换事件订阅 ID。
    uint64_t m_tabSubId = 0;

    /// @brief 根级横向布局缓存。
    CLayHBox m_rootHBox;

    /// @brief 内容区域纵向布局缓存。
    CLayVBox m_contentVBox;

    /// @brief 设置项行布局缓存池，用于减少 UI 热路径堆分配。
    std::deque<CLayHBox> m_settingRows;

    /// @brief 设置段落布局缓存池，用于减少 UI 热路径堆分配。
    std::deque<CLayVBox> m_sectionBoxes;

    /// @brief 当前谱面设置页正在编辑的元数据副本。
    ::MMM::BaseMapMeta m_editingMeta;

    /// @brief 上一次同步到元数据编辑副本的谱面路径。
    std::string m_lastBeatmapPath;

    /// @brief 下一帧是否聚焦设置窗口。
    bool m_focusNextFrame{ true };

    /// @brief 下一帧是否尝试将设置窗口停靠到主编辑区中心。
    bool m_dockToCenterNextFrame{ true };

    /// @brief 绘制设置窗口内部内容。
    void drawContent();

    /// @brief 计算设置窗口当前内容所需的最小整窗尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return ImGui 窗口最小尺寸。
    ImVec2 getMinWindowSize(float dpiScale) const;

    /// @brief 计算设置分类侧栏中不可再换行文本所需的宽度。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 分类侧栏宽度。
    float getCategorySidebarWidth(float dpiScale) const;

    /// @brief 绘制软件设置页。
    void drawSoftwareSettings();

    /// @brief 绘制视觉设置页。
    void drawVisualSettings();

    /// @brief 绘制项目设置页。
    void drawProjectSettings();

    /// @brief 绘制谱面设置页。
    void drawBeatmapSettings();

    /// @brief 绘制编辑器设置页。
    void drawEditorSettings();

    /// @brief 获取或创建一个行布局。
    /// @param index 行布局缓存索引。
    /// @return 可复用的横向行布局。
    CLayHBox& getRow(size_t index);

    /// @brief 获取或创建一个段落容器布局。
    /// @param index 段落布局缓存索引。
    /// @return 可复用的纵向段落布局。
    CLayVBox& getSection(size_t index);

    /// @brief 测量标签文本的像素宽度。
    /// @param label 标签文本。
    /// @return 当前字体下的标签宽度。
    float measureLabelWidth(const char* label);

    /// @brief 添加一个设置项行（标签 + 控件）。
    /// @param parent 接收设置行的父级布局。
    /// @param rowIndex 当前行索引，会在添加时递增。
    /// @param label 设置项标签。
    /// @param labelWidth 标签列宽度。
    /// @param widget 控件绘制回调。
    void addSettingItem(CLayVBox& parent, size_t& rowIndex, const char* label,
                        float labelWidth, CLayBox::DrawFunc widget);

    /// @brief 添加一个带自动换行的 RadioButton 组。
    /// @param parent 接收设置行的父级布局。
    /// @param rowIndex 当前行索引，会在添加时递增。
    /// @param sectionIndex 当前段落索引，会在添加时递增。
    /// @param label 设置项标签。
    /// @param labelWidth 标签列宽度。
    /// @param options 单选项文本和值列表。
    /// @param current 当前选中的值。
    /// @param changed 发生修改时写入 true。
    void addRadioSetting(
        CLayVBox& parent, size_t& rowIndex, size_t& sectionIndex,
        const char* label, float labelWidth,
        const std::vector<std::pair<std::string, int>>& options, int& current,
        bool& changed);
};

}  // namespace MMM::UI
