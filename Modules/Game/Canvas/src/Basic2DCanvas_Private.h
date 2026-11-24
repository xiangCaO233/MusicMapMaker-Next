#pragma once

#include "canvas/BackgroundVideoPlayer.h"
#include "canvas/Basic2DCanvas.h"
#include "canvas/ComposePolylineTarget.h"
#include "graphic/imguivk/VKTexture.h"

#include <array>
#include <optional>

namespace MMM::UI::Walkthrough
{
class Spotlight;
}

namespace MMM::Canvas
{
/// @brief 持有画布的背景媒体纹理及 CPU 视频帧到 GPU 的提交边界。
/// @details 组件跨帧复用像素缓存，并按资源/Seek 代际拒绝迟到帧。画布在 UI
/// 阶段替换背景，随后录制阶段使用同一对象，不与解码线程共享可变提交状态。
struct Basic2DCanvas::BackgroundPresentation {
    /// @brief 立即借用的 Vulkan 句柄；组件不延长设备生命周期。
    struct UploadContext {
        vk::PhysicalDevice& m_physicalDevice;  ///< 选择纹理内存类型。
        vk::Device&         m_logicalDevice;   ///< 管理纹理与在途工作。
        vk::CommandPool&    m_cmdPool;         ///< 背景重建使用的上传命令池。
        vk::Queue&          m_queue;           ///< 执行低频上传的队列。
    };
    /// @brief 只比较资源身份。相同来源不重启解码或重建纹理。
    /// @warning 每帧执行，无文件系统访问与 GPU 等待。
    [[nodiscard]] bool matches(
        const Common::Render::RenderSnapshot& snapshot) const
    {
        return snapshot.backgroundPath == m_loadedBgPath &&
               snapshot.backgroundIsVideo == m_loadedBackgroundIsVideo;
    }
    /// @brief 借用已加载的纹理，可为空，不转移 GPU 资源所有权。
    /// @warning 命令录制阶段只读取指针。
    [[nodiscard]] Graphic::VKTexture* texture() const
    {
        return m_bgTexture.get();
    }
    /// @brief 当前 GPU 纹理是否已经有允许显示的视频帧。
    /// @warning 命令录制阶段只读取布尔值。
    [[nodiscard]] bool visible() const { return m_videoFrameVisible; }

    /// @brief 录制一次尚未提交的像素上传。
    /// @warning 无新帧时只比较修订，不等待解码或 GPU。
    void recordUpload(vk::CommandBuffer& cmdBuf, std::uint32_t frameIndex);

    /// @brief 当快照背景路径或类型变化时加载或清理背景资源。
    /// @warning 低频阻塞路径：可能访问文件系统、创建 Vulkan 纹理并等待
    /// GPU；只能在背景路径或类型变化时调用，严禁每帧执行。
    void updateBackgroundTexture(const Common::Render::RenderSnapshot& snapshot,
                                 const UploadContext&                  upload);

    /// @brief 根据谱面原始播放时钟请求并消费最新视频帧。
    /// @warning UI 热路径：每帧只进行常数时间计算和非阻塞取帧；
    /// 只有分辨率或资源变化时允许低频重建 Vulkan 纹理。
    void updateBackgroundVideoFrame(
        const Common::Render::RenderSnapshot& snapshot,
        const UploadContext&                  upload);

private:
    /// @brief CPU 回归仅注入代表真实录制完成的确认，不构造假 Vulkan 对象。
    friend struct CanvasTabSnapshotTestAccess;
    /// @brief 提交帧时复核接收代际，丢弃跳转前已经排队的上传。
    /// @warning UI 或同帧后续录制阶段调用，不进入解码线程。
    void commitUploadedVideoFrame(double        timestamp,
                                  std::uint64_t requestGeneration,
                                  bool          reachedEnd);

    std::unique_ptr<Graphic::VKTexture> m_bgTexture{ nullptr };
    std::string                         m_loadedBgPath{ "" };

    /// @brief 已加载背景资源是否按视频处理。
    bool m_loadedBackgroundIsVideo{ false };

    /// @brief 专用后台背景视频解码器。
    BackgroundVideoPlayer m_backgroundVideoPlayer;

    /// @brief 当前是否已有可显示的视频帧内容。
    bool m_videoFrameAvailable{ false };

    /// @brief 当前背景视频路径是否指向可读取文件。
    bool m_videoSourceAvailable{ false };

    /// @brief 当前视频帧是否已到谱面显示时间。
    bool m_videoFrameVisible{ false };

