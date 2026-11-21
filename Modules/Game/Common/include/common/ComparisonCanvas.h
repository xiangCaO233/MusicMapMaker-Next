#pragma once

#include <memory>
#include <string_view>

namespace MMM::Config
{
struct VisualConfig;
}

namespace MMM::Common
{
/// @brief 对比相机复用谱面会话，但拥有独立的窗口和同步缓冲身份。
inline constexpr std::string_view COMPARISON_CANVAS_SUFFIX = "/Compare/";

/// @brief 只同步单个对比视口的临时选项，不写入项目或应用配置。
struct ComparisonCanvasSettings {
    /// @brief 是否忽略变速、跳跃和停顿对画布坐标的映射。
    bool linearMapping{ false };
    /// @brief 隐藏的停靠标签无需生产渲染快照。
    bool visible{ true };
    /// @brief 视口独立的布局与相机参数，UI 变更后发布为只读快照。
    /// @warning 渲染和输入路径借用字段；仅显式布局改动时传递所有权。
    std::shared_ptr<const Config::VisualConfig> visual{};
    /// @brief 布局工具的物件拖动约束只适用于此视口。
    bool disableVerticalObjectDrag{ false };
};

/// @brief 解析视口所属的原始谱面相机 ID，返回值借用传入字符串。
/// @warning 路由热路径：只截取字符串视图，不复制相机 ID 或访问会话容器。
inline std::string_view canvasSessionCameraId(std::string_view id)
{
    return id.substr(0, id.find(COMPARISON_CANVAS_SUFFIX));
}

/// @brief 判断给定标识是否属于原始谱面的一份对比视口。
/// @warning 逐帧窗口及逻辑路由：不分配内存或访问会话。
inline bool isComparisonCanvas(std::string_view id)
{
    return canvasSessionCameraId(id).size() != id.size();
}
}  // namespace MMM::Common
