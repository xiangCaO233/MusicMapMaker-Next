#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sol/sol.hpp>
#include <string_view>
#include <utility>
#include <vector>

namespace MMM
{
namespace Config
{
// 本文件负责把不受信任的皮肤 Lua 描述转换为运行期只读 SkinData。

/// @brief 旧版内置 IVM 皮肤迁移后使用的交互发光轮次。
constexpr int IVM_INTERACTION_GLOW_PASSES = 6;
// 该迁移值只作用于同时匹配 IVM 名称、目录和显式全零发光的旧资源。

/// @brief 旧版内置 IVM 皮肤迁移后使用的交互发光强度。
constexpr float IVM_INTERACTION_GLOW_INTENSITY = 0.5F;
// 强度与轮次成对恢复，避免只启用轮次却仍得到不可见效果。

namespace
{
/// @brief 应用资源包内固定提供的默认语言文件。
constexpr std::array<std::pair<std::string_view, std::string_view>, 2>
    DEFAULT_TRANSLATIONS{ std::pair{ "en_us", "en_us.lua" },
                          std::pair{ "zh_cn", "zh_cn.lua" } };
// 编译期固定清单使皮肤不能删除应用保证提供的基础语言。

/// @brief 判断候选路径是否位于指定根目录内。
/// @param root 已规范化的根目录。
/// @param candidate 已规范化的候选路径。
/// @return 候选路径等于根目录或属于其后代时返回 true。
bool isPathWithinRoot(const std::filesystem::path& root,
                      const std::filesystem::path& candidate)
{
    // 按路径分量比较而非字符串前缀，避免相邻同名前缀目录误判。
    auto rootIt      = root.begin();
    auto candidateIt = candidate.begin();
    // 两条路径已由调用方规范化，因此无需在循环内处理点段。
    for ( ; rootIt != root.end() && candidateIt != candidate.end();
          ++rootIt, ++candidateIt ) {
        if ( *rootIt != *candidateIt ) return false;
    }
    // 根路径全部匹配即表示候选等于根或位于其后代。
    return rootIt == root.end();
}

/// @brief 将皮肤翻译覆写路径解析为皮肤包内部文件。
/// @param skinPath 皮肤入口所在根目录。
/// @param configuredPath 皮肤 Lua 声明的相对或绝对路径。
/// @return 路径未逃逸皮肤根目录时返回规范化结果。
std::optional<std::filesystem::path> resolveSkinOverridePath(
    const std::filesystem::path& skinPath, const std::string& configuredPath)
{
    // 局部别名让安全路径步骤保持紧凑，不改变项目级命名空间。
    namespace fs = std::filesystem;
    // 空声明没有可解析目标，按未提供覆写处理。
    if ( configuredPath.empty() ) return std::nullopt;

    // 根目录先规范化，后续包含关系必须基于同一文件系统视图。
    std::error_code canonicalError;
    const fs::path  canonicalRoot =
        fs::weakly_canonical(skinPath, canonicalError);
    if ( canonicalError ) return std::nullopt;

    // 相对声明以皮肤入口目录为基准，绝对路径仍需通过根包含检查。
    fs::path candidate = Config::utf8ToPath(configuredPath);
    if ( !candidate.is_absolute() ) candidate = skinPath / candidate;
    candidate = fs::weakly_canonical(candidate, canonicalError);
    // 无法规范化或逃逸根目录的路径都不能进入翻译器文件读取入口。
    if ( canonicalError || !isPathWithinRoot(canonicalRoot, candidate) ) {
        return std::nullopt;
    }
    return candidate;
}
}  // namespace

/// @brief 获取进程内唯一的皮肤管理器。
/// @return 生命周期持续到进程退出的管理器引用。
/// @note 单例同时持有当前 SkinData 和对应翻译器。
SkinManager& SkinManager::instance()
{
    // 函数局部静态对象由语言保证线程安全地完成首次初始化。
    static SkinManager inst;
    return inst;
}

/// @brief 使用应用默认翻译资源加载皮肤。
/// @param luaFilePath 皮肤入口 Lua 文件的 UTF-8 路径。
/// @return 皮肤脚本、默认翻译及配置解析全部成功时返回 true。
/// @warning 用户触发的低频资源加载路径，会执行文件 I/O 和 Lua 脚本。
bool SkinManager::loadSkin(const std::string& luaFilePath)
{
    // 生产入口统一使用 AppPaths 管理的已同步默认翻译目录。
    return loadSkin(luaFilePath, AppPaths::translationsRootPath());
}

/// @brief 使用显式翻译根加载并发布皮肤数据。
/// @param luaFilePath 皮肤入口 Lua 文件的 UTF-8 路径。
/// @param translationsRoot 默认语言文件所在目录。
/// @return 所有必要资源描述成功解析时返回 true。
/// @warning 低频重载事务会清空当前翻译缓存并遍历皮肤全部配置表。
bool SkinManager::loadSkin(const std::string&           luaFilePath,
                           const std::filesystem::path& translationsRoot)
{
    // 每次加载使用独立 Lua 状态，旧皮肤脚本全局变量不会泄漏到新皮肤。
    sol::state lua;
    // 只开放皮肤描述需要的基础、包和 table 库。
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table);

    namespace fs = std::filesystem;
    // 输入 UTF-8 路径集中转换，并以绝对父目录作为全部资源解析锚点。
    fs::path       path     = Config::utf8ToPath(luaFilePath);
    fs::path       absPath  = fs::absolute(path);
    const fs::path skinPath = absPath.parent_path();
    // skinPath 同时作为资源和安全覆写根，在整个加载过程中保持不变。

