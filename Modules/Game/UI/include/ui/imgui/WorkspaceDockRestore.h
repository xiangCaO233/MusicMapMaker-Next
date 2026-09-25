#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace MMM::UI
{

/// @brief 从项目 ini 中读取指定画布窗口保存的 Dock 节点。
/// @param iniData 项目工作区的 ImGui ini 数据。
/// @param windowId 画布稳定窗口 ID，例如 Canvas_0。
/// @return 保存时已停靠则返回节点 ID；浮动或无记录返回空值。
/// @details ImGui 在 `[Window]` 节内分别保存各画布的 `DockId`，分栏树位于
/// 后续 `[Docking]` 节。这里仅提取窗口到叶节点的映射，实际树仍由 ImGui 加载。
/// 使用完整 section 标题与换行边界匹配，避免相似的 cameraId 互相串位。
/// `DockId` 后的逗号代表标签顺序，与叶节点身份无关，应停止十六进制解析。
/// 保持无记录与浮动窗口的区别：两者都不应被强行移入主 DockSpace。
/// @warning 仅在项目打开时解析，不得放入每帧稳定路径。
inline std::optional<ImGuiID> savedWorkspaceWindowDockId(
    std::string_view iniData, std::string_view windowId)
{
    // 精确匹配 section，避免 Canvas_1 命中 Canvas_10 的窗口记录。
    const std::string section    = "[Window][" + std::string(windowId) + "]";
    size_t            sectionPos = iniData.find(section);
    while ( sectionPos != std::string_view::npos ) {
        const size_t afterSection = sectionPos + section.size();
        // `find` 可能命中更长 ID 的前缀；只有完整标题才可读取其字段。
        if ( afterSection == iniData.size() || iniData[afterSection] == '\n' ||
             iniData[afterSection] == '\r' ) {
            // 只在本窗口的 section 内查找 DockId，下一标题属于其他窗口。
            const size_t nextSection = iniData.find('\n', afterSection);
            if ( nextSection == std::string_view::npos ) return std::nullopt;
            const size_t endSection = iniData.find("\n[", nextSection);
            // section 最后一行可能没有尾随换行，substr 的开放尾界兼容该格式。
            const auto body =
                iniData.substr(nextSection + 1,
                               endSection == std::string_view::npos
                                   ? std::string_view::npos
                                   : endSection - nextSection - 1);
            size_t lineStart = 0;
            while ( lineStart < body.size() ) {
                const size_t lineEnd = body.find('\n', lineStart);
                auto line = body.substr(lineStart,
                                        lineEnd == std::string_view::npos
                                            ? std::string_view::npos
                                            : lineEnd - lineStart);
                if ( line.starts_with("DockId=0x") ) {
                    // 逗号后是标签排序值；只解析固定十六进制节点身份。
                    line.remove_prefix(sizeof("DockId=0x") - 1);
                    const size_t comma = line.find(',');
                    if ( comma != std::string_view::npos )
                        line = line.substr(0, comma);
                    ImGuiID dockId          = 0;
                    const auto [end, error] = std::from_chars(
                        line.data(), line.data() + line.size(), dockId, 16);
                    // 非法或零 ID 无法定位节点；交由普通窗口恢复行为处理。
                    if ( error == std::errc{} &&
                         end == line.data() + line.size() && dockId != 0 )
                        return dockId;
                    return std::nullopt;
                }
                if ( lineEnd == std::string_view::npos ) break;
                lineStart = lineEnd + 1;
            }
            return std::nullopt;
        }
        sectionPos = iniData.find(section, afterSection);
    }
    return std::nullopt;
}

/// @brief 补回 ImGui 运行中加载布局后未能关联的画布停靠位置。
/// @param windowId 画布稳定窗口 ID。
/// @param savedDockId 项目 ini 中保存的节点。
/// @param centerDockId 保存节点失效时的主画布中心节点。
/// @return 当前已停靠到目标节点时返回 true；否则已发起下一帧停靠请求。
/// @details 保存的叶节点仍存在时必须优先使用它：左右并排的两个画布可能
/// 分别位于不同叶节点，直接送回中心会把它们合并为一个标签组。
/// 只有原节点确实缺失才使用中心节点，至少让已停靠的画布回到主窗口。
/// ImGui 的 DockBuilderDockWindow 在当前帧提交请求，窗口最终状态由下一帧
/// 的 DockSpace 与 Begin 确认，因此调用方须在少量后续帧里重新核对。
/// @warning 项目恢复阶段低频调用，不能作为每帧常态窗口定位入口。
inline bool reconcileSavedWorkspaceWindowDock(const std::string& windowId,
                                              ImGuiID            savedDockId,
                                              ImGuiID            centerDockId)
{
    // 已失效的项目节点退回主画布标签区，不能把画布留在独立浮动窗口。
    const ImGuiID targetDockId =
        ImGui::DockBuilderGetNode(savedDockId) ? savedDockId : centerDockId;
    if ( targetDockId == 0 || !ImGui::DockBuilderGetNode(targetDockId) )
        return false;

    // ImGui 内部 ID 使用 `###` 后面的稳定名称，与可见标题的语言无关。
    ImGuiWindow* window = ImGui::FindWindowByName(windowId.c_str());
    if ( window && window->DockId == targetDockId && window->DockNode )
        return true;

    // 已有窗口和下一帧才建立的窗口共用同一稳定名称请求。
    ImGui::DockBuilderDockWindow(windowId.c_str(), targetDockId);
    return false;
}

}  // namespace MMM::UI
