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
    // 权限、编码或瞬时文件系统错误都按不可用处理，校验路径不传播异常。
    std::error_code filesystemError;
    return std::filesystem::is_regular_file(path, filesystemError) &&
           !filesystemError;
}

}  // namespace

/// @brief 校验单个字体名称或外部路径，并在资源失效时回退默认字体。
/// @param preference 可为 Default、皮肤字体名称或 UTF-8 外部字体路径。
/// @param availableFonts 当前皮肤公布的名称与文件路径列表。
/// @return 本次把偏好改为 Default 时返回 true。
/// @warning 低频配置校验路径：会查询文件系统，只能在资源加载阶段调用。
bool resetUnavailableFontPreference(
    std::string& preference, std::span<const AvailableFont> availableFonts)
{
    // 空值和显式 Default 都由皮肤字体回退链处理，无需访问文件系统。
    if ( preference.empty() || preference == "Default" ) {
        return false;
    }

    // 先按皮肤公开名称查找，名称命中后只验证该皮肤提供的实际文件。
    const auto skinFont =
        std::find_if(availableFonts.begin(),
                     availableFonts.end(),
                     [&preference](const AvailableFont& font) {
                         return font.first == preference;
                     });
    // 未命中名称时把偏好解释为外部 UTF-8 路径，兼容用户自选字体文件。
    const bool preferenceAvailable =
        skinFont != availableFonts.end()
            ? isRegularFileNoError(skinFont->second)
            : isRegularFileNoError(utf8ToPath(preference));
    if ( preferenceAvailable ) {
        // 可用偏好保持原字符串，调用方无需为无变化配置触发保存。
        return false;
    }

    // 统一写入稳定文本 Default，后续序列化不会保留失效名称或路径。
    preference = "Default";
    return true;
}

/// @brief 校验编辑器中的 ASCII 与 CJK 两项字体偏好。
/// @param settings 需要原位纠正的编辑器设置。
/// @param skinManager 已加载并持有两类字体清单的皮肤管理器。
/// @return 任一偏好发生回退时返回 true。
/// @warning 低频配置校验路径：连续执行两次文件系统可用性检查。
bool resetUnavailableFontPreferences(EditorSettings&    settings,
                                     const SkinManager& skinManager)
{
    // 两项必须分别执行，不能用短路表达式跳过 CJK 校验。
    const bool asciiReset = resetUnavailableFontPreference(
        settings.preferredAsciiFont, skinManager.getAsciiFonts());
    const bool cjkReset = resetUnavailableFontPreference(
        settings.preferredCjkFont, skinManager.getCjkFonts());
    // 聚合变化结果供启动流程决定是否持久化修正后的配置。
    return asciiReset || cjkReset;
}

}  // namespace MMM::Config