    /// @brief 拼接皮肤 resources 下的相对资源路径。
    /// @param rpath Lua 表中声明的 UTF-8 资源相对路径。
    /// @return 词法规范化后的资源候选路径。
    /// @note 集中使用 UTF-8 转换，避开 libc++ 窄字符 locale 差异。
    auto makeResPath = [&](const std::string& rpath) {
        return (skinPath / Config::utf8ToPath("resources") /
                Config::utf8ToPath(rpath))
            .lexically_normal();
    };
    // Lua 中的路径拼接固定使用正斜线，并保证目录字符串以斜线结尾。
    std::string skinDir = pathToUtf8(skinPath);
    std::replace(skinDir.begin(), skinDir.end(), '\\', '/');
    if ( !skinDir.empty() && skinDir.back() != '/' ) skinDir += '/';
    // 注入只读目录变量，兼容皮肤脚本自行组合资源路径的历史约定。
    lua["__SKINLUA_DIR__"] = skinDir;
    XINFO("LuaJIT Loading skin from: " + skinDir);

    // 使用 filesystem::path 打开，避免 Windows fopen 窄字符编码损坏路径。
    std::ifstream skinFile(path, std::ios::in | std::ios::binary);
    if ( !skinFile ) {
        XERROR("Failed to open skin lua file: " + luaFilePath);
        return false;
    }
    // 皮肤仅在重载时完整读取，脚本文本交给受保护的 sol2 执行入口。
    std::string scriptContent((std::istreambuf_iterator<char>(skinFile)),
                              std::istreambuf_iterator<char>());
    // protected result 把 Lua 错误作为值返回，避免 C++ 异常穿过项目边界。
    sol::protected_function_result result =
        lua.script(scriptContent, luaFilePath);
    if ( !result.valid() ) {
        sol::error err = result;
        XERROR("Failed to load skin lua: " + std::string(err.what()));
        return false;
    }
    // Lua 执行失败不会覆盖旧 SkinData；成功后才进入新数据初始化阶段。

    // 皮肤契约要求脚本返回顶层 table，后续按固定字段逐项解析。
    sol::table skinTable = result;
    XINFO("Skin Lua table extracted successfully");

    // 脚本成功后才清空旧数据；随后任何必要资源失败都会返回加载失败。
    m_data          = SkinData();
    m_data.skinPath = skinPath;
    // 翻译器清空保留稳定字符串池，但移除旧皮肤字典和热缓存。
    m_translator.clear();

    // 默认字典独立于皮肤声明，确保公共界面键集合稳定且完整。
    std::size_t loadedTranslationCount = 0;
    for ( const auto& [languageId, fileName] : DEFAULT_TRANSLATIONS ) {
        // 文件名来自编译期清单，根目录可由测试显式隔离。
        const fs::path translationPath =
            translationsRoot / Config::utf8ToPath(std::string(fileName));
        if ( !m_translator.loadLanguage(std::string(languageId),
                                        pathToUtf8(translationPath)) ) {
            XERROR("Failed to load default translation: {}",
                   pathToUtf8(translationPath));
            return false;
        }
        // 任一默认语言失败都会提前返回，不发布不完整翻译集合。
        ++loadedTranslationCount;
    }
    XINFO("Default translations loaded: {} language(s)",
          loadedTranslationCount);

    // 元数据缺失时使用可展示默认值，特效帧率沿用稳定产品默认。
    m_data.themeName =
        skinTable["meta"]["name"].get_or<std::string>("Custom Skin");
    m_data.themeAuthor =
        skinTable["meta"]["author"].get_or<std::string>("Various Artists");
    m_data.themeVersion =
        skinTable["meta"]["version"].get_or<std::string>("1.0");
    m_data.effectBaseFps =
        skinTable["meta"]["effectbasefps"].get_or(DEFAULT_EFFECT_BASE_FPS);
    // 帧率只记录皮肤时间基准，具体播放步进由渲染系统消费。

    // 颜色表支持任意嵌套层级，递归入口从空前缀开始生成点分键。
    sol::optional<sol::table> colorsTableOpt = skinTable["colors"];
    if ( colorsTableOpt ) {
        parseColorsRecursive(colorsTableOpt.value(), "");
    }
    // 缺失颜色表保持空字典，查询端以洋红色明确暴露缺失键。

    // 皮肤只允许覆写默认字典中已存在的字段，旧 langs 配置不再参与加载。
    // 这样可阻止皮肤包通过自定义字典取代应用界面的公共翻译契约。
    sol::optional<sol::table> langOverridesTableOpt =
        skinTable["lang_overrides"];
    if ( langOverridesTableOpt ) {
        // Lua table 可混入其他类型，只有字符串语言 ID 到字符串路径有效。
        for ( const auto& kv : langOverridesTableOpt.value() ) {
            if ( !kv.first.is<std::string>() || !kv.second.is<std::string>() ) {
                continue;
            }

            // 路径在保存到 SkinData 前必须规范化并限制于当前皮肤根。
            const std::string languageId     = kv.first.as<std::string>();
            const std::string configuredPath = kv.second.as<std::string>();
            const auto        overridePath =
                resolveSkinOverridePath(skinPath, configuredPath);
            if ( !overridePath ) {
                XERROR("Skin language override escapes skin root: {}",
                       configuredPath);
                continue;
            }

            // 保存已验证路径供诊断，翻译器随后只合并默认字典已有字段。
            m_data.langOverrideLuaPaths[languageId] = *overridePath;
            auto overrideResult = m_translator.applyLanguageOverride(
                languageId, pathToUtf8(*overridePath));
            // 文件解析失败由翻译器记录；皮肤本身仍可使用默认语言继续加载。
            if ( !overrideResult.loaded ) continue;

            // 诊断键加上语言前缀，多个覆写文件中同名字段仍可区分。
            for ( auto& field : overrideResult.unknownFields ) {
                m_data.missingTranslationOverrideFields.push_back(
                    languageId + ":" + std::move(field));
            }
        }
    }
    // 排序去重使启动提示确定，不依赖 Lua table 的遍历顺序。
    std::sort(m_data.missingTranslationOverrideFields.begin(),
              m_data.missingTranslationOverrideFields.end());
    m_data.missingTranslationOverrideFields.erase(
        std::unique(m_data.missingTranslationOverrideFields.begin(),
                    m_data.missingTranslationOverrideFields.end()),
        m_data.missingTranslationOverrideFields.end());
    XINFO("Skin translation overrides loaded: {} file(s)",
          m_data.langOverrideLuaPaths.size());

