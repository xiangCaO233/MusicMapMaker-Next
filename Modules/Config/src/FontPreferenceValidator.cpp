#include "config/FontPreferenceValidator.h"

#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"

#include <algorithm>
#include <system_error>

namespace MMM::Config
{
namespace
{

/// @brief 无异常判断路径是否指向可加载的普通字体文件。
/// @param path 待检查路径。
/// @return 路径存在且是普通文件时返回 true。
bool isRegularFileNoError(const std::filesystem::path& path)
{
    std::error_code filesystemError;
    return std::filesystem::is_regular_file(path, filesystemError) &&
           !filesystemError;
}

}  // namespace

bool resetUnavailableFontPreference(
    std::string& preference, std::span<const AvailableFont> availableFonts)
{
    if ( preference.empty() || preference == "Default" ) {
        return false;
    }

    const auto skinFont =
        std::find_if(availableFonts.begin(),
                     availableFonts.end(),
                     [&preference](const AvailableFont& font) {
                         return font.first == preference;
                     });
    const bool preferenceAvailable =
        skinFont != availableFonts.end()
            ? isRegularFileNoError(skinFont->second)
            : isRegularFileNoError(utf8ToPath(preference));
    if ( preferenceAvailable ) {
        return false;
    }

    preference = "Default";
    return true;
}

bool resetUnavailableFontPreferences(EditorSettings&    settings,
                                     const SkinManager& skinManager)
{
    const bool asciiReset = resetUnavailableFontPreference(
        settings.preferredAsciiFont, skinManager.getAsciiFonts());
    const bool cjkReset = resetUnavailableFontPreference(
        settings.preferredCjkFont, skinManager.getCjkFonts());
    return asciiReset || cjkReset;
}

}  // namespace MMM::Config
