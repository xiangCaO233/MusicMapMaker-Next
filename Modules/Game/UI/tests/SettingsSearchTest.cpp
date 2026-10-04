#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/SettingsSearchIndex.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"
#include "ui/imgui/menu/interfaces/IMainMenuToggleItemActionHandler.h"
#include "ui/imgui/menu/items/MainMenuToggleItem.h"
#include "ui/imgui/status/IStatusMessageSink.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

/// @file SettingsSearchTest.cpp
/// @brief 搜索目录、跨语言近义词和真实设置页定位高亮的无 GPU 回归。
/// @details 直接复用生产索引与 Clay 设置行，禁止用复制的行表或假控件替代。
///
/// 回归边界：
/// - 目录只描述真实设置行，重复标签通过分类与分组身份区分；
/// - 双语名称和近义词在两种活动语言下都可检索；
/// - 多关键词可以跨字段命中，但不能把不相关行并入结果；
/// - 精确名称优先于仅选项或说明命中，结果缓存不会每帧翻译；
/// - 条件隐藏设置不自动启用依赖，而是定位到所属组；
/// - 折叠组和正文滚动均通过生产 SettingsView 的布局驱动；
/// - 高亮计时必须等真实设置行可见，三秒后边框停止绘制；
/// - 用户手动滚动不被导航持续拉回；
/// - 字体图集只为 ImGui 生成可检查顶点，不连接图形后端；
/// - 语言资源从源码只读读取，配置根由 CTest 单独隔离。

namespace MMM::UI
{
/// @brief 无 GPU 测试只提交导航请求并读取真实布局，不修改设置控件实现。
struct SettingsSearchTestAccess {
    /// @brief 根据稳定标签键选择目录目标；目标不存在即拒绝导航。
    /// @param view 本测试独占的设置视图，不发布外部打开窗口事件。
    /// @param tab 实际分类，不能根据标签键前缀猜测。
    /// @param key 标签翻译键，跨语言时保持不变。
    /// @return 找到目录条目并提交导航请求时返回 true。
    static bool choose(SettingsView& view, Event::SettingsTab tab,
                       std::string_view key)
    {
        // 此入口只代替结果按钮提交索引，不替代真实的展开、滚动或高亮。
        // 后续检查仍要求生产设置行回调提供实际矩形。
        const auto& entries = view.m_settingsSearch->entries();
        const auto  item    = std::find_if(
            entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.m_tab == tab && entry.m_labelKey == key;
            });
        if ( item == entries.end() ) return false;
        view.navigateToSetting(
            static_cast<std::size_t>(item - entries.begin()));
        return true;
    }
    /// @brief 检查真实行已经出现，且高亮在目标可见后仍有完整三秒。
    /// 检查必须放在滚动请求生效之后，不能只验证目标索引存在。
    /// 标题回退仍可高亮，但不算已找到实际设置行。
    /// 此判断只读取本帧状态，不延长截止时间。
    /// @param view 已完成本帧布局的生产视图。
    /// @return 行可见、没有回退且截止时间有效时返回 true。
    /// @details 2.9 秒容差仅吸收首帧 DeltaTime，不接受在点击时提前计时。
    static bool highlighted(const SettingsView& view)
    {
        return view.m_settingsSearchRowBounds &&
               !view.m_settingsSearchTargetUnavailable &&
               view.m_settingsSearchHighlightUntil - ImGui::GetTime() > 2.9;
    }
    /// @brief 返回本帧目标矩形；仅在调用者确认其存在之后使用。
    /// @param view 上一帧已绘制目标设置行的视图。
    /// @return 标准行或单选组报告的实际屏幕矩形。
    /// @details 不用测试估算标签长度或布局行号，防止假几何掩盖回归。
    static Clay_BoundingBox bounds(const SettingsView& view)
    {
        return *view.m_settingsSearchRowBounds;
    }
    /// @brief 验证条件隐藏设置回退到分组标题，没有伪造可编辑设置行。
    /// @param view 正在浏览隐藏设置所属分类的视图。
    /// @return 没有实际行但有真实所属分组标题时返回 true。
    /// @details 源控件的条件分支保持原样，搜索不能绕开它绘制输入框。
    static bool unavailable(const SettingsView& view)
    {
        return view.m_settingsSearchTargetUnavailable &&
               view.m_settingsSearchHeaderBounds &&
               !view.m_settingsSearchRowBounds;
    }
};
}  // namespace MMM::UI