    // 优先恢复全局语言偏好；字典不存在时切换到皮肤数据默认回退语言。
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    // switchLang 失败不修改当前字典，因此再尝试稳定的 fallback ID。
    if ( !m_translator.switchLang(settings.language) ) {
        m_translator.switchLang(m_data.fallBackLang);
    }

    // fonts 是按用途命名的基础字体映射，所有路径相对 resources 解析。
    sol::table fontsTable = skinTable["fonts"];
    // 表契约要求字符串键值，正式皮肤由资源测试保证内容合法。
    for ( const auto& kv : fontsTable ) {
        std::string key       = kv.first.as<std::string>();
        std::string rpath     = kv.second.as<std::string>();
        m_data.fontPaths[key] = makeResPath(rpath);
    }
    // 新版可独立指定图标字体；旧皮肤缺失时兼容复用 ASCII 字体。
    if ( !m_data.fontPaths.contains("icons") ) {
        if ( auto asciiFontPath = m_data.fontPaths.find("ascii");
             asciiFontPath != m_data.fontPaths.end() ) {
            // 旧皮肤没有独立图标字体字段时，复用 ASCII 字体保持兼容。
            m_data.fontPaths["icons"] = asciiFontPath->second;
        }
    }
    // icons 既未声明且 ascii 也缺失时保持空值，由 UI 字体层决定后备。
    XINFO("Fonts loaded: {} font(s)", m_data.fontPaths.size());

    // ascii_fonts 是字体候选列表，兼容有序数组和旧版无序字典两种格式。
    sol::optional<sol::table> asciiFontsTableOpt = skinTable["ascii_fonts"];
    m_data.asciiFonts.clear();
    if ( asciiFontsTableOpt ) {
        sol::table asciiFontsTable = asciiFontsTableOpt.value();
        // 索引一有效表示有序数组格式，保持皮肤作者声明的字体优先级。
        if ( asciiFontsTable[1].valid() ) {
            // Lua 数组从一开始，逐项要求包含名称和资源路径两个值。
            for ( size_t i = 1; i <= asciiFontsTable.size(); ++i ) {
                sol::object entry = asciiFontsTable[i];
                if ( entry.is<sol::table>() ) {
                    sol::table t = entry.as<sol::table>();
                    if ( t[1].valid() && t[2].valid() ) {
                        m_data.asciiFonts.emplace_back(
                            t[1].get<std::string>(),
                            makeResPath(t[2].get<std::string>()));
                    }
                }
            }
        } else {
            // 字典格式无法保证顺序，只用于兼容旧皮肤资源。
            for ( const auto& kv : asciiFontsTable ) {
                std::string name  = kv.first.as<std::string>();
                std::string rpath = kv.second.as<std::string>();
                m_data.asciiFonts.emplace_back(name, makeResPath(rpath));
            }
        }
    }
    // 候选列表允许为空，基础 fonts.ascii 仍可作为单一字体来源。
    XINFO("AsciiFonts loaded: {} font(s)", m_data.asciiFonts.size());

    // CJK 字体候选采用与 ASCII 字体相同的双格式兼容规则。
    sol::optional<sol::table> cjkFontsTableOpt = skinTable["cjk_fonts"];
    m_data.cjkFonts.clear();
    if ( cjkFontsTableOpt ) {
        sol::table cjkFontsTable = cjkFontsTableOpt.value();
        // 数组格式保留字体回退次序，字典格式仅保留名称到路径映射。
        if ( cjkFontsTable[1].valid() ) {
            for ( size_t i = 1; i <= cjkFontsTable.size(); ++i ) {
                // 跳过非 table 数组项，避免部分扩展字段阻断整个皮肤。
                sol::object entry = cjkFontsTable[i];
                if ( entry.is<sol::table>() ) {
                    sol::table t = entry.as<sol::table>();
                    if ( t[1].valid() && t[2].valid() ) {
                        // 每项按名称与相对资源路径二元组保存。
                        m_data.cjkFonts.emplace_back(
                            t[1].get<std::string>(),
                            makeResPath(t[2].get<std::string>()));
                    }
                }
            }
        } else {
            // 旧字典格式保留所有键值，但其迭代顺序不作为回退优先级契约。
            for ( const auto& kv : cjkFontsTable ) {
                std::string name  = kv.first.as<std::string>();
                std::string rpath = kv.second.as<std::string>();
                m_data.cjkFonts.emplace_back(name, makeResPath(rpath));
            }
        }
    }
    // CJK 和 ASCII 列表分别维护，脚本覆盖范围可独立选择。

