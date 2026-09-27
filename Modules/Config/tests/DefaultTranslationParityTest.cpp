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
    // 二进制模式避免平台换行转换影响按行扫描的字符位置。
    std::ifstream input(path, std::ios::binary);
    if ( !input ) return false;

    std::string line;
    while ( std::getline(input, line) ) {
        // 默认语言文件使用 ["key"] = value 形式，只扫描这一受控生成格式。
        const std::size_t keyBegin = line.find("[\"");
        if ( keyBegin == std::string::npos ) continue;
        // 跳过起始方括号和引号后再查找配对的结束序列。
        const std::size_t contentBegin = keyBegin + 2U;
        const std::size_t keyEnd       = line.find("\"]", contentBegin);
        if ( keyEnd == std::string::npos ||
             line.find('=', keyEnd + 2U) == std::string::npos ) {
            // 非赋值行或不完整语法不属于翻译键声明。
            continue;
        }

        // 键文本按字节保留，不解析 Lua 转义；默认资源约定键中不使用转义引号。
        std::string key = line.substr(contentBegin, keyEnd - contentBegin);
        if ( !result.keys.insert(key).second ) {
            // set 保留首次声明，duplicates 单独收集后续重复项供日志定位。
            result.duplicates.push_back(std::move(key));
        }
    }
    // 完整读取到 EOF 即成功，语法级一致性由后续键集比较判断。
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
    // 从 expected 单向扫描，调用方交换参数即可验证集合完全相等。
    bool complete = true;
    for ( const auto& key : expected.keys ) {
        // C++20 contains 不创建临时条目，检查不会修改待校验集合。
        if ( actual.keys.contains(key) ) continue;
        XERROR("DefaultTranslationParityTest: {} missing key '{}'",
               actualName,
               key);
        complete = false;
    }
    // 不在首个缺失键处返回，使一次测试输出完整缺失清单。
    return complete;
}
}  // namespace

/// @brief 校验默认中英文翻译文件键集完全一致。
/// @param argc 命令行参数数量。
/// @param argv 中文和英文默认翻译文件路径。
/// @return 两份翻译键集相同且无重复键时返回 0。
int main(int argc, char* argv[])
{
    // 两条路径由 CMake 指向源码资源，不从当前工作目录推断位置。
    if ( argc < 3 || !argv[1] || !argv[2] ) {
        XERROR("DefaultTranslationParityTest requires zh_cn and en_us paths");
        return 1;
    }

    // 两种语言分别保存键集和重复项，避免后一次加载覆盖前一次结果。
    TranslationKeySet chinese;
    TranslationKeySet english;
    if ( !loadTranslationKeys(MMM::Config::utf8ToPath(argv[1]), chinese) ||
         !loadTranslationKeys(MMM::Config::utf8ToPath(argv[2]), english) ) {
        // 任一文件不可读都无法证明一致性，立即返回基础设施失败。
        XERROR("DefaultTranslationParityTest failed to read translations");
        return 1;
    }

    // 重复项先分别报告，再执行双向缺失检查收集所有资源问题。
    bool ok = true;
    for ( const auto& key : chinese.duplicates ) {
        // 重复键在 Lua 中会静默覆盖，因此必须作为资源错误显式报告。
        XERROR("DefaultTranslationParityTest: zh_cn duplicate key '{}'", key);
        ok = false;
    }
    for ( const auto& key : english.duplicates ) {
        // 两份语言分别报告名称，维护者可直接定位对应默认资源。
        XERROR("DefaultTranslationParityTest: en_us duplicate key '{}'", key);
        ok = false;
    }
    // 英文不得缺少中文键，中文也不得缺少英文键。
    ok &= checkMissingKeys(chinese, english, "en_us");
    ok &= checkMissingKeys(english, chinese, "zh_cn");
    // 所有重复和缺失项检查完毕后统一返回测试结果。
    return ok ? 0 : 1;
}
