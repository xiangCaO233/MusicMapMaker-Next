#include "font/SystemFontResolver.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <dwrite.h>
#    include <dwrite_2.h>
#    include <windows.h>
#elif defined(__APPLE__)
#    include <CoreFoundation/CoreFoundation.h>
#    include <CoreText/CoreText.h>
#    include <ft2build.h>
#    include FT_FREETYPE_H
#else
#    include <fontconfig/fontconfig.h>
#endif

/// @file
/// @brief 将各桌面平台的首选界面字体解析为可由 ImGui/FreeType 加载的文件描述。
///
/// 平台 API 返回的字体对象不能直接跨越图形模块边界，因此本文件把结果收敛为
/// 绝对文件路径、集合 face index 和展示用家族名。解析仅发生在字体资源准备阶段，
/// 不属于渲染热路径；失败时返回较短列表并由上层继续使用内置字体回退。

namespace MMM::Font
{

namespace
{

/// @brief 将有效且尚未出现的字体追加到有序结果中。
/// @param fonts 已解析的字体列表。
/// @param font 待追加的字体。
/// @details 字体优先级由调用顺序决定；去重键由绝对路径和集合索引共同组成，
///          同一 TTC/OTC 文件中的不同 face 仍被视为不同字体。
void appendUniqueFont(std::vector<SystemFontFace>&         fonts,
                      const std::optional<SystemFontFace>& font)
{
    // 相对路径依赖进程工作目录，不适合作为可持久使用的字体来源。
    if ( !font || font->m_filePath.empty() ||
         !font->m_filePath.is_absolute() ) {
        return;
    }

    // 保留首次出现的位置，使首选 UI 字体始终排在语言回退之前。
    const auto existing =
        std::find_if(fonts.begin(), fonts.end(), [&](const auto& candidate) {
            return candidate.m_faceIndex == font->m_faceIndex &&
                   candidate.m_filePath == font->m_filePath;
        });
    // 结果对象只含值语义元数据，不延长任何平台字体句柄的生命周期。
    if ( existing == fonts.end() ) fonts.push_back(*font);
}

#ifdef _WIN32

/// @brief 释放 DirectWrite COM 接口。
/// @tparam T 具有 `Release` 成员的 DirectWrite 接口类型。
template<typename T> struct ComReleaser {
    /// @brief 释放一个由调用方持有引用的 COM 接口。
    /// @param value COM 接口。
    void operator()(T* value) const noexcept
    {
        // 查询失败时 unique_ptr 可能持有空值，删除器必须允许幂等清理。
        if ( value ) value->Release();
    }
};

/// @brief 独占持有一次 DirectWrite 查询取得的 COM 接口引用。
template<typename T> using UniqueCom = std::unique_ptr<T, ComReleaser<T>>;

/// @brief 用于触发 DirectWrite 系统中文字体回退的文本。
/// @note 内容保持最小，只要求系统选择能够覆盖常用简体中文的字体。
constexpr std::wstring_view CHINESE_FALLBACK_TEXT = L"中文";

/// @brief DirectWrite 中文回退匹配使用的区域名称。
/// @note 区域只参与字体匹配，不改变应用自身语言设置。
constexpr const wchar_t* CHINESE_FALLBACK_LOCALE = L"zh-CN";

/// @brief 为同步字体回退查询提供固定中文文本。
/// @details `MapCharacters` 只在当前栈帧同步使用此对象，因此引用计数方法不拥有
///          自身生命周期；文本与区域名称均指向静态存储。
class ChineseTextAnalysisSource final : public IDWriteTextAnalysisSource
{
public:
    /// @brief 查询该对象支持的 COM 接口。
    /// @param interfaceId 请求的接口标识。
    /// @param object 接收接口观察指针。
    /// @return 支持接口时返回 S_OK。
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId,
                                             void** object) override
    {
        // COM 约定要求先验证输出槽，再在不支持接口时保持其为空。
        if ( !object ) return E_POINTER;
        *object = nullptr;
        if ( IsEqualIID(interfaceId, __uuidof(IUnknown)) ||
             IsEqualIID(interfaceId, __uuidof(IDWriteTextAnalysisSource)) ) {
            // 对象只暴露 IUnknown 和字体回退所需的分析源接口。
            *object = static_cast<IDWriteTextAnalysisSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    /// @brief 保持同步 MapCharacters 调用期间的借用生命周期。
    /// @return 固定返回 1；对象生命周期由当前栈帧持有。
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }

    /// @brief 结束同步 MapCharacters 对该对象的借用。
    /// @return 固定返回 1；对象生命周期由当前栈帧持有。
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    /// @brief 返回指定位置开始的中文探测文本。
    /// @param position UTF-16 文本位置。
    /// @param text 接收文本观察指针。
    /// @param textLength 接收剩余 UTF-16 长度。
    /// @return 参数有效时返回 S_OK。
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32        position,
                                                const WCHAR** text,
                                                UINT32* textLength) override
    {
        if ( !text || !textLength ) return E_POINTER;
        // 允许 position 等于末尾，此时以空区间回应 DirectWrite。
        if ( position > CHINESE_FALLBACK_TEXT.size() ) return E_INVALIDARG;

        // 返回值借用静态探测文本，不要求调用方释放。
        const auto remainingLength =
            static_cast<UINT32>(CHINESE_FALLBACK_TEXT.size() - position);
        *text = remainingLength > 0 ? CHINESE_FALLBACK_TEXT.data() + position
                                    : nullptr;
        *textLength = remainingLength;
        return S_OK;
    }

