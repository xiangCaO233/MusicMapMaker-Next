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
        // 通过 error_code 获取系统临时根，测试不因文件系统异常中止进程。
        std::error_code filesystemError;
        const auto      tempRoot =
            std::filesystem::temp_directory_path(filesystemError);
        if ( filesystemError ) {
            // 空路径成为构造失败哨兵，main 会在任何写入前终止。
            return;
        }
        // 单调时间戳降低并行测试使用同一目录名的概率。
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path =
            tempRoot / ("mmm-font-preference-test-" + std::to_string(suffix));
        std::filesystem::create_directories(m_path, filesystemError);
        if ( filesystemError ) {
            // 目录创建失败时清空所有权，析构不会误删未创建的路径。
            m_path.clear();
        }
    }

    /// @brief 清理测试创建的临时目录。
    ~TemporaryFontDirectory()
    {
        // 空路径表示构造未取得目录所有权，禁止调用宽泛 remove_all。
        if ( m_path.empty() ) {
            return;
        }
        // 清理失败只留下临时测试目录，不覆盖测试主体的行为结果。
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
    // 校验器只检查普通文件存在性，不要求占位内容是真实字体格式。
    std::ofstream output(path, std::ios::binary);
    // 写入一个字节确保文件非空，并通过流状态确认落盘成功。
    output.put('\0');
    return output.good();
}

/// @brief 验证默认值和存在的皮肤字体不会被重置。
/// @param fontPath 存在的字体占位文件。
/// @return 行为符合预期时返回 true。
bool testAvailablePreferences(const std::filesystem::path& fontPath)
{
    // 皮肤清单把显示名称映射到已创建的普通文件。
    const std::vector<AvailableFont> fonts{ { "Skin Font", fontPath } };
    // Default 覆盖显式回退分支，名称值覆盖皮肤字体查找分支。
    std::string defaultPreference = "Default";
    std::string skinPreference    = "Skin Font";
    return !resetUnavailableFontPreference(defaultPreference, fonts) &&
           // 两项都必须保持原文本，false 返回值表示无需保存配置。
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
    // 未出现在皮肤清单的偏好会按 UTF-8 外部文件路径解释。
    std::string externalPreference = MMM::Config::pathToUtf8(fontPath);
    // 文件移动前路径有效，校验器不得主动把它重置。
    if ( resetUnavailableFontPreference(externalPreference, {}) ) {
        return false;
    }

    // rename 模拟用户在应用关闭期间移动自选字体。
    std::error_code filesystemError;
    std::filesystem::rename(
        fontPath, directory / "moved-font.ttf", filesystemError);
    return !filesystemError &&
           // 原路径失效后应报告变化，并写入稳定回退文本 Default。
           resetUnavailableFontPreference(externalPreference, {}) &&
           externalPreference == "Default";
}

/// @brief 验证失效皮肤名称和目录路径都会回退为默认值。
/// @param directory 测试临时目录。
/// @return 行为符合预期时返回 true。
bool testInvalidPreferences(const std::filesystem::path& directory)
{
    // 名称能在皮肤清单中找到，但对应文件刻意不存在。
    const std::vector<AvailableFont> missingSkinFont{
        { "Missing Skin Font", directory / "missing.ttf" }
    };
    // 目录路径存在但不是普通文件，不能被当作外部字体接受。
    std::string missingSkinPreference = "Missing Skin Font";
    std::string directoryPreference   = MMM::Config::pathToUtf8(directory);
    return resetUnavailableFontPreference(missingSkinPreference,
                                          missingSkinFont) &&
           // 两种无效来源都必须收敛到相同 Default 持久化值。
           missingSkinPreference == "Default" &&
           resetUnavailableFontPreference(directoryPreference, {}) &&
           directoryPreference == "Default";
}

}  // namespace

/// @brief 覆盖字体偏好在文件移动或失效后的回退行为。
/// @return 所有检查通过时返回 0。
/// @warning 测试只操作 RAII 临时目录，不读取或保存用户配置。
int main()
{
    // RAII 目录覆盖所有用例，任一返回路径都会尝试清理夹具。
    TemporaryFontDirectory directory;
    if ( directory.path().empty() ) {
        // 临时根不可用属于测试基础设施失败，无法验证文件移动行为。
        return 1;
    }

    const auto fontPath = directory.path() / "custom-font.ttf";
    // 创建失败时不进入偏好断言，避免把夹具错误误判为校验器问题。
    if ( !createPlaceholderFont(fontPath) ) {
        return 1;
    }

    // 顺序必须先验证现存文件，再由移动用例改变夹具状态。
    return testAvailablePreferences(fontPath) &&
                   testMovedExternalFont(directory.path(), fontPath) &&
                   testInvalidPreferences(directory.path())
               ? 0
               : 1;
}
