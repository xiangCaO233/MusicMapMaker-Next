#pragma once

#include <cstddef>
#include <string_view>

namespace MMM::UI::WindowIdUtils
{

/// @brief 从 ImGui 窗口名称中提取最终生效的稳定 ID。
/// @param windowName ImGui 窗口名称。
/// @return 最后一个 ### 后的稳定 ID；没有 ### 时返回原始名称。
/// @details ImGui 约定 `###` 之前是可变可见标题，之后是持久布局使用的 ID；
/// 多个标记存在时以最后一个为准，解析过程不分配或复制文本。
constexpr std::string_view stableWindowId(std::string_view windowName)
{
    // string_view 查找和切片均不复制标题文本。
    const std::size_t marker = windowName.rfind("###");
    if ( marker == std::string_view::npos ) {
        // 无显式稳定标记时，完整窗口名就是 ImGui ID。
        return windowName;
    }
    // 空后缀也原样返回，具体 ID 合法性仍由调用方保证。
    return windowName.substr(marker + 3U);
}

}  // namespace MMM::UI::WindowIdUtils
