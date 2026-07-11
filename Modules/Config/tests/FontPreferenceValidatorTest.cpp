#include "config/FontPreferenceValidator.h"
#include "config/Utf8Path.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{

using MMM::Config::AvailableFont;
using MMM::Config::resetUnavailableFontPreference;

/// @brief 测试期间创建并自动清理的临时字体目录。
class TemporaryFontDirectory
{
public:
    /// @brief 创建当前测试进程专用的临时目录。
    TemporaryFontDirectory()
    {
        std::error_code filesystemError;
        const auto      tempRoot =
            std::filesystem::temp_directory_path(filesystemError);
        if ( filesystemError ) {
            return;
        }
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path =
            tempRoot / ("mmm-font-preference-test-" + std::to_string(suffix));
        std::filesystem::create_directories(m_path, filesystemError);
        if ( filesystemError ) {
            m_path.clear();
        }
    }

    /// @brief 清理测试创建的临时目录。
    ~TemporaryFontDirectory()
    {
        if ( m_path.empty() ) {
            return;
        }
        std::error_code filesystemError;
        std::filesystem::remove_all(m_path, filesystemError);
    }

    /// @brief 禁止复制临时目录所有权。
    TemporaryFontDirectory(const TemporaryFontDirectory&) = delete;

    /// @brief 禁止复制赋值临时目录所有权。
    TemporaryFontDirectory& operator=(const TemporaryFontDirectory&) = delete;

    /// @brief 获取临时目录路径。
    /// @return 创建失败时返回空路径。
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    /// @brief 当前测试拥有的临时目录。
    std::filesystem::path m_path;
};

/// @brief 创建用于路径校验的空字体占位文件。
/// @param path 需要创建的文件路径。
/// @return 文件创建成功时返回 true。
bool createPlaceholderFont(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary);
    output.put('\0');
    return output.good();
}

/// @brief 验证默认值和存在的皮肤字体不会被重置。
/// @param fontPath 存在的字体占位文件。
/// @return 行为符合预期时返回 true。
bool testAvailablePreferences(const std::filesystem::path& fontPath)
{
    const std::vector<AvailableFont> fonts{ { "Skin Font", fontPath } };
    std::string                      defaultPreference = "Default";
    std::string                      skinPreference    = "Skin Font";
    return !resetUnavailableFontPreference(defaultPreference, fonts) &&
           defaultPreference == "Default" &&
           !resetUnavailableFontPreference(skinPreference, fonts) &&
           skinPreference == "Skin Font";
}

/// @brief 验证已移动的外部字体会回退为默认值。
/// @param directory 测试临时目录。
/// @param fontPath 需要模拟移动的字体路径。
/// @return 行为符合预期时返回 true。
bool testMovedExternalFont(const std::filesystem::path& directory,
                           const std::filesystem::path& fontPath)
{
    std::string externalPreference = MMM::Config::pathToUtf8(fontPath);
    if ( resetUnavailableFontPreference(externalPreference, {}) ) {
        return false;
    }

    std::error_code filesystemError;
    std::filesystem::rename(
        fontPath, directory / "moved-font.ttf", filesystemError);
    return !filesystemError &&
           resetUnavailableFontPreference(externalPreference, {}) &&
           externalPreference == "Default";
}

/// @brief 验证失效皮肤名称和目录路径都会回退为默认值。
/// @param directory 测试临时目录。
/// @return 行为符合预期时返回 true。
bool testInvalidPreferences(const std::filesystem::path& directory)
{
    const std::vector<AvailableFont> missingSkinFont{
        { "Missing Skin Font", directory / "missing.ttf" }
    };
    std::string missingSkinPreference = "Missing Skin Font";
    std::string directoryPreference   = MMM::Config::pathToUtf8(directory);
    return resetUnavailableFontPreference(missingSkinPreference,
                                          missingSkinFont) &&
           missingSkinPreference == "Default" &&
           resetUnavailableFontPreference(directoryPreference, {}) &&
           directoryPreference == "Default";
}

}  // namespace

/// @brief 覆盖字体偏好在文件移动或失效后的回退行为。
/// @return 所有检查通过时返回 0。
int main()
{
    TemporaryFontDirectory directory;
    if ( directory.path().empty() ) {
        return 1;
    }

    const auto fontPath = directory.path() / "custom-font.ttf";
    if ( !createPlaceholderFont(fontPath) ) {
        return 1;
    }

    return testAvailablePreferences(fontPath) &&
                   testMovedExternalFont(directory.path(), fontPath) &&
                   testInvalidPreferences(directory.path())
               ? 0
               : 1;
}
