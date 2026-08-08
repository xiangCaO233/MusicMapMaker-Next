#include "ui/imgui/menu/items/MainMenuRecentProjectsItem.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/utils/UIWidgetUtils.h"
#include <filesystem>
#include <imgui.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 最近项目子菜单中项目名的最大显示宽度。
constexpr float RECENT_PROJECT_NAME_MAX_WIDTH = 260.0f;

/// @brief 最近项目子菜单中路径列的最大显示宽度。
constexpr float RECENT_PROJECT_PATH_MAX_WIDTH = 420.0f;

/// @brief 菜单中用于截断文本的省略号。
constexpr std::string_view MENU_TEXT_ELLIPSIS = "...";

/// @brief 计算 UTF-8 文本在当前 ImGui 字体中的宽度。
/// @param text UTF-8 文本视图。
/// @return 当前字体下的像素宽度。
/// @warning UI 热路径低频分支：仅在菜单展开时执行；禁止用于每帧大批量列表。
float calcUtf8TextWidth(std::string_view text)
{
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// @brief 判断字节是否为 UTF-8 续字节。
/// @param byte 输入字节。
/// @return 续字节返回 true。
bool isUtf8ContinuationByte(unsigned char byte)
{
    return (byte & 0xC0U) == 0x80U;
}

/// @brief 收集 UTF-8 文本的字符边界。
/// @param text UTF-8 文本视图。
/// @return 包含 0 和末尾位置的边界表。
/// @warning UI 热路径低频分支：仅用于菜单文本截断；输入异常时按单字节推进。
std::vector<size_t> collectUtf8Boundaries(std::string_view text)
{
    std::vector<size_t> boundaries;
    boundaries.reserve(text.size() + 1);
    boundaries.push_back(0);

    size_t i = 0;
    while ( i < text.size() ) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        size_t              step = 1;
        if ( (byte & 0x80U) == 0U ) {
            step = 1;
        } else if ( (byte & 0xE0U) == 0xC0U ) {
            step = 2;
        } else if ( (byte & 0xF0U) == 0xE0U ) {
            step = 3;
        } else if ( (byte & 0xF8U) == 0xF0U ) {
            step = 4;
        }

        if ( i + step > text.size() ) {
            step = 1;
        } else {
            for ( size_t j = 1; j < step; ++j ) {
                if ( !isUtf8ContinuationByte(
                         static_cast<unsigned char>(text[i + j])) ) {
                    step = 1;
                    break;
                }
            }
        }

        i += step;
        boundaries.push_back(i);
    }
    return boundaries;
}

/// @brief 从尾部截断 UTF-8 文本以适配给定宽度。
/// @param text UTF-8 文本视图。
/// @param maxWidth 最大显示宽度。
/// @return 截断后的显示文本。
/// @warning UI 热路径低频分支：仅在最近项目菜单展开时执行。
std::string truncateUtf8TailToWidth(std::string_view text, float maxWidth)
{
    if ( maxWidth <= 0.0f ) {
        return {};
    }
    if ( calcUtf8TextWidth(text) <= maxWidth ) {
        return std::string(text);
    }
    if ( calcUtf8TextWidth(MENU_TEXT_ELLIPSIS) > maxWidth ) {
        return std::string(MENU_TEXT_ELLIPSIS);
    }

    const auto boundaries = collectUtf8Boundaries(text);
    size_t     lo         = 0;
    size_t     hi         = boundaries.size() - 1;
    while ( lo < hi ) {
        const size_t mid = (lo + hi + 1) / 2;
        std::string  candidate(text.substr(0, boundaries[mid]));
        candidate += MENU_TEXT_ELLIPSIS;
        if ( calcUtf8TextWidth(candidate) <= maxWidth ) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    std::string result(text.substr(0, boundaries[lo]));
    result += MENU_TEXT_ELLIPSIS;
    return result;
}

/// @brief 从中间截断 UTF-8 文本以保留路径开头和结尾。
/// @param text UTF-8 文本视图。
/// @param maxWidth 最大显示宽度。
/// @return 截断后的显示文本。
/// @warning UI 热路径低频分支：仅在最近项目菜单展开时执行。
std::string truncateUtf8MiddleToWidth(std::string_view text, float maxWidth)
{
    if ( maxWidth <= 0.0f ) {
        return {};
    }
    if ( calcUtf8TextWidth(text) <= maxWidth ) {
        return std::string(text);
    }
    if ( calcUtf8TextWidth(MENU_TEXT_ELLIPSIS) > maxWidth ) {
        return std::string(MENU_TEXT_ELLIPSIS);
    }

    const auto   boundaries = collectUtf8Boundaries(text);
    const size_t charCount  = boundaries.empty() ? 0 : boundaries.size() - 1;
    for ( size_t keep = charCount; keep > 0; --keep ) {
        const size_t prefixCount = (keep + 1) / 2;
        const size_t suffixCount = keep / 2;
        if ( prefixCount + suffixCount >= charCount ) {
            continue;
        }

        std::string candidate(text.substr(0, boundaries[prefixCount]));
        candidate += MENU_TEXT_ELLIPSIS;
        candidate += text.substr(boundaries[charCount - suffixCount]);
        if ( calcUtf8TextWidth(candidate) <= maxWidth ) {
            return candidate;
        }
    }
    return std::string(MENU_TEXT_ELLIPSIS);
}

}  // namespace

