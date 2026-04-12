#pragma once

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

protected:
    const std::vector<Graphic::Vertex::VKBasicVertex>&
                                 getVertices() const override;
    const std::vector<uint32_t>& getIndices() const override;

    void onRecordDrawCmds(vk::CommandBuffer& cmdBuf,
                          vk::PipelineLayout pipelineLayout,
                          vk::DescriptorSet  defaultDescriptor) override;

private:
    /// @brief 绘制背景
    void drawBackground(float width, float height);

    /// @brief 绘制刻度线
    void drawTicks(float width, float height, double visualTime, float zoom);

    /// @brief 绘制当前时间指示器
    void drawCurrentTimeIndicator(float width, float height, double visualTime,
                                  double currentTime, float zoom);

    // 渲染编辑器弹窗
    void renderEventEditorPopup();

    std::string                               m_canvasName;
    bool                                      m_needReload{ true };
    std::shared_ptr<Logic::BeatmapSyncBuffer> m_syncBuffer;
    Logic::RenderSnapshot*                    m_currentSnapshot{ nullptr };

    // 弹窗状态
    bool         m_isPopupOpen{ false };
    entt::entity m_editingEntity{ entt::null };
    double       m_editTime{ 0.0 };
    double       m_editValue{ 1.0 };
    std::string  m_editType; // "BPM" or "Scroll"

    // 缓存 Shader 源码
    std::unordered_map<std::string, std::vector<std::string>>
        m_shaderSourceCache;
};


}  // namespace MMM::Canvas
