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
/// @note 逻辑像素值在绘制时乘以当前 DPI 缩放。
constexpr float RECENT_PROJECT_NAME_MAX_WIDTH = 260.0f;

/// @brief 最近项目子菜单中路径列的最大显示宽度。
/// @note 路径列更宽，以尽量同时保留目录前缀与文件名。
constexpr float RECENT_PROJECT_PATH_MAX_WIDTH = 420.0f;

/// @brief 菜单中用于截断文本的省略号。
/// @note 使用 ASCII 形式以兼容缺少省略号字形的皮肤字体。
constexpr std::string_view MENU_TEXT_ELLIPSIS = "...";

/// @brief 计算 UTF-8 文本在当前 ImGui 字体中的宽度。
/// @param text UTF-8 文本视图。
/// @return 当前字体下的像素宽度。
/// @warning UI 热路径低频分支：仅在菜单展开时执行；禁止用于每帧大批量列表。
/// @note 显式传递结束指针，保证 string_view 不需要空字符结尾。
float calcUtf8TextWidth(std::string_view text)
{
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// @brief 判断字节是否为 UTF-8 续字节。
/// @param byte 输入字节。
/// @return 续字节返回 true。
/// @note 仅验证 10xxxxxx 位型，不负责判断完整 UTF-8 序列合法性。
bool isUtf8ContinuationByte(unsigned char byte)
{
    return (byte & 0xC0U) == 0x80U;
}

/// @brief 收集 UTF-8 文本的字符边界。
/// @param text UTF-8 文本视图。
/// @return 包含 0 和末尾位置的边界表。
/// @warning UI 热路径低频分支：仅用于菜单文本截断；输入异常时按单字节推进。
/// @note 边界表保证后续 substr 不会从合法多字节字符中间切开。
std::vector<size_t> collectUtf8Boundaries(std::string_view text)
{
    std::vector<size_t> boundaries;
    // 最坏情况每个字节都是独立字符，预留完整容量避免循环中扩容。
    boundaries.reserve(text.size() + 1);
    // 起点边界允许截断结果保留零个字符。
    boundaries.push_back(0);

    size_t i = 0;
    while ( i < text.size() ) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        size_t              step = 1;
        // 根据首字节位型推导候选 UTF-8 序列长度。
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
            // 尾部残缺序列退化为单字节，确保扫描始终向前推进。
            step = 1;
        } else {
            // 候选多字节序列必须全部具有续字节位型。
            for ( size_t j = 1; j < step; ++j ) {
                if ( !isUtf8ContinuationByte(
                         static_cast<unsigned char>(text[i + j])) ) {
                    step = 1;
                    // 非法序列只消费首字节，后续字节仍可独立解析。
                    break;
                }
            }
        }

        i += step;
        // 每次推进后记录字符尾边界，最终自然包含 text.size()。
        boundaries.push_back(i);
    }
    return boundaries;
}

/// @brief 从尾部截断 UTF-8 文本以适配给定宽度。
/// @param text UTF-8 文本视图。
/// @param maxWidth 最大显示宽度。
/// @return 截断后的显示文本。
/// @warning UI 热路径低频分支：仅在最近项目菜单展开时执行。
/// @note 使用二分搜索寻找可容纳的最长前缀，保留项目名开头。
std::string truncateUtf8TailToWidth(std::string_view text, float maxWidth)
{
    // 非正宽度表示没有可用绘制空间，直接返回空文本。
    if ( maxWidth <= 0.0f ) {
        return {};
    }
    // 未超宽时保留原文，避免不必要的边界扫描。
    if ( calcUtf8TextWidth(text) <= maxWidth ) {
        return std::string(text);
    }
    // 连省略号都无法容纳时仍返回统一截断标记，由布局负责裁剪。
    if ( calcUtf8TextWidth(MENU_TEXT_ELLIPSIS) > maxWidth ) {
        return std::string(MENU_TEXT_ELLIPSIS);
    }

    // 边界索引是字符数量到安全字节偏移的映射。
    const auto boundaries = collectUtf8Boundaries(text);
    size_t     lo         = 0;
    size_t     hi         = boundaries.size() - 1;
    while ( lo < hi ) {
        // 取上中位数，保证可容纳分支能收缩到最长前缀。
        const size_t mid = (lo + hi + 1) / 2;
        std::string  candidate(text.substr(0, boundaries[mid]));
        candidate += MENU_TEXT_ELLIPSIS;
        if ( calcUtf8TextWidth(candidate) <= maxWidth ) {
            // 当前前缀可用，继续尝试保留更多字符。
            lo = mid;
        } else {
            // 当前候选过宽，排除它及所有更长前缀。
            hi = mid - 1;
        }
    }

    // 使用最终安全边界构造结果，绝不切断 UTF-8 字符。
    std::string result(text.substr(0, boundaries[lo]));
    result += MENU_TEXT_ELLIPSIS;
    return result;
}