    /// @brief 返回指定位置之前的中文探测文本。
    /// @param position UTF-16 文本位置。
    /// @param text 接收文本观察指针。
    /// @param textLength 接收前置 UTF-16 长度。
    /// @return 参数有效时返回 S_OK。
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32        position,
                                                    const WCHAR** text,
                                                    UINT32* textLength) override
    {
        if ( !text || !textLength ) return E_POINTER;
        // DirectWrite 可能查询起点零；该位置之前没有可返回文本。
        if ( position > CHINESE_FALLBACK_TEXT.size() ) return E_INVALIDARG;

        *text       = position > 0 ? CHINESE_FALLBACK_TEXT.data() : nullptr;
        *textLength = position;
        return S_OK;
    }

    /// @brief 返回中文探测文本的段落阅读方向。
    /// @return 始终为从左到右。
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE
    GetParagraphReadingDirection() override
    {
        // 简体中文探测文本使用水平从左到右方向，不影响实际应用文本布局。
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    /// @brief 返回中文探测文本使用的区域名称。
    /// @param position UTF-16 文本位置。
    /// @param textLength 接收区域名称适用的剩余文本长度。
    /// @param localeName 接收静态区域名称观察指针。
    /// @return 参数有效时返回 S_OK。
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* textLength,
                                            const WCHAR** localeName) override
    {
        if ( !textLength || !localeName ) return E_POINTER;
        // locale 覆盖从查询位置到探测文本末尾的完整区间。
        if ( position > CHINESE_FALLBACK_TEXT.size() ) return E_INVALIDARG;

        *textLength =
            static_cast<UINT32>(CHINESE_FALLBACK_TEXT.size() - position);
        *localeName = CHINESE_FALLBACK_LOCALE;
        return S_OK;
    }

    /// @brief 表明中文探测文本不使用数字替换。
    /// @param position UTF-16 文本位置。
    /// @param textLength 接收设置适用的剩余文本长度。
    /// @param substitution 接收空数字替换对象。
    /// @return 参数有效时返回 S_OK。
    HRESULT STDMETHODCALLTYPE
    GetNumberSubstitution(UINT32 position, UINT32* textLength,
                          IDWriteNumberSubstitution** substitution) override
    {
        if ( !textLength || !substitution ) return E_POINTER;
        // 探测文本不含数字，明确返回空替换对象即可满足分析源契约。
        if ( position > CHINESE_FALLBACK_TEXT.size() ) return E_INVALIDARG;

        *textLength =
            static_cast<UINT32>(CHINESE_FALLBACK_TEXT.size() - position);
        *substitution = nullptr;
        return S_OK;
    }
};

/// @brief 将 Win32 宽字符文本转换为 UTF-8。
/// @param value Win32 宽字符文本。
/// @return 转换后的 UTF-8 文本；转换失败时返回空字符串。
/// @details 先查询精确输出长度，再写入已经定长的字符串；启用
///          `WC_ERR_INVALID_CHARS` 后不把非法 UTF-16 静默替换为占位符。
std::string wideToUtf8(std::wstring_view value)
{
    // 空家族名是允许的元数据缺失状态，无需调用 Win32 转换 API。
    if ( value.empty() ) return {};

    // DirectWrite 和 LOGFONT 字符串长度受 int 参数限制，来源均为系统短字符串。
    const int inputSize = static_cast<int>(value.size());
    // 第一次调用只计算 UTF-8 所需字节数，不包含空终止符。
    const int outputSize = WideCharToMultiByte(CP_UTF8,
                                               WC_ERR_INVALID_CHARS,
                                               value.data(),
                                               inputSize,
                                               nullptr,
                                               0,
                                               nullptr,
                                               nullptr);
    if ( outputSize <= 0 ) return {};

    // std::string 按精确长度分配，第二次调用直接填充其连续存储。
    std::string output(static_cast<std::size_t>(outputSize), '\0');
    if ( WideCharToMultiByte(CP_UTF8,
                             WC_ERR_INVALID_CHARS,
                             value.data(),
                             inputSize,
                             output.data(),
                             outputSize,
                             nullptr,
                             nullptr) <= 0 ) {
        return {};
    }
    // 输出按长度持有，不依赖 Win32 API 额外写入空终止符。
    return output;
}

