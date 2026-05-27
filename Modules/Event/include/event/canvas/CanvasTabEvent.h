#pragma once

#include <cstdint>
#include <string>

namespace MMM::Event
{

/// @brief 画布 Tab 切换事件 — 用户点击了不同的画布 Tab
struct CanvasTabSwitchEvent {
    /// @brief 新激活的 Session 索引
    int32_t newActiveIndex{ -1 };
    /// @brief 新激活的画布 cameraId
    std::string cameraId;
};

/// @brief 画布 Tab 关闭请求事件
struct CanvasTabCloseRequestEvent {
    /// @brief 被关闭的 Session 索引
    int32_t sessionIndex{ -1 };
};

/// @brief 画布 Tab 创建完成事件
struct CanvasTabCreatedEvent {
    /// @brief 新创建的 Session 索引
    int32_t sessionIndex{ -1 };
    /// @brief 新创建的画布 cameraId
    std::string cameraId;
};

}  // namespace MMM::Event
