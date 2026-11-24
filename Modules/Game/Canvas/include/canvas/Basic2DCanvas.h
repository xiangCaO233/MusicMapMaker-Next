#pragma once

#include "common/ComparisonCanvas.h"

#include "canvas/CanvasSnapshotPrepare.h"
#include "common/AsciiFontData.h"
#include "common/UnicodeFontData.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "event/core/EventBus.h"
#include "graphic/imguivk/VKTextureAtlas.h"
#include "network/collaboration/CollaborationPlaybackSync.h"
#include "ui/ICanvasView.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/IRenderableView.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::Canvas
{
class Basic2DCanvasInteraction;
class CameraLayoutEditor;
}  // namespace MMM::Canvas

namespace MMM::Config
{
struct VisualConfig;
}

namespace MMM::Event
{
struct GLFWDropEvent;
}

namespace MMM::Canvas
{
class Basic2DCanvas : public UI::IRenderableView,
                      public UI::IParallelUiPreparable,
                      public UI::ICanvasView
{
public:
    Basic2DCanvas(
        const std::string& name, uint32_t w, uint32_t h,
        std::shared_ptr<Common::Render::RenderSnapshotBuffer> syncBuffer,
        const std::string& cameraId = "", std::uint64_t comparisonIndex = 0);
    Basic2DCanvas(Basic2DCanvas&&)                 = delete;
    Basic2DCanvas(const Basic2DCanvas&)            = delete;
    Basic2DCanvas& operator=(Basic2DCanvas&&)      = delete;
    Basic2DCanvas& operator=(const Basic2DCanvas&) = delete;

    ~Basic2DCanvas() override;

    /// @brief 返回窗口上一次提交内容时的可见性。
    /// @warning 每帧访问，只读取 UI 线程布尔值。
    [[nodiscard]] bool isCanvasContentVisible() const override
    {
        return m_isCanvasVisible;
    }

    // 接口实现
    /// @brief 更新画布 ImGui 窗口和交互状态。
    /// @warning 热路径：主渲染线程每帧执行；禁止文件系统访问、完整 ECS
    /// 遍历、完整排序和共享指针所有权复制。
    void update(UI::UIManager* sourceManager) override;

    /// @brief 把当前布局编辑器控件绘制到工具栏附着窗口。
    /// @warning UI 每帧调用，只借用已有编辑器，不分配或取得会话锁。
    bool drawCameraLayoutControls() override;
    /// @brief 绘制拾取框编辑展开页，返回当前画布的临时编辑模式。
    /// @warning UI 每帧只借用已建立的交互控制器，不获取会话锁。
    bool drawPickingBoundsLayoutControls() override;

    /// @brief 返回当前已准备画布快照的玩家键数，未载谱时为零。
    /// @warning UI 每帧只读本地稳定快照，不复制所有权或取得会话锁。
    [[nodiscard]] int layoutKeyCount() const override;

    /// @brief 获取窗口是否打开，用于拦截未保存的关闭
    bool isOpen() const override;

    /// @brief 请求关闭画布，复用未保存确认弹窗拦截 dirty 状态
    void requestClose() override;

    /// @brief 消费用户取消关闭操作的标记
    bool consumeCloseCancelled() override;

    /// @brief 请求下一次显示时停靠到主编辑区
    void requestDockToCenter() override;

    /// @brief 请求下一次更新时将画布窗口聚焦到前台。
    void requestFocus() override;

    /// @brief 获取画布当前所在的 ImGui Dock 节点。
    /// @return 当前窗口停靠节点 ID；未停靠时返回 0。
    ImGuiID getDockId() const override;

    /// @brief 安全转换为画布能力接口。
    UI::ICanvasView* asCanvasView() override { return this; }

    /// @brief 安全转换为 UI 并行准备接口。
    /// @return 当前画布的并行准备接口。
    UI::IParallelUiPreparable* asParallelUiPreparable() override
    {
        return this;
    }

    /// @brief 判断当前帧是否需要准备画布快照。
    /// @param snapshot 当前帧 UI 快照。
    /// @return 需要准备时返回 true。
    /// @warning UI 热路径：每帧主线程调用，只检查同步缓冲区和窗口状态。
    bool needsParallelUiPrepare(
        const UI::UiFrameSnapshot& snapshot) const override;

    /// @brief 声明画布快照准备可在线程池执行。
    /// @return 始终返回 false。
    /// @warning UI 热路径：每帧只返回固定能力标记。
    bool requiresMainThreadUiPrepare() const override { return false; }

    /// @brief 在线程池中拉取并准备画布渲染快照。
    /// @param snapshot 当前帧 UI 快照。
    /// @warning 后台线程路径：只消费 BeatmapSyncBuffer 并处理动态顶点偏移。
    void prepareUiFrameData(const UI::UiFrameSnapshot& snapshot) override;

    /// @brief 将准备好的画布快照切换到主线程可见状态。
    void swapPreparedUiFrameData() override;

    ///@brief 是否需要重新记录命令 (比如数据变了)
    bool isDirty() const override;

    /// @brief 当前帧是否需要录制主画布离屏渲染命令。
    /// @return 主画布窗口可见时返回 true。
    /// @warning 渲染热路径：每帧命令录制前调用，只读取 UI
    /// 线程维护的可见状态。
    bool shouldRecordOffscreen() const override;

    // --- 改变尺寸后的回调 ---
    void resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                    uint32_t h) const override;

