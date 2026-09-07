#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <cstdint>
#include <string>

namespace MMM::Event
{
/// @brief 项目打开请求的用户入口，与教程内容无关。
enum class ProjectOpenOrigin : std::uint8_t {
    Unknown,      ///< 程序内部或未指定入口。
    FileMenu,     ///< 文件菜单打开目录。
    Shortcut,     ///< 快捷键打开目录。
    FolderDrop,   ///< 系统文件夹拖放。
    BeatmapDrop,  ///< 谱面文件拖放到画布。
    PackageDrop   ///< 谱包拖放并临时阅览。
};
/// @brief 项目打开交互结果；仅实际打开就绪后发布 m_completed。
struct ProjectOpenInteractionEvent : BaseEvent {
    /// @brief 本次用户操作入口，随异步请求传递。
    ProjectOpenOrigin m_origin{ ProjectOpenOrigin::Unknown };
    /// @brief false 表示唤起选择器，true 表示项目已完成打开。
    bool m_completed{ false };
    /// @brief 本次打开的原始路径。
    std::string m_path;
    /// @brief 临时只读项目已就绪。
    bool m_readOnly{ false };
    /// @brief 指定的谱面已成功加载为会话。
    bool m_beatmapOpened{ false };
};
}  // namespace MMM::Event
EVENT_REGISTER_PARENTS(MMM::Event::ProjectOpenInteractionEvent,
                       MMM::Event::BaseEvent);