namespace
{
using MMM::Event::SettingsTab;
using MMM::UI::SettingsSearchIndex;
using MMM::UI::SettingsSearchTestAccess;

/// @brief 从当前翻译池借用字符串，测试与生产使用相同版本失效条件。
/// @param key 生命周期稳定且以空字符结尾的翻译键。
/// @return 翻译器稳定字符串池中的零拥有视图。
/// @details 不构造独立测试字典，皮肤默认语言与生产缓存完全一致。
std::string_view translate(std::string_view key)
{
    return MMM::Translation::getActiveTranslator()
        .translate(MMM::Hash::hashString(key), key.data())
        .view();
}

/// @brief 将查询转换为稳定标签键集合，避免断言依赖可见语言或结果行号。
/// @param index 使用生产内置目录的索引。
/// @param query 用户可输入的关键词或多词组合。
/// @return 命中真实行的稳定标签键集合。
/// @details 排序在单独用例验证，此处只检查跨语言命中身份。
std::set<std::string> matches(SettingsSearchIndex& index,
                              std::string_view     query)
{
    index.update(
        query, MMM::Translation::getActiveTranslator().getVersion(), translate);
    std::set<std::string> result;
    for ( const auto& match : index.results() )
        result.insert(index.entries()[match.m_index].m_labelKey);
    return result;
}

/// @brief 菜单查询测试的状态接收器，不持有真正状态栏或原生窗口。
/// @details 本场景只查询入口能力，不应产生消息；接收器满足生产上下文契约。
class MenuQueryStatusSink final : public MMM::UI::IStatusMessageSink
{
public:
    /// @brief 接收并忽略不参与能力判断的状态消息。
    /// @param message 查询测试不检查的临时文本。
    /// @param durationSeconds 不启动真实计时器的显示时长。
    void showStatusMessage(std::string message, float durationSeconds) override
    {
        (void)message;
        (void)durationSeconds;
    }
};

/// @brief 验证专业总开关约束高级菜单，并保留 BMS 子开关偏好。
/// @return 普通模式入口隐藏或禁用，专业模式恢复且子偏好未被修改时返回 true。
/// @details 使用生产动作处理器，不调用 execute 或 save，也不创建项目会话。
/// 展示值与持久偏好分别检查，避免把置灰的勾选误解为仍可使用 BMS。
/// 测试先关闭再开启总开关，保证不依赖动作构造时的模式快照。
/// 音量菜单需要真正隐藏；仅禁止点击不足以简化普通编辑的入口。
/// 模式恢复时复用相同动作实例，检查其查询会采用最新软件配置。
/// 两个原始配置值均在返回前恢复，失败路径也不会污染其他套件用例。
/// 不消费快捷键或提交命令，逻辑命令拒绝另由会话回归覆盖。
bool checkProfessionalMenuPolicy()
{
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    const auto originalProfessional = settings.professionalMode;
    const auto originalBms          = settings.enableBmsEditing;
    auto       bms                  = MMM::UI::createBmsEditingToggleAction();
    auto       volume = MMM::UI::createEditSelectedObjectVolumeAction();
    MenuQueryStatusSink      sink;
    MMM::UI::MainMenuContext context{ sink };
    settings.enableBmsEditing = true;
    settings.professionalMode = false;
    // 菜单展示有效状态，但不能把普通模式的 false 写回已保存的子偏好。
    const char* reason = bms->disabledTooltipKey(context);
    // 原因由动作提供，空原因不能构造 string_view 或误判为模式限制。
    const bool disabled = !bms->isEnabled(context) && !*bms->value(context) &&
                          !volume->isVisible(context) &&
                          !volume->isEnabled(context) &&
                          settings.enableBmsEditing && reason &&
                          std::string_view(reason) ==
                              "ui.settings.software.professional_required";
    settings.professionalMode = true;
    const bool restored = bms->isEnabled(context) && *bms->value(context) &&
                          volume->isVisible(context) &&
                          volume->isEnabled(context) &&
                          !bms->disabledTooltipKey(context);
    // 恢复测试前的配置，避免在同进程套件中影响其他 UI 用例。
    settings.professionalMode = originalProfessional;
    settings.enableBmsEditing = originalBms;
    return disabled && restored;
}

/// @brief 检查分类覆盖与复合身份唯一性，防止同名字段跨页合并。
/// @return 所有八个分类都有可检索设置且身份不重复时返回 true。
/// @details 不固定总行数，允许将来增加设置而不改测试期望。
/// 同一标签在软件和项目页分别指向不同作用域，必须保留两个条目。
bool checkCatalog()
{
    SettingsSearchIndex                                         index;
    std::set<SettingsTab>                                       tabs;
    std::set<std::tuple<SettingsTab, std::string, std::string>> identities;
    for ( const auto& entry : index.entries() ) {
        // 自动备份同时存在软件和项目页，两者必须以分类区分。
        if ( entry.m_labelKey.empty() || entry.m_sectionKey.empty() ||
             !identities
                  .emplace(entry.m_tab, entry.m_sectionKey, entry.m_labelKey)
                  .second )
            return false;
        tabs.insert(entry.m_tab);
    }
    return tabs.size() == 8;
}

/// @brief 验证当前中英文界面都能用 offset、偏移、误差找到三项视觉偏移。
/// @details 组合词还应区分波形与对数谱图，不能全部指向同一个泛化结果。
/// @return 两种界面语言的同义查询、分词筛选和空查询都正确时返回 true。
/// 默认双语别名与当前活动语言无关，因此每次切换都复用同一个索引。
/// 其他明确包含偏移的设置可以同时出现，不能把搜索硬编码为三条结果。
bool checkSynonyms()
{
    SettingsSearchIndex index;
    auto&               translator = MMM::Translation::getActiveTranslator();
    const std::set<std::string> offsetKeys{
        "ui.settings.visual.visual_offset",
        "ui.settings.visual.waveform_visual_offset",
        "ui.settings.visual.spectrum_visual_offset"
    };
    for ( const auto* language : { "zh_cn", "en_us" } ) {
        if ( !translator.switchLang(language) ) return false;
        for ( const auto* query : { "offset", "  OFFSET\t", "偏移", "误差" } ) {
            const auto result = matches(index, query);
            // 允许其他明确含偏移的设置出现，但截图中的三项必须全部命中。
            for ( const auto& key : offsetKeys )
                if ( !result.contains(key) ) return false;
        }
        const auto waveform = matches(index, "waveform 误差");
        const auto spectrum = matches(index, "对数图 offset");
        if ( !waveform.contains("ui.settings.visual.waveform_visual_offset") ||
             waveform.contains("ui.settings.visual.spectrum_visual_offset") ||
             !spectrum.contains("ui.settings.visual.spectrum_visual_offset") ||
             spectrum.contains("ui.settings.visual.waveform_visual_offset") )
            return false;
        // 搜索选项名称同样指向其实际设置行，未知词与空白查询不返回全目录。
        if ( !matches(index, "Experimental")
                  .contains("ui.settings.visual.spectrum_detail") ||
             !matches(index, "\t  ").empty() ||
             !matches(index, "不存在的关键词xyz").empty() )
            return false;
    }
    return true;
}

/// @brief 验证精确名称优先与查询缓存，常态帧不再调用翻译器。
/// @return 相同输入不重译、版本失效会重译且精确结果首位时返回 true。
/// @details 计数回调只观察正式 update 是否查询翻译，不改变候选数据。
/// 版本递增模拟语言覆写发布，避免为缓存测试扫描额外皮肤文件。
bool checkCacheAndRanking()
{
    SettingsSearchIndex index;
    int                 lookups             = 0;
    const auto          countingTranslation = [&](std::string_view key) {
        ++lookups;
        return translate(key);
    };
    const auto version = MMM::Translation::getActiveTranslator().getVersion();
    // 原设置名直接取当前语言文案，不能依赖测试机器的默认语言。
    const std::string name(
        translate("ui.settings.visual.waveform_visual_offset"));
    index.update(name, version, countingTranslation);
    if ( index.results().empty() ||
         index.entries()[index.results().front().m_index].m_labelKey !=
             "ui.settings.visual.waveform_visual_offset" )
        return false;
    const auto before = lookups;
    index.update(name, version, countingTranslation);
    if ( lookups != before ) return false;
    // 语言版本变化必须更新展示文本，而稳定目录索引不变。
    index.update(name, version + 1, countingTranslation);
    return lookups > before;
}

/// @brief 查找生产设置正文 Child，搜索结果必须使用另一个滚动身份。
/// @return 本帧活动正文 Child；缺失时返回空指针。
/// @details 只借用 ImGui 管理的窗口，测试不拥有或销毁该指针。
/// 窗口标题允许本地化，固定 Child 后缀仍用于确定目标容器。
ImGuiWindow* settingsContent()
{
    for ( auto* window : ImGui::GetCurrentContext()->Windows )
        if ( window->Active &&
             std::string_view(window->Name).find("SettingsContent") !=
                 std::string_view::npos )
            return window;
    return nullptr;
}

/// @brief 检查行边框顶点确实使用主题强调色，避免仅修改状态却没有实际绘制。
/// @param content 实际正文窗口，不能读取前景遮罩的 DrawList。
/// @param bounds 原设置行报告的真实边界。
/// @return 上边框角点存在主题强调色顶点时返回 true。
/// @details 同色分组背景处于另一高度，边界几何同时参与判定。
bool hasHighlightVertices(const ImGuiWindow& content, Clay_BoundingBox bounds)
{
    const auto color = ImGui::GetColorU32(ImGuiCol_HeaderActive);
    return std::any_of(content.DrawList->VtxBuffer.begin(),
                       content.DrawList->VtxBuffer.end(),
                       [&](const auto& vertex) {
                           // 设置行上边框有两个同色端点，不与分组标题背景混淆。
                           return vertex.col == color &&
                                  std::abs(vertex.pos.y - bounds.y) < 2.0f &&
                                  std::abs(vertex.pos.x - bounds.x) < 4.0f;
                       });
}

/// @brief 通过真实禁用菜单项验证专业模式提示能在悬浮时绘制。
/// @return 普通模式有提示且不改变 BMS 偏好，专业模式不再显示限制时为 true。
/// @details 不以单独检查翻译键代替控件渲染，覆盖提示内部再次判断悬浮的回归。
/// @note 使用与主菜单相同的动作、图标菜单项和动画提示入口。
/// @note 鼠标事件送入 ImGui，矩形来自生产控件，不猜测文字宽度或行高。
/// @note 不创建真实系统窗口或 GPU，检查当前帧 Tooltip 窗口的可见几何。
/// @note 暂时修改的模式与 BMS 偏好在返回前恢复，不污染导航测试。
/// @note 只测试原因提示，不消费快捷键、不调用配置持久化。
/// @warning 无 GPU 测试仅推进有限帧，动画用 DeltaTime 演进而非阻塞等待。
bool checkProfessionalDisabledTooltip()
{
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    const bool originalProfessional = settings.professionalMode;
    const bool originalBms          = settings.enableBmsEditing;
    // 故意保留已开启的 BMS 偏好，覆盖切回普通模式后的禁用状态。
    // 专业限制只能改变可操作性，不能因绘制提示而清除用户偏好。
    settings.professionalMode = false;
    settings.enableBmsEditing = true;
    MenuQueryStatusSink      sink;
    MMM::UI::MainMenuContext context{ sink };
    // 图标分支与真实 BMS 菜单一致，动作负责决定禁用及原因。
    MMM::UI::MainMenuToggleItem item("BMS Editing",
                                     MMM::UI::MainMenuItemTextKind::Literal,
                                     MMM::UI::createBmsEditingToggleAction(),
                                     "*");
    auto&                       io = ImGui::GetIO();
    ImVec2                      itemCenter;
    bool                        initialFrame = true;
    const auto                  frame        = [&]() {
        // 用独立窗口 ID 隔离导航页；禁用提示动画仍由生产组件管理。
        // 固定尺寸让鼠标命中不受此前设置页的布局与滚动位置影响。
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 400, 200 });
        ImGui::Begin("ProfessionalTooltipTest",
                     nullptr,
                     ImGuiWindowFlags_NoSavedSettings);
        item.render(context);
        // 首帧没有提示，LastItem 是菜单项，可安全读取真实矩形供后续悬浮。
        if ( initialFrame ) {
            const auto low  = ImGui::GetItemRectMin();
            const auto high = ImGui::GetItemRectMax();
            itemCenter   = { (low.x + high.x) * .5f, (low.y + high.y) * .5f };
            initialFrame = false;
        }
        ImGui::End();
        ImGui::Render();
        // 只看本帧活动且非隐藏的 Tooltip，旧帧留下的窗口对象不能算通过。
        // 检查顶点同时排除存在窗口但内容未提交的首帧布局占位。
        // 不要求具体顶点数量，字体与主题变化不应改变提示可见性结论。
        for ( const auto* window : ImGui::GetCurrentContext()->Windows ) {
            if ( (window->Flags & ImGuiWindowFlags_Tooltip) && window->Active &&
                 !window->Hidden && !window->DrawList->VtxBuffer.empty() )
                return true;
        }
        return false;
    };
    // 先建立窗口及菜单项布局，窗口命中由下一帧鼠标事件计算。
    io.AddMousePosEvent(-100.0f, -100.0f);
    frame();
    io.AddMousePosEvent(itemCenter.x, itemCenter.y);
    bool displayed = false;
    // 有限帧推进现有提示动画，不能通过直接改动画缓存伪造提示可见。
    // 持续悬浮而不发送点击，专门覆盖灰色菜单项仍可解释禁用原因。
    // 多帧结果取并集，允许 ImGui 在首次出现时先完成弹窗布局。
    for ( int index = 0; index < 8; ++index ) displayed |= frame();
    const bool preserved = settings.enableBmsEditing;
    // 模式恢复后同一个动作立即停止给出限制，不靠重新构造菜单生效。
    settings.professionalMode = true;
    // 先推进一帧撤销旧提示，再检查下一帧，避免旧帧几何造成假失败。
    frame();
    const bool cleared        = !frame();
    settings.professionalMode = originalProfessional;
    settings.enableBmsEditing = originalBms;
    // 失败也先恢复配置，日志和后续检查不得观察临时测试模式。
    // 鼠标位置也恢复到窗口外，避免后续导航测试意外悬浮某个设置。
    io.AddMousePosEvent(-100.0f, -100.0f);
    if ( !displayed || !cleared || !preserved ) {
        XERROR("专业模式禁用提示回归失败：显示={}，恢复={}，偏好={}",
               displayed,
               cleared,
               preserved);
    }
    return displayed && cleared && preserved;
}

