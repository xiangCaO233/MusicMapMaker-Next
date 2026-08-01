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

namespace MMM::Font
{

namespace
{

/// @brief 将有效且尚未出现的字体追加到有序结果中。
/// @param fonts 已解析的字体列表。
/// @param font 待追加的字体。
void appendUniqueFont(std::vector<SystemFontFace>&         fonts,
                      const std::optional<SystemFontFace>& font)
{
    if ( !font || font->m_filePath.empty() ||
         !font->m_filePath.is_absolute() ) {
        return;
    }

    const auto existing =
        std::find_if(fonts.begin(), fonts.end(), [&](const auto& candidate) {
            return candidate.m_faceIndex == font->m_faceIndex &&
                   candidate.m_filePath == font->m_filePath;
        });
    if ( existing == fonts.end() ) fonts.push_back(*font);
}

#ifdef _WIN32

/// @brief 释放 DirectWrite COM 接口。
template<typename T> struct ComReleaser {
    /// @brief 释放一个由调用方持有引用的 COM 接口。
    /// @param value COM 接口。
    void operator()(T* value) const noexcept
    {
        if ( value ) value->Release();
    }
};

template<typename T> using UniqueCom = std::unique_ptr<T, ComReleaser<T>>;

/// @brief 用于触发 DirectWrite 系统中文字体回退的文本。
constexpr std::wstring_view CHINESE_FALLBACK_TEXT = L"中文";

/// @brief DirectWrite 中文回退匹配使用的区域名称。
constexpr const wchar_t* CHINESE_FALLBACK_LOCALE = L"zh-CN";

/// @brief 为同步字体回退查询提供固定中文文本。
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
        if ( !object ) return E_POINTER;
        *object = nullptr;
        if ( IsEqualIID(interfaceId, __uuidof(IUnknown)) ||
             IsEqualIID(interfaceId, __uuidof(IDWriteTextAnalysisSource)) ) {
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
        if ( position > CHINESE_FALLBACK_TEXT.size() ) return E_INVALIDARG;

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
std::string wideToUtf8(std::wstring_view value)
{
    if ( value.empty() ) return {};

    const int inputSize  = static_cast<int>(value.size());
    const int outputSize = WideCharToMultiByte(CP_UTF8,
                                               WC_ERR_INVALID_CHARS,
                                               value.data(),
                                               inputSize,
                                               nullptr,
                                               0,
                                               nullptr,
                                               nullptr);
    if ( outputSize <= 0 ) return {};

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
    return output;
}

/// @brief 从 DirectWrite 字体对象解析本地文件和集合索引。
/// @param font DirectWrite 字体对象。
/// @return 可直接加载的本地字体；多文件字体或非本地字体返回空。
std::optional<SystemFontFace> resolveDirectWriteFont(IDWriteFont& font,
                                                     std::string  familyName)
{
    IDWriteFontFace*           rawFace    = nullptr;
    const HRESULT              faceResult = font.CreateFontFace(&rawFace);
    UniqueCom<IDWriteFontFace> face(rawFace);
    if ( FAILED(faceResult) || !face ) return std::nullopt;

    UINT32  fileCount   = 0;
    HRESULT filesResult = face->GetFiles(&fileCount, nullptr);
    if ( FAILED(filesResult) || fileCount != 1 ) return std::nullopt;

    IDWriteFontFile* rawFile = nullptr;
    filesResult              = face->GetFiles(&fileCount, &rawFile);
    UniqueCom<IDWriteFontFile> file(rawFile);
    if ( FAILED(filesResult) || !file ) return std::nullopt;

    const void* referenceKey     = nullptr;
    UINT32      referenceKeySize = 0;
    if ( FAILED(file->GetReferenceKey(&referenceKey, &referenceKeySize)) ) {
        return std::nullopt;
    }

    IDWriteFontFileLoader*           rawLoader    = nullptr;
    const HRESULT                    loaderResult = file->GetLoader(&rawLoader);
    UniqueCom<IDWriteFontFileLoader> loader(rawLoader);
    if ( FAILED(loaderResult) || !loader ) return std::nullopt;

    IDWriteLocalFontFileLoader* rawLocalLoader = nullptr;
    const HRESULT               localLoaderResult =
        loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                               reinterpret_cast<void**>(&rawLocalLoader));
    UniqueCom<IDWriteLocalFontFileLoader> localLoader(rawLocalLoader);
    if ( FAILED(localLoaderResult) || !localLoader ) return std::nullopt;

    UINT32 pathLength = 0;
    if ( FAILED(localLoader->GetFilePathLengthFromKey(
             referenceKey, referenceKeySize, &pathLength)) ) {
        return std::nullopt;
    }

    std::vector<wchar_t> pathBuffer(static_cast<std::size_t>(pathLength) + 1);
    if ( FAILED(localLoader->GetFilePathFromKey(referenceKey,
                                                referenceKeySize,
                                                pathBuffer.data(),
                                                pathLength + 1)) ) {
        return std::nullopt;
    }

    return SystemFontFace{ std::filesystem::path(pathBuffer.data()),
                           static_cast<int>(face->GetIndex()),
                           std::move(familyName) };
}

/// @brief 通过 DirectWrite 系统回退解析中文字体。
/// @param factory 基础 DirectWrite 工厂。
/// @param interop DirectWrite GDI 互操作对象。
/// @param baseFont 系统首选消息字体描述。
/// @return 可覆盖中文探测文本的系统字体；API 不可用或映射失败时返回空。
std::optional<SystemFontFace> resolveWindowsChineseFallback(
    IDWriteFactory& factory, IDWriteGdiInterop& interop,
    const LOGFONTW& baseFont)
{
    IDWriteFactory2* rawFactory2    = nullptr;
    const HRESULT    factory2Result = factory.QueryInterface(
        __uuidof(IDWriteFactory2), reinterpret_cast<void**>(&rawFactory2));
    UniqueCom<IDWriteFactory2> factory2(rawFactory2);
    if ( FAILED(factory2Result) || !factory2 ) return std::nullopt;

    IDWriteFontFallback* rawFallback = nullptr;
    const HRESULT        fallbackResult =
        factory2->GetSystemFontFallback(&rawFallback);
    UniqueCom<IDWriteFontFallback> fallback(rawFallback);
    if ( FAILED(fallbackResult) || !fallback ) return std::nullopt;

    const auto baseWeight = static_cast<DWRITE_FONT_WEIGHT>(
        baseFont.lfWeight > 0 ? std::clamp<LONG>(baseFont.lfWeight, 1, 999)
                              : static_cast<LONG>(DWRITE_FONT_WEIGHT_NORMAL));
    const auto baseStyle = baseFont.lfItalic != 0 ? DWRITE_FONT_STYLE_ITALIC
                                                  : DWRITE_FONT_STYLE_NORMAL;

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
    if ( FAILED(mapResult) || !mappedFont ||
         mappedLength != CHINESE_FALLBACK_TEXT.size() || mappedScale <= 0.0F ) {
        return std::nullopt;
    }

    LOGFONTW    mappedLogFont{};
    BOOL        isSystemFont = FALSE;
    std::string familyName;
    if ( SUCCEEDED(interop.ConvertFontToLOGFONT(
             mappedFont.get(), &mappedLogFont, &isSystemFont)) &&
         isSystemFont ) {
        familyName = wideToUtf8(mappedLogFont.lfFaceName);
    }

    return resolveDirectWriteFont(*mappedFont, std::move(familyName));
}

/// @brief 解析 Win32 当前消息字体及其中文系统回退字体。
/// @param fonts 接收按优先级排列的字体列表。
void appendWindowsPreferredFonts(std::vector<SystemFontFace>& fonts)
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if ( !SystemParametersInfoW(
             SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) ) {
        return;
    }