    // 解析皮肤推荐主题。旧字符串格式同时绑定亮暗分支，保持原有行为。
    sol::object themeObject = skinTable["theme"];
    // 单字符串代表旧版唯一主题，对亮色与暗色外观使用同一 ID。
    if ( themeObject.is<std::string>() ) {
        const std::string legacyTheme = themeObject.as<std::string>();
        m_data.defaultThemes.light    = legacyTheme;
        m_data.defaultThemes.dark     = legacyTheme;
    } else if ( themeObject.is<sol::table>() ) {
        // 新格式可分别声明亮暗主题；只声明一侧时复制到另一侧作后备。
        sol::table themeTable = themeObject.as<sol::table>();
        const sol::optional<std::string> lightTheme = themeTable["light"];
        const sol::optional<std::string> darkTheme  = themeTable["dark"];

        if ( lightTheme ) {
            // 非空性由主题注册表后续验证，此处只保存皮肤推荐稳定 ID。
            m_data.defaultThemes.light = lightTheme.value();
        }
        if ( darkTheme ) {
            m_data.defaultThemes.dark = darkTheme.value();
        }
        // 补齐缺失分支保证自动主题切换始终得到可用主题 ID。
        if ( lightTheme && !darkTheme ) {
            m_data.defaultThemes.dark = lightTheme.value();
        } else if ( darkTheme && !lightTheme ) {
            m_data.defaultThemes.light = darkTheme.value();
        }
    }

    // assets 支持嵌套路径和序列描述，递归解析后统一分配纹理 ID。
    sol::table assetsTable = skinTable["assets"];
    if ( assetsTable.valid() ) {
        // 空前缀使顶层键不携带多余分隔符。
        parseAssetsRecursive(assetsTable, "");
        XINFO("Assets parsed: {} asset(s), {} sequence(s)",
              m_data.assetPaths.size(),
              m_data.effectSequences.size());

        // 统一从 TextureID::EffectStart 对应的 1000 分配连续序列帧 ID。
        // 渲染层只消费 startId 和帧数，不需要硬编码各皮肤的偏移。
        uint32_t                 currentId = 1000;
        std::vector<std::string> seqKeys;
        // 先收集键再排序，unordered_map 的遍历顺序不能参与 ID 契约。
        for ( const auto& [key, seq] : m_data.effectSequences ) {
            // seq 此轮只用于结构化绑定，帧数在分配循环中读取。
            seqKeys.push_back(key);
        }
        // 稳定键序保证跨平台及重复加载时同一序列获得同一起始 ID。
        std::sort(seqKeys.begin(), seqKeys.end());

        // 每个序列占用与帧数相同的连续 ID 区间。
        for ( const auto& key : seqKeys ) {
            m_data.effectSequences[key].startId = currentId;
            uint32_t frameCount                 = static_cast<uint32_t>(
                m_data.effectSequences[key].frames.size());
            XINFO("Assigning sequence {} start ID: {}, frames: {}",
                  key,
                  currentId,
                  frameCount);
            // 下一序列紧接当前末帧，避免空洞并维持唯一性。
            currentId += frameCount;
        }
    }

    sol::table audiosTable = skinTable["audios"];
    // audios 不存在时 sol table 无效，解析分支安全跳过并保持空映射。
    // 音频路径与起音延迟共同重建，旧皮肤数据不得残留到新加载结果。
    m_data.audioPaths.clear();
    m_data.audioLeadInSeconds.clear();
    if ( audiosTable.valid() ) {
        parseAudiosRecursive(audiosTable, "");
    }

    // effects 是可选扩展；缺失时 SkinData 构造默认值直接生效。
    sol::optional<sol::table> effectsTableOpt = skinTable["effects"];
    if ( effectsTableOpt ) {
        // 所有效果字段仅改变 SkinData 值，不保留对 Lua table 的长期引用。
        sol::table effectsTable = effectsTableOpt.value();
        // hit_effect.layout 只接受 fixed 与 track_fill 两种稳定标识。
        sol::optional<sol::table> hitEffectOpt = effectsTable["hit_effect"];
        if ( hitEffectOpt ) {
            const std::string layout =
                hitEffectOpt.value()["layout"].get_or<std::string>("fixed");
            // 只有明确 track_fill 才覆盖默认 Fixed；未知标识仅记录警告。
            if ( layout == "track_fill" ) {
                m_data.effects.hitEffect.layout =
                    HitEffectLayoutMode::TrackFill;
            } else if ( layout != "fixed" ) {
                XWARN("未知的打击特效布局模式 '{}', 已回退到 fixed", layout);
            }
        }

        // 混合规则只在加载时解析；序列缓存布尔值供渲染热路径直接读取。
        if ( hitEffectOpt ) {
            // blend 按已经解析的序列键查询，未声明键默认 alpha 覆盖混合。
            sol::optional<sol::table> blendOpt = hitEffectOpt.value()["blend"];
            if ( blendOpt ) {
                // 只遍历已经成功展开的序列，未知 blend 键不会创建空序列。
                for ( auto& [key, sequence] : m_data.effectSequences ) {
                    const auto mode =
                        blendOpt.value()[key].get_or<std::string>("alpha");
                    // 热路径只读取布尔值，不再比较字符串或访问 Lua 状态。
                    sequence.additiveBlend = mode == "additive";
                    // 未声明或拼写错误的模式保持旧皮肤覆盖语义。
                    if ( mode != "alpha" && mode != "additive" ) {
                        XWARN("未知的特效混合模式 '{}': {}，已回退到 alpha",
                              mode,
                              key);
                    }
                }
            }
        }

        // 发光轮次与强度缺失时使用 SkinData 的历史默认参数。
        sol::optional<sol::table> glowOpt = effectsTable["glow"];
        if ( glowOpt ) {
            // 数值类型不匹配时 sol2 get_or 使用产品默认值。
            m_data.effects.glow.passes = glowOpt.value()["passes"].get_or(8);
            m_data.effects.glow.intensity =
                glowOpt.value()["intensity"].get_or(1.0f);
        }
    }

    // 旧版内置 IVM 资源曾将发光完全关闭。资源包尚未更新时也要恢复悬浮与
    // 选中反馈；限定内置目录和主题名，避免改变其他皮肤显式关闭发光的语义。
    if ( m_data.themeName == "IVM" && skinPath.filename() == "ivm" &&
         m_data.effects.glow.passes <= 0 &&
         m_data.effects.glow.intensity <= 0.0F ) {
        m_data.effects.glow.passes    = IVM_INTERACTION_GLOW_PASSES;
        m_data.effects.glow.intensity = IVM_INTERACTION_GLOW_INTENSITY;
        XINFO("已迁移旧版 IVM 交互发光配置");
    }
    // 任一条件不满足都尊重皮肤显式值，不把内置迁移扩散到第三方皮肤。