    /// @brief 当前 UI 帧的谱面时钟是否允许显示视频。
    bool m_videoShouldBeVisibleThisFrame{ false };

    /// @brief 当前资源是否已提交过视频帧请求。
    bool m_hasRequestedVideoFrame{ false };

    /// @brief 最近一次提交给解码线程的视频时间。
    double m_lastRequestedVideoTime{ 0.0 };

    /// @brief 最近一次提交视频帧请求的 steady_clock 时间（秒）。
    double m_lastVideoFrameRequestSysTime{ 0.0 };

    /// @brief 当前暂停 Seek 代际已经补发的有限重试次数。
    std::uint32_t m_pendingVideoSeekRetryCount{ 0 };

    /// @brief 是否正在等待当前资源/Seek 代际的首帧。
    bool m_videoDiscontinuityPending{ false };

    /// @brief 当前待完成资源/Seek 代际的目标时间。
    double m_videoDiscontinuityTargetTime{ 0.0 };

    /// @brief 当前帧是否已到达视频末尾。
    bool m_videoReachedEnd{ false };

    /// @brief 当前 GPU 纹理内容是否已有对应视频时间戳。
    bool m_hasUploadedVideoTimestamp{ false };

    /// @brief 当前 GPU 纹理内容对应的 VFR 帧时间戳。
    double m_uploadedVideoTimestamp{ 0.0 };

    /// @brief 允许恢复显示的最小资源/Seek 代际。
    std::uint64_t m_requiredVideoRequestGeneration{ 0 };

    /// @brief 等待下一次命令录制上传的 RGBA 视频帧。
    /// @warning UI 主线程写入后，只由同帧后续渲染录制阶段读取；
    /// 两阶段由 VKRenderer 帧调度串行，不得从解码线程直接访问。
    std::vector<unsigned char> m_pendingVideoPixels;

    /// @brief 待上传视频帧的本地修订号。
    std::uint64_t m_pendingVideoUploadRevision{ 0 };

    /// @brief 待上传帧是否携带有效时间与代际状态。
    bool m_pendingVideoStateValid{ false };

    /// @brief 待上传帧的 VFR 时间戳。
    double m_pendingVideoTimestamp{ 0.0 };

    /// @brief 待上传帧所属资源/Seek 代际。
    std::uint64_t m_pendingVideoRequestGeneration{ 0 };

    /// @brief 待上传帧是否已到达视频末尾。
    bool m_pendingVideoReachedEnd{ false };

    /// @brief 已记录到 GPU 命令缓冲的视频帧修订号。
    std::uint64_t m_recordedVideoUploadRevision{ 0 };
};

/// @brief 保存创作教程的识别状态，将用户手势和本代谱面快照配对。
/// @details 不读取 EditorEngine 或保存外部快照；仅借用本帧的聚光灯、
/// 画布输入接口和局部坐标。所有代际标记都由此对象维护。
struct Basic2DCanvas::WalkthroughRecognizer {
    /// @brief 绘制并验证创作教程的单键、Shift 滑键和长条拖拽路径。
    /// @param spotlight 提供当前演练步骤和可视目标。
    /// @param snapshot 当前谱面与画笔状态快照。
    /// @param canvasScreenPosition 主画布左上角屏幕坐标。
    /// @param canvasSize 主画布逻辑像素尺寸。
    /// @warning UI 热路径：仅在对应教程步骤中绘制固定几何；随机路径只在
    /// 步骤进入或谱面切换时生成一次，失败释放通过取消命令清理临时画笔。
    void updateComposeWalkthrough(
        UI::Walkthrough::Spotlight& spotlight,
        Basic2DCanvasInteraction* interaction, const std::string& cameraId,
        float appliedYOffset, const Common::Render::RenderSnapshot& snapshot,
        const ImVec2& canvasScreenPosition, const ImVec2& canvasSize);

private:
    /// @brief 检查创作教程中的编辑操作并定位真实物件上的操作起点。
    /// @return 当前步骤属于编辑练习时返回 true。
    /// @warning UI 热路径：仅扫描固定数量的练习物件与当前帧命中框。
    bool updateComposeEditWalkthrough(
        UI::Walkthrough::Spotlight&           spotlight,
        Basic2DCanvasInteraction*             interaction,
        const Common::Render::RenderSnapshot& snapshot,
        const ImVec2& canvasScreenPosition, const ImVec2& canvasSize);

    /// @brief 当前教学路径的几何语义，防止步骤切换复用上一种物件的目标。
    enum class WalkthroughPlacementKind : std::uint8_t {
        Note,   ///< 跨轨、跨拍移动后放下单键。
        Flick,  ///< 同拍横向跨轨绘制滑键。
        Hold,   ///< 同轨向后延伸绘制长条。
    };