    /**
     * @brief 获取 Shader 源码接口 (在固定时刻需要创建)
     */
    std::vector<std::string> getShaderSources(
        const std::string& shader_name) override;

    /**
     * @brief 获取 Shader 名称(需要按唯一名称名称储存和销毁)
     */
    std::string getShaderName(const std::string& shader_module_name) override;

    /// @brief 是否需要重载
    bool needReload() override;

    /// @brief 重载纹理
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

protected:
    const std::vector<Graphic::Vertex::VKBasicVertex>&
                                 getVertices() const override;
    const std::vector<uint32_t>& getIndices() const override;

    /// @brief 在离屏 RenderPass 开始前上传最新视频帧。
    /// @warning 渲染命令录制热路径：仅在存在新解码帧时向当前映射
    /// staging 槽复制固定尺寸 RGBA 数据并录制传输命令。
    void onRecordResourceUploads(vk::CommandBuffer& cmdBuf,
                                 uint32_t           frameIndex) override;

    /// @brief 缓存当前观察矩阵，保持绘制与裁剪使用同一代角度。
    /// @warning 每帧一次，仅执行固定规模矩阵计算。
    glm::mat4 getCanvasProjectionMatrix() override;
    /// @brief 把逻辑裁剪区域转换到当前摄像机的物理视口。
    /// @warning 每批次调用；仅投影四个角点，不遍历物件。
    vk::Rect2D getCanvasScissor(Common::Render::CanvasScissor scissor) const;
    /// @brief 当前帧各几何层共用的观察矩阵。
    glm::mat4 m_observationProjection{ 1.0f };
    /// @brief 是否启用透视裁剪；零角度保持原二维裁剪路径。
    bool m_isObservationCamera{ false };
    /// @brief 仅布局工具期间存在的相机编辑状态，退出即释放预览和手势。
    std::unique_ptr<CameraLayoutEditor> m_layoutEditor;

    /// @warning
    /// 热路径：每帧离屏命令录制时执行；只允许遍历当前快照命令并绑定已存在的
    /// descriptor。
    void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

    /// @warning 热路径：启用发光时每帧离屏命令录制执行；只允许遍历 glow
    /// 命令列表。
    void onRecordGlowCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

    /// @brief 记录主画布最终覆盖层离屏绘制命令。
    /// @warning 热路径：最终覆盖层每帧命令录制时执行；只遍历 overlay 命令列表。
    void onRecordOverlayCmds(vk::CommandBuffer&      cmdBuf,
                             vk::PipelineLayout      pipelineLayout,
                             vk::DescriptorSetLayout setLayout,
                             vk::DescriptorSet       defaultDescriptor,
                             uint32_t                frameIndex) override;

    /// @brief 判断当前快照是否包含发光绘制命令。
    /// @return 当前快照存在发光命令时返回 true。
    /// @warning 渲染热路径：每帧离屏命令录制前执行，只能读取快照命令数量。
    bool hasGlowDrawCmds() const override;

    /// @brief 判断当前快照是否包含最终覆盖层绘制命令。
    /// @return 存在覆盖层命令时返回 true。
    /// @warning 渲染热路径：每帧离屏命令录制时执行，只读取命令数量。
    bool hasOverlayDrawCmds() const override;