/// @brief 从 DirectWrite 字体对象解析本地文件和集合索引。
/// @param font DirectWrite 字体对象。
/// @param familyName 已由调用方取得的 UTF-8 字体家族名称。
/// @return 可直接加载的本地字体；多文件字体或非本地字体返回空。
/// @details DirectWrite Font、FontFace、FontFile 和 Loader 逐层提供更底层信息；
///          每一步都用 RAII 接管新获得的 COM
///          引用。当前加载链只接受单文件本地字体， 因为 ImGui/FreeType
///          接口无法表达远程字体或多文件组合字体。
std::optional<SystemFontFace> resolveDirectWriteFont(IDWriteFont& font,
                                                     std::string  familyName)
{
    // FontFace 固化字体的样式与集合索引，是取得底层文件列表的入口。
    IDWriteFontFace*           rawFace    = nullptr;
    const HRESULT              faceResult = font.CreateFontFace(&rawFace);
    UniqueCom<IDWriteFontFace> face(rawFace);
    if ( FAILED(faceResult) || !face ) return std::nullopt;

    UINT32 fileCount = 0;
    // 第一次查询只取得文件数量；组合字体不能安全折叠为单一路径。
    HRESULT filesResult = face->GetFiles(&fileCount, nullptr);
    if ( FAILED(filesResult) || fileCount != 1 ) return std::nullopt;

    IDWriteFontFile* rawFile = nullptr;
    // 数量确认后再次调用，取得调用方持有引用的唯一 FontFile。
    filesResult = face->GetFiles(&fileCount, &rawFile);
    UniqueCom<IDWriteFontFile> file(rawFile);
    if ( FAILED(filesResult) || !file ) return std::nullopt;

    const void* referenceKey     = nullptr;
    UINT32      referenceKeySize = 0;
    // reference key 由 loader 解释，其内存仍归 FontFile 管理。
    if ( FAILED(file->GetReferenceKey(&referenceKey, &referenceKeySize)) ) {
        return std::nullopt;
    }

    IDWriteFontFileLoader* rawLoader = nullptr;
    // 通用 loader 可能对应远程或自定义字体，下一步必须验证本地 loader 接口。
    const HRESULT                    loaderResult = file->GetLoader(&rawLoader);
    UniqueCom<IDWriteFontFileLoader> loader(rawLoader);
    if ( FAILED(loaderResult) || !loader ) return std::nullopt;

    IDWriteLocalFontFileLoader* rawLocalLoader = nullptr;
    // 只有本地 loader 才能把 reference key 解析为稳定文件系统路径。
    const HRESULT localLoaderResult =
        loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                               reinterpret_cast<void**>(&rawLocalLoader));
    UniqueCom<IDWriteLocalFontFileLoader> localLoader(rawLocalLoader);
    if ( FAILED(localLoaderResult) || !localLoader ) return std::nullopt;

    UINT32 pathLength = 0;
    // 先取得不含终止符的字符数量，以便准确分配宽字符缓冲区。
    if ( FAILED(localLoader->GetFilePathLengthFromKey(
             referenceKey, referenceKeySize, &pathLength)) ) {
        return std::nullopt;
    }

    // 额外一个 wchar_t 供 DirectWrite 写入终止符。
    std::vector<wchar_t> pathBuffer(static_cast<std::size_t>(pathLength) + 1);
    if ( FAILED(localLoader->GetFilePathFromKey(referenceKey,
                                                referenceKeySize,
                                                pathBuffer.data(),
                                                pathLength + 1)) ) {
        return std::nullopt;
    }

    // filesystem 直接接收宽路径，避免在 Windows 上发生本地代码页转换。
    return SystemFontFace{ std::filesystem::path(pathBuffer.data()),
                           static_cast<int>(face->GetIndex()),
                           std::move(familyName) };
}

