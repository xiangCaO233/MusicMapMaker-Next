#include "font/SystemFontResolver.h"

#include "log/colorful-log.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits>
#include <memory>
#include <system_error>
#include <type_traits>
#include <vector>

/// @file
/// @brief 验证 Windows 启动字体的系统解析、文件定位和集合索引确实可用。
/// @details 必须链接生产 Font 目标，才能覆盖发布型 ThinLTO 的 COM 调用边界。
/// 不以项目内置字体替代系统字体，避免资源尚未同步时的缺字问题被掩盖。
/// 空列表是本回归的核心失败信号，不能通过跳过候选而把它视为成功。
/// 所有输入均来自本机字体服务，不读取用户项目，也不生成皮肤配置。

namespace
{

/// @brief 释放本测试独占的 FreeType 字体库。
/// @details 与各循环中的 face 分层持有，避免提前关闭仍被引用的库状态。
struct LibraryReleaser {
    /// @brief 在所有字体 face 析构之后释放字体库。
    /// @param library 当前测试持有的字体库。
    void operator()(std::remove_pointer_t<FT_Library>* library) const noexcept
    {
        // 允许初始化失败时持有空句柄，调用端只处理错误返回值。
        if ( library ) FT_Done_FreeType(library);
    }
};

/// @brief 释放当前候选字体的 FreeType face。
/// @details 仅管理 face 句柄，字体字节始终由当前循环的 vector 持有。
struct FaceReleaser {
    /// @brief 在借用的字体字节缓冲区销毁之前释放 face。
    /// @param face 当前循环持有的字体 face。
    void operator()(std::remove_pointer_t<FT_Face>* face) const noexcept
    {
        // face 只借用字体数据，不能让释放顺序反向依赖字节缓冲区。
        if ( face ) FT_Done_Face(face);
    }
};

}  // namespace

/// @brief 验证系统字体可以实际加载，并提供启动界面必要的拉丁字形。
/// @return 全部候选定位有效且基本字形完整时返回零。
/// @note 中文覆盖随系统安装的语言字体变化；单独记录结果，不强制安装语言包。
/// @note 测试不启动 ImGui 或 Vulkan，失败可直接定位到原生字体桥接层。
int main()
{
    // 正常 Windows 桌面至少存在系统消息字体；空列表会退回无中文的内置字体。
    const auto fonts = MMM::Font::resolvePreferredSystemFonts();
    if ( fonts.empty() ) {
        XERROR("No startup system fonts resolved from Windows");
        return 1;
    }

    FT_Library rawLibrary = nullptr;
    if ( FT_Init_FreeType(&rawLibrary) != 0 ) return 2;
    // 独立字体库防止测试套件中其他用例的字体缓存掩盖错误索引或无效文件。
    // 字体库覆盖整轮探测，所有 face 都在循环结束时先销毁。
    std::unique_ptr<std::remove_pointer_t<FT_Library>, LibraryReleaser> library(
        rawLibrary);
    // 英文、数字、百分号覆盖标题与进度；中文用于诊断系统回退是否可用。
    constexpr std::array<FT_ULong, 5> codepoints{
        'M', '6', '%', 0x4E2D, 0x6587
    };
    std::array<bool, codepoints.size()> covered{};
    // 覆盖表初始全假，只接受真实字体 charmap 返回的非零索引。

    for ( const auto& font : fonts ) {
        std::error_code error;
        // 相对路径或错误的 TTC 索引不能传给启动窗口，也不能悄悄跳过。
        if ( !font.m_filePath.is_absolute() || font.m_faceIndex < 0 ||
             !std::filesystem::is_regular_file(font.m_filePath, error) ||
             error ) {
            XERROR("Invalid resolved startup font: {}", font.m_familyName);
            return 3;
        }

        // 通过原生 filesystem 路径读取，避免测试自身依赖 Windows 窄路径代码页。
        std::ifstream file(font.m_filePath, std::ios::binary | std::ios::ate);
        const auto    length = file.tellg();
        // FreeType 长度采用 FT_Long；先限制上界，防止文件长度窄化后变为负值。
        if ( length <= 0 || length > std::numeric_limits<FT_Long>::max() ) {
            XERROR("Unreadable startup font: {}", font.m_familyName);
            return 4;
        }
        std::vector<FT_Byte> bytes(static_cast<std::size_t>(length));
        // 从文件开头读取完整内容，不能把打开时的末尾游标当作字体数据起点。
        file.seekg(0);
        if ( !file.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())) ) {
            // 字体在读取中被替换或截断时，不把不完整字节交给解析器继续探测。
            return 5;
        }

        // 必须使用系统返回的 face index，不能总打开集合中的第一个字体。
        FT_Face rawFace = nullptr;
        if ( FT_New_Memory_Face(library.get(),
                                bytes.data(),
                                static_cast<FT_Long>(bytes.size()),
                                font.m_faceIndex,
                                &rawFace) != 0 ) {
            XERROR("Invalid startup font face: {} ({})",
                   font.m_familyName,
                   font.m_faceIndex);
            return 6;
        }
        std::unique_ptr<std::remove_pointer_t<FT_Face>, FaceReleaser> face(
            rawFace);
        // face 在 bytes 之后声明，离开循环时会先释放对内存数据的借用。
        // Unicode charmap 与 UI 的 UTF-8
        // 输入对应；失败时不能用其他编码猜测字形。
        if ( FT_Select_Charmap(face.get(), FT_ENCODING_UNICODE) != 0 ) return 7;
        for ( std::size_t index = 0; index < codepoints.size(); ++index ) {
            // 多个系统字体按覆盖并集判断，不要求拉丁字体自身携带全部 CJK 字形。
            // 索引零表示 .notdef；它不是可接受的替代字符或真实中文字形。
            covered[index] =
                covered[index] ||
                FT_Get_Char_Index(face.get(), codepoints[index]) != 0;
        }
    }

    // 缺少任一基本字符都会破坏启动提示；中文缺失仅说明本机未提供对应字体。
    if ( !covered[0] || !covered[1] || !covered[2] ) return 8;
    // 日志明确区分系统查询成功与中文覆盖成功，便于对照实际启动窗口排查。
    XINFO("Startup system fonts verified: {}, Chinese coverage: {}",
          fonts.size(),
          covered[3] && covered[4]);
    return 0;
}
