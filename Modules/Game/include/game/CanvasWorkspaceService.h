#pragma once

#include "ui/ICanvasWorkspaceService.h"

namespace MMM::Game
{

/// @brief 组合根中的画布工作区适配器，桥接 UI 端口与 Logic/Canvas 实现。
class CanvasWorkspaceService final : public UI::ICanvasWorkspaceService
{
public:
    /// @copydoc UI::ICanvasWorkspaceService::fillEntries
    void fillEntries(std::vector<UI::CanvasWorkspaceEntry>& entries) override;

    /// @copydoc UI::ICanvasWorkspaceService::getActiveEntryIndex
    [[nodiscard]] std::int32_t getActiveEntryIndex() const override;

    /// @copydoc UI::ICanvasWorkspaceService::hasPendingProjectSwitch
    [[nodiscard]] bool hasPendingProjectSwitch() const override;

    /// @copydoc UI::ICanvasWorkspaceService::saveProject
    void saveProject() override;

    /// @copydoc UI::ICanvasWorkspaceService::createLogoPlaceholderSession
    void createLogoPlaceholderSession(const std::string& displayName) override;

    /// @copydoc UI::ICanvasWorkspaceService::closeSession
    void closeSession(std::int32_t index, bool updateWorkspace) override;

    /// @copydoc UI::ICanvasWorkspaceService::getEntryCount
    [[nodiscard]] std::int32_t getEntryCount() const override;

    /// @copydoc UI::ICanvasWorkspaceService::consumePendingFocusIndex
    [[nodiscard]] std::int32_t consumePendingFocusIndex() override;

    /// @copydoc UI::ICanvasWorkspaceService::requestEntryFocus
    void requestEntryFocus(std::int32_t index) override;

    /// @brief 创建主编辑画布并绑定对应 Logic 同步缓冲区。
    /// @warning 低频会话创建路径：复制 shared_ptr 以保证跨线程同步缓冲区
    /// 在画布销毁前存活；不得在稳定的每帧路径调用。
    [[nodiscard]] std::unique_ptr<UI::IUIView> createMainCanvas(
        const UI::CanvasWorkspaceEntry& entry, std::uint32_t width,
        std::uint32_t height) override;

    /// @copydoc UI::ICanvasWorkspaceService::createPreviewCanvas
    [[nodiscard]] std::unique_ptr<UI::IUIView> createPreviewCanvas(
        const std::string& name, std::uint32_t width,
        std::uint32_t height) override;

    /// @copydoc UI::ICanvasWorkspaceService::createTimelineCanvas
    [[nodiscard]] std::unique_ptr<UI::IUIView> createTimelineCanvas(
        const std::string& name, std::uint32_t width,
        std::uint32_t height) override;
};

}  // namespace MMM::Game
