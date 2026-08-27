#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace MMM::Logic
{
class BeatmapSyncBuffer;
}

namespace MMM::UI
{
class IUIView;

/// @brief 在组合根注入的画布视图创建接口，避免 UI 模块依赖 Canvas 具体类。
class ICanvasViewFactory
{
public:
    virtual ~ICanvasViewFactory() = default;

    /// @brief 创建主编辑画布并把所有权交给调用方。
    /// @warning 低频会话创建路径：复制 shared_ptr 以把跨线程同步缓冲区生命周期
    /// 延长到画布销毁；不得在每帧 UI 更新中调用。
    virtual std::unique_ptr<IUIView> createBasic2DCanvas(
        const std::string& name, std::uint32_t width, std::uint32_t height,
        std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer,
        const std::string&                        cameraId) = 0;

    /// @brief 创建预览画布并把所有权交给调用方。
    /// @warning 应用启动低频路径：复制 shared_ptr 以保证同步缓冲区跨线程存活。
    virtual std::unique_ptr<IUIView> createPreviewCanvas(
        const std::string& name, std::uint32_t width, std::uint32_t height,
        std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer) = 0;

    /// @brief 创建时间线画布并把所有权交给调用方。
    /// @warning 应用启动低频路径：复制 shared_ptr 以保证同步缓冲区跨线程存活。
    virtual std::unique_ptr<IUIView> createTimelineCanvas(
        const std::string& name, std::uint32_t width, std::uint32_t height,
        std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer) = 0;
};

}  // namespace MMM::UI