    /// @brief 创作教程一次固定拖拽路径的分拍与轨道目标。
    struct WalkthroughNoteDragTarget {
        std::uintptr_t beatmapInstanceId{ 0 };  ///< 防止跨谱面复用旧路径。
        int            sourceTrack{ 0 };        ///< 按下起点所在玩家轨。
        int            destinationTrack{ 0 };   ///< 松开目标所在玩家轨。
        double         sourceTime{ 0.0 };       ///< 起点精确分拍时间。
        double         destinationTime{ 0.0 };  ///< 目标精确分拍时间。
        /// @brief 路径对应的物件类型，用于进入新步骤时清除旧目标。
        WalkthroughPlacementKind kind{ WalkthroughPlacementKind::Note };
    };

    /// @brief 当前创作教程固定使用的起点和目标分拍。
    std::optional<WalkthroughNoteDragTarget> m_walkthroughNoteDragTarget;
    /// @brief 单键成功落点，跨滑键步骤保留以约束后续长条必须邻近且异轨。
    std::optional<WalkthroughNoteDragTarget> m_walkthroughPlacedNote;
    /// @brief 滑键成功起点，用于避开随后长条尾部的同轨同拍重叠。
    std::optional<WalkthroughNoteDragTarget> m_walkthroughPlacedFlick;
    /// @brief 本轮成功绘制的长条路径，供折线练习避开已有长条。
    std::optional<WalkthroughNoteDragTarget> m_walkthroughPlacedHold;
    /// @brief 本轮 Note、Hold、Flick、Polyline 的步骤标记，只核对当前谱面。
    std::array<std::uint64_t, 4> m_walkthroughPracticeTokens{};
    /// @brief 上述标记所属的谱面实例，防止跨标签误认相同实体号。
    std::uint64_t m_walkthroughPracticeBeatmapInstanceId{ 0 };
    /// @brief 新的单键步骤标记，用来在重新演练时废弃上一轮身份。
    std::uint64_t m_walkthroughNoteStepToken{ 0 };
    /// @brief 本次左键尝试是否从画布内开始。
    bool m_walkthroughNoteAttemptActive{ false };
    /// @brief 本次尝试是否从指定起点框按下。
    bool m_walkthroughNoteStartedAtSource{ false };
    /// @brief 折线练习是否已从画布按下左键，直到该次释放才结束。
    bool m_walkthroughPolylineAttemptActive{ false };
    /// @brief 固定的五段折线检查点，视野失效后再重选。
    std::optional<ComposePolylineTarget> m_walkthroughPolylineTarget;
    /// @brief 折线路线所属谱面实例，防止不同标签复用同一组拍位。
    std::uintptr_t m_walkthroughPolylineTargetBeatmapInstanceId{ 0 };
    /// @brief 当前手势下一个必须经过的折线路线检查点。
    std::size_t m_walkthroughPolylineNextWaypoint{ 0 };
    /// @brief 折线练习的轨道、工具及修饰键在整段手势中是否合规。
    bool m_walkthroughPolylineAttemptValid{ false };
    /// @brief 本次折线练习是否发生真实左键拖动。
    bool m_walkthroughPolylineDragged{ false };
    /// @brief 已释放并等待逻辑快照确认最终子段数的步骤身份。
    std::uint64_t m_walkthroughPolylinePendingToken{ 0 };
    /// @brief 编辑练习进入时固定的四个物件几何，避免拖动预览抵扣正式结果。
    std::array<Common::Render::RenderSnapshot::WalkthroughPracticeNoteState, 4>
        m_walkthroughEditBaselines{};
    /// @brief 当前编辑练习步骤身份与开始时已完成的编辑事务序号。
    std::uint64_t m_walkthroughEditStepToken{ 0 };
    std::uint64_t m_walkthroughEditFirstRevision{ 0 };
    /// @brief 缓存所属谱面；换标签后不复用旧谱面的实体句柄。
    std::uintptr_t m_walkthroughEditBeatmapInstanceId{ 0 };
    /// @brief 本次尝试是否已经形成可见拖动距离。
    bool m_walkthroughNoteDragged{ false };
    /// @brief 逻辑快照是否确认本次画笔已激活。
    bool m_walkthroughNoteBrushObserved{ false };
    /// @brief 本次尝试是否违反修饰键规则：单键禁 Shift，滑键和长条持续 Shift。
    bool m_walkthroughNoteModifierUsed{ false };
};
}  // namespace MMM::Canvas