    // canvases_2d 可选；未声明时渲染器通过空配置回退基础画布。
    sol::optional<sol::table> canvasesTableOpt = skinTable["canvases_2d"];
    if ( canvasesTableOpt ) {
        // CanvasConfig 使用值语义保存，加载结束后不依赖局部 Lua 状态。
        sol::table canvasesTable = canvasesTableOpt.value();
        // 每个条目描述一个具名画布及其 shader 模块映射。
        for ( const auto& kv : canvasesTable ) {
            // 外层键仅作 Lua 组织用途，运行时以内部 name 作为稳定查找键。
            sol::table canvasData = kv.second.as<sol::table>();

            SkinData::CanvasConfig config;
            // 空 name 的不完整配置不会发布到 canvas_configs。
            config.canvas_name = canvasData["name"].get_or<std::string>("");

            // shader_modules 按 main、effect 等逻辑名称映射到资源路径。
            sol::optional<sol::table> modulesOpt = canvasData["shader_modules"];
            if ( modulesOpt ) {
                sol::table modulesTable = modulesOpt.value();
                // 模块键和值均由皮肤契约要求为字符串。
                for ( const auto& modKv : modulesTable ) {
                    std::string modKey =
                        modKv.first.as<std::string>();  // "main", "effect"
                    std::string rpath = modKv.second.as<std::string>();
                    // 所有 shader 路径仍经统一 resources 根解析。
                    config.canvas_shader_modules[modKey] = makeResPath(rpath);
                }
            }
            // 没有 shader_modules 的具名画布仍可发布，消费方决定默认模块。

            // 同名后项覆盖前项，最终 map 只保留每个稳定画布名称一份配置。
            if ( !config.canvas_name.empty() ) {
                m_data.canvas_configs[config.canvas_name] = config;
            }
        }
    }

    // layout 的字符串、数值和布尔叶子统一扁平化为点分字符串键。
    sol::table layoutTable = skinTable["layout"];
    if ( layoutTable.valid() ) {
        // 空前缀从顶层字段名开始，不写入多余的 layout. 前缀。
        parseLayoutRecursive(layoutTable, "");
    }
    // layout 缺失时查询返回空字符串，交由每个 UI 消费点使用默认值。

    // fontsize 复用布局字符串存储，但显式添加命名空间前缀避免键冲突。
    sol::optional<sol::table> fontsizeTableOpt = skinTable["fontsize"];
    if ( fontsizeTableOpt ) {
        parseLayoutRecursive(fontsizeTableOpt.value(), "fontsize");
    }

    // values 仅接收数值叶子，供运行时按键读取皮肤参数。
    sol::optional<sol::table> valuesTableOpt = skinTable["values"];
    if ( valuesTableOpt ) {
        parseValuesRecursive(valuesTableOpt.value(), "");
    }
    // 非数值 values 叶子被忽略，不会覆盖调用方提供的默认参数。

    // beat_divisors 使用 Lua 一基数组，并过滤非整数扩展项。
    sol::optional<sol::table> divisorsTableOpt = skinTable["beat_divisors"];
    m_data.commonDivisors.clear();
    if ( divisorsTableOpt ) {
        sol::table divisorsTable = divisorsTableOpt.value();
        // 保持皮肤声明条目，再在解析结束后统一排序。
        for ( size_t i = 1; i <= divisorsTable.size(); ++i ) {
            sol::object entry = divisorsTable[i];
            if ( entry.is<int>() ) {
                m_data.commonDivisors.push_back(entry.as<int>());
            }
        }
    }
    // 缺失或没有有效整数时恢复编辑器常用分拍集合。
    if ( m_data.commonDivisors.empty() ) {
        m_data.commonDivisors = { 1, 2, 3, 4, 6, 8, 12, 16 };
    }
    // 这里不去重，保留皮肤声明；消费者如需集合语义应按自身规则处理。
    // 升序保证工具栏展示及后续查找不依赖 Lua 表顺序。
    std::sort(m_data.commonDivisors.begin(), m_data.commonDivisors.end());

    XINFO("Skin loaded: " + m_data.themeName);
    // 成功返回前所有 SkinData 字段已脱离 Lua 状态，可安全销毁局部 state。
    return true;
}

/// @brief 递归解析音频配置表。
/// @param currentTable 当前处理的 Lua 表。
/// @param prefix 键前缀。
/// @note 字符串叶子表示路径，含 path 字段的表可额外声明起音延迟。
/// @warning 皮肤加载路径递归遍历 Lua 表并分配字符串，不可在音频回调调用。
void SkinManager::parseAudiosRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    // 每层键通过点号拼接，形成与 getAudioPath 使用一致的稳定 ID。
    for ( const auto& kv : currentTable ) {
        // 递归函数只处理当前皮肤新建数据，不与旧皮肤映射合并。
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        // 简写字符串只声明路径，同时清除同键可能残留的延迟。
        if ( value.is<std::string>() ) {
            std::string rpath = value.as<std::string>();
            // 路径统一锚定当前皮肤 resources，避免受进程工作目录影响。
            m_data.audioPaths[fullKey] =
                (m_data.skinPath / Config::utf8ToPath("resources") /
                 Config::utf8ToPath(rpath))
                    .lexically_normal();
            m_data.audioLeadInSeconds.erase(fullKey);
        } else if ( value.is<sol::table>() ) {
            // 表既可能是带元数据的叶子，也可能是继续嵌套的命名空间。
            sol::table table = value.as<sol::table>();
            // path 字段存在即按叶子解析，不再递归其其他元数据键。
            if ( sol::optional<std::string> path = table["path"] ) {
                m_data.audioPaths[fullKey] =
                    (m_data.skinPath / Config::utf8ToPath("resources") /
                     Config::utf8ToPath(path.value()))
                        .lexically_normal();

                // 起音延迟兼容 snake_case 新字段和 camelCase 旧字段。
                double leadInMs = 0.0;
                if ( sol::optional<double> valueMs = table["lead_in_ms"] ) {
                    leadInMs = valueMs.value();
                } else if ( sol::optional<double> valueMs =
                                table["leadInMs"] ) {
                    leadInMs = valueMs.value();
                }
                // 负延迟按零处理，并在加载时转换为音频层使用的秒。
                m_data.audioLeadInSeconds[fullKey] =
                    std::max(0.0, leadInMs) / 1000.0;
            } else {
                // 没有 path 的表继续展开，允许 audios 任意分组嵌套。
                parseAudiosRecursive(table, fullKey);
            }
        }
    }
}

