#include "graphic/theme/ImGuiTheme.h"

#include <utility>

namespace MMM::Graphic
{

ImGuiTheme::ImGuiTheme(std::string id, std::string displayName,
                       ImGuiThemeOrigin origin, std::string baseThemeId,
                       std::filesystem::path sourcePath,
                       ApplyFunction         applyFunction)
    : m_id(std::move(id))
    , m_displayName(std::move(displayName))
    , m_origin(origin)
    , m_baseThemeId(std::move(baseThemeId))
    , m_sourcePath(std::move(sourcePath))
    , m_applyFunction(std::move(applyFunction))
{
}

void ImGuiTheme::apply(ImGuiStyle& style) const
{
    if ( m_applyFunction ) {
        m_applyFunction(style);
    }
}

}  // namespace MMM::Graphic
