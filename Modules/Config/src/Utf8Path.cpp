#include "config/Utf8Path.h"

// SDK 仅在此翻译单元可见，路径消费者无需承受平台声明与兼容宏。
// 不依赖日志初始化，错误回退仍直接通过返回值表达。
#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace MMM::Config
{
/// @brief 在私有实现中完成 UTF-8 到平台原生路径的转换。
/// @param s 调用方提供的 UTF-8 路径文本。
/// @return 平台路径；Windows 长度查询失败时保持原有空路径回退。
/// @details 保留原转换参数及空字符终止语义，不改为本地代码页解析。
/// 此库不使用日志或配置单例，保证日志初始化也能调用且不产生循环依赖。
std::filesystem::path utf8ToPath(const std::string& s)
{
#ifdef _WIN32
    // Windows 的 MSVC STL 与 libc++ 都可直接接管宽字符路径。
    // 两次转换沿用同一组参数，避免下沉实现时改变非法输入的既有处理。
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if ( len <= 0 ) {
        // 不构造回退窄字符路径，防止系统代码页改变文件定位结果。
        return {};
    }
    // 长度包含终止符；逻辑文本不计入终止符，转换仍写入其预留位置。
    std::wstring wstr(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, wstr.data(), len);
    return std::filesystem::path(wstr);
#else
    // POSIX 路径继续保留输入字节，不新增区域设置转换或字符过滤。
    return std::filesystem::path(s);
#endif
}
}  // namespace MMM::Config