/// @brief 从中间截断 UTF-8 文本以保留路径开头和结尾。
/// @param text UTF-8 文本视图。
/// @param maxWidth 最大显示宽度。
/// @return 截断后的显示文本。
/// @warning UI 热路径低频分支：仅在最近项目菜单展开时执行。
/// @note 从最长保留字符数向下搜索，优先保留路径两端信息。
std::string truncateUtf8MiddleToWidth(std::string_view text, float maxWidth)
{
    // 与尾部截断保持一致的无可用宽度语义。
    if ( maxWidth <= 0.0f ) {
        return {};
    }
    // 宽度足够时无需构造候选字符串。
    if ( calcUtf8TextWidth(text) <= maxWidth ) {
        return std::string(text);
    }
    // 最小可见结果统一为省略号。
    if ( calcUtf8TextWidth(MENU_TEXT_ELLIPSIS) > maxWidth ) {
        return std::string(MENU_TEXT_ELLIPSIS);
    }

    const auto boundaries = collectUtf8Boundaries(text);
    // 边界数量比字符数量多一个起点位置。
    const size_t charCount = boundaries.empty() ? 0 : boundaries.size() - 1;
    for ( size_t keep = charCount; keep > 0; --keep ) {
        // 奇数保留量优先给前缀一个字符，强化路径根部辨识度。
        const size_t prefixCount = (keep + 1) / 2;
        const size_t suffixCount = keep / 2;
        if ( prefixCount + suffixCount >= charCount ) {
            // 必须实际删除至少一个字符，否则候选仍与原文等长。
            continue;
        }

        std::string candidate(text.substr(0, boundaries[prefixCount]));
        candidate += MENU_TEXT_ELLIPSIS;
        // 后缀通常包含文件名或最深层目录，应与前缀同时保留。
        candidate += text.substr(boundaries[charCount - suffixCount]);
        if ( calcUtf8TextWidth(candidate) <= maxWidth ) {
            // 从大到小搜索保证首个命中候选保留字符最多。
            return candidate;
        }
    }
    // 没有任何两端组合可容纳时回退到最小标记。
    return std::string(MENU_TEXT_ELLIPSIS);
}

}  // namespace

/// @brief 构造最近项目子菜单项。
/// @param actionHandler 最近项目点击业务处理器。
/// @note 处理器所有权移入菜单项，并与菜单项保持相同生命周期。
MainMenuRecentProjectsItem::MainMenuRecentProjectsItem(
    std::unique_ptr<IMainMenuItemActionHandler> actionHandler)
    : m_actionHandler(std::move(actionHandler))
{
}

/// @brief 更新最近项目动作处理器跨帧状态。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
/// @note 最近项目列表本身只在子菜单展开时读取。
void MainMenuRecentProjectsItem::update(MainMenuContext& context)
{
    if ( m_actionHandler ) {
        m_actionHandler->update(context);
    }
}

/// @brief 绘制最近项目子菜单并在点击时执行自身 action handler。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径低频分支：仅在最近项目子菜单展开时格式化显示文本。
/// @note 配置路径保持原值，截断只影响菜单显示和提示布局。
void MainMenuRecentProjectsItem::render(MainMenuContext& context)
{
    // 子菜单关闭时不读取列表、不分配字符串，也不执行路径转换。
    if ( !::MMM::UI::FeedbackBeginMenu(TR("ui.file.open_recent").data()) ) {
        return;
    }

    // 引用权威配置列表，避免为每次展开复制全部最近项目路径。
    const auto& recent =
        Config::AppConfig::instance().getEditorConfig().recentProjects;
    if ( recent.empty() ) {
        // 空列表以禁用项明确反馈，不提供可激活占位操作。
        ::MMM::UI::FeedbackMenuItem(
            TR("ui.file.no_recent").data(), nullptr, false, false);
    } else {
        // 配置顺序即最近使用顺序，逐项保留稳定索引。
        for ( size_t i = 0; i < recent.size(); ++i ) {
            const auto&           path = recent[i];
            std::filesystem::path p    = Config::utf8ToPath(path);
            // 项目名取路径末段，完整路径另列显示以消除同名歧义。
            std::string       name        = Config::pathToUtf8(p.filename());
            const std::string displayName = truncateUtf8TailToWidth(
                name, RECENT_PROJECT_NAME_MAX_WIDTH * context.dpiScale);
            const std::string displayPath = truncateUtf8MiddleToWidth(
                path, RECENT_PROJECT_PATH_MAX_WIDTH * context.dpiScale);
            // 索引 ID 区分同名项目，且不依赖截断后的可见文本。
            ImGui::PushID(static_cast<int>(i));
            if ( ::MMM::UI::FeedbackMenuItem(displayName.c_str(),
                                             displayPath.c_str()) &&
                 m_actionHandler ) {
                // 激活载荷传递原始完整路径，绝不使用截断显示字符串。
                m_actionHandler->execute(context,
                                         MainMenuItemActivation{
                                             .textPayload = path,
                                         });
            }
            if ( ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) ) {
                // 延迟提示恢复未经截断的项目名和路径，便于精确确认目标。
                ImGui::SetTooltip("%s\n%s", name.c_str(), path.c_str());
            }
            // 每次循环严格恢复 ID 栈，避免影响后续菜单项标识。
            ImGui::PopID();
        }
    }
    // 与成功 BeginMenu 配对，空列表和非空列表共用结束路径。
    ::MMM::UI::FeedbackEndMenu();
}

/// @brief 渲染最近项目动作处理器延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
/// @note 项目打开失败弹窗由动作处理器在菜单作用域外渲染。
void MainMenuRecentProjectsItem::renderDeferred(MainMenuContext& context)
{
    if ( m_actionHandler ) {
        m_actionHandler->renderDeferred(context);
    }
}

}  // namespace MMM::UI