/// @brief 通过 DirectWrite 系统回退解析中文字体。
/// @param factory 基础 DirectWrite 工厂。
/// @param interop DirectWrite GDI 互操作对象。
/// @param baseFont 系统首选消息字体描述。
/// @return 可覆盖中文探测文本的系统字体；API 不可用或映射失败时返回空。
/// @details 系统回退 API 从 DirectWrite 2 开始提供；旧系统或接口查询失败时，
///          调用方仍保留基础消息字体。匹配必须覆盖完整探测字符串且缩放有效。
std::optional<SystemFontFace> resolveWindowsChineseFallback(
    IDWriteFactory& factory, IDWriteGdiInterop& interop,
    const LOGFONTW& baseFont)
{
    // 从基础工厂查询可选的新接口，不把 DirectWrite 2 作为硬性运行条件。
    IDWriteFactory2* rawFactory2    = nullptr;
    const HRESULT    factory2Result = factory.QueryInterface(
        __uuidof(IDWriteFactory2), reinterpret_cast<void**>(&rawFactory2));
    UniqueCom<IDWriteFactory2> factory2(rawFactory2);
    if ( FAILED(factory2Result) || !factory2 ) return std::nullopt;

    IDWriteFontFallback* rawFallback = nullptr;
    // 系统 fallback 包含用户语言和字体链接规则，比硬编码字体名称可靠。
    const HRESULT fallbackResult =
        factory2->GetSystemFontFallback(&rawFallback);
    UniqueCom<IDWriteFontFallback> fallback(rawFallback);
    if ( FAILED(fallbackResult) || !fallback ) return std::nullopt;

    // 保留系统消息字体的字重和斜体倾向，使回退字形视觉上尽量一致。
    const auto baseWeight = static_cast<DWRITE_FONT_WEIGHT>(
        baseFont.lfWeight > 0 ? std::clamp<LONG>(baseFont.lfWeight, 1, 999)
                              : static_cast<LONG>(DWRITE_FONT_WEIGHT_NORMAL));
    const auto baseStyle = baseFont.lfItalic != 0 ? DWRITE_FONT_STYLE_ITALIC
                                                  : DWRITE_FONT_STYLE_NORMAL;

    // 分析源只描述固定中文探测文本；映射结果在本函数内同步消费。
    ChineseTextAnalysisSource textSource;
    UINT32                    mappedLength  = 0;
    IDWriteFont*              rawMappedFont = nullptr;
    FLOAT                     mappedScale   = 1.0F;
    const HRESULT             mapResult     = fallback->MapCharacters(
        &textSource,
        0,
        static_cast<UINT32>(CHINESE_FALLBACK_TEXT.size()),
        nullptr,
        baseFont.lfFaceName,
        baseWeight,
        baseStyle,
        DWRITE_FONT_STRETCH_NORMAL,
        &mappedLength,
        &rawMappedFont,
        &mappedScale);
    UniqueCom<IDWriteFont> mappedFont(rawMappedFont);
    // 部分映射不足以保证两个中文字符来自同一可用字体，因此要求完整长度。
    if ( FAILED(mapResult) || !mappedFont ||
         mappedLength != CHINESE_FALLBACK_TEXT.size() || mappedScale <= 0.0F ) {
        return std::nullopt;
    }

    LOGFONTW    mappedLogFont{};
    BOOL        isSystemFont = FALSE;
    std::string familyName;
    // 家族名仅用于展示；转换失败不影响已解析字体文件的可用性。
    if ( SUCCEEDED(interop.ConvertFontToLOGFONT(
             mappedFont.get(), &mappedLogFont, &isSystemFont)) &&
         isSystemFont ) {
        familyName = wideToUtf8(mappedLogFont.lfFaceName);
    }

    // 最终仍通过统一本地文件解析，拒绝 DirectWrite 虚拟或远程字体。
    return resolveDirectWriteFont(*mappedFont, std::move(familyName));
}

/// @brief 解析 Win32 当前消息字体及其中文系统回退字体。
/// @param fonts 接收按优先级排列的字体列表。
/// @details 消息字体代表当前桌面主题的 UI 字体；中文回退随后追加，重复文件会由
///          `appendUniqueFont` 消除。任一平台 API 失败都保留已取得的较短结果。
void appendWindowsPreferredFonts(std::vector<SystemFontFace>& fonts)
{
    // NONCLIENTMETRICS 必须声明结构大小，系统才会按当前 ABI 填充字段。
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if ( !SystemParametersInfoW(
             SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) ) {
        return;
    }

    IDWriteFactory* rawFactory = nullptr;
    // 共享工厂适合启动期只读查询，且避免为一次解析创建隔离缓存。
    const HRESULT factoryResult =
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&rawFactory));
    UniqueCom<IDWriteFactory> factory(rawFactory);
    if ( FAILED(factoryResult) || !factory ) return;

    IDWriteGdiInterop* rawInterop = nullptr;
    // GDI 互操作把系统 LOGFONT 映射到 DirectWrite 字体集合。
    const HRESULT interopResult = factory->GetGdiInterop(&rawInterop);
    UniqueCom<IDWriteGdiInterop> interop(rawInterop);
    if ( FAILED(interopResult) || !interop ) return;

    IDWriteFont* rawFont = nullptr;
    // 基础消息字体失败时仍继续尝试回退没有意义，因此工厂和 interop 已提前验证。
    const HRESULT fontResult =
        interop->CreateFontFromLOGFONT(&metrics.lfMessageFont, &rawFont);
    UniqueCom<IDWriteFont> font(rawFont);
    if ( SUCCEEDED(fontResult) && font ) {
        // 首选字体先入列表，维持拉丁字符优先使用桌面原生外观。
        appendUniqueFont(
            fonts,
            resolveDirectWriteFont(
                *font, wideToUtf8(metrics.lfMessageFont.lfFaceName)));
    }

    // 中文回退排在首选字体之后，只补足首选字体缺失的字符覆盖。
    appendUniqueFont(fonts,
                     resolveWindowsChineseFallback(
                         *factory, *interop, metrics.lfMessageFont));
}