/// @brief 驱动真实视觉页，验证展开、滚动、三秒高亮及手动滚动不被抢回。
/// @param manager 唯一 UIManager，负责初始化 Clay 字体测量桥。
/// @return 所有实际布局和隐藏设置场景都通过时返回 true。
/// @details 初始折叠状态与滚动位置故意不利于目标显示，以覆盖自动定位。
/// 前方折叠组的行数改变不能使目标组状态 ID 漂移。
/// 导航提交后只允许一次自动滚动，不能持续覆盖用户的滚动输入。
/// 隐藏设置使用系统光标作为场景，避免依赖项目或谱面的测试资源。
/// 实际边框与计时状态分别验证，防止只改成员而遗漏 DrawList 绘制。
/// 检查不写配置文件、不提交命令，仅改变隔离实例的 UI 状态。
/// 失败输出包含实际几何参数，正常通过时保持日志简洁。
/// 每次 frame 都执行 SettingsView::update 与 ImGui::Render，不使用虚拟行。
/// 软件光标用例临时修改隔离配置草稿，检查结束后恢复原值。
/// @warning 无 GPU 测试只推进有限帧；模拟时间通过 DeltaTime，不执行 sleep。
bool checkNavigation(MMM::UI::UIManager& manager)
{
    MMM::UI::SettingsView view("SettingsSearchTest");
    view.open(SettingsTab::Visual);
    auto&      io    = ImGui::GetIO();
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 1050, 700 });
        view.update(&manager);
        ImGui::Render();
    };
    // 两帧让 Child 建立完整滚动上限，再模拟所有分组已被用户折叠。
    frame();
    frame();
    auto* content = settingsContent();
    if ( !content ) {
        XERROR("设置搜索测试：正文窗口未创建");
        return false;
    }
    // 模拟真实用户已折叠的标题状态，而非直接设置目标组为展开。
    // 导航必须在生产 header helper 中建立实际设置子树才能通过。
    for ( auto& item : content->StateStorage.Data ) item.val_i = 0;
    content->Scroll.y = content->ScrollMax.y;
    if ( !SettingsSearchTestAccess::choose(
             view,
             SettingsTab::Visual,
             "ui.settings.visual.spectrum_visual_offset") )
        return false;
    // 第一帧展开组并提出滚动请求，第二帧目标可见才开始三秒计时。
    frame();
    frame();
    // 重新查找活动窗口，防止误读取首帧之前的 Child 绘制缓存。
    // 几何必须落在正文可见区域，不能用离屏矩形启动高亮计时。
    content = settingsContent();
    if ( !content || !SettingsSearchTestAccess::highlighted(view) ) {
        XERROR("设置搜索测试：目标未获得完整高亮，正文存在 {}",
               content != nullptr);
        return false;
    }
    const auto bounds = SettingsSearchTestAccess::bounds(view);
    if ( bounds.y < content->Pos.y ||
         bounds.y + bounds.height > content->Pos.y + content->Size.y ||
         !hasHighlightVertices(*content, bounds) ) {
        XERROR(
            "设置搜索测试：目标边框检查失败，行 y={} 高={}，窗口 y={} "
            "高={}，顶点={}",
            bounds.y,
            bounds.height,
            content->Pos.y,
            content->Size.y,
            hasHighlightVertices(*content, bounds));
        return false;
    }
    // 经过三秒后边框必须消失，不能让持续渲染重置截止时间。
    // 推进 ImGui 的单调帧时间，不等待真实三秒，保持测试快速可重复。
    // 状态截止时间和真实边框顶点分别检查，不能只验证其中之一。
    io.DeltaTime = 1.1f;
    frame();
    frame();
    frame();
    if ( hasHighlightVertices(*content,
                              SettingsSearchTestAccess::bounds(view)) )
        return false;
    io.DeltaTime      = 1.0f / 60.0f;
    content->Scroll.y = content->ScrollMax.y;
    frame();
    if ( std::abs(content->Scroll.y - content->ScrollMax.y) > 1.0f )
        return false;

    // 系统光标模式隐藏软件光标尺寸，定位不能擅自打开软件光标。
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    const auto original  = settings.cursorStyle;
    settings.cursorStyle = MMM::Config::CursorStyle::System;
    const bool chosen    = SettingsSearchTestAccess::choose(
        view, SettingsTab::Software, "ui.settings.software.cursor_size");
    frame();
    frame();
    const bool hidden =
        chosen && SettingsSearchTestAccess::unavailable(view) &&
        settings.cursorStyle == MMM::Config::CursorStyle::System;
    // 搜索导航没有配置提交；恢复草稿避免影响后续同进程套件测试。
    // 此检查不调用 save，也不创建音频、光标纹理或原生窗口。
    settings.cursorStyle = original;
    if ( !hidden ) XERROR("设置搜索测试：隐藏设置未回退到所属分组");
    if ( !hidden ) return false;
    // 专业模式位于软件页通用组，搜索导航应能定位到真实开关并绘制高亮。
    const bool professionalChosen = SettingsSearchTestAccess::choose(
        view, SettingsTab::Software, "ui.settings.software.professional_mode");
    frame();
    frame();
    return professionalChosen && SettingsSearchTestAccess::highlighted(view);
}
}  // namespace