    IDWriteFactory* rawFactory = nullptr;
    const HRESULT   factoryResult =
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&rawFactory));
    UniqueCom<IDWriteFactory> factory(rawFactory);
    if ( FAILED(factoryResult) || !factory ) return;

    IDWriteGdiInterop* rawInterop    = nullptr;
    const HRESULT      interopResult = factory->GetGdiInterop(&rawInterop);
    UniqueCom<IDWriteGdiInterop> interop(rawInterop);
    if ( FAILED(interopResult) || !interop ) return;

    IDWriteFont*  rawFont = nullptr;
    const HRESULT fontResult =
        interop->CreateFontFromLOGFONT(&metrics.lfMessageFont, &rawFont);
    UniqueCom<IDWriteFont> font(rawFont);
    if ( SUCCEEDED(fontResult) && font ) {
        appendUniqueFont(
            fonts,
            resolveDirectWriteFont(
                *font, wideToUtf8(metrics.lfMessageFont.lfFaceName)));
    }

    appendUniqueFont(fonts,
                     resolveWindowsChineseFallback(
                         *factory, *interop, metrics.lfMessageFont));
}

#elif defined(__APPLE__)

/// @brief 释放 Core Foundation 所有权对象。
struct CoreFoundationReleaser {
    /// @brief 释放一个遵循 Create/Copy 规则的 Core Foundation 对象。
    /// @param value Core Foundation 对象。
    template<typename T> void operator()(T* value) const noexcept
    {
        if ( value ) CFRelease(value);
    }
};