/// @brief 递归解析普通资产路径与序列帧范围表达式。
/// @param currentTable 当前正在处理的 Lua 资产表。
/// @param prefix 当前点分键前缀。
/// @note 字符串中的 [start .. end] 语法展开为有序特效帧列表。
/// @warning 皮肤加载路径会使用正则并分配路径，不可在渲染热路径调用。
void SkinManager::parseAssetsRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    // 每个叶子生成普通资产或特效序列，子表只扩展键命名空间。
    for ( const auto& kv : currentTable ) {
        // 皮肤资产表契约要求字符串键，完整键供运行时稳定查找。
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        // 点分格式保留嵌套语义，又避免运行时遍历 Lua table。
        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        if ( value.is<std::string>() ) {
            // 同一完整键的后声明叶子按 map 赋值语义覆盖前值。
            // 字符串叶子可能是单文件路径，也可能包含序列范围。
            std::string rpath = value.as<std::string>();

            // 序列语法分离路径前缀、整数起止值和文件后缀。
            // 示例为 image/note/effect/flick/[1 .. 16].png。
            std::regex  seqRegex(R"(^(.*)\[(\d+)\s*\.\.\s*(\d+)\](.*)$)");
            std::smatch match;
            if ( std::regex_match(rpath, match, seqRegex) ) {
                // 局部 prefix 是资源路径前缀，与递归键前缀职责不同。
                std::string prefix    = match[1].str();
                int         start     = 0;
                int         end       = 0;
                std::string suffix    = match[4].str();
                const auto  startText = match[2].str();
                const auto  endText   = match[3].str();
                // from_chars 不分配且明确报告未完整消费的非法数字文本。
                const auto [startPtr, startEc] =
                    std::from_chars(startText.data(),
                                    startText.data() + startText.size(),
                                    start);
                const auto [endPtr, endEc] = std::from_chars(
                    endText.data(), endText.data() + endText.size(), end);
                if ( startEc != std::errc{} ||
                     startPtr != startText.data() + startText.size() ||
                     endEc != std::errc{} ||
                     endPtr != endText.data() + endText.size() ) {
                    XWARN("皮肤序列帧范围解析失败: {}", rpath);
                    continue;
                }

                // 限制展开帧数，防止恶意范围造成过量路径分配。
                constexpr int MAX_EFFECT_SEQUENCE_FRAMES = 4096;
                const int     frameCount =
                    (start <= end) ? (end - start + 1) : (start - end + 1);
                if ( frameCount > MAX_EFFECT_SEQUENCE_FRAMES ) {
                    XWARN("皮肤序列帧数量过大，已跳过: {}", rpath);
                    continue;
                }

                // 支持升序和降序范围，输出顺序与皮肤声明方向一致。
                SkinData::EffectSequence seq;
                int                      step    = (start <= end) ? 1 : -1;
                int                      current = start;
                // 每轮生成一个经 resources 根锚定和词法规范化的路径。
                while ( true ) {
                    std::string framePath =
                        prefix + std::to_string(current) + suffix;
                    seq.frames.push_back(
                        (m_data.skinPath / Config::utf8ToPath("resources") /
                         Config::utf8ToPath(framePath))
                            .lexically_normal());
                    // 在更新前检查末端，避免整数跨越范围后多生成一帧。
                    if ( current == end ) break;
                    current += step;
                }
                // 完整展开后一次性发布序列，解析失败不会留下部分帧。
                m_data.effectSequences[fullKey] = seq;
            } else {
                // 普通资产只保存规范化路径，文件存在性由资源消费或测试验证。
                m_data.assetPaths[fullKey] =
                    (m_data.skinPath / Config::utf8ToPath("resources") /
                     Config::utf8ToPath(rpath))
                        .lexically_normal();
            }
        } else if ( value.is<sol::table>() ) {
            // 子表递归扩展点分键，保持皮肤可按职责组织资源。
            parseAssetsRecursive(value.as<sol::table>(), fullKey);
        }
    }
}

