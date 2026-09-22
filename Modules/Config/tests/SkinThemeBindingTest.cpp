#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace
{
// 本测试以生产 SkinManager 加载临时和仓库皮肤，验证配置与资源契约。
/// @brief 记录测试断言失败。
/// @param condition 待验证条件。
/// @param message 条件失败时写入日志的说明。
/// @return 条件原值。
/// @note 成功不输出逐项日志，保持资源批量验证结果简洁。
bool check(bool condition, std::string_view message)
{
    // 断言继续返回原值，调用方可用 &= 汇总多个独立资源检查。
    if ( !condition ) XERROR("SkinThemeBindingTest failed: {}", message);
    return condition;
}

/// @brief 判断两个皮肤颜色是否完全一致。
/// @param lhs 左侧颜色。
/// @param rhs 右侧颜色。
/// @return 四个颜色通道均相同时返回 true。
/// @note Lua 常量无需近似比较，精确相等能发现通道映射错误。
bool sameColor(const MMM::Config::Color& lhs, const MMM::Config::Color& rhs)
{
    // 皮肤配置值来自 Lua 常量，精确比较可发现任一通道被错误替换。
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

/// @brief 写入用于皮肤主题解析测试的最小 Lua 文件。
/// @param path 输出文件路径。
/// @param themeExpression theme 字段对应的 Lua 表达式。
/// @param textureScalesExpression 可选纹理缩放表，nil 模拟旧皮肤缺省。
/// @param assetsExpression 可选资产表，用于验证序列帧的倍率展开。
/// @return 文件成功写入时返回 true。
/// @note 其余必需表保持固定，调用方仅覆写当前测试需要的字段。
/// @warning 仅写入 CTest 提供的专用输出目录。
bool writeTestSkin(const std::filesystem::path& path,
                   std::string_view             themeExpression,
                   std::string_view             textureScalesExpression = "nil",
                   std::string_view             assetsExpression        = "{}")
{
    // 二进制模式保持脚本字节不受平台换行转换影响。
    std::ofstream file(path, std::ios::binary);
    // 文件由 main 放在已创建的专用目录，不让 helper 隐式改变目录结构。
    if ( !file ) return false;
    // 元数据和各资源表满足生产加载器的最小结构要求。
    file << "return {\n"
            "  meta = { name = 'Theme Test', author = 'Test', version = "
            "'1.0' },\n"
            "  langs = {},\n"
            "  fonts = {},\n"
            "  audios = {},\n"
            "  layout = {},\n"
            "  assets = "
         << assetsExpression
         << ",\n  texture_scales = " << textureScalesExpression
         << ",\n"
            "  theme = "
         << themeExpression << "\n}\n";
    // 流状态同时捕获写入和刷新阶段的失败。
    return file.good();
}

/// @brief 验证任意纹理键、非法倍率和跨皮肤重载时的缓存隔离。
/// @param path 专用输出目录中的临时皮肤路径。
/// @param translationsRoot 应用默认翻译目录。
/// @return 全部有效值、缺省值与缓存边界满足契约时返回 true。
/// @warning 只写入临时夹具；不读取图片、不调用图形设备。
bool verifyTextureScales(const std::filesystem::path& path,
                         const std::filesystem::path& translationsRoot)
{
    auto& manager = MMM::Config::SkinManager::instance();
    // 使用不需要实际图片的三帧序列，隔离配置解析与纹理加载。
    // 两个序列共享文件路径但倍率独立，保护按逻辑 ID 而非路径索引的约定。
    constexpr std::string_view ASSETS =
        "{ custom = { enlarged = 'frame[1 .. 3].png', "
        "reduced = 'frame[1 .. 3].png' } }";
    // Lua 运行时不开放 math 库，使用浮点除法构造非有限值。
    // 正负、零和非有限数值分别覆盖可见性、翻转和顶点有效性约束。
    // 1e100 在 Lua double 中有效，但不能安全窄化为有限 float。
    // 1e-100 验证下溢保护，不能让合法 double 在渲染中变成零倍率。
    // 数字字符串故意保留为字符串，不能因 Lua 隐式转换而被接受。
    // 空键、数字键和嵌套表分别保护稳定资产键与扁平配置契约。
    constexpr std::string_view SCALES =
        "{ ['custom.enlarged'] = 1.6, ['custom.reduced'] = 0.5, "
        "['cursor'] = 2, ['extension.texture'] = 0.75, "
        "['negative'] = -1, ['zero'] = 0, ['infinite'] = 1/0, "
        "['nan'] = 0/0, ['overflow'] = 1e100, ['underflow'] = 1e-100, "
        "['string'] = '1.6', ['boolean'] = true, ['nested'] = { value = 2 }, "
        "[''] = 3, [42] = 2 }";
    // 写入失败时立即退出，不能误把上一份脚本加载成功当作当前结果。
    if ( !check(writeTestSkin(path, "'DeepDark'", SCALES, ASSETS),
                "纹理缩放夹具写入失败") ||
         !check(
             manager.loadSkin(MMM::Config::pathToUtf8(path), translationsRoot),
             "纹理缩放夹具应成功加载") ) {
        return false;
    }
    // 未出现在 assets 中的合法键也应保留，允许扩展渲染器自行消费。
    // 比较 float 常量而非 double，断言关注保存后的公开数据类型。
    // 同时检测放大、缩小和整数值，不能只支持大于一的特效专用参数。
    bool ok = check(manager.getTextureScale("extension.texture") == 0.75F &&
                        manager.getTextureScale("cursor") == 2.0F &&
                        manager.getTextureScale("custom.enlarged") == 1.6F &&
                        manager.getTextureScale("custom.reduced") == 0.5F,
                    "普通纹理与序列应支持独立放大和缩小");
    // 非法声明不应部分进入字典，避免查询端反复修正 NaN 或溢出值。
    ok &= check(manager.getData().textureScales.size() == 4U,
                "只应保存非空字符串键对应的正有限 float 倍率");
    // 每个非法键都通过公开查询再验证一次，不依赖内部计数碰巧正确。
    // nested.value 不得出现，用于发现错误复用递归数值解析器的情况。
    // 数字键对应的字符串 42 也必须缺失，避免隐式字符串化改变配置含义。
    for ( const auto* key : { "negative",
                              "zero",
                              "infinite",
                              "nan",
                              "overflow",
                              "underflow",
                              "string",
                              "boolean",
                              "nested",
                              "nested.value",
                              "",
                              "42",
                              "missing" } ) {
        ok &= check(manager.getTextureScale(key) == 1.0F,
                    "非法或缺失倍率必须回退到 1");
    }
    // 从实际分配的起始 ID 验证整个序列，不硬编码动画排序和偏移。
    // 共享路径的两组序列必须保留不同倍率，否则 Hold/Flick 无法独立调整。
    // 覆盖中间帧可发现只给起始 ID 或末帧登记倍率的实现遗漏。
    for ( const auto* key : { "custom.enlarged", "custom.reduced" } ) {
        const auto* sequence = manager.getEffectSequence(key);
        ok &= check(sequence && sequence->frames.size() == 3U,
                    "缩放测试序列必须完整展开");
        // 先记失败再跳过空观察指针，让其它序列仍能输出独立诊断。
        if ( !sequence ) continue;
        for ( std::uint32_t frame = 0; frame < sequence->frames.size();
              ++frame ) {
            ok &= check(
                manager.getEffectTextureScale(sequence->startId + frame) ==
                    manager.getTextureScale(key),
                "每个序列帧必须缓存所属序列的倍率");
        }
    }
    // 未分配数值 ID 不应碰巧继承首帧或最近序列的倍率。
    // 零位于固定纹理范围，高位值位于测试序列范围外，覆盖两侧缺省。
    ok &= check(manager.getEffectTextureScale(0U) == 1.0F &&
                    manager.getEffectTextureScale(999999U) == 1.0F,
                "非序列 ID 应保持单位倍率");

    // 先载入相同序列但缺少缩放声明，再载入非法顶层值。
    // 保留相同 ID 能直接暴露旧皮肤数值缓存未清除的错误。
    // nil 对应历史皮肤未提供该字段，字符串/数字/布尔均不是合法表。
    // 无效可选字段不应导致整套皮肤失效，其它资源仍应按原配置加载。
    for ( const auto* expression : { "nil", "'invalid'", "42", "true" } ) {
        if ( !check(writeTestSkin(path, "'DeepDark'", expression, ASSETS),
                    "缺省缩放夹具写入失败") ||
             !check(manager.loadSkin(MMM::Config::pathToUtf8(path),
                                     translationsRoot),
                    "缺省或非法顶层缩放配置不应使皮肤加载失败") ) {
            return false;
        }
        // 字符串键与数值帧键两个缓存都必须随皮肤数据一并重建。
        // 单查纹理键不能发现旧帧 ID 泄漏，单查帧也不能发现旧光标倍率。
        // 单位倍率不要求展开存储，以免没有启用此功能的皮肤增加无用容量。
        ok &= check(manager.getData().textureScales.empty() &&
                        manager.getData().effectTextureScales.empty() &&
                        manager.getTextureScale("cursor") == 1.0F &&
                        manager.getEffectTextureScale(1000U) == 1.0F,
                    "重载旧皮肤必须清除全部自定义缩放");
    }
    // 每次加载均替换单例状态，调用方后续检查应重新加载目标皮肤。
    return ok;
}

/// @brief 写入发光被关闭的旧版内置 IVM 测试皮肤。
/// @param path 输出文件路径；父目录必须使用内置皮肤目录名 ivm。
/// @return 文件成功写入时返回 true。
/// @note 名称、父目录和全零发光共同触发限定迁移条件。
/// @warning 夹具只模拟旧配置，不复制或修改仓库内置皮肤。
bool writeLegacyIvmSkin(const std::filesystem::path& path)
{
    // 父目录由 main 预先创建为 ivm，文件只负责写最小脚本。
    std::ofstream file(path, std::ios::binary);
    // 每次测试开始会清空输出根，因此无需追加或覆盖旧夹具内容。
    if ( !file ) return false;
    // theme 和 meta.name 都声明 IVM，匹配生产迁移的身份约束。
    file << "return {\n"
            "  meta = { name = 'IVM', author = 'Test', version = '1.0' },\n"
            "  langs = {},\n"
            "  fonts = {},\n"
            "  assets = {},\n"
            "  audios = {},\n"
            "  layout = {},\n"
            "  theme = 'IVM',\n"
            // 两个发光字段均为零，区别于第三方皮肤单独关闭某一参数。
            "  effects = { glow = { passes = 0, intensity = 0.0 } }\n"
            "}\n";
    return file.good();
}

/// @brief 加载测试皮肤并核对亮暗主题绑定。
/// @param path 测试皮肤入口路径。
/// @param expectedLight 期望亮色主题。
/// @param expectedDark 期望暗色主题。
/// @return 加载与两个分支断言均成功时返回 true。
/// @note 同时验证未声明特效布局和帧率的旧皮肤默认值。
/// @warning 使用单例加载会替换当前测试进程的皮肤状态。
bool verifyThemeBinding(const std::filesystem::path& path,
                        std::string_view             expectedLight,
                        std::string_view             expectedDark,
                        const std::filesystem::path& translationsRoot)
{
    // 每个调用都重新加载独立脚本，避免上一个主题绑定残留。
    auto& skinManager = MMM::Config::SkinManager::instance();
    bool  ok          = check(
        skinManager.loadSkin(MMM::Config::pathToUtf8(path), translationsRoot),
        "皮肤应成功加载");
    // 亮暗分支分别查询，覆盖成对表和单字符串兼容格式。
    ok &= check(skinManager.getDefaultTheme(
                    MMM::Config::SkinThemeAppearance::Light) == expectedLight,
                "亮色主题绑定不匹配");
    ok &= check(skinManager.getDefaultTheme(
                    MMM::Config::SkinThemeAppearance::Dark) == expectedDark,
                "暗色主题绑定不匹配");
    // 最小夹具不声明 effects，默认必须维持固定尺寸和 120 FPS。
    ok &= check(skinManager.getHitEffectLayoutMode() ==
                    MMM::Config::HitEffectLayoutMode::Fixed,
                "未声明布局的旧皮肤必须保持固定尺寸打击特效");
    ok &= check(skinManager.getEffectBaseFps() == 120.0F,
                "未声明序列帧速率的旧皮肤必须默认使用 120 FPS");
    // 默认值断言保护旧皮肤无需新增 meta 字段即可保持既有动画速度。
    // 所有断言都执行，返回值汇总该皮肤的完整主题绑定契约。
    return ok;
}

/// @brief 验证旧版内置 IVM 在资源文件未更新时仍恢复交互发光。
/// @param path 位于 ivm 目录内的旧版测试皮肤路径。
/// @return 悬浮与选中共用的发光配置已迁移时返回 true。
/// @note 迁移只需检查最终轮次和强度，不进入图形渲染路径。
/// @warning 使用临时 ivm 目录模拟身份，绝不覆盖正式资源。
bool verifyLegacyIvmGlowMigration(const std::filesystem::path& path,
                                  const std::filesystem::path& translationsRoot)
{
    // 生产加载器同时验证目录名、主题名和原始全零参数。
    auto& skinManager = MMM::Config::SkinManager::instance();
    bool  ok          = check(
        skinManager.loadSkin(MMM::Config::pathToUtf8(path), translationsRoot),
        "旧版 IVM 测试皮肤应成功加载");
    // 轮次和强度必须成对恢复，任一遗漏都会使视觉效果异常。
    ok &= check(skinManager.getGlowPasses() == 6,
                "旧版 IVM 必须恢复交互发光轮次");
    ok &= check(skinManager.getGlowIntensity() == 0.5F,
                "旧版 IVM 必须恢复交互发光强度");
    // 精确值来自公开迁移契约，测试不接受近似或只大于零。
    // check 已记录具体失败，调用方只消费汇总布尔值。
    return ok;
}

/// @brief 验证旧版应用配置能区分自动与手动主题偏好。
/// @return 兼容语义与序列化值均正确时返回 true。
/// @note 覆盖 Auto、普通内置主题、插件主题和 MmmDefault 迁移。
/// @warning 仅操作内存 JSON，不读取或保存个人 AppConfig。
bool verifyLegacyAppConfigSemantics()
{
    // Auto 是稳定哨兵，恢复后仍应表示跟随皮肤或系统的自动模式。
    MMM::Config::EditorSettings automaticSettings;
    const nlohmann::json        automaticJson{ { "theme", "Auto" } };
    from_json(automaticJson, automaticSettings);
    // 直接调用 ADL 函数，避免 AppConfig 单例和个人文件参与测试。

    // 普通非 Auto 字符串代表用户手动选择，必须原样保留。
    MMM::Config::EditorSettings manualSettings;
    const nlohmann::json        manualJson{ { "theme", "Moonlight" } };
    from_json(manualJson, manualSettings);
    // 手动主题不是内置白名单也应原样保留，允许插件稳定 ID。

    // 插件主题同时携带禁用插件 ID，验证两个相关字段独立往返。
    MMM::Config::EditorSettings pluginSettings;
    const nlohmann::json        pluginJson{
               { "theme", "example.twilight" },
               { "disabledPluginIds",
                 nlohmann::json::array({ "themes/example.lua" }) },
    };
    from_json(pluginJson, pluginSettings);
    // 禁用列表与主题值在同一 EditorSettings 读取中独立恢复。

    // 旧 MmmDefault 名称已被 Cecilia 取代，只在读取路径执行迁移。
    MMM::Config::EditorSettings legacyCeciliaSettings;
    const nlohmann::json        legacyCeciliaJson{ { "theme", "MmmDefault" } };
    from_json(legacyCeciliaJson, legacyCeciliaSettings);

    // 当前格式重新序列化自动与插件设置，确认不写出内部替代值。
    nlohmann::json serializedAutomatic;
    to_json(serializedAutomatic, automaticSettings);
    nlohmann::json serializedPlugin;
    to_json(serializedPlugin, pluginSettings);
    // 写出对象随后通过 value 读取，覆盖真实字段名称和类型。

    // 前四项检查读取语义，后四项检查当前格式写出语义。
    bool ok = check(automaticSettings.theme == MMM::Config::UI_THEME_AUTO_ID,
                    "旧版 Auto 应继续表示未手动指定主题");
    ok &= check(manualSettings.theme == "Moonlight",
                "旧版非 Auto 主题应继续表示用户手动选择");
    ok &= check(pluginSettings.theme == "example.twilight",
                "插件主题稳定 ID 应原样载入");
    ok &= check(pluginSettings.disabledPluginIds ==
                    std::vector<std::string>{ "themes/example.lua" },
                "禁用插件 ID 应原样载入");
    ok &= check(legacyCeciliaSettings.theme == "Cecilia",
                "旧版 MmmDefault 主题应迁移为 Cecilia");
    // 自动主题必须继续使用公开 Auto 文本，插件稳定 ID 则保持原文。
    ok &= check(serializedAutomatic.value("theme", std::string()) == "Auto",
                "自动主题序列化哨兵必须保持为 Auto");
    ok &= check(
        serializedPlugin.value("theme", std::string()) == "example.twilight",
        "插件主题稳定 ID 应原样序列化");
    ok &= check(serializedPlugin.value("disabledPluginIds",
                                       std::vector<std::string>()) ==
                    std::vector<std::string>{ "themes/example.lua" },
                "禁用插件 ID 应原样序列化");
    // 禁用列表的序列化也必须保持完整，避免插件主题升级后被重新启用。
    return ok;
}

/// @brief 从 PNG 文件头读取像素尺寸。
/// @param path PNG 文件路径。
/// @param width 成功时写入宽度。
/// @param height 成功时写入高度。
/// @return 文件包含有效 PNG 签名和 IHDR 尺寸时返回 true。
/// @note 只读取 PNG 固定头部，不解码像素或依赖图形库。
/// @warning 仅用于资源测试的低频文件读取。
bool readPngDimensions(const std::filesystem::path& path, std::uint32_t& width,
                       std::uint32_t& height)
{
    // 资源路径来自皮肤管理器，测试以二进制模式读取原始头部。
    std::ifstream file(path, std::ios::binary);
    if ( !file ) return false;

    // 前 24 字节覆盖签名、IHDR 类型以及宽高两个大端字段。
    std::array<std::uint8_t, 24> header{};
    file.read(reinterpret_cast<char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    // 截断或非普通文件不能提供完整尺寸头，立即返回失败。
    if ( file.gcount() != static_cast<std::streamsize>(header.size()) ) {
        return false;
    }

    // 同时验证 PNG 签名和首块 IHDR，避免在任意二进制中误读偏移。
    constexpr std::array<std::uint8_t, 8> PNG_SIGNATURE{ 0x89U, 0x50U, 0x4eU,
                                                         0x47U, 0x0dU, 0x0aU,
                                                         0x1aU, 0x0aU };
    if ( !std::equal(
             PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), header.begin()) ||
         header[12] != 'I' || header[13] != 'H' || header[14] != 'D' ||
         header[15] != 'R' ) {
        return false;
    }
    // 只有合法 PNG/IHDR 才进入大端宽高读取，避免越界解释随机字节。

    // PNG 以网络字节序存储尺寸，显式移位保持主机端无关。
    auto readBigEndian = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(header[offset]) << 24U |
               static_cast<std::uint32_t>(header[offset + 1U]) << 16U |
               static_cast<std::uint32_t>(header[offset + 2U]) << 8U |
               static_cast<std::uint32_t>(header[offset + 3U]);
    };
    // IHDR 宽高位于固定偏移，零尺寸不构成有效图像资源。
    width  = readBigEndian(16U);
    height = readBigEndian(20U);
    // 两个尺寸均需为正，零宽或零高资源不能满足纹理几何契约。
    return width > 0U && height > 0U;
}

/// @brief 验证内置皮肤声明完整且与正式物件不同的草稿区配色。
/// @param skinPath 内置皮肤入口路径。
/// @param translationsRoot 默认翻译资源目录。
/// @return 草稿物件和轨道颜色均存在且物件与正式区可区分时返回 true。
/// @note 先检查全部键存在，再读取颜色，避免缺失键的洋红后备干扰比较。
/// @warning 只读皮肤资源并替换测试进程内 SkinManager 状态。
bool verifyDraftPalette(const std::filesystem::path& skinPath,
                        const std::filesystem::path& translationsRoot)
{
    // 每套内置皮肤分别调用该 helper，确保两者都满足草稿视觉契约。
    auto& skinManager = MMM::Config::SkinManager::instance();
    bool  ok = check(skinManager.loadSkin(MMM::Config::pathToUtf8(skinPath),
                                         translationsRoot),
                    "内置皮肤草稿配色检查前应成功加载");
    // 键清单覆盖六类草稿物件和五类草稿轨道呈现。
    constexpr std::array<std::string_view, 11> DRAFT_COLOR_KEYS{
        "draft_notes.note_tap",      "draft_notes.note_head",
        "draft_notes.note_hold",     "draft_notes.note_end",
        "draft_notes.note_node",     "draft_notes.note_flick_arrow",
        "draft_tracks.texture_tint", "draft_tracks.overlay",
        "draft_tracks.border",       "draft_tracks.judgment_tint",
        "draft_tracks.label",
    };
    // 直接检查字典存在性，不调用 getColor 的错误色后备。
    const auto& colors = skinManager.getData().colors;
    for ( const auto key : DRAFT_COLOR_KEYS ) {
        // 每个键转为拥有字符串以匹配生产 unordered_map 的键类型。
        ok &= check(colors.contains(std::string(key)),
                    "内置皮肤必须声明完整草稿物件与轨道配色");
    }
    // 任何键缺失时停止颜色差异比较，避免产生连带误报。
    if ( !ok ) return false;

    // 每种草稿物件与对应正式物件一一配对比较。
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6>
        NOTE_COLOR_PAIRS{
            std::pair{ "draft_notes.note_tap", "note_tap" },
            std::pair{ "draft_notes.note_head", "note_head" },
            std::pair{ "draft_notes.note_hold", "note_hold" },
            std::pair{ "draft_notes.note_end", "note_end" },
            std::pair{ "draft_notes.note_node", "note_node" },
            std::pair{ "draft_notes.note_flick_arrow", "note_flick_arrow" },
        };
    // 精确 RGBA 相同即判定不可区分，不对具体色相作额外限制。
    for ( const auto& [draftKey, playerKey] : NOTE_COLOR_PAIRS ) {
        // 两次 getColor 均在已确认键存在后调用，不会比较错误色后备。
        ok &= check(!sameColor(skinManager.getColor(std::string(draftKey)),
                               skinManager.getColor(std::string(playerKey))),
                    "草稿物件颜色必须与对应正式物件颜色可区分");
    }
    return ok;
}

/// @brief 验证 IVM 内置皮肤的固定主题、颜色和纹理几何约束。
/// @param skinPath 仓库中 IVM 皮肤入口路径。
/// @param referenceSkinPath mmm-default 老皮肤入口路径。
/// @param translationsRoot 默认翻译资源目录。
/// @return 皮肤配置与全部自维护资源符合设计时返回 true。
/// @note 先加载默认皮肤取得 BGM 参考色，再加载 IVM 进行跨皮肤比较。
/// @warning 会读取多张 PNG 头和全部特效帧路径，但不解码或上传纹理。
bool verifyIvmSkin(const std::filesystem::path& skinPath,
                   const std::filesystem::path& referenceSkinPath,
                   const std::filesystem::path& translationsRoot)
{
    // SkinManager 是单例，因此必须在加载 IVM 前复制需要的默认皮肤颜色。
    auto& skinManager = MMM::Config::SkinManager::instance();
    bool  ok =
        check(skinManager.loadSkin(MMM::Config::pathToUtf8(referenceSkinPath),
                                   translationsRoot),
              "mmm-default 老皮肤应成功加载");
    // 三个参考值按值复制，不会在下一次 loadSkin 后悬空。
    const auto referenceSample = skinManager.getColor("bgm_tracks.sample");
    const auto referenceOffset = skinManager.getColor("bgm_tracks.offset");
    const auto referenceText   = skinManager.getColor("bgm_tracks.text");

    // 第二次加载切换到 IVM，后续所有资源查询都应落到该皮肤根。
    ok &= check(skinManager.loadSkin(MMM::Config::pathToUtf8(skinPath),
                                     translationsRoot),
                "IVM 内置皮肤应成功加载");
    ok &= check(skinManager.getData().themeName == "IVM", "IVM 皮肤名称不匹配");
    // IVM 使用单主题旧格式，亮暗分支都必须绑定相同稳定 ID。
    ok &= check(skinManager.getDefaultTheme(
                    MMM::Config::SkinThemeAppearance::Light) == "IVM" &&
                    skinManager.getDefaultTheme(
                        MMM::Config::SkinThemeAppearance::Dark) == "IVM",
                "IVM 皮肤亮暗分支都必须绑定内置 IVM 主题");
    // 特效布局与基础帧率直接影响资源几何和动画时间解释。
    ok &= check(skinManager.getHitEffectLayoutMode() ==
                    MMM::Config::HitEffectLayoutMode::TrackFill,
                "IVM 皮肤必须启用整轨填充打击特效");
    ok &= check(skinManager.getEffectBaseFps() == 120.0F,
                "IVM 序列帧动画必须以 120 FPS 播放");

    // Body 与 Node 必须完全同色，防止折线连接处出现色差接缝。
    const auto holdColor = skinManager.getColor("note_hold");
    const auto nodeColor = skinManager.getColor("note_node");
    ok &= check(holdColor.r == nodeColor.r && holdColor.g == nodeColor.g &&
                    holdColor.b == nodeColor.b && holdColor.a == nodeColor.a,
                "IVM 节点颜色必须与 Body 完全一致");

    // IVM 的 BGM 三类颜色应沿用默认皮肤，而不是随音符主题重绘。
    const auto bgmSampleColor = skinManager.getColor("bgm_tracks.sample");
    const auto offsetColor    = skinManager.getColor("bgm_tracks.offset");
    const auto textColor      = skinManager.getColor("bgm_tracks.text");
    ok &= check(sameColor(bgmSampleColor, referenceSample),
                "IVM 普通 BGM 物件必须沿用老皮肤配色");
    ok &= check(sameColor(offsetColor, referenceOffset),
                "IVM BGM 物件偏移提示必须沿用老皮肤配色");
    ok &= check(sameColor(textColor, referenceText),
                "IVM BGM 物件文字必须沿用老皮肤配色");
    // 三类 BGM 颜色全部比较，避免只验证普通样本而遗漏提示文本。

    // 拍头单独要求纯红，其余分拍槽位使用统一不透明灰色。
    const auto beatHead = skinManager.getColor("beat_lines.beat_1");
    ok &= check(beatHead.r == 1.0f && beatHead.g == 0.0f &&
                    beatHead.b == 0.0f && beatHead.a == 1.0f,
                "IVM 拍头线必须是完全不透明的纯红色");

    const auto referenceBeatLine = skinManager.getColor("beat_lines.beat_2");
    // 清单覆盖所有内置常用分拍和 default 后备槽位。
    constexpr std::array<std::string_view, 8> BEAT_LINE_KEYS{
        "beat_lines.beat_2",  "beat_lines.beat_3",  "beat_lines.beat_4",
        "beat_lines.beat_6",  "beat_lines.beat_8",  "beat_lines.beat_12",
        "beat_lines.beat_16", "beat_lines.default",
    };
    ok &= check(referenceBeatLine.r == referenceBeatLine.g &&
                    referenceBeatLine.g == referenceBeatLine.b &&
                    referenceBeatLine.a == 1.0f,
                "IVM 默认分拍线必须是完全不透明的灰色");
    // 先验证参考槽自身是灰色，再用它约束其他所有槽位。
    // 每个槽位逐通道与 beat_2 参考色比较，避免仅检查灰度性质。
    for ( std::string_view key : BEAT_LINE_KEYS ) {
        const auto color = skinManager.getColor(std::string(key));
        ok &= check(color.r == referenceBeatLine.r &&
                        color.g == referenceBeatLine.g &&
                        color.b == referenceBeatLine.b &&
                        color.a == referenceBeatLine.a,
                    "IVM 全部分拍线槽位必须使用同一灰色");
    }

    /// @brief 单张 IVM 纹理应暴露的资产键与像素尺寸。
    /// @details 键通过生产资产映射解析，尺寸直接读取落盘 PNG 头。
    struct TextureExpectation {
        /// @brief SkinManager 中的资产键。
        std::string_view key;
        /// @brief 期望像素宽度。
        std::uint32_t width;
        /// @brief 期望像素高度。
        std::uint32_t height;
    };
    // 八项覆盖音符头、节点、长条体、尾、方向箭头和判定区域。
    constexpr std::array<TextureExpectation, 8> TEXTURE_EXPECTATIONS{
        TextureExpectation{ "note.note", 256U, 128U },
        TextureExpectation{ "note.node", 24U, 24U },
        TextureExpectation{ "note.holdbodyvertical", 24U, 128U },
        TextureExpectation{ "note.holdbodyhorizontal", 256U, 24U },
        TextureExpectation{ "note.holdend", 24U, 12U },
        TextureExpectation{ "note.arrowleft", 128U, 96U },
        TextureExpectation{ "note.arrowright", 128U, 96U },
        TextureExpectation{ "panel.track.judgearea", 256U, 128U },
    };
    // 每项先验证 PNG 有效，再比较与渲染布局约定一致的像素尺寸。
    for ( const auto& expectation : TEXTURE_EXPECTATIONS ) {
        std::uint32_t width  = 0U;
        std::uint32_t height = 0U;
        const auto    path =
            skinManager.getAssetPath(std::string(expectation.key));
        ok &= check(readPngDimensions(path, width, height),
                    "IVM 纹理必须是有效 PNG");
        // 即使 PNG 读取失败也继续尺寸断言，由汇总结果统一返回失败。
        ok &= check(width == expectation.width && height == expectation.height,
                    "IVM 纹理尺寸不符合约定");
    }

    /// @brief IVM 特效序列应暴露的键、目录和帧数。
    /// @details 同时限制序列来源目录，防止意外复用默认皮肤资源。
    struct EffectExpectation {
        /// @brief SkinManager 中的特效序列键。
        std::string_view key;
        /// @brief IVM 资源目录下的序列子目录。
        std::string_view directory;
        /// @brief 期望序列帧数。
        std::size_t frameCount;
    };
    // 普通音符与 Flick 使用不同帧数，但共享单帧纵向渐变尺寸。
    // Hold 是独立序列，但初始复用单键的六张原图，不额外复制二进制资源。
    constexpr std::array<EffectExpectation, 3> EFFECT_EXPECTATIONS{
        EffectExpectation{ "note.effect.note", "note", 6U },
        EffectExpectation{ "note.effect.hold", "note", 6U },
        EffectExpectation{ "note.effect.flick", "flick", 16U },
    };
    // 期望根由 IVM 入口推导，不依赖进程工作目录。
    const auto effectRoot =
        skinPath.parent_path() / "resources/image/note/effect";
    for ( const auto& expectation : EFFECT_EXPECTATIONS ) {
        const auto* sequence =
            skinManager.getEffectSequence(std::string(expectation.key));
        ok &= check(sequence != nullptr, "IVM 特效序列必须存在");
        // 缺失序列已记录断言，跳过解引用以继续检查另一序列。
        if ( !sequence ) continue;

        ok &= check(sequence->frames.size() == expectation.frameCount,
                    "IVM 特效序列帧数不符合约定");
        // 帧数在遍历前验证，但实际循环仍检查加载器返回的全部路径。
        // 单键和 Hold 共享单键子目录，Flick 独立；所有帧保持在声明的根内。
        const auto expectedDirectory =
            (effectRoot / expectation.directory).lexically_normal();
        // 遍历全部帧而非首尾抽样，能发现中间缺失或跨目录引用。
        for ( const auto& framePath : sequence->frames ) {
            std::uint32_t width  = 0U;
            std::uint32_t height = 0U;
            ok &= check(
                framePath.parent_path().lexically_normal() == expectedDirectory,
                "IVM 特效不得复用默认皮肤资源");
            ok &= check(readPngDimensions(framePath, width, height),
                        "IVM 特效帧必须是有效 PNG");
            ok &= check(width == 32U && height == 256U,
                        "IVM 特效帧必须使用纵向渐变纹理尺寸");
            // 每帧相同尺寸保证序列切换时纹理布局无需重建。
        }
    }

    // IVM 未声明 blend 表，所有已解析序列应保持默认覆盖混合。
    for ( const auto& [key, sequence] :
          skinManager.getData().effectSequences ) {
        // key 只标识诊断上下文，混合契约对 IVM 全部序列统一生效。
        ok &=
            check(!sequence.additiveBlend, "IVM 未声明混合模式时必须保持覆盖");
    }
    // 字体文件名、存在性和同目录许可证共同验证分发完整性。
    const auto      fontPath = skinManager.getFontPath("ascii");
    std::error_code fontError;
    ok &= check(fontPath.filename() == "LiberationSans-Regular.ttf" &&
                    std::filesystem::is_regular_file(fontPath, fontError) &&
                    !fontError,
                "IVM 必须使用随皮肤分发的 Windows 风格字体");
    // 清除前一次查询状态，再独立验证许可证文件。
    fontError.clear();
    ok &= check(std::filesystem::is_regular_file(
                    fontPath.parent_path() / "OFL-1.1.txt", fontError) &&
                    !fontError,
                "IVM 字体必须随附 SIL OFL 1.1 许可证");
    // 许可证与字体同目录检查，保证打包时不会只复制字体二进制。
    // 正式 IVM 也必须启用与迁移结果一致的交互发光参数。
    ok &= check(skinManager.getGlowPasses() == 6 &&
                    skinManager.getGlowIntensity() == 0.5f,
                "IVM 悬浮或选中物件必须启用发光");
    // 发光检查位于资源验证末尾，确保正式皮肤不是仅依赖迁移夹具通过。
    return ok;
}

/// @brief 验证默认内置皮肤使用统一的 120 FPS 序列帧速率。
/// @param skinPath 默认内置皮肤入口路径。
/// @param translationsRoot 默认翻译资源目录。
/// @return 皮肤加载成功且序列帧速率为 120 FPS 时返回 true。
/// @note 同时验证普通判定反馈与 Flick 爆炸光使用不同混合策略。
/// @warning 只读默认皮肤脚本和序列描述，不打开图形设备。
bool verifyDefaultSkinEffectFrameRate(
    const std::filesystem::path& skinPath,
    const std::filesystem::path& translationsRoot)
{
    // 通过生产加载入口解析默认皮肤，避免测试直接读取 Lua 文本。
    auto& skinManager = MMM::Config::SkinManager::instance();
    bool  ok = check(skinManager.loadSkin(MMM::Config::pathToUtf8(skinPath),
                                         translationsRoot),
                    "默认内置皮肤应成功加载");
    // 帧率值来自 meta.effectbasefps 或加载器默认契约。
    ok &= check(skinManager.getEffectBaseFps() == 120.0F,
                "默认内置皮肤序列帧动画必须以 120 FPS 播放");
    // 同一个图集可同时包含覆盖型判定反馈与加法型爆炸光。
    const auto* note  = skinManager.getEffectSequence("note.effect.note");
    const auto* flick = skinManager.getEffectSequence("note.effect.flick");
    const auto* hold  = skinManager.getEffectSequence("note.effect.hold");
    // 独立 ID 保证 Hold 可单独替换，帧路径相等则验证初始序列逐帧复制。
    ok &= check(note && hold && hold->frames == note->frames &&
                    hold->startId != note->startId && !hold->additiveBlend,
                "默认 Hold 必须独立配置且复用完整单键帧序列");
    // 短路条件先验证指针存在，再读取混合标志，避免空指针访问。
    ok &= check(note && flick && !note->additiveBlend && flick->additiveBlend,
                "默认皮肤只对爆炸光启用加法混合");
    // 两个序列都必须存在才能安全检查各自 additiveBlend。
    return ok;
}
/// @brief 验证 RM 的真实入口、独立头部和完整打击序列可加载。
/// @param skinPath 仓库中 RM 皮肤入口。
/// @param translationsRoot 与皮肤分离的公共翻译根。
/// @return 资源路径、尺寸、配色及序列帧全部有效时返回 true。
/// @note 只读仓库资源，不启动图形设备或保存个人配置。
/// @details 同一 helper 根据父目录名区分 RM 与 RM(old) 的资源规格。
/// 两代皮肤都必须使用独立 Hold 头、完整帧序和加法打击光。
bool verifyRmSkin(const std::filesystem::path& skinPath,
                  const std::filesystem::path& translationsRoot)
{
    // 两代资源包独立分发，按目录确定各自帧数与图集导出尺寸。
    const bool old = skinPath.parent_path().filename() == "rm-old";
    // 目录名是测试输入的一部分，不依赖皮肤显示名称判断资源代际。
    auto& manager = MMM::Config::SkinManager::instance();
    // 使用生产加载器解析 Lua，覆盖嵌套资源路径及默认资源复用。
    if ( !check(manager.loadSkin(MMM::Config::pathToUtf8(skinPath),
                                 translationsRoot),
                "RM 皮肤必须成功加载") )
        return false;
    // 加载失败直接停止该皮肤检查，避免后续查询读到前一套皮肤数据。
    // 显示名必须与代际一致，避免两个包在设置列表中无法区分。
    bool ok = check(manager.getData().themeName == (old ? "RM(old)" : "RM"),
                    "RM 显示名必须准确");
    // 贴图自身携带色彩，额外的米黄乘色会破坏蓝键和绿色长条。
    // 五类正式物件颜色必须为白，确保纹理原色不被额外乘色。
    for ( const auto* key : { "note_tap",
                              "note_head",
                              "note_hold",
                              "note_node",
                              "note_flick_arrow" } ) {
        const auto color = manager.getColor(key);
        // 逐键检查完整 RGBA 白色，透明度偏差同样会改变原图呈现。
        ok &= check(color.r == 1.0F && color.g == 1.0F && color.b == 1.0F &&
                        color.a == 1.0F,
                    "RM 正式物件必须保持原图色彩");
    }
    // 两张头部纹理分开加载但共享尺寸，防止切换物件类型时发生布局跳变。
    // Tap 与 Hold 头分别读取 PNG 头，并按代际比较相同期望尺寸。
    for ( const auto* key : { "note.note", "note.holdhead" } ) {
        std::uint32_t width = 0U, height = 0U;
        ok &= check(readPngDimensions(manager.getAssetPath(key), width, height),
                    "RM 头部必须是可读 PNG");
        // 读取路径来自生产映射，尺寸断言同时覆盖 Lua 资产键绑定。
        ok &=
            check(width == (old ? 281U : 256U) && height == (old ? 123U : 112U),
                  "RM 头部布局尺寸必须一致");
    }
    // 路径必须指向真实文件；独立头部不能意外别名到蓝色 Tap。
    ok &= check(manager.getAssetPath("note.note") !=
                    manager.getAssetPath("note.holdhead"),
                "RM 长条头不能复用单键贴图");
    // 放大打击光不能改变普通 Note 或长条头部本身的显示比例。
    ok &= check(manager.getTextureScale("note.note") == 1.0F &&
                    manager.getTextureScale("note.holdhead") == 1.0F,
                "RM 打击光倍率不得影响物件纹理");
    // 路径不等比像素差异更直接保护独立头部资源选择。
    // 全部普通资产都必须落到真实普通文件，不能仅验证两张头部纹理。
    for ( const auto& [key, path] : manager.getData().assetPaths ) {
        // key 用于遍历完整映射，存在性判断只依赖每个实际 path。
        std::error_code error;
        ok &= check(std::filesystem::is_regular_file(path, error) && !error,
                    "RM 资产引用必须落到真实文件");
    }
    // 原包两组连续序列长度不同，分别检查可避免错接到默认六帧特效。
    // 单键与滑键保持原序列长度，Hold 单独检查所截取的循环范围。
    for ( const auto* key :
          { "note.effect.note", "note.effect.flick", "note.effect.hold" } ) {
        const auto*       sequence = manager.getEffectSequence(key);
        const bool        hold = std::string_view(key) == "note.effect.hold";
        const std::size_t expected =
            hold                                          ? (old ? 8U : 9U)
            : std::string_view(key) == "note.effect.note" ? (old ? 9U : 17U)
                                                          : (old ? 16U : 18U);
        // 期望帧数按序列类型和资源代际二维选择，避免交叉套用。
        ok &= check(sequence && sequence->frames.size() == expected,
                    "RM 原包帧序必须完整");
        // 缺失序列已计入失败，跳过后续帧访问以继续汇总其他断言。
        if ( !sequence ) continue;
        // 三类动画分别配置，Hold 不应因共享 Flick 图片而丢失独立倍率。
        ok &= check(manager.getTextureScale(key) == 1.6F,
                    "RM 三类打击特效应分别放大到 1.6 倍");
        for ( std::uint32_t frame = 0; frame < sequence->frames.size();
              ++frame ) {
            ok &= check(manager.getEffectTextureScale(sequence->startId +
                                                      frame) == 1.6F,
                        "RM 全部打击特效帧应应用 1.6 倍缩放");
        }
        if ( hold ) {
            const auto* flick = manager.getEffectSequence("note.effect.flick");
            // 后半段与滑键共享文件但保持独立序列，不能误取单键消散帧。
            // 不只检查首尾路径，逐帧比较保证中间范围也保持顺序。
            // 先约束两侧范围长度，错误配置也不能令断言自身越过迭代器边界。
            ok &= check(flick && flick->frames.size() >= expected &&
                            sequence->frames.size() == expected &&
                            sequence->startId != flick->startId &&
                            std::equal(sequence->frames.begin(),
                                       sequence->frames.end(),
                                       flick->frames.end() - expected),
                        "RM Hold 必须精确使用滑键后半段稳定帧");
        }
        ok &= check(sequence->additiveBlend, "RM 打击光应使用加法混合");
        // 三类 RM 特效都使用加法混合，与默认皮肤普通反馈不同。
        // Hold 不能因新增资源键遗漏 blend 声明而出现覆盖型暗斑。
        // 独立帧 ID 和独立混合配置共同构成可单独替换的动画契约。
        // 路径复用只节省源文件，不表示运行时回退成 Flick 类型。
        // 校验落盘帧而非仅检查 Lua 的范围字符串，缺失末帧也应失败。
        // 每一帧都验证可读 PNG，覆盖范围展开后的中间路径。
        for ( const auto& frame : sequence->frames ) {
            std::uint32_t width = 0U, height = 0U;
            ok &= check(readPngDimensions(frame, width, height),
                        "RM 特效帧必须是可读 PNG");
        }
    }
    return ok;
}
}  // namespace