template<typename T>
using UniqueCoreFoundation =
    std::unique_ptr<std::remove_pointer_t<T>, CoreFoundationReleaser>;

/// @brief 释放 FreeType 字体库。
struct FreeTypeLibraryReleaser {
    /// @brief 释放 FreeType 字体库。
    /// @param value FreeType 字体库。
    void operator()(std::remove_pointer_t<FT_Library>* value) const noexcept
    {
        if ( value ) FT_Done_FreeType(value);
    }
};

/// @brief 释放 FreeType 字体 face。
struct FreeTypeFaceReleaser {
    /// @brief 释放 FreeType 字体 face。
    /// @param value FreeType 字体 face。
    void operator()(std::remove_pointer_t<FT_Face>* value) const noexcept
    {
        if ( value ) FT_Done_Face(value);
    }
};

using UniqueFreeTypeLibrary =
    std::unique_ptr<std::remove_pointer_t<FT_Library>, FreeTypeLibraryReleaser>;
using UniqueFreeTypeFace =
    std::unique_ptr<std::remove_pointer_t<FT_Face>, FreeTypeFaceReleaser>;

/// @brief 将 Core Foundation 字符串转换为 UTF-8 文本。
/// @param value Core Foundation 字符串。
/// @return 转换后的文本；转换失败时返回空。
std::optional<std::string> utf8FromCoreFoundationString(CFStringRef value)
{
    if ( !value ) return std::nullopt;

    const CFIndex maximumSize =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(value),
                                          kCFStringEncodingUTF8) +
        1;
    if ( maximumSize <= 1 ) return std::nullopt;

    std::vector<char> buffer(static_cast<std::size_t>(maximumSize));
    if ( !CFStringGetCString(
             value, buffer.data(), maximumSize, kCFStringEncodingUTF8) ) {
        return std::nullopt;
    }
    return std::string(buffer.data());
}

/// @brief 按 PostScript 名称解析字体在 TTC/OTC 文件中的 face index。
/// @param path CoreText 返回的字体文件路径。
/// @param postScriptName CoreText 返回的 PostScript 名称。
/// @return 匹配的集合索引；多 face 文件无法确认时返回空。
std::optional<int> resolveFreeTypeFaceIndex(const std::filesystem::path& path,
                                            std::string_view postScriptName)
{
    FT_Library rawLibrary = nullptr;
    if ( FT_Init_FreeType(&rawLibrary) != 0 ) return std::nullopt;
    UniqueFreeTypeLibrary library(rawLibrary);

    const std::string filePath = path.string();
    FT_Face           rawProbe = nullptr;
    if ( FT_New_Face(library.get(), filePath.c_str(), -1, &rawProbe) != 0 ) {
        return std::nullopt;
    }
    UniqueFreeTypeFace probe(rawProbe);

    const FT_Long faceCount = probe->num_faces;
    if ( faceCount <= 0 ||
         faceCount > static_cast<FT_Long>(std::numeric_limits<int>::max()) ) {
        return std::nullopt;
    }

    for ( FT_Long faceIndex = 0; faceIndex < faceCount; ++faceIndex ) {
        FT_Face rawFace = nullptr;
        if ( FT_New_Face(
                 library.get(), filePath.c_str(), faceIndex, &rawFace) != 0 ) {
            continue;
        }
        UniqueFreeTypeFace face(rawFace);

        const char* candidateName = FT_Get_Postscript_Name(face.get());
        if ( candidateName && postScriptName == candidateName ) {
            return static_cast<int>(faceIndex);
        }
    }

    // 单字体文件不存在集合歧义，缺少 PostScript 名称时仍可安全使用索引 0。
    if ( faceCount == 1 ) return 0;
    return std::nullopt;
}