#elif defined(__APPLE__)

/// @brief 释放 Core Foundation 所有权对象。
/// @details 仅接管遵循 Create/Copy 规则返回的对象，借用引用不得交给此删除器。
struct CoreFoundationReleaser {
    /// @brief 释放一个遵循 Create/Copy 规则的 Core Foundation 对象。
    /// @param value Core Foundation 对象。
    template<typename T> void operator()(T* value) const noexcept
    {
        // Core Foundation 的空引用无需释放，便于统一包装失败结果。
        if ( value ) CFRelease(value);
    }
};

/// @brief 独占持有遵循 Core Foundation Create/Copy 所有权规则的对象。
template<typename T>
using UniqueCoreFoundation =
    std::unique_ptr<std::remove_pointer_t<T>, CoreFoundationReleaser>;

/// @brief 释放 FreeType 字体库。
struct FreeTypeLibraryReleaser {
    /// @brief 释放 FreeType 字体库。
    /// @param value FreeType 字体库。
    void operator()(std::remove_pointer_t<FT_Library>* value) const noexcept
    {
        // 与后续 Face 局部变量的逆序析构共同保证 FreeType 销毁顺序。
        if ( value ) FT_Done_FreeType(value);
    }
};

/// @brief 释放 FreeType 字体 face。
struct FreeTypeFaceReleaser {
    /// @brief 释放 FreeType 字体 face。
    /// @param value FreeType 字体 face。
    void operator()(std::remove_pointer_t<FT_Face>* value) const noexcept
    {
        // 探测单个集合 face 后立即释放，避免扫描字体集合时累积句柄。
        if ( value ) FT_Done_Face(value);
    }
};

/// @brief 独占持有 macOS 字体集合探测使用的 FreeType Library。
using UniqueFreeTypeLibrary =
    std::unique_ptr<std::remove_pointer_t<FT_Library>, FreeTypeLibraryReleaser>;
/// @brief 独占持有探测中的单个 FreeType Face。
using UniqueFreeTypeFace =
    std::unique_ptr<std::remove_pointer_t<FT_Face>, FreeTypeFaceReleaser>;

/// @brief 将 Core Foundation 字符串转换为 UTF-8 文本。
/// @param value Core Foundation 字符串。
/// @return 转换后的文本；转换失败时返回空。
/// @details 目标缓冲区按 Core Foundation 给出的最坏编码长度分配，返回字符串
///          截止于写入的空终止符，不把预留空间带入结果。
std::optional<std::string> utf8FromCoreFoundationString(CFStringRef value)
{
    // 空引用代表上游属性不存在，与转换失败统一返回空 optional。
    if ( !value ) return std::nullopt;

    // 最大长度按 UTF-8 最坏情况计算，并额外保留一个终止符位置。
    const CFIndex maximumSize =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(value),
                                          kCFStringEncodingUTF8) +
        1;
    if ( maximumSize <= 1 ) return std::nullopt;

    // Core Foundation 写入调用需要可变连续缓冲区。
    std::vector<char> buffer(static_cast<std::size_t>(maximumSize));
    if ( !CFStringGetCString(
             value, buffer.data(), maximumSize, kCFStringEncodingUTF8) ) {
        return std::nullopt;
    }
    // CFStringGetCString 保证成功时写入终止符，因此可安全构造 C 字符串。
    return std::string(buffer.data());
}