/// @brief 皮肤亮暗主题绑定与旧配置兼容回归测试入口。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数；附加参数为测试输出目录和 IVM 皮肤入口。
/// @return 全部断言通过时返回 0。
/// @note 所有生成文件限制在调用方提供的 test_output 子目录。
/// @warning 仓库皮肤和翻译资源只读使用，不触碰个人配置目录。
int main(int argc, char* argv[])
{
    // 需要输出根、IVM、翻译和默认皮肤四个附加路径才能完整执行。
    if ( argc < 5 || !argv[1] || !argv[2] || !argv[3] || !argv[4] ) {
        XERROR(
            "SkinThemeBindingTest requires output, IVM skin, translation and "
            "default skin paths");
        return 1;
    }

    // 输出路径由 CTest 固定，清理只作用于该专用目录。
    const std::filesystem::path outputDirectory =
        MMM::Config::utf8ToPath(argv[1]);
    std::error_code filesystemError;
    std::filesystem::remove_all(outputDirectory, filesystemError);
    // 清理错误随后由 create_directories 的最终结果统一判定目录可用性。
    filesystemError.clear();
    // 重建空输出根保证重复执行不读取上轮临时皮肤。
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if ( filesystemError ) {
        XERROR("Failed to create SkinThemeBindingTest output directory: {}",
               filesystemError.message());
        return 1;
    }

    // 三个主题夹具分别覆盖旧字符串、成对表和单分支表格式。
    const std::filesystem::path legacySkinPath =
        outputDirectory / "legacy-skin.lua";
    const std::filesystem::path translationsRoot =
        MMM::Config::utf8ToPath(argv[3]);
    // 默认翻译根只读共享给每次 loadSkin，避免夹具自带旧 langs 字典。
    const std::filesystem::path pairedSkinPath =
        outputDirectory / "paired-skin.lua";
    const std::filesystem::path lightOnlySkinPath =
        outputDirectory / "light-only-skin.lua";
    // 旧 IVM 夹具必须置于名为 ivm 的父目录才能匹配限定迁移。
    const std::filesystem::path legacyIvmDirectory = outputDirectory / "ivm";
    std::filesystem::create_directories(legacyIvmDirectory, filesystemError);
    if ( filesystemError ) {
        XERROR("Failed to create legacy IVM test directory: {}",
               filesystemError.message());
        return 1;
    }
    const std::filesystem::path legacyIvmSkinPath =
        legacyIvmDirectory / "skin.lua";
    // 文件名仍为标准 skin.lua，使身份条件与正式内置皮肤完全一致。

    // 所有临时脚本先完整写好，任一失败则不进入加载断言。
    bool ok = check(writeTestSkin(legacySkinPath, "'Cecilia'"),
                    "旧格式测试皮肤写入失败");
    ok &= check(writeTestSkin(pairedSkinPath,
                              "{ light = 'Cecilia', dark = 'Moonlight' }"),
                "亮暗双主题测试皮肤写入失败");
    ok &= check(
        writeTestSkin(lightOnlySkinPath, "{ light = 'ComfortableLight' }"),
        "单分支测试皮肤写入失败");
    ok &= check(writeLegacyIvmSkin(legacyIvmSkinPath),
                "旧版 IVM 测试皮肤写入失败");
    // 写入阶段汇总所有夹具错误，加载前只执行一次失败判定。
    if ( !ok ) return 1;

    // 旧字符串绑定亮暗同值，成对表保持差异，单分支复制到缺失侧。
    ok &= verifyThemeBinding(
        legacySkinPath, "Cecilia", "Cecilia", translationsRoot);
    ok &= verifyThemeBinding(
        pairedSkinPath, "Cecilia", "Moonlight", translationsRoot);
    ok &= verifyThemeBinding(lightOnlySkinPath,
                             "ComfortableLight",
                             "ComfortableLight",
                             translationsRoot);
    ok &= verifyLegacyIvmGlowMigration(legacyIvmSkinPath, translationsRoot);
    // 缩放解析使用独立夹具，后续内置皮肤检查继续验证生产资源配置。
    ok &= verifyTextureScales(outputDirectory / "texture-scales.lua",
                              translationsRoot);
    // 临时旧 IVM 检查完成后，后续 helper 会继续替换单例皮肤状态。
    // 皮肤解析之后独立验证应用级主题偏好和默认皮肤特效契约。
    ok &= verifyLegacyAppConfigSemantics();
    ok &= verifyDefaultSkinEffectFrameRate(MMM::Config::utf8ToPath(argv[4]),
                                           translationsRoot);
    // argv[4] 同时作为默认皮肤行为和草稿配色的权威入口。
    // 默认与 IVM 两套内置皮肤都必须声明完整且可区分的草稿配色。
    ok &=
        verifyDraftPalette(MMM::Config::utf8ToPath(argv[4]), translationsRoot);
    ok &=
        verifyDraftPalette(MMM::Config::utf8ToPath(argv[2]), translationsRoot);
    // IVM 深度资源检查需要同时读取默认皮肤作为 BGM 色彩参考。
    ok &= verifyIvmSkin(MMM::Config::utf8ToPath(argv[2]),
                        MMM::Config::utf8ToPath(argv[4]),
                        translationsRoot);
    // RM 与默认皮肤共同分发，从已传入的资源根定位，避免依赖运行目录。
    ok &= verifyRmSkin(
        MMM::Config::utf8ToPath(argv[4]).parent_path().parent_path() /
            "rm/skin.lua",
        translationsRoot);
    // RM 两代使用相同 translationsRoot，差异仅来自各自皮肤资源目录。
    // 旧版使用相同生产加载器校验完整资源及加法打击光。
    ok &= verifyRmSkin(
        MMM::Config::utf8ToPath(argv[4]).parent_path().parent_path() /
            "rm-old/skin.lua",
        translationsRoot);
    // 所有 helper 均记录具体原因，入口只把汇总结果转换为退出码。
    return ok ? 0 : 1;
}