/// @brief 递归解析嵌套颜色表为点分颜色键。
/// @param currentTable 当前处理的 Lua 颜色表。
/// @param prefix 当前点分键前缀，例如 preview。
/// @note 数字数组至少需要 RGB 三项，第四项 Alpha 缺失时默认为一。
/// @warning 皮肤加载路径会创建临时向量，不可在渲染热路径调用。
void SkinManager::parseColorsRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    // 每个表项只可能成为颜色叶子或继续递归的命名空间。
    for ( const auto& kv : currentTable ) {
        // 字符串键按原文保存，运行时查询使用相同点分命名约定。
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        // 递归前缀为空时避免产生开头点号。
        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        if ( value.is<sol::table>() ) {
            // 非 table 颜色字段被忽略，不向 colors 插入错误哨兵。
            // Lua table 可能同时表示数组或对象，以第一项数值类型区分。
            sol::table valTable = value.as<sol::table>();
            // float 与 double 都接受，兼容 LuaJIT 数值桥接的具体表示。
            sol::object firstElem = valTable[1];
            if ( firstElem.is<float>() || firstElem.is<double>() ) {
                // 数组转换只发生在加载阶段，随后保存为固定大小 Color。
                std::vector<float> val = valTable.as<std::vector<float>>();
                // 少于三个分量的表不是有效颜色，也不作为嵌套对象继续解析。
                if ( val.size() >= 3 ) {
                    m_data.colors[fullKey] = {
                        val[0], val[1], val[2], val.size() > 3 ? val[3] : 1.0f
                    };
                }
            } else {
                // 非数值首项按嵌套命名空间继续展开。
                parseColorsRecursive(valTable, fullKey);
            }
        }
    }
}

/// @brief 递归解析布局表并把标量叶子保存为字符串。
/// @param currentTable 当前处理的 Lua 布局表。
/// @param prefix 当前点分键前缀，例如 side_bar。
/// @note 字符串、数字和布尔值统一进入 layoutConfigs 文本字典。
/// @warning 皮肤加载路径会递归和格式化数值，不可在 UI 每帧调用。
void SkinManager::parseLayoutRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    // 扁平字典让下游无需持有 Lua 状态即可按稳定键读取布局。
    for ( const auto& kv : currentTable ) {
        // 当前键与前缀组合后唯一标识该叶子。
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        // 示例键为 side_bar.width；调用方可显式传入 fontsize 前缀。
        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        if ( value.is<sol::table>() ) {
            // 子表不直接保存，只扩展点分命名空间。
            parseLayoutRecursive(value.as<sol::table>(), fullKey);
        } else {
            // 基本类型转换成统一文本，兼容既有字符串读取接口。
            if ( value.is<std::string>() ) {
                m_data.layoutConfigs[fullKey] = value.as<std::string>();
            } else if ( value.is<double>() ) {
                double val = value.as<double>();
                // 精确整数去除小数点，便于消费者使用整数文本语义。
                if ( val == static_cast<long long>(val) ) {
                    m_data.layoutConfigs[fullKey] =
                        std::to_string(static_cast<long long>(val));
                } else {
                    // 非整数保留 std::to_string 形式，解析精度由消费方决定。
                    m_data.layoutConfigs[fullKey] = std::to_string(val);
                }
            } else if ( value.is<bool>() ) {
                // 布尔值使用小写 Lua/JSON 风格文本，保持现有比较契约。
                m_data.layoutConfigs[fullKey] =
                    value.as<bool>() ? "true" : "false";
            }
        }
    }
}

/// @brief 递归解析通用数值表为点分浮点键。
/// @param currentTable 当前处理的 Lua 数值表。
/// @param prefix 当前点分键前缀。
/// @note 仅 double 叶子进入 values，其他标量类型按未声明处理。
/// @warning 只允许皮肤加载阶段调用，会递归遍历完整 values 表。
void SkinManager::parseValuesRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    // 子表扩展命名空间，数值叶子转换为运行时直接读取的 float。
    for ( const auto& kv : currentTable ) {
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        if ( value.is<sol::table>() ) {
            // 递归保持同一 m_data 目标，不构造中间嵌套容器。
            parseValuesRecursive(value.as<sol::table>(), fullKey);
        } else if ( value.is<double>() ) {
            // 皮肤数值最终用于 float 渲染参数，此处集中完成窄化。
            m_data.values[fullKey] = static_cast<float>(value.as<double>());
        }
    }
}

/// @brief 按稳定资源键查询字体文件路径。
/// @param key fonts 表中的点分或顶层键。
/// @return 命中时返回路径副本，否则记录错误并返回空路径。
/// @warning 运行期查询只做哈希查找；调用方应缓存高频结果。
std::filesystem::path SkinManager::getFontPath(const std::string& key)
{
    // find 避免缺失键通过 operator[] 意外插入空路径。
    if ( auto fontPathit = m_data.fontPaths.find(key);
         fontPathit != m_data.fontPaths.end() ) {
        return fontPathit->second;
    }
    XERROR("Asset key not found: " + key);
    // 空路径不会意外指向进程当前目录中的同名资源。
    return "";
}

/// @brief 按稳定资源键查询音频文件路径。
/// @param key audios 表展开后的点分键。
/// @return 命中时返回路径副本，否则记录错误并返回空路径。
/// @warning 音频回调不得调用该接口或记录日志，应在加载阶段缓存结果。
std::filesystem::path SkinManager::getAudioPath(const std::string& key)
{
    // 缺失查询不修改 audioPaths，避免错误键污染后续统计。
    if ( auto audioPathit = m_data.audioPaths.find(key);
         audioPathit != m_data.audioPaths.end() ) {
        return audioPathit->second;
    }
    XERROR("Audio key not found: " + key);
    // 缺失音频显式返回空路径，调用方可跳过可选音效。
    return "";
}

/// @brief 获取音效文件开头到有效出声点的延迟。
/// @param key 音频 ID。
/// @return 延迟，单位为秒；不存在时返回 0。
/// @warning 音频热路径只执行哈希查找，不分配或访问文件系统。
double SkinManager::getAudioLeadInSeconds(const std::string& key) const
{
    // 未声明延迟的简写音频与未知键都使用零秒。
    if ( auto it = m_data.audioLeadInSeconds.find(key);
         it != m_data.audioLeadInSeconds.end() ) {
        return it->second;
    }
    return 0.0;
}