/// @brief 从 CoreText 字体解析本地文件路径。
/// @param font CoreText 字体。
/// @return 字体文件；CoreText 未提供本地 URL 时返回空。
std::optional<SystemFontFace> resolveCoreTextFont(CTFontRef font)
{
    if ( !font ) return std::nullopt;

    UniqueCoreFoundation<CFTypeRef> urlAttribute(
        CTFontCopyAttribute(font, kCTFontURLAttribute));
    if ( !urlAttribute ||
         CFGetTypeID(urlAttribute.get()) != CFURLGetTypeID() ) {
        return std::nullopt;
    }

    UniqueCoreFoundation<CFStringRef> pathString(CFURLCopyFileSystemPath(
        static_cast<CFURLRef>(urlAttribute.get()), kCFURLPOSIXPathStyle));
    const auto path = utf8FromCoreFoundationString(pathString.get());
    if ( !path ) return std::nullopt;

    UniqueCoreFoundation<CFStringRef> postScriptName(
        CTFontCopyPostScriptName(font));
    const auto postScriptNameUtf8 =
        utf8FromCoreFoundationString(postScriptName.get());
    if ( !postScriptNameUtf8 ) return std::nullopt;

    const std::filesystem::path filePath(*path);
    const auto                  faceIndex =
        resolveFreeTypeFaceIndex(filePath, *postScriptNameUtf8);
    if ( !faceIndex ) return std::nullopt;

    UniqueCoreFoundation<CFTypeRef> familyAttribute(
        CTFontCopyAttribute(font, kCTFontFamilyNameAttribute));
    std::string familyName;
    if ( familyAttribute &&
         CFGetTypeID(familyAttribute.get()) == CFStringGetTypeID() ) {
        familyName = utf8FromCoreFoundationString(
                         static_cast<CFStringRef>(familyAttribute.get()))
                         .value_or(std::string{});
    }

    return SystemFontFace{ filePath, *faceIndex, std::move(familyName) };
}

/// @brief 解析可由 FreeType 读取的 macOS 中文界面字体。
/// @param preferredFont CoreText 当前首选界面字体。
/// @return 可用于 ImGui 合并的中文字体；系统字体均不可读时返回空。
std::optional<SystemFontFace> resolveMacOSChineseFallback(
    CTFontRef preferredFont)
{
    if ( preferredFont ) {
        const CFStringRef               chineseProbe = CFSTR("中文");
        UniqueCoreFoundation<CTFontRef> coreTextFallback(CTFontCreateForString(
            preferredFont,
            chineseProbe,
            CFRangeMake(0, CFStringGetLength(chineseProbe))));
        if ( auto resolved = resolveCoreTextFont(coreTextFallback.get()) ) {
            return resolved;
        }
    }

    // 新版 macOS 可能把 CoreText 中文回退解析到受保护的 PingFangUI.ttc；
    // FreeType 无法打开其中的具体 face，因此退回到系统自带的可读简体中文字体。
    UniqueCoreFoundation<CTFontRef> readableFallback(
        CTFontCreateWithName(CFSTR("Hiragino Sans GB"), 0.0, nullptr));
    return resolveCoreTextFont(readableFallback.get());
}

#else

/// @brief 释放 Fontconfig 配置。
struct FontConfigReleaser {
    /// @brief 释放 Fontconfig 配置。
    /// @param value Fontconfig 配置。
    void operator()(FcConfig* value) const noexcept
    {
        if ( value ) FcConfigDestroy(value);
    }
};

/// @brief 释放 Fontconfig 匹配模式。
struct FontPatternReleaser {
    /// @brief 释放 Fontconfig 匹配模式。
    /// @param value Fontconfig 匹配模式。
    void operator()(FcPattern* value) const noexcept
    {
        if ( value ) FcPatternDestroy(value);
    }
};

/// @brief 释放 Fontconfig 字符集。
struct FontCharSetReleaser {
    /// @brief 释放 Fontconfig 字符集。
    /// @param value Fontconfig 字符集。
    void operator()(FcCharSet* value) const noexcept
    {
        if ( value ) FcCharSetDestroy(value);
    }
};

using UniqueFontConfig  = std::unique_ptr<FcConfig, FontConfigReleaser>;
using UniqueFontPattern = std::unique_ptr<FcPattern, FontPatternReleaser>;
using UniqueFontCharSet = std::unique_ptr<FcCharSet, FontCharSetReleaser>;