/// @brief 构造最近项目子菜单项。
/// @param actionHandler 最近项目点击业务处理器。
MainMenuRecentProjectsItem::MainMenuRecentProjectsItem(
    std::unique_ptr<IMainMenuItemActionHandler> actionHandler)
    : m_actionHandler(std::move(actionHandler))
{
}

/// @brief 更新最近项目动作处理器跨帧状态。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
void MainMenuRecentProjectsItem::update(MainMenuContext& context)
{
    if ( m_actionHandler ) {
        m_actionHandler->update(context);
    }
}

/// @brief 绘制最近项目子菜单并在点击时执行自身 action handler。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径低频分支：仅在最近项目子菜单展开时格式化显示文本。
void MainMenuRecentProjectsItem::render(MainMenuContext& context)
{
    if ( !::MMM::UI::FeedbackBeginMenu(TR("ui.file.open_recent").data()) ) {
        return;
    }

    const auto& recent =
        Config::AppConfig::instance().getEditorConfig().recentProjects;
    if ( recent.empty() ) {
        ::MMM::UI::FeedbackMenuItem(
            TR("ui.file.no_recent").data(), nullptr, false, false);
    } else {
        for ( size_t i = 0; i < recent.size(); ++i ) {
            const auto&           path = recent[i];
            std::filesystem::path p    = Config::utf8ToPath(path);
            std::string           name = Config::pathToUtf8(p.filename());
            const std::string     displayName = truncateUtf8TailToWidth(
                name, RECENT_PROJECT_NAME_MAX_WIDTH * context.dpiScale);
            const std::string displayPath = truncateUtf8MiddleToWidth(
                path, RECENT_PROJECT_PATH_MAX_WIDTH * context.dpiScale);
            ImGui::PushID(static_cast<int>(i));
            if ( ::MMM::UI::FeedbackMenuItem(displayName.c_str(),
                                             displayPath.c_str()) &&
                 m_actionHandler ) {
                m_actionHandler->execute(context,
                                         MainMenuItemActivation{
                                             .textPayload = path,
                                         });
            }
            if ( ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) ) {
                ImGui::SetTooltip("%s\n%s", name.c_str(), path.c_str());
            }
            ImGui::PopID();
        }
    }
    ::MMM::UI::FeedbackEndMenu();
}

/// @brief 渲染最近项目动作处理器延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
void MainMenuRecentProjectsItem::renderDeferred(MainMenuContext& context)
{
    if ( m_actionHandler ) {
        m_actionHandler->renderDeferred(context);
    }
}

}  // namespace MMM::UI
