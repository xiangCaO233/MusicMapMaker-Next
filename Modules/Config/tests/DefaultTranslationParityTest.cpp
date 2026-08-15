#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
/// @brief 单个默认翻译文件的键集与重复项。
struct TranslationKeySet {
    /// @brief 文件中声明的所有翻译键。
    std::unordered_set<std::string> keys;

    /// @brief 在同一文件中重复声明的翻译键。
    std::vector<std::string> duplicates;
};

/// @brief 从默认 Lua 翻译表中提取双引号键。
/// @param path 翻译文件路径。
/// @param result 接收键集与重复项。
/// @return 文件可读时返回 true。
bool loadTranslationKeys(const std::filesystem::path& path,
                         TranslationKeySet&           result)
{
    std::ifstream input(path, std::ios::binary);
    if ( !input ) return false;

    std::string line;
    while ( std::getline(input, line) ) {
        const std::size_t keyBegin = line.find("[\"");
        if ( keyBegin == std::string::npos ) continue;
        const std::size_t contentBegin = keyBegin + 2U;
        const std::size_t keyEnd       = line.find("\"]", contentBegin);
        if ( keyEnd == std::string::npos ||
             line.find('=', keyEnd + 2U) == std::string::npos ) {
            continue;
        }

        std::string key = line.substr(contentBegin, keyEnd - contentBegin);
        if ( !result.keys.insert(key).second ) {
            result.duplicates.push_back(std::move(key));
        }
    }
    return true;
}

/// @brief 校验一份翻译不缺少另一份的任何键。
/// @param expected 基准翻译键集。
/// @param actual 待校验翻译键集。
/// @param actualName 待校验语言名称。
/// @return 键集完整时返回 true。
bool checkMissingKeys(const TranslationKeySet& expected,
                      const TranslationKeySet& actual,
                      std::string_view         actualName)
{
    bool complete = true;
    for ( const auto& key : expected.keys ) {
        if ( actual.keys.contains(key) ) continue;
        XERROR("DefaultTranslationParityTest: {} missing key '{}'",
               actualName,
               key);
        complete = false;
    }
    return complete;
}
}  // namespace

/// @brief 校验默认中英文翻译文件键集完全一致。
/// @param argc 命令行参数数量。
/// @param argv 中文和英文默认翻译文件路径。
/// @return 两份翻译键集相同且无重复键时返回 0。
int main(int argc, char* argv[])
{
    if ( argc < 3 || !argv[1] || !argv[2] ) {
        XERROR("DefaultTranslationParityTest requires zh_cn and en_us paths");
        return 1;
    }

    TranslationKeySet chinese;
    TranslationKeySet english;
    if ( !loadTranslationKeys(MMM::Config::utf8ToPath(argv[1]), chinese) ||
         !loadTranslationKeys(MMM::Config::utf8ToPath(argv[2]), english) ) {
        XERROR("DefaultTranslationParityTest failed to read translations");
        return 1;
    }

    bool ok = true;
    for ( const auto& key : chinese.duplicates ) {
        XERROR("DefaultTranslationParityTest: zh_cn duplicate key '{}'", key);
        ok = false;
    }
    for ( const auto& key : english.duplicates ) {
        XERROR("DefaultTranslationParityTest: en_us duplicate key '{}'", key);
        ok = false;
    }
    ok &= checkMissingKeys(chinese, english, "en_us");
    ok &= checkMissingKeys(english, chinese, "zh_cn");
    return ok ? 0 : 1;
}
