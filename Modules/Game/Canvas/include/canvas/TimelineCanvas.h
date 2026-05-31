#pragma once

#include "graphic/imguivk/VKTextureAtlas.h"
#include "logic/BeatmapSyncBuffer.h"
#include "ui/IRenderableView.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::Logic
{
class BeatmapSession;
struct RenderSnapshot;
}  // namespace MMM::Logic

namespace MMM::Canvas
{

/**
 * @brief 时间线标尺画布类
 * 停靠在侧边栏与主画布之间，显示小节线、拍线以及 BPM/流速变更标记。
 */
class TimelineCanvas : public UI::IRenderableView
{
public:
    TimelineCanvas(const std::string& name, uint32_t w, uint32_t h,
                   std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer);
    ~TimelineCanvas() override = default;

    // IUIView 接口
    void update(UI::UIManager* sourceManager) override;

    // IRenderableView 接口
    bool isDirty() const override;
    void resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                    uint32_t h) const override;

    std::vector<std::string> getShaderSources(
        const std::string& shader_name) override;
    std::string getShaderName(const std::string& shader_module_name) override;
    bool        needReload() override;
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

    /// @brief 获取时间点批量编辑表格窗口是否打开。
    /// @return 表格窗口当前是否打开。
    bool isTimingPointsTableOpen() const { return m_isTableWindowOpen; }

    /// @brief 设置时间点批量编辑表格窗口打开状态。
    /// @param open 是否打开表格窗口。
    void setTimingPointsTableOpen(bool open) { m_isTableWindowOpen = open; }

protected:
    const std::vector<Graphic::Vertex::VKBasicVertex>&
                                 getVertices() const override;
    const std::vector<uint32_t>& getIndices() const override;

    void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

private:
    // 渲染编辑器弹窗
    void renderEventEditorPopup();

    // 渲染创建事件弹窗
    void renderEventCreationPopup();

    // 渲染时间点表格大窗口
    void renderTimingPointsTableWindow();

    /// @brief 开始跟踪一次“保持画布速度”创建出的 BPM/Scroll 联动。
    /// @param time 新建 BPM 与 Scroll 所在时间点。
    void beginKeepSpeedBinding(double time);

    /// @brief 刷新当前“保持画布速度”联动关联的实体。
    /// @param elements 当前时间点表格展示的完整事件列表。
    void refreshKeepSpeedBinding(
        const std::vector<Logic::TimelineInteractiveElement>& elements);

    /// @brief 判断表格行是否属于当前临时联动。
    /// @param entity 当前行实体。
    /// @return 属于联动中的 BPM 或 Scroll 行则返回 true。
    bool isKeepSpeedBindingEntity(entt::entity entity) const;

    /// @brief 使用编辑中的 BPM 值刷新联动 Scroll 值。
    /// @param bpm 当前 BPM 编辑值。
    void updateKeepSpeedBindingScroll(double bpm);

    /// @brief 结束“保持画布速度”临时联动并恢复普通编辑状态。
    void finishKeepSpeedBinding();

    void handleRightClick(const ImVec2& size);

    std::string                               m_canvasName;
    bool                                      m_needReload{ true };
    std::shared_ptr<Logic::BeatmapSyncBuffer> m_syncBuffer;
    Logic::RenderSnapshot*                    m_currentSnapshot{ nullptr };

    // 弹窗状态
    bool         m_isPopupOpen{ false };
    bool         m_isTableWindowOpen{ false };
    entt::entity m_editingEntity{ entt::null };
    double       m_editTime{ 0.0 };
    double       m_editValue{ 1.0 };
    std::string  m_editType;  ///< @brief 编辑中的 Timing 类型名称

    // 创建弹窗状态
    bool   m_isCreatePopupOpen{ false };
    double m_createTimeRaw{ 0.0 };
    double m_createTimeSnapped{ 0.0 };
    double m_createTimeManual{ 0.0 };
    double m_createValue{ 120.0 };
    int    m_createType{ 0 };     ///< @brief 0: BPM, 1: Scroll, 2: Jump, 3: HS
    int    m_createPosType{ 0 };  // 0: Click, 1: Current
    bool   m_isTimeSnapped{ false };
    bool   m_keepSpeedOnBpmChange{ false };

    /// @brief 最近一次新建 Timing 的目标时间（秒），用于表格高亮定位
    double m_lastCreatedTimingTime{ -1.0 };
    /// @brief 最近一次新建 Timing 的类型
    ::MMM::TimingEffect m_lastCreatedTimingEffect{ ::MMM::TimingEffect::BPM };
    /// @brief 最近一次新建 Timing 的高亮截止时间（ImGui 时间）
    double m_lastCreatedTimingHighlightUntil{ 0.0 };

    /// @brief “保持画布速度”临时联动是否正在等待 BPM 编辑结束。
    bool m_keepSpeedBindingActive{ false };
    /// @brief 联动创建的 BPM 与 Scroll 所在时间点。
    double m_keepSpeedBindingTime{ -1.0 };
    /// @brief 联动创建或匹配到的 BPM 实体。
    entt::entity m_keepSpeedBindingBpmEntity{ entt::null };
    /// @brief 联动创建或匹配到的 Scroll 实体。
    entt::entity m_keepSpeedBindingScrollEntity{ entt::null };
    /// @brief 是否需要在表格中自动聚焦联动 BPM 输入框。
    bool m_keepSpeedBindingFocusBpm{ false };

    // 缓存 Shader 源码
    std::unordered_map<std::string, std::vector<std::string>>
        m_shaderSourceCache;

    /// @brief Timeline 使用的轻量纹理图集。
    std::unique_ptr<Graphic::VKTextureAtlas> m_textureAtlas{ nullptr };
    /// @brief Timeline 图集 UV 缓存，用于同步给逻辑线程。
    std::unordered_map<uint32_t, glm::vec4> m_atlasUVs;

    /// @brief 上一次应用到动态顶点上的 Y 偏移量
    float m_lastAppliedYOffset{ 0.0f };

    /// @brief 上一次应用偏移的快照指针
    Logic::RenderSnapshot* m_lastOffsetSnapshot{ nullptr };
};


}  // namespace MMM::Canvas