/// @brief 按稳定资源键查询普通资产路径。
/// @param key assets 表展开后的点分键。
/// @return 命中时返回路径副本，否则记录错误并返回空路径。
/// @warning 渲染路径应在资源加载阶段缓存纹理，不应每帧复制路径。
std::filesystem::path SkinManager::getAssetPath(const std::string& key)
{
    // effectSequences 使用独立查询接口，不会混入普通资产字典。
    if ( auto assetPathit = m_data.assetPaths.find(key);
         assetPathit != m_data.assetPaths.end() ) {
        return assetPathit->second;
    }
    XERROR("Asset key not found: " + key);
    // 仅在缺失分支记录一次调用日志，高频调用方应预先验证键。
    return "";
}

/// @brief 查询已经展开并分配 ID 的特效序列。
/// @param key assets 表中的序列点分键。
/// @return 命中时返回 SkinData 内部观察指针，否则返回 nullptr。
/// @warning 指针只在下一次 loadSkin 前稳定，调用方不得跨皮肤重载持有。
const SkinData::EffectSequence* SkinManager::getEffectSequence(
    const std::string& key) const
{
    // find 不插入缺失序列，允许调用方用 nullptr 表达可选特效。
    if ( auto it = m_data.effectSequences.find(key);
         it != m_data.effectSequences.end() ) {
        return &it->second;
    }
    return nullptr;
}

/// @brief 获取指定名称的画布着色器配置。
/// @param canvasName 皮肤声明的稳定画布名称。
/// @return 精确命中、Basic2DCanvas 后备或空哨兵配置的引用。
/// @warning 返回引用只在下一次皮肤重载前有效。
const SkinData::CanvasConfig& SkinManager::getCanvasConfig(
    const std::string& canvasName)
{
    // 优先使用调用方请求的专用画布配置。
    if ( auto canvas_config_it = m_data.canvas_configs.find(canvasName);
         canvas_config_it != m_data.canvas_configs.end() ) {
        return canvas_config_it->second;
    }
    // 找不到指定配置时回退 Basic2DCanvas，避免可选画布使渲染初始化失败。
    if ( canvasName != "Basic2DCanvas" ) {
        if ( auto canvas_config_it =
                 m_data.canvas_configs.find("Basic2DCanvas");
             canvas_config_it != m_data.canvas_configs.end() ) {
            return canvas_config_it->second;
        }
    }
    XERROR("CanvasConfig key not found: " + canvasName);
    // null_canvas_config 由 SkinData 持有，返回引用不会悬空。
    return m_data.null_canvas_config;
}

/// @brief 查询扁平化的布局文本值。
/// @param key layout 或 fontsize 表展开后的点分键。
/// @return 命中时返回文本副本，否则返回空字符串。
/// @warning UI 高频消费者应缓存解析后的数值或布尔结果。
std::string SkinManager::getLayoutConfig(const std::string& key)
{
    // 缺失布局是合法的皮肤回退信号，不记录错误日志。
    if ( auto layout_config_it = m_data.layoutConfigs.find(key);
         layout_config_it != m_data.layoutConfigs.end() ) {
        return layout_config_it->second;
    }
    return {};
}

/// @brief 查询通用皮肤数值并提供调用方默认值。
/// @param key values 表展开后的点分键。
/// @param defaultValue 缺失时使用的业务默认值。
/// @return 已声明值或 defaultValue。
/// @warning 渲染热路径仅执行哈希查找，不分配或记录日志。
float SkinManager::getValue(const std::string& key, float defaultValue)
{
    // 默认值由具体消费场景决定，加载器不为未知键猜测语义。
    if ( auto it = m_data.values.find(key); it != m_data.values.end() ) {
        return it->second;
    }
    return defaultValue;
}

/// @brief 查询皮肤颜色并以高可见错误色标记缺失键。
/// @param key colors 表展开后的点分键。
/// @return 已声明 RGBA，或缺失时返回完全不透明洋红色。
/// @warning 渲染热路径执行哈希查找；缺失分支不记录逐帧日志。
Color SkinManager::getColor(const std::string& key)
{
    // 先 find 再索引保持既有返回值逻辑，缺失时不向 map 插入。
    if ( m_data.colors.find(key) != m_data.colors.end() ) {
        return m_data.colors[key];
    }
    // 洋红色在常见皮肤配色中醒目，便于定位缺失资源键。
    return { 1.0f, 0.0f, 1.0f, 1.0f };
}

/// @brief 查询 UI 层已绑定的运行期字体指针。
/// @param key fonts 表使用的字体用途键。
/// @return 命中时返回非拥有 ImFont 指针，否则返回 nullptr。
/// @warning 指针由 ImGui 字体图集拥有，重建字体前必须清空绑定。
ImFont* SkinManager::getFont(const std::string& key)
{
    // runtimeFonts 与文件路径分离，皮肤加载不直接创建 ImGui 资源。
    if ( auto it = m_data.runtimeFonts.find(key);
         it != m_data.runtimeFonts.end() ) {
        return it->second;
    }
    return nullptr;
}

/// @brief 绑定由 UI 字体图集创建的运行期字体。
/// @param key 字体用途键。
/// @param font 非拥有 ImFont 指针，可由调用方显式更新。
/// @warning 仅在低频字体图集构建阶段调用，生命周期由外部保证。
void SkinManager::setFont(const std::string& key, ImFont* font)
{
    // 同键覆盖支持字体图集重建后刷新观察指针。
    m_data.runtimeFonts[key] = font;
}

/// @brief 清空全部非拥有运行期字体绑定。
/// @note 不修改字体文件路径，后续可从同一皮肤重新构建字体图集。
/// @warning 必须在外部销毁或重建 ImGui 字体图集前调用。
void SkinManager::clearRuntimeFonts()
{
    // 容器只保存观察指针，清空不会释放任何 ImFont 对象。
    m_data.runtimeFonts.clear();
}

}  // namespace Config
}  // namespace MMM
