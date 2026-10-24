#pragma once

#include <cstdint>

namespace MMM::Event
{
/// @brief 项目打开请求的用户入口，与教程内容无关。
/// @note 入口值随异步流程传递，用于完成后恢复对应的交互反馈。
enum class ProjectOpenOrigin : std::uint8_t {
    /// @brief 程序内部或未指定入口。
    Unknown,
    /// @brief 文件菜单打开现有目录。
    FileMenu,
    /// @brief 快捷键打开现有目录。
    Shortcut,
    /// @brief 系统文件夹拖放。
    FolderDrop,
    /// @brief 谱面文件拖放到画布。
    BeatmapDrop,
    /// @brief 谱包拖放并临时阅览。
    PackageDrop,
    /// @brief 文件菜单新建并打开项目。
    CreateFileMenu,
    /// @brief 快捷键新建并打开项目。
    CreateShortcut
};
}  // namespace MMM::Event
