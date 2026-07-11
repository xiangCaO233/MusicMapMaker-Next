#pragma once

#include <cstddef>
#include <string_view>

namespace MMM::UI::WindowIdUtils
{

/// @brief 从 ImGui 窗口名称中提取最终生效的稳定 ID。
/// @param windowName ImGui 窗口名称。
/// @return 最后一个 ### 后的稳定 ID；没有 ### 时返回原始名称。
constexpr std::string_view stableWindowId(std::string_view windowName)
{
    const std::size_t marker = windowName.rfind("###");
    if ( marker == std::string_view::npos ) {
        return windowName;
    }
    return windowName.substr(marker + 3U);
}

}  // namespace MMM::UI::WindowIdUtils