    /// @brief 清空缓存的 shader 源码。
    /// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
    void invalidateShaderSourceCache() override;

private:
    /// @brief 无 GPU 回归仅设置隐藏状态并核对实际快照消费，不暴露业务接口。
    friend struct CanvasTabSnapshotTestAccess;
    /// @brief 获取画布字体逻辑像素到物理栅格像素的当前倍率。
    /// @return 有效窗口内容缩放；无效配置回退为 1。
    [[nodiscard]] static float currentFontRasterScale();

    /// @brief 发布本地主画布状态、应用跟随目标并绘制可点击的远端视野提示。
    /// @param sourceManager 提供应用级协作房间观察入口的 UI 管理器。
    /// @param canvasScreenPosition 主画布左上角屏幕坐标。
    /// @param canvasSize 主画布逻辑像素尺寸。
    /// @warning UI 热路径：活动主画布每帧调用；只遍历最多 8 个房间成员并绘制
    /// ImGui 覆盖层，网络编码由房间层按配置频率节流。
    void updateCollaborationViewports(UI::UIManager* sourceManager,
                                      const ImVec2&  canvasScreenPosition,
                                      const ImVec2&  canvasSize);

    /// @brief 用批注样式标出本段待操作位置和错误物件，并提供定向删除按钮。
    /// @return 鼠标位于教学按钮上时返回 true，阻止同帧被画布工具解释。
    /// @warning UI 热路径：只访问已剔除的命中框和低频更新的段落反馈。
    bool updateComposeLessonHints(
        UI::UIManager*                        sourceManager,
        const Common::Render::RenderSnapshot& snapshot,
        const ImVec2& canvasScreenPosition, const ImVec2& canvasSize);

    /// @brief 画布名称
    std::string m_canvasName;

    /// @brief 逻辑视口 ID (对应 BeatmapSession 中的 Camera)
    std::string m_cameraId;
    /// @brief 非零表示只关闭独立窗口，不关闭拥有谱面的会话。
    std::uint64_t m_comparisonIndex{ 0 };
    /// @brief 当前对比标签的映射与观察状态，只在本地控件中修改。
    Common::ComparisonCanvasSettings m_comparisonSettings{};
    /// @brief 对比视口编辑布局及右侧按钮时独享的 UI 状态。
    struct ComparisonLayout;
    std::unique_ptr<ComparisonLayout> m_comparisonLayout;

    /// @brief 取得本画布可编辑的视觉状态，普通画布仍使用软件配置。
    /// @warning 逐帧及录制路径只借用稳定 UI 值，不复制配置容器。
    Config::VisualConfig& canvasVisualConfig() const;

    /// @brief 绘制右侧工具条，并保留固定画布高度。
    /// @warning 每帧只绘制固定数量按钮，布局弹层仅在主动打开时绘制。
    void drawComparisonToolbar(UI::UIManager& manager, ImVec2 railPosition,
                               ImVec2 railSize);
    /// @brief 首次可见时建立视口；后续只在状态边沿同步。
    bool m_comparisonSettingsPublished{ false };

    /// @brief 下一帧是否调用 ImGui::SetNextWindowFocus。
    bool m_shouldFocusNextFrame{ false };
    /// @brief 上一可见帧的根窗口焦点，用于去重会话切换请求。
    bool m_hadSessionFocus{ false };

    /// @brief 同步缓冲区
    /// @warning 热路径/共享指针：画布仅持有所有权确保缓冲区生命周期，update
    /// 中不得复制该 shared_ptr。
    std::shared_ptr<Common::Render::RenderSnapshotBuffer> m_syncBuffer;

    /// @brief 当前正在使用的渲染快照
    Common::Render::RenderSnapshot* m_currentSnapshot{ nullptr };

    /// @brief 缓存spv源码，避免重复读盘
    std::unordered_map<std::string, std::vector<std::string>>
        m_shaderSourceCache;

    ///@brief 是否需要重载
    bool m_needReload{ true };