/// @brief 通过 Fontconfig 匹配一个无衬线字体。
/// @param config Fontconfig 配置。
/// @param requireChinese 是否要求字体覆盖 U+4E2D。
/// @return 最佳匹配的本地字体；查询失败时返回空。
std::optional<SystemFontFace> resolveFontconfigFont(FcConfig& config,
                                                    bool      requireChinese)
{
    UniqueFontPattern pattern(FcPatternCreate());
    if ( !pattern ) return std::nullopt;

    if ( !FcPatternAddString(pattern.get(),
                             FC_FAMILY,
                             reinterpret_cast<const FcChar8*>("sans-serif")) ||
         !FcPatternAddBool(pattern.get(), FC_OUTLINE, FcTrue) ||
         !FcPatternAddBool(pattern.get(), FC_SCALABLE, FcTrue) ) {
        return std::nullopt;
    }

    UniqueFontCharSet requiredCharacters;
    if ( requireChinese ) {
        requiredCharacters.reset(FcCharSetCreate());
        if ( !requiredCharacters ||
             !FcCharSetAddChar(requiredCharacters.get(), 0x4E2D) ||
             !FcPatternAddCharSet(
                 pattern.get(), FC_CHARSET, requiredCharacters.get()) ||
             !FcPatternAddString(pattern.get(),
                                 FC_LANG,
                                 reinterpret_cast<const FcChar8*>("zh-cn")) ) {
            return std::nullopt;
        }
    }

    if ( !FcConfigSubstitute(&config, pattern.get(), FcMatchPattern) ) {
        return std::nullopt;
    }
    FcDefaultSubstitute(pattern.get());

    FcResult          matchResult = FcResultNoMatch;
    UniqueFontPattern match(FcFontMatch(&config, pattern.get(), &matchResult));
    if ( !match || matchResult != FcResultMatch ) return std::nullopt;

    if ( requireChinese ) {
        FcCharSet* matchedCharacters = nullptr;
        if ( FcPatternGetCharSet(
                 match.get(), FC_CHARSET, 0, &matchedCharacters) !=
                 FcResultMatch ||
             !matchedCharacters ||
             !FcCharSetHasChar(matchedCharacters, 0x4E2D) ) {
            return std::nullopt;
        }
    }

    FcChar8* filePath = nullptr;
    if ( FcPatternGetString(match.get(), FC_FILE, 0, &filePath) !=
             FcResultMatch ||
         !filePath ) {
        return std::nullopt;
    }

    int faceIndex = 0;
    if ( FcPatternGetInteger(match.get(), FC_INDEX, 0, &faceIndex) !=
         FcResultMatch ) {
        faceIndex = 0;
    }

    // Fontconfig 高 16 位可编码可变字体实例；ImGui FontNo 只需要集合槽位。
    faceIndex &= 0xFFFF;

    std::string familyName        = "sans-serif";
    FcChar8*    matchedFamilyName = nullptr;
    if ( FcPatternGetString(match.get(), FC_FAMILY, 0, &matchedFamilyName) ==
             FcResultMatch &&
         matchedFamilyName ) {
        familyName = reinterpret_cast<const char*>(matchedFamilyName);
    }
    return SystemFontFace{ std::filesystem::path(
                               reinterpret_cast<const char*>(filePath)),
                           faceIndex,
                           std::move(familyName) };
}

#endif

}  // namespace

std::vector<SystemFontFace> resolvePreferredSystemFonts()
{
    std::vector<SystemFontFace> fonts;

#ifdef _WIN32
    appendWindowsPreferredFonts(fonts);
#elif defined(__APPLE__)
    UniqueCoreFoundation<CTFontRef> preferredFont(
        CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 0.0, nullptr));
    appendUniqueFont(fonts, resolveCoreTextFont(preferredFont.get()));
    appendUniqueFont(fonts, resolveMacOSChineseFallback(preferredFont.get()));
#else
    UniqueFontConfig config(FcInitLoadConfigAndFonts());
    if ( config ) {
        appendUniqueFont(fonts, resolveFontconfigFont(*config, false));
        appendUniqueFont(fonts, resolveFontconfigFont(*config, true));
    }
#endif

    return fonts;
}

}  // namespace MMM::Font
