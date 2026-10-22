#include "config/LayoutPresets.h"
#include "ConfigSnapshotWriter.h"
#include "config/AppConfig.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace
{
using namespace MMM::Config;
/// @brief 构造两个差异明显的键数配置，覆盖辅助区域和逐轨信息组件。
/// @return 模拟用户在不同谱面上分别编辑后的完整视觉配置。
/// @note 数值均处于生产反序列化器的有效范围，不以非法值验证往返。
VisualConfig configured()
{
    // 回退模板保持默认，用键数覆盖模拟用户实际布局编辑。
    // 后续导入须同时保留显式边界和仍为空的辅助区边界。
    VisualConfig visual;
    // 4K 和 7K 使用不同轨道宽度，防止只保存当前键数也能误通过。
    auto& four = visual.editableTrackLayoutForKeyCount(4);
    four.left  = 0.12F;
    four.right = 0.75F;
    // 负坐标允许辅助区伸出主轨道，不应被导入误钳制为零。
    four.draftLanes.left  = -0.2F;
    four.draftLanes.width = 0.08F;
    auto& seven           = visual.editableTrackLayoutForKeyCount(7);
    seven.left            = 0.3F;
    seven.bgmLanes.right  = 1.2F;
    // 批注区按总宽保存，与草稿区的单轨宽使用不同语义。
    seven.annotation.width = 0.1F;
    // 每个键数的逐轨 KPS 数组必须深拷贝，不能共享另一个方案的布局。
    auto& components = visual.editableCanvasComponentsForKeyCount(7);
    components.kpsTracks.push_back({ 2, {} });
    components.kpsTracks.front().placement.anchorX = 0.62F;
    // 独立判定线不与另一个键数的默认位置合并。
    visual.editableJudgmentLinePositionForKeyCount(4) = 0.7F;
    visual.canvasCameraPoseEnabled                    = true;
    visual.canvasCameraPosition                       = { 0.2F, -0.4F, 1.2F };
    // 三个旋转分量均设为非零，防止只保存倾斜角。
    visual.canvasCameraRotation = { 30, 5, 10 };
    visual.canvasCameraLength   = 4;
    // 新增补偿与相机姿态一起参与所有预设往返、迁移和内置保护案例。
    visual.canvasMotionCompensation    = 35;
    visual.canvasThicknessCompensation = 75;
    return visual;
}
/// @brief 比较全部布局，而非仅比较主轨道的某一个坐标。
/// @note 转为单项白名单序列化结果，排除无关视觉设置的差异。
/// @param a 第一份现场，可能尚未存入预设缓存。
/// @param b 第二份现场，可能来自导入或重启恢复。
/// @return 文件协议内所有布局字段都一致时为真。
bool sameLayout(const VisualConfig& a, const VisualConfig& b)
{
    // 只比较当前项，不让两个库自带的斜轨项掩盖差异。
    LayoutPresets first, second;
    captureLayoutPreset(first.current(), a);
    captureLayoutPreset(second.current(), b);
    // 序列化结果还会包含组件数组和可选区域，比较覆盖全部层级。
    const nlohmann::json x = first, y = second;
    return x["items"][0]["layout"] == y["items"][0]["layout"];
}
/// @brief 验证切换收存、深拷贝、默认斜轨参数和无关视觉偏好的隔离。
/// @return 每次回到原方案均恢复所有键数，且方案管理保持非空时为真。
/// @note 相机参数是方案公共字段，键数变化仅选择轨道与组件覆盖。
bool switching()
{
    LayoutPresets library;
    auto          visual   = configured();
    const auto    original = visual;
    // 第一次实际修改立即选中新项，代码默认保持原值。
    if ( !library.captureCurrent(visual) || library.m_selected != "自定义布局" )
        return false;
    // 保留自动生成名称，后续可验证再次选中时仍能恢复完整现场。
    const auto firstName = library.m_selected;
    if ( !library.select("默认", visual) ||
         !sameLayout(visual, VisualConfig{}) )
        return false;
    if ( library.rename("改名") || library.remove(visual) ) return false;
    // 仅访问默认键数覆盖并不改变实际布局，不应创建新项。
    const auto count = library.m_items.size();
    // 仅创建等价覆盖模拟打开面板，用户还没有调整任何坐标。
    visual.editableTrackLayoutForKeyCount(4);
    if ( library.captureCurrent(visual) || library.m_items.size() != count )
        return false;
    if ( !library.select(firstName, visual) || !sameLayout(original, visual) )
        return false;
    if ( !library.duplicate("副本", visual) ) return false;
    visual.editableTrackLayoutForKeyCount(4).left = 0.4F;
    // 连续调整只更新当前项，不会每次重新创建自定义配置。
    // 第二次修改不能重复生成自动项，否则持续拖动会让方案列表无限增长。
    if ( !library.captureCurrent(visual) ||
         library.m_items.size() != count + 1 )
        return false;
    if ( !library.select(firstName, visual) || !sameLayout(original, visual) )
        return false;
    // 默认保护不能破坏另一内置斜轨预设的精确相机参数。
    // 选择内置方案须恢复适度厚度补偿，而不是沿用此前自定义布局的强度。
    if ( !library.select("斜轨预览", visual) ) return false;
    if ( visual.canvasCameraPosition !=
             std::array<float, 3>{ 0, -0.8F, 0.9F } ||
         visual.canvasCameraRotation != std::array<float, 3>{ 45, 0, 0 } ||
         visual.canvasCameraLength != 3 || !visual.canvasCameraPoseEnabled ||
         visual.canvasThicknessCompensation != 35 )
        return false;
    // 未定制键数也继承九成宽度，不残留上一个方案的 4K 或 7K。
    const auto& layout = visual.trackLayoutForKeyCount(10);
    if ( std::abs(layout.left - 0.05F) > 0.00001F ||
         std::abs(layout.right - 0.95F) > 0.00001F ||
         !visual.keyCountLayouts.empty() )
        return false;
    visual.enableHitEffects = false;
    library.resetToDefault(visual);
    // 一次重置清除全部键数覆写与相机，仅布局以外偏好保持现场。
    if ( library.m_selected != "默认" || !sameLayout(visual, VisualConfig{}) ||
         visual.enableHitEffects )
        return false;
    if ( !library.select(firstName, visual) || !sameLayout(original, visual) )
        return false;
    if ( library.rename("副本") || library.duplicate("", visual) ||
         library.duplicate("   ", visual) )
        return false;
    return library.rename("改名") && library.remove(visual) &&
           library.m_selected == "默认";
}
/// @brief 用真实配置文件验证旧版迁移、全键数保存和重启后的选择。
/// @param root 由 CTest 提供的独立测试输出目录。
/// @return 配置往返完整恢复，且首次升级保留旧布局时为真。
/// @note AppConfig 是真实单例，但所有文件路径都由测试显式提供。
/// 不使用默认 load/save 路径，避免测试重置用户当前布局预设。
bool persistence(const std::filesystem::path& root)
{
    auto& app = AppConfig::instance();
    // 清空单例状态，测试不继承其他用例的当前预设选择。
    app.reset();
    app.getVisualConfig() = configured();
    const auto file       = root / "config.json";
    // 首次生成没有预设字段的旧格式配置，覆盖真实升级路径。
    {
        std::ofstream output(file);
        output << nlohmann::json(app.getEditorConfig()).dump();
    }
    // 旧配置加载同时触发迁移，随后必须仍能复制或切换方案。
    if ( !app.load(file) || !sameLayout(configured(), app.getVisualConfig()) )
        return false;
    // 检查收存进入库后的值，防止仅视觉现场保留但预设内容为空。
    // 用户配置中的方案集合损坏时，仍保留 visual 中可用的旧布局。
    // 此输入不是独立导入文件，因此允许启动并回退已有配置。
    // 先建立带损坏新字段的文档，再走 AppConfig 实际加载入口。
    {
        nlohmann::json damaged   = app.getEditorConfig();
        damaged["layoutPresets"] = { { "version", 99 }, { "items", nullptr } };
        std::ofstream output(file);
        output << damaged.dump();
    }
    // 不能回退普通默认布局，否则升级后的用户会丢掉已保存的 4K/7K。
    // 校验现场与库快照两者，避免下一次切换才暴露迁移数据丢失。
    if ( !app.load(file) || !sameLayout(configured(), app.getVisualConfig()) ||
         !sameLayout(configured(), app.getLayoutPresets().current().m_visual) )
        return false;
    auto& library = app.getLayoutPresets();
    if ( !sameLayout(library.current().m_visual, app.getVisualConfig()) ||
         !library.select("斜轨预览", app.getVisualConfig()) )
        return false;
    app.getVisualConfig().editableTrackLayoutForKeyCount(7).left = 0.11F;
    // 斜轨方案的回退模板和本次新建的 7K 覆盖必须一起保留。
    // 记录实时值而非方案缓存，才能检出 save 未捕获现场的问题。
    const auto expected = app.getVisualConfig();
    // 磁盘写入失败应直接让测试失败，不能继续比较内存值。
    if ( !app.save(file) ) return false;
    // 重置进程内状态，防止把内存中的残留误判为磁盘恢复成功。
    app.reset();
    // 对内置斜轨的修改必须在保存时自动成为自定义项，重启仍选择该副本。
    return app.load(file) &&
           app.getLayoutPresets().m_selected == "自定义布局 (2)" &&
           sameLayout(expected, app.getVisualConfig());
}
/// @brief 验证斜轨内置保护、自动复制以及已有用户斜轨配置的兼容迁移。
/// @param root 测试独立目录，不访问用户正在使用的布局配置。
/// @return 编辑、切回和重启均保持内置定义且完整保留用户改动时为真。
/// @details 名称后缀只用于显示，序列化和选择仍保持稳定的原始名称。
/// 自动副本应包含全部键数与公共相机，不能只复制当前画布上的键数。
/// 历史配置已存在自定义名称时，迁移不能覆盖同名用户数据。
/// 重复加载必须幂等，不能在每次启动时继续创建斜轨副本。
bool builtInProtection(const std::filesystem::path& root)
{
    LayoutPresets library;
    VisualConfig  visual;
    // 两个内置名称都显示标志，自定义项不由文件字段声明只读身份。
    if ( !library.current().isBuiltIn() ||
         std::string(library.current().displayName()) != "默认(内置)" ||
         !library.select("斜轨预览", visual) ||
         std::string(library.current().displayName()) != "斜轨预览(内置)" )
        return false;
    // 原始快照包含全部布局字段，不能只用三个相机数值判断内置项未被污染。
    const auto original = visual;
    const auto count    = library.m_items.size();
    // 只读保护不仅体现在界面禁用按钮，底层命名和删除同样必须拒绝。
    // 名称与布局各自检查，避免仅加显示标志却仍允许修改保留项身份。
    if ( library.rename("覆盖内置") || library.remove(visual) ) return false;
    visual.editableTrackLayoutForKeyCount(4).left  = 0.2F;
    visual.editableTrackLayoutForKeyCount(7).right = 0.8F;
    visual.canvasCameraPosition                    = { 0.1F, -0.6F, 1.1F };
    // 第一帧改变就立即切到新项，不能等到切换预设或退出程序才创建。
    const auto edited = visual;
    if ( !library.captureCurrent(visual) || library.current().isBuiltIn() ||
         library.m_selected != "自定义布局" ||
         library.m_items.size() != count + 1 ||
         !sameLayout(edited, library.current().m_visual) ||
         std::string(library.current().displayName()) != "自定义布局" )
        return false;
    // 连续修改仍更新同一副本，停止调整后切回内置必须恢复精确代码参数。
    // 第二次只改公共相机长度，与第一次的按键数修改走不同字段路径。
    // 连续捕获后数量稳定，证明保存会继续写同一副本而非每帧复制内置项。
    visual.canvasCameraLength = 5;
    if ( !library.captureCurrent(visual) ||
         library.m_items.size() != count + 1 ||
         !library.select("斜轨预览", visual) || !sameLayout(original, visual) )
        return false;

    // 模拟上一版允许覆盖斜轨预设，同时用户已有一个同名自动自定义方案。
    // 直接构造旧记录用于迁移测试，正常编辑不能绕过 captureCurrent 的保护。
    LayoutPresets legacy;
    // 旧记录仍用保留名称，加载器需要用布局内容区分改动与正常代码基准。
    // 文件没有额外只读标记，不能靠用户可编辑的布尔值决定是否保护。
    captureLayoutPreset(legacy.m_items[1], edited);
    LayoutPreset existing;
    existing.m_name = "自定义布局";
    // 原有自定义使用与斜轨副本不同的布局，名称避让后也必须恢复各自内容。
    captureLayoutPreset(existing, configured());
    legacy.m_items.push_back(existing);
    legacy.m_selected = "斜轨预览";
    auto& app         = AppConfig::instance();
    app.reset();
    app.getVisualConfig() = edited;
    nlohmann::json json   = app.getEditorConfig();
    json["layoutPresets"] = legacy;
    const auto file       = root / "legacy-slanted.json";
    {
        // 走真实配置读取和自动迁移保存，不把纯 JSON 转换当成重启验收。
        std::ofstream output(file);
        output << json.dump();
    }
    if ( !app.load(file) ||
         app.getLayoutPresets().m_selected != "自定义布局 (2)" ||
         !sameLayout(edited, app.getVisualConfig()) )
        return false;
    // 后续重启比较数量，额外重复副本即使当前现场正确也应判定迁移失败。
    const auto size = app.getLayoutPresets().m_items.size();
    // 原有自定义方案和新迁移方案必须都保留，而不是覆盖名字相同的旧方案。
    const auto oldCustom = std::ranges::find(app.getLayoutPresets().m_items,
                                             std::string("自定义布局"),
                                             &LayoutPreset::m_name);
    if ( oldCustom == app.getLayoutPresets().m_items.end() ||
         !sameLayout(configured(), oldCustom->m_visual) )
        return false;
    // 丢弃内存库，不能让测试把未落盘的迁移选择误认为持久化成功。
    app.reset();
    // 再加载时保持同一个副本和完整现场，确认第一次迁移确实已经持久化。
    if ( !app.load(file) || app.getLayoutPresets().m_items.size() != size ||
         app.getLayoutPresets().m_selected != "自定义布局 (2)" ||
         !sameLayout(edited, app.getVisualConfig()) )
        return false;
    // 用户可以随时返回代码斜轨原型，迁移后的自定义项不会反向影响基准。
    if ( !app.getLayoutPresets().select("斜轨预览", app.getVisualConfig()) ||
         !sameLayout(original, app.getVisualConfig()) )
        return false;
    // 配置中完全缺少某个内置项时也须补回，不从用户副本猜测代码基准。
    json["layoutPresets"]["items"].erase(1);
    // 省略内置项是合法的旧配置形态，补齐规则不依赖导出时是否列出了它。
    LayoutPresets restored;
    from_json(json["layoutPresets"], restored);
    // 切换前先采用恢复库的当前现场，避免测试自身制造一次额外布局编辑。
    visual = restored.current().m_visual;
    return restored.select("斜轨预览", visual) && sameLayout(original, visual);
}
/// @brief 验证文件往返、同名导入及不可信 JSON 的拒绝语义。
/// @note 错误类型深入到逐轨 KPS 和相机数组，防止表层校验掩盖类型崩溃。
/// @param root 测试文件专用目录，所有输出禁止写入 tests/data。
/// @return 合法文件可往返，所有失败导入保持原数据时为真。
/// @details 名称冲突不覆盖、嵌套类型错误不崩溃是此用例的主要边界。
/// 导出到不存在的父目录还需提供明确错误反馈。
bool files(const std::filesystem::path& root)
{
    LayoutPresets library;
    auto          visual = configured();
    library.captureCurrent(visual);
    const auto  file = root / std::filesystem::path(u8"布局.mmlayout");
    std::string error;
    if ( !exportLayoutPreset(file, library.current(), error) ) return false;
    // 改动当前实值后导入，必须恢复导出时所有键数，原项另行保留。
    visual.trackLayout.left = 0.45F;
    if ( !library.importFile(file, visual, error) ||
         library.m_selected != "自定义布局 (2)" ||
         !sameLayout(configured(), visual) )
        return false;
    // 保留合法文档作为每个损坏用例的独立起点。
    // 若累计破坏同一对象，前一个错误可能掩盖深层校验遗漏。
    const auto valid = [&] {
        std::ifstream input(file);
        return nlohmann::json::parse(input, nullptr, false);
    }();
    // 库和实时配置分别记录，拒绝导入必须同时保持两者不变。
    const auto before = nlohmann::json(library);
    const auto live   = visual;
    // 每个损坏文件都必须失败，不能部分写入库或替换画布。
    for ( int kind = 0; kind < 5; ++kind ) {
        // 每次只破坏一个字段，确保拒绝来自本轮要测试的类型边界。
        auto broken = valid;
        if ( kind == 0 ) broken["version"] = 2;
        // 数组元素错误要在浮点转换之前拒绝，不能进入 typed get。
        if ( kind == 1 )
            broken["preset"]["layout"]["canvasCameraPosition"][1] = "错误";
        // 嵌套逐轨数组的错误验证模板是否覆盖动态容器内的结构。
        if ( kind == 2 )
            broken["preset"]["layout"]["keyCountLayouts"][1]["canvasComponents"]
                  ["kpsTracks"][0]["placement"]["anchorX"] = false;
        // 超大数值避免整型截断；合法浮点坐标仍不受此限制。
        if ( kind == 3 )
            broken["preset"]["layout"]["keyCountLayouts"][0]["keyCount"] = 1e20;
        // 非对象布局不得被当成“缺失字段使用默认值”接纳。
        if ( kind == 4 ) broken["preset"]["layout"] = nullptr;
        {
            std::ofstream output(file);
            output << broken.dump();
        }
        // 错误提示必须非空，否则界面仍会表现为点击后没有反馈。
        // 失败前后的库相等也检查没有悄悄增加重名后缀项。
        if ( library.importFile(file, visual, error) || error.empty() ||
             nlohmann::json(library) != before || !sameLayout(live, visual) )
            return false;
    }
    // 无法创建的目标要返回可见失败，不能只报告已打开保存对话框。
    return !exportLayoutPreset(
        root / "missing" / "file.mmlayout", library.current(), error);
}
/// @brief 覆盖旧版本没有方案集合、以及旧实现写坏默认项的升级路径。
/// @param root 专用输出目录，不写入真实用户配置。
/// @return 已改布局迁移后选中自定义，未改布局仍选择代码默认时为真。
/// @note 每种输入都重新 reset，避免前一用例的库状态影响迁移判断。
/// @details 无预设字段的旧配置应按实际布局差异迁移，不能只检查版本号。
/// 已有错误默认项的旧配置则需恢复代码基准，并保留原项的所有键数数据。
/// 重复启动不再创建新项，迁移结果在首次加载结束前已写入原文件。
bool legacyMigration(const std::filesystem::path& root)
{
    auto&      app  = AppConfig::instance();
    const auto file = root / "legacy.json";
    // 三种来源分别是纯代码默认、旧版自定义现场、曾被覆盖的默认预设。
    // 每种情况都必须经真实 load，而非只调用内存转换函数。
    for ( int kind = 0; kind < 3; ++kind ) {
        app.reset();
        const auto visual = kind == 0 ? VisualConfig{} : configured();
        auto       config = app.getEditorConfig();
        config.visual     = visual;
        // 根级 visual 是旧版本唯一布局来源，缺少集合不是丢弃布局的理由。
        nlohmann::json json = config;
        // 旧实现集合保留原有名字与选择，迁移需修复语义而非删除内容。
        if ( kind == 2 ) {
            LayoutPresets legacy;
            // 模拟上个实现把用户布局直接写入“默认”，不能继续信任该名称。
            captureLayoutPreset(legacy.current(), visual);
            json["layoutPresets"] = legacy;
        }
        {
            std::ofstream output(file);
            output << json.dump();
        }
        // 迁移必须保持用户看到的轨道位置与相机角度不变。
        if ( !app.load(file) || !sameLayout(app.getVisualConfig(), visual) )
            return false;
        // 按来源区分应选中的方案，不能所有用户一律新建自定义。
        const auto& library = app.getLayoutPresets();
        if ( (library.m_selected == "默认") != (kind == 0) ) return false;
        // 迁移已在 load 返回前落盘，不能只修改进程内状态。
        std::ifstream input(file);
        const auto    saved = nlohmann::json::parse(input, nullptr, false);
        if ( saved["layoutPresets"]["selected"] != library.m_selected )
            return false;
        // 再加载同一份文件应具有幂等性，避免每次启动重复迁移。
        const auto size = library.m_items.size();
        if ( !app.load(file) || app.getLayoutPresets().m_items.size() != size )
            return false;
        // 迁移后的软件默认仍须可恢复，不借用刚被迁走的旧用户布局。
        app.getLayoutPresets().resetToDefault(app.getVisualConfig());
        if ( !sameLayout(app.getVisualConfig(), VisualConfig{}) ) return false;
    }
    return true;
}
/// @brief 验证后台即时保存、连续编辑合并及同步最终保存的顺序。
/// @param root CTest 分配的测试文件目录。
/// @return 最新布局已经写出且旧请求不能覆盖新配置时为真。
/// @note 使用条件变量等待实际完成，不以 sleep 猜测磁盘延迟。
/// 库的选择在保存请求返回时就应更新，磁盘完成与 UI 状态分开验证。
/// @details 测试分别核验即时选择、拖动中间的最终落盘、重置后的保存顺序。
/// 最终重置必须胜过此前所有待处理拖动快照，不能在后台又恢复旧布局。
/// 重启后再次比较完整布局，覆盖只更新内存库或仅保存名字的错误实现。
bool realtimeSaving(const std::filesystem::path& root)
{
    auto& app = AppConfig::instance();
    app.reset();
    const auto file       = root / "realtime.json";
    app.getVisualConfig() = configured();
    // 发布请求后立即核验选择，不等待后台文件完成才更新界面。
    app.saveLayoutChanges(file);
    if ( app.getLayoutPresets().m_selected != "自定义布局" ) return false;
    // 后续二十次移动应更新同一自定义项，项数在第一帧之后保持稳定。
    const auto size = app.getLayoutPresets().m_items.size();
    // 模拟高于磁盘吞吐的连续拖动，允许合并中间快照但必须保存最终值。
    // 不依赖每个中间状态落盘，也不限制后台调度顺序。
    for ( int i = 0; i < 20; ++i ) {
        app.getVisualConfig().editableTrackLayoutForKeyCount(4).left =
            0.2F + i * 0.001F;
        app.saveLayoutChanges(file);
    }
    // 等待真实队列完成，确认拖动期间已经保存，而非只在松手时写入。
    MMM::Config::flushConfigSnapshots();
    {
        std::ifstream input(file);
        const auto    saved = nlohmann::json::parse(input, nullptr, false);
        if ( saved["layoutPresets"]["selected"] != "自定义布局" ||
             saved["visual"]["keyCountLayouts"][0]["trackLayout"]["left"] !=
                 app.getVisualConfig().trackLayoutForKeyCount(4).left )
            return false;
    }
    // 名称和项数必须稳定，不能把实时保存误实现为持续复制方案。
    if ( app.getLayoutPresets().m_items.size() != size ) return false;
    // 排队状态之后立即完整重置并同步保存，旧拖动请求不可复活。
    // 最后排队一次不同于默认的相机长度，让旧快照覆盖问题可以被检测。
    app.getVisualConfig().canvasCameraLength = 6;
    app.saveLayoutChanges(file);
    app.getLayoutPresets().resetToDefault(app.getVisualConfig());
    // 同步最终保存与后台队列共用文件顺序门禁，测试显式等待所有旧请求完成。
    if ( !app.save(file) ) return false;
    MMM::Config::flushConfigSnapshots();
    app.reset();
    // 重启后检查实际布局与选择，防止只保存名称而仍留有旧相机或键数覆盖。
    return app.load(file) && app.getLayoutPresets().m_selected == "默认" &&
           sameLayout(app.getVisualConfig(), VisualConfig{});
}
}  // namespace
/// @brief 在专用输出目录执行方案隔离和文件持久化回归。
/// @note 测试使用值比较与真实文件，不依赖图形上下文或用户配置。
/// @return 任一行为不符合契约时返回非零并记录失败阶段。
int main(int argc, char** argv)
{
    // 没有独立输出目录就停止，禁止回退当前源码目录写测试文件。
    if ( argc != 2 ) return 1;
    // 文件路径来自测试注册参数，不读取客户端默认配置路径。
    const std::filesystem::path root(argv[1]);
    std::error_code             ec;
    std::filesystem::create_directories(root, ec);
    // 目录创建失败属于环境失败，不继续产生不明确的后续断言。
    if ( ec ) return 1;
    // 分阶段报告能区分模板参数、客户端迁移和导入校验错误。
    // 先检查纯内存契约，失败时不执行后续磁盘测试。
    if ( !switching() ) {
        XERROR("Layout preset switching failed");
        return 1;
    }
    // 真实 AppConfig 用例验证库与视觉字段的集成保存顺序。
    if ( !persistence(root) ) {
        XERROR("Layout preset persistence failed");
        return 1;
    }
    // 斜轨和软件默认遵守同一只读规则，历史调整必须迁移而非被强行清空。
    if ( !builtInProtection(root) ) {
        XERROR("Built-in layout protection failed");
        return 1;
    }
    // 独立文件校验覆盖导入导出入口，而不是只测 JSON 往返。
    if ( !files(root) ) {
        XERROR("Layout preset file validation failed");
        return 1;
    }
    // 旧版用户布局与新用户默认必须分开验证。
    if ( !legacyMigration(root) ) {
        XERROR("Legacy layout migration failed");
        return 1;
    }
    // 后台保存测试最后运行，避免其单例状态影响之前的迁移用例。
    if ( !realtimeSaving(root) ) {
        XERROR("Realtime layout saving failed");
        return 1;
    }
    return 0;
}