    /// @brief 当前离屏画布图集中已加载的多档 ASCII 字体度量。
    Common::AsciiFontAtlasMetrics m_asciiFontAtlasMetrics;
    /// @brief 当前离屏画布图集中按项目资源名加载的 Unicode 字体度量。
    Common::UnicodeFontMetrics m_unicodeFontMetrics;
    /// @brief 最近一次加载图集时使用的软件 ASCII 字体偏好。
    std::string m_loadedAsciiFontPreference;
    /// @brief 最近一次加载图集时使用的软件 CJK 字体偏好。
    std::string m_loadedCjkFontPreference;
    /// @brief 最近一次发布字体图集时使用的 DPI 栅格倍率。
    float m_loadedFontRasterScale{ 0.0F };
    /// @brief 可见标签已请求补载的 Unicode 码点，跨图集重建保留。
    std::vector<std::uint32_t> m_requestedUnicodeCodepoints;

    ///@brief 全局图集
    std::unique_ptr<Graphic::VKTextureAtlas> m_textureAtlas{ nullptr };

    ///@brief 图集 UV 缓存，用于同步给逻辑线程
    std::unordered_map<uint32_t, glm::vec4> m_atlasUVs;

    // --- 动态加载使用的 Vulkan 设备 ---
    vk::PhysicalDevice m_physicalDevice{ nullptr };
    vk::Device         m_logicalDevice{ nullptr };
    vk::CommandPool    m_cmdPool{ nullptr };
    vk::Queue          m_queue{ nullptr };

    /// @brief 独占背景纹理和视频提交代际，画布构造时分配一次。
    struct BackgroundPresentation;
    std::unique_ptr<BackgroundPresentation>   m_background;
    std::unique_ptr<Basic2DCanvasInteraction> m_interaction;

    /// @brief 教学识别拥有本地手势和谱面身份，不保存外部快照。
    struct WalkthroughRecognizer;
    std::unique_ptr<WalkthroughRecognizer> m_walkthrough;


    /// @brief 上一次应用到动态顶点上的 Y 偏移量
    float m_lastAppliedYOffset{ 0.0f };

    /// @brief 上一次应用偏移的快照指针 (用于检测快照是否更新)
    Common::Render::RenderSnapshot* m_lastOffsetSnapshot{ nullptr };

    /// @brief 后台准备出的画布快照消费结果。
    PreparedCanvasSnapshot m_preparedSnapshot;

    /// @brief 是否有后台准备结果等待主线程切换。
    bool m_hasPreparedSnapshot{ false };

    /// @brief 当前主画布窗口是否真实可见。
    bool m_isCanvasVisible{ true };

    /// @brief 是否显示保存确认弹窗
    bool m_showSaveConfirm{ false };

    /// @brief 上一次关闭请求是否被用户取消
    bool m_closeCancelled{ false };

    /// @brief 用户是否已经确认关闭并允许跳过 dirty 拦截
    bool m_closeConfirmed{ false };

    /// @brief 是否需要在下一次 Begin 前停靠到主编辑区
    bool m_shouldDockToCenter{ false };

    /// @brief 当前画布窗口最近一次更新时所在的 ImGui Dock 节点。
    ImGuiID m_lastDockId{ 0 };

    /// @brief 上一帧从房间读取的列表跟随目标。
    std::uint64_t m_lastFollowedPeerId{ 0 };

    /// @brief 已经应用到逻辑相机的远端视口状态序号。
    std::uint64_t m_lastFollowedViewportSequence{ 0 };
    /// @brief 仅当前画布接管的跟随音频状态；取消或失联后停止其播放。
    Network::Collaboration::PlaybackFollowTracker m_playbackFollowTracker;

    /// @brief 当前主画布是否已经绑定过协作谱面会话。
    bool m_isCollaborationCanvas{ false };

    /// @brief 上一帧当前 Session 是否为欢迎占位页，用于识别远端谱面原地复用。
    bool m_wasLogoPlaceholder{ false };

    /// @brief 上一帧是否观察到非 Idle 的房间生命周期，用于识别启动边沿。
    bool m_wasCollaborationRoomLifecycleActive{ false };

private:
    /// @brief 判断已关闭窗口是否应保留并重置为 Logo 占位画布。
    /// @return 唯一真实谱面正在关闭时返回 true。
    /// @warning UI 热路径辅助：UIManager 每帧关闭检查时可能调用；只读取
    /// SessionRegistry 的常量规模状态。
    bool shouldKeepOpenForLastSessionReset() const;
};

}  // namespace MMM::Canvas
