#pragma once

#include "canvas/CanvasSnapshotPrepare.h"
#include "graphic/imguivk/VKTextureAtlas.h"
#include "logic/BeatmapSyncBuffer.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/IRenderableView.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
class TimelineCanvas : public UI::IRenderableView,
                       public UI::IParallelUiPreparable
{
public:
    TimelineCanvas(const std::string& name, uint32_t w, uint32_t h,
                   std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer);
    ~TimelineCanvas() override = default;

    // IUIView 接口
    void update(UI::UIManager* sourceManager) override;

    /// @brief 安全转换为 UI 并行准备接口。
    /// @return 当前时间线画布的并行准备接口。
    UI::IParallelUiPreparable* asParallelUiPreparable() override
    {
        return this;
    }

    /// @brief 判断当前帧是否需要准备时间线快照。
    /// @param snapshot 当前帧 UI 快照。
    /// @return 需要准备时返回 true。
    /// @warning UI 热路径：每帧主线程调用，只检查同步缓冲区和窗口状态。
    bool needsParallelUiPrepare(
        const UI::UiFrameSnapshot& snapshot) const override;

    /// @brief 在线程池中拉取并准备时间线快照。
    /// @param snapshot 当前帧 UI 快照。
    /// @warning 后台线程路径：只消费 BeatmapSyncBuffer 并处理动态顶点偏移。
    void prepareUiFrameData(const UI::UiFrameSnapshot& snapshot) override;

    /// @brief 将准备好的时间线快照切换到主线程可见状态。
    /// @warning UI 热路径：每帧只切换快照；无活跃谱面时清空旧帧。
    void swapPreparedUiFrameData() override;

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

    /// @brief 请求下一帧将时间线窗口聚焦到前台。
    void requestFocus();

    /// @brief 判断时间线上一帧是否拥有 Timing 编辑焦点。
    /// @return 上一帧时间线拥有 Timing 编辑焦点时返回 true。
    bool wasFocusedLastFrame() const { return m_wasFocusedLastFrame; }

protected:
    const std::vector<Graphic::Vertex::VKBasicVertex>&
                                 getVertices() const override;
    const std::vector<uint32_t>& getIndices() const override;

    void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

    /// @brief 录制 Timeline Timing 发光层离屏绘制命令。
    /// @warning 渲染热路径：每帧离屏命令录制时执行；只遍历 glow 命令列表。
    void onRecordGlowCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

    /// @brief 判断当前 Timeline 快照是否包含发光绘制命令。
    /// @return 当前快照存在发光命令时返回 true。
    /// @warning 渲染热路径：每帧离屏命令录制前执行，只读取命令数量。
    bool hasGlowDrawCmds() const override;

private:
    /// @brief Timeline 画布中的一个可拾取 Timing 目标。
    struct TimelineHitTarget;

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

    /// @brief 根据画布本地 Y 坐标换算谱面时间。
    /// @param size 当前 Timeline 画布尺寸。
    /// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
    /// @return 换算出的谱面时间，单位秒。
    double canvasTimeAtLocalY(const ImVec2& size, float localMouseY) const;

    /// @brief 根据谱面时间换算 Timeline 画布本地 Y 坐标。
    /// @param size 当前 Timeline 画布尺寸。
    /// @param time 谱面时间，单位秒。
    /// @return Timeline 画布内 Y 坐标。
    double canvasYAtTime(const ImVec2& size, double time) const;

    /// @brief 将时间吸附到附近已有 Timing 事件或分拍网格。
    /// @param size 当前 Timeline 画布尺寸。
    /// @param rawTime 未吸附的谱面时间，单位秒。
    /// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
    /// @param snapped 输出是否发生吸附。
    /// @return 吸附后的谱面时间，单位秒。
    double snapTimingTime(const ImVec2& size, double rawTime, float localMouseY,
                          bool& snapped) const;

    /// @brief 在指定画布 Y 坐标处准备并打开 Timing 创建弹窗。
    /// @param size 当前 Timeline 画布尺寸。
    /// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
    /// @param useCurrentTime 是否使用当前播放时间而非鼠标命中时间。
    void openTimingCreatePopupAtY(const ImVec2& size, float localMouseY,
                                  bool useCurrentTime);

    /// @brief 处理 Timeline 画布上的 Timing 选择、框选、拖动和快捷键。
    /// @param canvasPos 画布左上角屏幕坐标。
    /// @param size 当前 Timeline 画布尺寸。
    /// @param isHovered 鼠标是否悬浮在画布 Image 上。
    /// @param isFocused Timeline 窗口是否聚焦。
    void handleTimingCanvasInteraction(const ImVec2& canvasPos,
                                       const ImVec2& size, bool isHovered,
                                       bool isFocused);

    /// @brief 绘制 Timeline Timing 的 hover、选中、拖动和框选反馈。
    /// @param canvasPos 画布左上角屏幕坐标。
    /// @param size 当前 Timeline 画布尺寸。
    void renderTimingInteractionOverlay(const ImVec2& canvasPos,
                                        const ImVec2& size);

    /// @brief 清除上一帧追加到 Timeline 快照中的交互修饰。
    void resetTimelineInteractionDecoration();

    /// @brief 根据当前 Timeline 交互状态刷新快照半透明与发光命令。
    /// @param size 当前 Timeline 画布尺寸。
    void refreshTimelineInteractionDecoration(const ImVec2& size);

    /// @brief 清理选中集中已经不存在于当前快照的 Timing 实体。
    void pruneInvalidTimingSelection();

    /// @brief 删除当前选中的 Timing 事件。
    void deleteSelectedTimingEvents();

    /// @brief 更新 Timeline 画笔右键擦除预览目标。
    /// @param hoveredTarget 当前鼠标命中的 Timing 目标。
    void updateTimingEraseTarget(
        const std::optional<TimelineHitTarget>& hoveredTarget);

    /// @brief 提交当前 Timeline 画笔右键擦除目标。
    void commitTimingEraseTargets();

    /// @brief 将当前选中的 Timing 复制到 Timeline 本地剪贴板。
    /// @param cut 是否在复制后删除原 Timing。
    void copySelectedTimingEvents(bool cut);

    /// @brief 将 Timeline 本地剪贴板粘贴到指定锚点时间。
    /// @param anchorTime 粘贴锚点时间，单位秒。
    void pasteTimingClipboard(double anchorTime);

    /// @brief Timeline 画布中的一个可拾取 Timing 目标。
    struct TimelineHitTarget {
        /// @brief Timing 实体。
        entt::entity entity{ entt::null };

        /// @brief Timing 类型。
        ::MMM::TimingEffect effect{ ::MMM::TimingEffect::SCROLL };

        /// @brief Timing 时间，单位秒。
        double time{ 0.0 };

        /// @brief Timing 原始参数值。
        double value{ 0.0 };

        /// @brief Timeline 画布内 Y 坐标。
        float y{ 0.0f };

        /// @brief 是否存在可修饰的 marker 几何体。
        bool hasMarkerGeometry{ false };

        /// @brief marker 顶点起点。
        uint32_t markerVertexOffset{ 0 };

        /// @brief marker 顶点数量。
        uint32_t markerVertexCount{ 0 };

        /// @brief marker 索引起点。
        uint32_t markerIndexOffset{ 0 };

        /// @brief marker 索引数量。
        uint32_t markerIndexCount{ 0 };
    };

    /// @brief Timeline 本地剪贴板条目。
    struct TimelineClipboardEntry {
        /// @brief 相对剪贴板锚点时间，单位秒。
        double relativeTime{ 0.0 };

        /// @brief Timing 类型。
        ::MMM::TimingEffect effect{ ::MMM::TimingEffect::SCROLL };

        /// @brief Timing 原始参数值。
        double value{ 0.0 };
    };

    /// @brief 拖动开始时记录的 Timing 原始状态。
    struct TimelineDragEntry {
        /// @brief Timing 实体。
        entt::entity entity{ entt::null };

        /// @brief 拖动开始前时间，单位秒。
        double originalTime{ 0.0 };

        /// @brief Timing 原始参数值。
        double value{ 0.0 };
    };

    /// @brief Timeline 顶点颜色恢复记录。
    struct TimelineVertexColorRestore {
        /// @brief 顶点索引。
        uint32_t vertexIndex{ 0 };

        /// @brief 修饰前完整颜色。
        Graphic::Vertex::Color color;
    };

    /// @brief 收集当前快照中可交互的 Timing 目标。
    /// @return 当前可见 Timing 目标列表。
    std::vector<TimelineHitTarget> collectVisibleTimingTargets() const;

    /// @brief 判断指定 Timing 目标是否允许被时间线选择操作选中。
    /// @param target Timing 目标。
    /// @return 允许选中时返回 true。
    bool isTimingTargetSelectable(const TimelineHitTarget& target) const;

    /// @brief 拾取鼠标附近的 Timing 目标。
    /// @param canvasPos 画布左上角屏幕坐标。
    /// @param size 当前 Timeline 画布尺寸。
    /// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
    /// @return 命中的 Timing 目标；未命中时为空。
    std::optional<TimelineHitTarget> pickTimingTarget(const ImVec2& canvasPos,
                                                      const ImVec2& size,
                                                      float localMouseY) const;

    /// @brief 将单个 Timing 目标转换为显示用 X 坐标。
    /// @param target Timing 目标。
    /// @param canvasPos 画布左上角屏幕坐标。
    /// @param size 当前 Timeline 画布尺寸。
    /// @return 目标中心 X 坐标。
    float timingTargetCenterX(const TimelineHitTarget& target,
                              const ImVec2&            canvasPos,
                              const ImVec2&            size) const;

    /// @brief 将 Timing 类型转换为 ImGui 绘制颜色。
    /// @param effect Timing 类型。
    /// @param alpha 透明度。
    /// @return ImGui 颜色。
    ImU32 timingEffectColor(::MMM::TimingEffect effect, int alpha) const;

    std::string                               m_canvasName;
    bool                                      m_needReload{ true };
    std::shared_ptr<Logic::BeatmapSyncBuffer> m_syncBuffer;
    Logic::RenderSnapshot*                    m_currentSnapshot{ nullptr };

    // 弹窗状态
    bool m_isPopupOpen{ false };
    bool m_isTableWindowOpen{ false };
    /// @brief 下一帧是否调用 ImGui::SetNextWindowFocus 聚焦时间线窗口。
    bool m_shouldFocusNextFrame{ false };
    /// @brief 时间线上一帧是否拥有 Timing 编辑焦点。
    bool m_wasFocusedLastFrame{ false };
    /// @brief Timeline Timing 编辑焦点是否仍归属于时间线窗口。
    bool m_hasTimingInteractionFocus{ false };
    /// @brief 时间点批量编辑窗口绑定的谱面快照键。
    std::string  m_tableBeatmapKey;
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

    /// @brief Timeline 当前 hover 的 Timing 实体。
    entt::entity m_hoveredTimingEntity{ entt::null };

    /// @brief Timeline 当前选中的 Timing 实体集合。
    std::unordered_set<entt::entity> m_selectedTimingEntities;

    /// @brief Timeline 本地 Timing 剪贴板。
    std::vector<TimelineClipboardEntry> m_timingClipboard;

    /// @brief 是否正在拖动 Timeline Timing。
    bool m_isTimingDragging{ false };

    /// @brief 拖动开始时鼠标对应时间，单位秒。
    double m_timingDragStartTime{ 0.0 };

    /// @brief 当前拖动预览时间偏移，单位秒。
    double m_timingDragPreviewDelta{ 0.0 };

    /// @brief 拖动开始时选中 Timing 的原始状态。
    std::vector<TimelineDragEntry> m_timingDragEntries;

    /// @brief 画笔工具右键是否正在 Timeline Timing 擦除预览中。
    bool m_isTimingErasing{ false };

    /// @brief 当前右键擦除预览命中的 Timing 实体。
    std::unordered_set<entt::entity> m_timingEraseTargetEntities;

    /// @brief 是否正在 Timeline 中框选 Timing。
    bool m_isTimingMarqueeSelecting{ false };

    /// @brief Timeline 框选起点 Y 坐标。
    float m_timingMarqueeStartY{ 0.0f };

    /// @brief Timeline 框选终点 Y 坐标。
    float m_timingMarqueeEndY{ 0.0f };

    /// @brief 画笔工具是否正在预览放置 Timing。
    bool m_isTimingDrawPreviewing{ false };

    /// @brief 画笔工具预览 Timing 时间，单位秒。
    double m_timingDrawPreviewTime{ 0.0 };

    /// @brief 画笔工具预览 Timing 在画布中的 Y 坐标。
    float m_timingDrawPreviewY{ 0.0f };

    /// @brief 当前被 UI 侧交互修饰过的 Timeline 快照。
    Logic::RenderSnapshot* m_decoratedTimelineSnapshot{ nullptr };

    /// @brief 修饰前快照顶点数量。
    size_t m_decoratedTimelineVertexCount{ 0 };

    /// @brief 修饰前快照索引数量。
    size_t m_decoratedTimelineIndexCount{ 0 };

    /// @brief 修饰前普通绘制命令数量。
    size_t m_decoratedTimelineCmdCount{ 0 };

    /// @brief 修饰前发光绘制命令数量。
    size_t m_decoratedTimelineGlowCmdCount{ 0 };

    /// @brief 修饰前顶点颜色恢复列表。
    std::vector<TimelineVertexColorRestore> m_timelineColorRestore;

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

    /// @brief 后台准备出的时间线快照消费结果。
    PreparedCanvasSnapshot m_preparedSnapshot;

    /// @brief 是否有后台准备结果等待主线程切换。
    bool m_hasPreparedSnapshot{ false };
};


}  // namespace MMM::Canvas