/// @brief 加载只读默认语言资源，并运行内存索引与无 GPU 界面检查。
/// @param argc 必须提供默认翻译目录。
/// @param argv 翻译目录由 CMake 显式传入，不依赖工作目录。
/// @return 分阶段退出码分别表示目录、语言、匹配、缓存或导航失败。
/// @details 语言加载与索引先验证，通过后才创建 ImGui 上下文。
/// 无 GPU 场景使用正式字体图集，边框检查读取真实绘制顶点。
/// UIManager 的作用域先结束，防止视图析构引用已销毁的上下文。
/// 配置隔离由套件注册入口完成，测试不覆盖用户主配置。
/// 不连接音频、网络、Vulkan 或原生平台后端，避免依赖桌面状态。
/// 每个阶段都有独立退出码，CI 能区分数据目录与几何导航问题。
/// 对界面失败记录实际行与窗口尺寸，方便定位裁剪和计时差异。
/// 测试推进虚拟时间后恢复正常帧步长，避免污染后续断言。
/// 所有资源都通过值语义或上下文作用域释放，无跨线程等待。
int main(int argc, char** argv)
{
    if ( argc != 2 || !checkCatalog() || !checkProfessionalMenuPolicy() )
        return 1;
    auto&      translator   = MMM::Translation::getActiveTranslator();
    const auto translations = std::filesystem::path(argv[1]);
    if ( !translator.loadLanguage("zh_cn",
                                  (translations / "zh_cn.lua").string()) ||
         !translator.loadLanguage("en_us",
                                  (translations / "en_us.lua").string()) )
        return 2;
    if ( !checkSynonyms() ) return 3;
    if ( !checkCacheAndRanking() ) return 4;
    // CTest 配置根已隔离；关闭 ini 与后端，测试不创建真实系统窗口。
    ImGui::CreateContext();
    auto& io              = ImGui::GetIO();
    io.IniFilename        = nullptr;
    io.DisplaySize        = { 1050, 720 };
    io.DeltaTime          = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    bool passed = false;
    {
        // 管理器初始化 Clay 文本测量，并早于 ImGui 上下文析构释放视图。
        MMM::UI::UIManager manager;
        passed = checkProfessionalDisabledTooltip() && checkNavigation(manager);
    }
    ImGui::DestroyContext();
    return passed ? 0 : 5;
}