/// @brief 按 PostScript 名称解析字体在 TTC/OTC 文件中的 face index。
/// @param path CoreText 返回的字体文件路径。
/// @param postScriptName CoreText 返回的 PostScript 名称。
/// @return 匹配的集合索引；多 face 文件无法确认时返回空。
/// @details 先以 `face_index=-1` 查询集合规模，再逐 face 比较 PostScript 名称。
///          该过程只在启动期字体解析中执行，避免在绘制阶段扫描 TTC/OTC 文件。
std::optional<int> resolveFreeTypeFaceIndex(const std::filesystem::path& path,
                                            std::string_view postScriptName)
{
    FT_Library rawLibrary = nullptr;
    // 独立 FreeType Library 将探测生命周期限制在当前解析调用中。
    if ( FT_Init_FreeType(&rawLibrary) != 0 ) return std::nullopt;
    UniqueFreeTypeLibrary library(rawLibrary);

    // CoreText 已返回 POSIX 路径；macOS 文件系统路径可由 FreeType 直接打开。
    const std::string filePath = path.string();
    FT_Face           rawProbe = nullptr;
    // 负索引只打开集合元数据，不选择具体可渲染 face。
    if ( FT_New_Face(library.get(), filePath.c_str(), -1, &rawProbe) != 0 ) {
        return std::nullopt;
    }
    UniqueFreeTypeFace probe(rawProbe);

    const FT_Long faceCount = probe->num_faces;
    // 返回类型使用 int，先拒绝无法无损表达的异常集合规模。
    if ( faceCount <= 0 ||
         faceCount > static_cast<FT_Long>(std::numeric_limits<int>::max()) ) {
        return std::nullopt;
    }

    // PostScript 名称能区分同一集合中家族和样式相近的多个 face。
    for ( FT_Long faceIndex = 0; faceIndex < faceCount; ++faceIndex ) {
        FT_Face rawFace = nullptr;
        // 单个损坏 face 不应阻止继续寻找集合中的其他有效项。
        if ( FT_New_Face(
                 library.get(), filePath.c_str(), faceIndex, &rawFace) != 0 ) {
            continue;
        }
        UniqueFreeTypeFace face(rawFace);

        const char* candidateName = FT_Get_Postscript_Name(face.get());
        // FreeType 返回的名称由当前 Face 持有，只在 Face 生命周期内比较。
        if ( candidateName && postScriptName == candidateName ) {
            return static_cast<int>(faceIndex);
        }
    }

    // 单字体文件不存在集合歧义，缺少 PostScript 名称时仍可安全使用索引 0。
    if ( faceCount == 1 ) return 0;
    // 多 face 文件缺少匹配名称时不能猜测，否则可能返回错误字重或语言字体。
    return std::nullopt;
}

/// @brief 从 CoreText 字体解析本地文件路径。
/// @param font CoreText 字体。
/// @return 字体文件；CoreText 未提供本地 URL 时返回空。
/// @details CoreText 字体可能来自受保护或非文件来源；只有同时取得文件 URL、
///          PostScript 名称和无歧义集合索引时才导出给 FreeType/ImGui。
std::optional<SystemFontFace> resolveCoreTextFont(CTFontRef font)
{
    // CoreText 回退查询允许返回空字体，先在属性访问前拦截。
    if ( !font ) return std::nullopt;

    // Copy 返回值由 RAII 持有，并验证动态类型确实是文件 URL。
    UniqueCoreFoundation<CFTypeRef> urlAttribute(
        CTFontCopyAttribute(font, kCTFontURLAttribute));
    if ( !urlAttribute ||
         CFGetTypeID(urlAttribute.get()) != CFURLGetTypeID() ) {
        return std::nullopt;
    }

    // 使用 POSIX 路径表示供 std::filesystem 和 FreeType 后续消费。
    UniqueCoreFoundation<CFStringRef> pathString(CFURLCopyFileSystemPath(
        static_cast<CFURLRef>(urlAttribute.get()), kCFURLPOSIXPathStyle));
    const auto path = utf8FromCoreFoundationString(pathString.get());
    if ( !path ) return std::nullopt;

    // 文件集合索引必须依据唯一 PostScript 名称解析，不能假定总是 face 0。
    UniqueCoreFoundation<CFStringRef> postScriptName(
        CTFontCopyPostScriptName(font));
    const auto postScriptNameUtf8 =
        utf8FromCoreFoundationString(postScriptName.get());
    if ( !postScriptNameUtf8 ) return std::nullopt;

    const std::filesystem::path filePath(*path);
    // 无法定位集合 face 时拒绝返回，避免加载同文件中的错误字体样式。
    const auto faceIndex =
        resolveFreeTypeFaceIndex(filePath, *postScriptNameUtf8);
    if ( !faceIndex ) return std::nullopt;

    // 家族名只用于展示和诊断，缺失时仍保留已经验证的路径与索引。
    UniqueCoreFoundation<CFTypeRef> familyAttribute(
        CTFontCopyAttribute(font, kCTFontFamilyNameAttribute));
    std::string familyName;
    if ( familyAttribute &&
         CFGetTypeID(familyAttribute.get()) == CFStringGetTypeID() ) {
        familyName = utf8FromCoreFoundationString(
                         static_cast<CFStringRef>(familyAttribute.get()))
                         .value_or(std::string{});
    }

    // 返回值脱离全部 Core Foundation 句柄后仍保持自包含。
    return SystemFontFace{ filePath, *faceIndex, std::move(familyName) };
}

/// @brief 解析可由 FreeType 读取的 macOS 中文界面字体。
/// @param preferredFont CoreText 当前首选界面字体。
/// @return 可用于 ImGui 合并的中文字体；系统字体均不可读时返回空。
/// @details 优先让 CoreText 根据当前 UI 字体选择覆盖探测文本的同风格字体；
///          若结果不可由 FreeType 读取，再尝试系统自带的明确可读字体。
std::optional<SystemFontFace> resolveMacOSChineseFallback(
    CTFontRef preferredFont)
{
    if ( preferredFont ) {
        // 固定中文探针只用于询问覆盖字体，不作为应用实际渲染文本。
        const CFStringRef               chineseProbe = CFSTR("中文");
        UniqueCoreFoundation<CTFontRef> coreTextFallback(CTFontCreateForString(
            preferredFont,
            chineseProbe,
            CFRangeMake(0, CFStringGetLength(chineseProbe))));
        if ( auto resolved = resolveCoreTextFont(coreTextFallback.get()) ) {
            // 只有成功解析到本地文件和正确 face 后才接受 CoreText 回退。
            return resolved;
        }
    }

    // 新版 macOS 可能把 CoreText 中文回退解析到受保护的 PingFangUI.ttc；
    // FreeType 无法打开其中的具体 face，因此退回到系统自带的可读简体中文字体。
    UniqueCoreFoundation<CTFontRef> readableFallback(
        CTFontCreateWithName(CFSTR("Hiragino Sans GB"), 0.0, nullptr));
    // 明确字体同样必须经过本地路径和集合索引验证。
    return resolveCoreTextFont(readableFallback.get());
}

#else

/// @brief 释放 Fontconfig 配置。
struct FontConfigReleaser {
    /// @brief 释放 Fontconfig 配置。
    /// @param value Fontconfig 配置。
    void operator()(FcConfig* value) const noexcept
    {
        // FcInitLoadConfigAndFonts 返回的独立配置由本解析调用负责销毁。
        if ( value ) FcConfigDestroy(value);
    }
};

/// @brief 释放 Fontconfig 匹配模式。
struct FontPatternReleaser {
    /// @brief 释放 Fontconfig 匹配模式。
    /// @param value Fontconfig 匹配模式。
    void operator()(FcPattern* value) const noexcept
    {
        // 查询模式和匹配结果都使用相同销毁函数，允许空结果。
        if ( value ) FcPatternDestroy(value);
    }
};

/// @brief 释放 Fontconfig 字符集。
struct FontCharSetReleaser {
    /// @brief 释放 Fontconfig 字符集。
    /// @param value Fontconfig 字符集。
    void operator()(FcCharSet* value) const noexcept
    {
        // 字符集被模式引用期间保持存活，并在模式销毁前按局部逆序释放。
        if ( value ) FcCharSetDestroy(value);
    }
};

using UniqueFontConfig = std::unique_ptr<FcConfig, FontConfigReleaser>;
/// @brief 独占持有 Fontconfig 查询或匹配模式。
using UniqueFontPattern = std::unique_ptr<FcPattern, FontPatternReleaser>;
/// @brief 独占持有中文覆盖条件使用的 Fontconfig 字符集。
using UniqueFontCharSet = std::unique_ptr<FcCharSet, FontCharSetReleaser>;

/// @brief 通过 Fontconfig 匹配一个无衬线字体。
/// @param config Fontconfig 配置。
/// @param requireChinese 是否要求字体覆盖 U+4E2D。
/// @return 最佳匹配的本地字体；查询失败时返回空。
/// @details 基础查询要求无衬线、轮廓和可缩放字体；中文回退查询额外附加
///          字符集与语言条件，并在匹配后再次验证 U+4E2D 覆盖，防止配置替换
///          弱化硬性字符条件。返回的文件路径和集合槽位均复制为值。
std::optional<SystemFontFace> resolveFontconfigFont(FcConfig& config,
                                                    bool      requireChinese)
{
    // 每次查询使用独立 Pattern，避免首选字体条件污染中文回退查询。
    UniqueFontPattern pattern(FcPatternCreate());
    if ( !pattern ) return std::nullopt;

    // 通用族名交由 Fontconfig 按桌面配置映射到具体首选 UI 字体。
    if ( !FcPatternAddString(pattern.get(),
                             FC_FAMILY,
                             reinterpret_cast<const FcChar8*>("sans-serif")) ||
         !FcPatternAddBool(pattern.get(), FC_OUTLINE, FcTrue) ||
         !FcPatternAddBool(pattern.get(), FC_SCALABLE, FcTrue) ) {
        return std::nullopt;
    }

    UniqueFontCharSet requiredCharacters;
    if ( requireChinese ) {
        // 字符集对象必须活到匹配完成，因为 Pattern 保存的是其引用。
        requiredCharacters.reset(FcCharSetCreate());
        if ( !requiredCharacters ||
             !FcCharSetAddChar(requiredCharacters.get(), 0x4E2D) ||
             !FcPatternAddCharSet(
                 pattern.get(), FC_CHARSET, requiredCharacters.get()) ||
             !FcPatternAddString(pattern.get(),
                                 FC_LANG,
                                 reinterpret_cast<const FcChar8*>("zh-cn")) ) {
            // 任一条件无法写入都放弃该候选，不执行不完整的回退查询。
            return std::nullopt;
        }
    }

    // 应用系统配置中的别名、替换和用户首选规则后再补充默认属性。
    if ( !FcConfigSubstitute(&config, pattern.get(), FcMatchPattern) ) {
        return std::nullopt;
    }
    // 默认替换补齐未显式指定的样式、像素尺寸和渲染相关属性。
    FcDefaultSubstitute(pattern.get());

    FcResult matchResult = FcResultNoMatch;
    // FcFontMatch 返回的新 Pattern 由调用方销毁，同时检查显式结果枚举。
    UniqueFontPattern match(FcFontMatch(&config, pattern.get(), &matchResult));
    if ( !match || matchResult != FcResultMatch ) return std::nullopt;

    if ( requireChinese ) {
        FcCharSet* matchedCharacters = nullptr;
        // 某些配置规则可能替换请求字符集，因此对最终候选进行二次覆盖确认。
        if ( FcPatternGetCharSet(
                 match.get(), FC_CHARSET, 0, &matchedCharacters) !=
                 FcResultMatch ||
             !matchedCharacters ||
             !FcCharSetHasChar(matchedCharacters, 0x4E2D) ) {
            return std::nullopt;
        }
    }

    FcChar8* filePath = nullptr;
    // 没有本地文件的候选无法交给 FreeType，不能只凭家族名返回。
    if ( FcPatternGetString(match.get(), FC_FILE, 0, &filePath) !=
             FcResultMatch ||
         !filePath ) {
        return std::nullopt;
    }

    int faceIndex = 0;
    // 普通单字体文件可能没有 FC_INDEX，此时约定使用首个 face。
    if ( FcPatternGetInteger(match.get(), FC_INDEX, 0, &faceIndex) !=
         FcResultMatch ) {
        faceIndex = 0;
    }

    // Fontconfig 高 16 位可编码可变字体实例；ImGui FontNo 只需要集合槽位。
    // 保留低 16 位可避免把命名实例信息误当作 TTC/OTC face index。
    faceIndex &= 0xFFFF;

    std::string familyName        = "sans-serif";
    FcChar8*    matchedFamilyName = nullptr;
    // 匹配后的具体家族名用于界面展示，缺失时保留通用族名。
    if ( FcPatternGetString(match.get(), FC_FAMILY, 0, &matchedFamilyName) ==
             FcResultMatch &&
         matchedFamilyName ) {
        familyName = reinterpret_cast<const char*>(matchedFamilyName);
    }
    // FcPattern 销毁前复制所有借用字符串，返回对象不依赖 Fontconfig 生命周期。
    return SystemFontFace{ std::filesystem::path(
                               reinterpret_cast<const char*>(filePath)),
                           faceIndex,
                           std::move(familyName) };
}

#endif

}  // namespace

/// @brief 解析当前桌面首选 UI 字体及中文覆盖回退。
/// @return 按优先级去重的本地字体描述；平台查询失败时返回空列表。
/// @warning 会同步调用平台字体数据库，只能在启动或字体资源重建路径执行。
std::vector<SystemFontFace> resolvePreferredSystemFonts()
{
    // 顺序具有语义：首选 UI 字体在前，语言覆盖字体随后补充。
    std::vector<SystemFontFace> fonts;

#ifdef _WIN32
    // DirectWrite 同时解析消息字体与按当前系统规则选择的中文回退。
    appendWindowsPreferredFonts(fonts);
#elif defined(__APPLE__)
    // CoreText 返回当前语言环境的系统 UI 字体，RAII 接管 Copy/Create 所有权。
    UniqueCoreFoundation<CTFontRef> preferredFont(
        CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 0.0, nullptr));
    appendUniqueFont(fonts, resolveCoreTextFont(preferredFont.get()));
    appendUniqueFont(fonts, resolveMacOSChineseFallback(preferredFont.get()));
#else
    // Fontconfig 配置加载失败时返回空列表，让上层继续使用内置字体。
    UniqueFontConfig config(FcInitLoadConfigAndFonts());
    if ( config ) {
        // 两次独立匹配分别保留桌面首选外观和中文覆盖能力。
        appendUniqueFont(fonts, resolveFontconfigFont(*config, false));
        appendUniqueFont(fonts, resolveFontconfigFont(*config, true));
    }
#endif

    // 所有平台句柄已在局部作用域内转换为值语义描述。
    return fonts;
}

}  // namespace MMM::Font
