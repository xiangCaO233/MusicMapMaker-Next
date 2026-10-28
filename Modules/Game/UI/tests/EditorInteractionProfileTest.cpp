#include "common/LogicCommands.h"
#include "common/SelectionOperation.h"
#include "config/EditorSettings.h"
#include "config/EditorShortcutActions.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"
#include "ui/imgui/menu/interfaces/IMainMenuToggleItemActionHandler.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/interaction/EditorInteraction.h"
#include <imgui.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace
{
namespace Interaction = MMM::UI::Interaction;
/// @brief 测试只验证行为契约，失败标签使用项目日志而非标准输出。
/// @param value 生产路径已经计算的行为结果，不在日志分支重新执行动作。
/// @param stage 失败阶段的静态标签，方便单个 CTest 返回多个独立失败点。
/// @return 保留原断言值，使同一进程继续检查其他独立契约。
bool require(bool value, const char* stage)
{
    if ( !value ) XERROR("EditorInteractionProfileTest: {}", stage);
    return value;
}
/// @brief 状态接收端不建立窗口，避免测试依赖主菜单绘制。
class Sink final : public MMM::UI::IStatusMessageSink
{
public:
    /// @brief 动作请求可以提供反馈，但此用例不访问文件或显示系统窗口。
    /// @param message 本测试不检查可见反馈，不持有跨帧的字符串或窗口引用。
    /// @param durationSeconds 不建立定时器，避免把时序测试变成阻塞等待。
    void showStatusMessage(std::string, float) override {}
};
/// @brief 业务动作与输入绑定分离，可从快捷键和方案两种入口使用。
class Action final : public MMM::UI::IMainMenuItemActionHandler
{
public:
    /// @brief 稳定身份由用例拥有的字符串字面量提供。
    /// @param id 由测试字面量持有，不从临时 std::string 借用。
    /// @details 不给动作分配 ImGui 控件，才能证明派发不依赖实际菜单点击。
    explicit Action(std::string_view id) : m_id(id) {}
    /// @brief 用同一处理器类型测试多种动作身份，不依赖 RTTI。
    std::string_view operationId() const override { return m_id; }
    /// @brief 禁用门禁必须在方案拦截和实际执行之前生效。
    /// @details 门禁属于业务动作，新的输入方案不能直接访问其副作用。
    /// @warning 只读取测试 UI 线程状态，不等待逻辑会话。
    bool isEnabled(const MMM::UI::MainMenuContext&) const override
    {
        return m_enabled;
    }
    /// @brief 累计真正执行业务的次数，不把命中快捷键视为成功执行。
    /// @details 计数在真正执行后才变化，命中输入或查询可用性不算执行。
    /// @warning 测试动作没有 IO，失败不会留下工程或配置输出。
    void execute(MMM::UI::MainMenuContext&,
                 const MMM::UI::MainMenuItemActivation&) override
    {
        ++m_executions;
    }
    /// @brief 当前动作是否允许执行。
    bool m_enabled{ true };
    /// @brief 实际执行次数用于检测丢失和重复派发。
    int m_executions{};

private:
    /// @brief 字面量身份不拥有动态内存，也不受翻译影响。
    std::string_view m_id;
};
/// @brief 勾选动作的菜单和方案入口必须只翻转一次。
class Toggle final : public MMM::UI::IMainMenuToggleItemActionHandler
{
public:
    /// @brief 独立的测试身份不会与内置动作冲突。
    std::string_view operationId() const override { return "test-toggle"; }
    /// @brief 指向实际状态，不能让渲染层提前修改。
    /// @details 复用生产 perform 的翻转逻辑，测试不自己预先切换状态。
    /// @return 测试对象拥有的布尔值，生命周期覆盖整个动作登记期。
    bool* value(MMM::UI::MainMenuContext&) override { return &m_state; }
    /// @brief 执行时记录已经翻转后的状态。
    void execute(MMM::UI::MainMenuContext&,
                 const MMM::UI::MainMenuItemActivation&) override
    {
        ++m_executions;
    }
    /// @brief 状态默认关闭，第一次激活必须打开。
    bool m_state{};
    /// @brief 副作用次数与每次激活一一对应。
    int m_executions{};
};
/// @brief 代表隐藏但仍持有事务的旧画布，不能只检查物理鼠标。
class GestureGuard final : public Interaction::IInteractionGuard
{
public:
    /// @brief 状态来自测试 UI 线程，没有逻辑线程锁或原子查询。
    /// @return 即使物理按钮为空闲，仍持有事务就必须拒绝全局切换。
    /// @warning 观察指针在对象销毁前移除，不返回或复制控制器所有权。
    bool hasActivePointerGesture() const override { return m_active; }
    /// @brief true 表示释放事务尚未完成，不允许切换。
    bool m_active{ true };
};
/// @brief 测试方案同时替换键位、指针、滚轮、语义动作和完整表面处理。
class Profile final : public Interaction::IEditorInteractionProfile
{
public:
    /// @brief 名称只在测试进程注册，不出现在生产可选方案中。
    std::string_view id() const override { return "test-profile"; }
    /// @brief 展示名与稳定身份分开，可独立翻译。
    std::string_view name() const override { return "Test profile"; }
    /// @brief 拷贝动作改用 Ctrl+B，其他用户绑定继续保留。
    /// @param action 只有精确的 copy 身份被替换，不能依据原按键猜测动作。
    /// @param configured 其他用户绑定原样借用，避免切换方案时抹去自定义值。
    /// @warning 返回稳定成员引用，禁止把局部绑定的地址交给下一层。
    const MMM::Config::ShortcutBinding& shortcut(
        std::string_view                    action,
        const MMM::Config::ShortcutBinding& configured) const override
    {
        return action == "copy" ? m_copy : configured;
    }
    /// @brief 只在主谱面交换左右按钮，不改变时间线或标准控件。
    /// @param surface 只修改主谱面输入，时间线保持自己的物理按钮对应。
    /// @param button 逻辑主按钮和次按钮相互交换，平移按钮保持不变。
    /// @warning 每帧仅返回整数，不伪造或删除 ImGui 原始事件。
    int pointerButton(Interaction::Surface surface, int button) const override
    {
        return surface == Interaction::Surface::MainCanvas && button < 2
                   ? 1 - button
                   : button;
    }
    /// @brief 主键映射也可独立于动作绑定替换。
    /// @details 该映射与 copy 动作绑定各自独立，验证两层抽象不会混为一谈。
    /// @warning 按下、保持与释放均经过同一映射，不能每帧改变对应关系。
    int key(Interaction::Surface, int physical) const override
    {
        return physical == ImGuiKey_A ? ImGuiKey_B : physical;
    }
    /// @brief 反向滚轮只影响指定表面，横轴保留。
    /// @param horizontal 横向滚动不反转，验证两个轴分别解释。
    /// @details 其他表面的滚动不得被主画布策略污染。
    /// @warning 只返回输入值的变换，不累计残余或修改全局 IO。
    float wheel(Interaction::Surface surface, float value,
                bool horizontal) const override
    {
        return surface == Interaction::Surface::MainCanvas && !horizontal
                   ? -value
                   : value;
    }
    /// @brief 将源动作映射到目标业务动作，不能递归进入同一拦截器。
    /// @details 新方案已经选择目标后必须消费源请求，即使目标不可用也不回退源。
    /// @warning 目标执行经过能力校验，不能直接调用其 execute 绕过禁用状态。
    bool handleAction(
        std::string_view action, MMM::UI::MainMenuContext& context,
        const MMM::UI::MainMenuItemActivation& activation) override
    {
        if ( action != "test-source" ) return false;
        Interaction::executeAction("test-destination", context, activation);
        return true;
    }
    /// @brief 只消费本地 Undo，其他命令必须继续进入真实总线。
    /// @details 测试只拦截 Undo，确保消费一个命令不会吞掉相邻的 Redo。
    /// @warning 事件只在本次调用有效，不复制大型命令或保留内部引用。
    bool handleCommand(const MMM::Event::LogicCommandEvent& event) override
    {
        return std::holds_alternative<MMM::Logic::CmdUndo>(event.command);
    }
    /// @brief 新方案可以独占表面而不执行经典状态机。
    /// @details 时间线返回 false 继续经典操作，主画布返回 true 取得独占所有权。
    /// @warning 每次生产表面派发只增加一次计数，不在此创建新的视图。
    bool handleSurface(const Interaction::SurfaceFrame& frame) override
    {
        ++m_surfaces;
        return frame.m_surface == Interaction::Surface::MainCanvas;
    }
    /// @brief 切入和切出必须各执行一次。
    /// @details 连续安全帧只造成一次激活，重复请求不得重复清空新方案状态。
    void activate() override { ++m_activations; }
    /// @brief 切出时清理方案拥有的状态，不修改谱面。
    /// @details 未知方案回退经典同样需要执行一次完整的切出生命周期。
    void deactivate() override { ++m_deactivations; }
    /// @brief 生命周期和表面调用计数均由 UI 测试线程维护。
    int m_activations{}, m_deactivations{}, m_surfaces{};

private:
    /// @brief 方案自有绑定返回稳定引用，不在查询时分配字符串。
    MMM::Config::ShortcutBinding m_copy{ true, "B", true, false, false, false };
};
/// @brief 建立无 GPU 的真实输入帧，验证映射并未反向修改 ImGui 物理状态。
/// @details 测试直接驱动公开 AddKeyEvent 和
/// AddMouseButtonEvent，再调用正式映射查询。
/// @return 按下、拖动、释放、滚轮和嵌套上下文全部满足契约时返回 true。
/// @warning 只创建 CPU 字体数据，不创建平台窗口、Vulkan 设备或音频输出。
bool testInput()
{
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    // 本用例将不同设备事件放入同一帧；不把 ImGui 的分类拆帧延迟混入映射断言。
    io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize                  = { 800, 600 };
    io.DeltaTime                    = 1.0f / 60;
    unsigned char* pixels{};
    int            width{}, height{};
    // 字体初始化只满足 ImGui CPU 帧前置条件，无图形设备可用时仍可运行。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 输入排队顺序与 GLFW/SDL 平台后端一致，不直接写内部按键缓存。
    // 修饰键先于主键入队，避免用直接写 IO 的方式伪造一个不可能的状态。
    // Ctrl+B 同时验证语义绑定和物理主键重映射，避免只测静态配置值。
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_B, true);
    io.AddMousePosEvent(123, 234);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    io.AddMouseWheelEvent(0, 2);
    // NewFrame 发布这批输入，所有后续查询都读取相同的帧快照。
    ImGui::NewFrame();
    bool ok = true;
    {
        const Interaction::SurfaceScope scope(Interaction::Surface::MainCanvas);
        ok &= require(Interaction::mouseDown(0) && Interaction::mouseClicked(0),
                      "mapped press");
        ok &= require(!ImGui::IsMouseDown(0) && ImGui::IsMouseDown(1),
                      "physical state unchanged");
        // 按下位置必须与映射后按钮一致，不能继续从物理左键读取空起点。
        ok &= require(Interaction::mouseClickedPosition(0).x == 123,
                      "matching press origin");
        // 同一物理增量只在主画布反向，不能通过修改 IO 影响其他控件。
        ok &= require(Interaction::mouseWheel() == -2, "mapped wheel");
        ok &= require(Interaction::keyPressed(ImGuiKey_A, false),
                      "mapped primary key");
        ok &= require(MMM::UI::ShortcutUtils::isActionShortcutPressed("copy"),
                      "semantic shortcut override");
        {
            // 嵌套域退出后仍须回到主画布，而非直接重置到应用默认域。
            const Interaction::SurfaceScope nested(
                Interaction::Surface::Timeline);
            ok &=
                require(!Interaction::mouseDown(0) && Interaction::mouseDown(1),
                        "surface isolation");
        }
        ok &= require(
            Interaction::currentSurface() == Interaction::Surface::MainCanvas,
            "nested context restore");
    }
    ok &= require(
        Interaction::currentSurface() == Interaction::Surface::Application,
        "outer context restore");
    ImGui::EndFrame();
    // 第二帧保持同一物理按钮，连续手势应继续来自映射后的右键。
    io.AddMousePosEvent(200, 300);
    // NewFrame 发布这批输入，所有后续查询都读取相同的帧快照。
    ImGui::NewFrame();
    {
        const Interaction::SurfaceScope scope(Interaction::Surface::MainCanvas);
        // 保持帧不再添加按下事件，验证拖动没有依赖重复点击边沿。
        ok &= require(Interaction::mouseDragging(0), "mapped drag");
        ok &= require(Interaction::mouseDragDelta(0).x == 77,
                      "mapped delta origin");
    }
    ImGui::EndFrame();
    // 第三帧释放，不能读取另一个按钮的空释放状态。
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    // NewFrame 发布这批输入，所有后续查询都读取相同的帧快照。
    ImGui::NewFrame();
    {
        const Interaction::SurfaceScope scope(Interaction::Surface::MainCanvas);
        // 释放对应右键的同一事务，旧左键状态不能被误当作这次结束。
        ok &=
            require(Interaction::mouseReleased(0) && !Interaction::mouseDown(0),
                    "mapped release");
    }
    ImGui::EndFrame();
    // 所有帧都已 EndFrame 后才销毁上下文，不让断言隐藏在进程退出阶段。
    ImGui::DestroyContext();
    return ok;
}
/// @brief 验证真实镜像动作处理器共享纯鼠标粘贴锚点，兼容无锚点菜单入口。
/// @return 稳定动作身份、命令类型及逐次锚点都正确时返回真。
/// @details 通过正式动作注册表请求处理器，不能只构造正确 CmdPaste 模拟成功。
/// 系统剪贴板回调在测试上下文中返回空值，不读取用户桌面剪贴板。
/// 使用两个不同锚点后再用无锚点请求，能够检出落点在处理器里被缓存的回归。
/// @pre 当前活动方案为纯鼠标，测试独占 ImGui 上下文和动作登记生命周期。
/// @note 不绑定正在编辑的真实谱面，事件仅作观察，不启动文件或音频操作。
/// @note 空剪贴板不阻止观察命令，实际复制内容由独立粘贴控制器测试验证。
/// @note 锚点比较不做舍入，菜单此前已决定最终吸附位置。
/// @note 原地镜像与粘贴分开计数，不能用相同总次数掩盖动作路由错误。
/// @post 订阅和动作登记均先于栈对象销毁清理，保持观察地址有效。
bool testMirrorActions()
{
    using namespace MMM;
    ImGui::CreateContext();
    // 粘贴会调用平台读取回调，测试必须隔离外部剪贴板以保证可重复性。
    ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext*) {
        return "";
    };
    Sink                sink;
    UI::MainMenuContext context{ sink };
    auto                paste  = UI::createMirrorPasteAction();
    auto                mirror = UI::createMirrorAction();
    // 使用工厂创建生产处理器，避免替身动作只验证到注册表协议。
    // 登记对象由当前栈独占拥有，注销之前地址保持稳定。
    Interaction::registerAction(*paste);
    Interaction::registerAction(*mirror);
    std::optional<Logic::CmdPaste> received;
    // 可选值区分完全没有发命令与发出了缺省字段命令两种失败。
    int        pasteCount = 0, mirrorCount = 0;
    auto&      bus = Event::EventBus::instance();
    const auto subscription =
        bus.subscribe<Event::LogicCommandEvent>([&](const auto& event) {
            // 观察真正发往逻辑层的命令，验证动作不是只被注册表找到。
            if ( const auto* command =
                     std::get_if<Logic::CmdPaste>(&event.command) ) {
                received = *command;
                ++pasteCount;
            }
            mirrorCount +=
                std::holds_alternative<Logic::CmdMirrorSelected>(event.command);
        });
    bool ok = true;
    // 两次落点均通过右键来源传入；命令必须保留镜像标志和精确秒锚点。
    for ( const double anchor : { 3.25, 8.5 } ) {
        // 每轮清空上一条命令，不能让第一次成功掩盖后续请求丢失。
        received.reset();
        ok &= Interaction::executeAction(
            "MirrorPasteAction",
            context,
            UI::MainMenuItemActivation{
                .m_source    = UI::MainMenuActivationSource::InteractionProfile,
                .m_pasteTime = anchor });
        ok &= require(received && received->m_mirrored &&
                          received->m_targetTime == anchor,
                      "mirrored paste explicit anchor");
    }
    // 顶栏和快捷键不携带锚点，保持原设置控制定位；上一落点不能泄漏。
    received.reset();
    ok &= Interaction::executeAction("MirrorPasteAction", context, {});
    // 普通菜单未传时间时必须为空，而不是由处理器补一个零秒假锚点。
    ok &= require(received && received->m_mirrored && !received->m_targetTime,
                  "mirrored paste default anchor");
    // 原地镜像复用既有动作，只允许一次语义请求对应一条命令。
    ok &= Interaction::executeAction("MirrorAction", context, {});
    ok &= require(pasteCount == 3 && mirrorCount == 1,
                  "mirror actions dispatch once");
    bus.unsubscribe<Event::LogicCommandEvent>(subscription);
    // 解除监听后再注销处理器，销毁顺序不会留下引用测试局部状态的闭包。
    Interaction::unregisterAction(*paste);
    Interaction::unregisterAction(*mirror);
    // 平台回调由新上下文独占，随上下文销毁，无需改全局桌面服务。
    // 处理器只剩字符串缓存，析构不再访问已经注销的总线订阅。
    ImGui::DestroyContext();
    return ok;
}
}  // namespace

namespace
{
/// @brief 使用真实 ImGui 修饰键帧验证教学门禁，不以方案 ID 绕过实际输入。
/// @pre 输入上下文由用例独立创建，运行时没有活动画笔或拖动守卫。
/// @post 不保存软件配置，键盘状态随独立上下文销毁。
/// @note 两套方案共用相同测试矩阵，不能用不同测试输入规避经典门禁差异。
/// @return 纯鼠标免 Shift，经典严格保持原规则，且 Ctrl 始终被拒绝时返回真。
/// @details 两套方案都覆盖点按与延伸；最终几何验收仍由既有画笔测试覆盖。
bool testLessonGesturePolicies()
{
    ImGui::CreateContext();
    auto& io                        = ImGui::GetIO();
    io.IniFilename                  = nullptr;
    io.DisplaySize                  = { 800, 600 };
    io.DeltaTime                    = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels{};
    int            width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // - 用例拥有独立上下文，任何真实键盘状态都不能从前一测试继承。
    // - 只测试本轮的输入门禁，路线和最终几何仍由既有画笔回归负责。
    bool ok = true;
    for ( const bool mouseOnly : { false, true } ) {
        const auto id = mouseOnly ? "mouse-only" : "classic-desktop";
        // 按安全帧门禁切换，不能直接替换注册表的当前指针。
        Interaction::synchronizeProfile(id, true);
        Interaction::synchronizeProfile(id, true);
        // - 两种 Shift 状态都需要覆盖，纯鼠标不能只在默认未按键时碰巧通过。
        // - Ctrl 是禁止吸附变化的独立条件，不能用免 Shift 顺带放宽 Ctrl。
        // - 每种组合单独推进 NewFrame，查询值必须来自同一真实输入帧。
        for ( const bool shift : { false, true } )
            for ( const bool control : { false, true } ) {
                io.AddKeyEvent(ImGuiMod_Shift, shift);
                io.AddKeyEvent(ImGuiMod_Ctrl, control);
                ImGui::NewFrame();
                const Interaction::SurfaceScope scope(
                    Interaction::Surface::MainCanvas);
                // - 点按和延伸共享策略入口，但经典对 Shift 的要求恰好相反。
                // - 这能检出简单把 extendBrush
                // 替换成真导致单键练习全部失效的实现。
                // - 期望值按契约独立计算，不调用待测函数生成自己的答案。
                for ( const bool extended : { false, true } ) {
                    const bool expected =
                        !control && (mouseOnly || shift == extended);
                    ok &= require(Interaction::lessonBrushModifiersMatch(
                                      extended) == expected,
                                  "lesson gesture modifiers");
                }
                // 策略查询不能写回 ImGuiIO，避免影响选择、滚轮和文本输入。
                ok &= require(io.KeyShift == shift && io.KeyCtrl == control,
                              "lesson leaves physical input intact");
                ImGui::EndFrame();
            }
    }
    // - 测试不修改持久化方案设置，键盘缓存随本上下文销毁。
    // - 返回累计结果，不因最后一个组合通过而掩盖更早的门禁错误。
    ImGui::DestroyContext();
    return ok;
}
}  // namespace
/// @brief 验证应用级方案、真实输入映射、动作权限、生命周期和配置兼容。
/// @details 本用例由独立 CTest 进程运行，测试方案不会进入用户的生产注册表。
/// 配置目录由 mmm_register_test_case 隔离，不读写真实用户的键位设置。
/// 每组检查均走生产入口，不用复制一份映射或动作派发算法代替。
/// 生命周期断言不依赖固定毫秒等待，使用真实的安全帧与事务状态。
/// 重复身份校验同时防止方案覆盖现有对象，观察指针只在测试中借用。
/// 配置往返使用真正的 to_json/from_json，旧版本缺字段采用原默认。
/// 菜单业务通过稳定 ID 执行，不创建或自动点击可见菜单控件。
/// 禁用门禁同时覆盖源与目标，不能因重映射改变编辑能力。
/// 命令出口使用正式 EventBus 订阅，私有副本变化不算发布成功。
/// 所有订阅与动作登记在返回前清理，避免影响套件 runner 的退出。
int main()
{
    bool ok = true;
    // 旧配置仍保留原来的镜像绑定，新动作自动使用之前硬编码的组合。
    MMM::Config::EditorSettings source;
    source.m_interactionProfileId    = "test-profile";
    source.shortcutConfig.m_copy.key = "F8";
    nlohmann::json json              = source;
    const auto     restored          = json.get<MMM::Config::EditorSettings>();
    ok &= require(
        restored.m_interactionProfileId == source.m_interactionProfileId &&
            restored.shortcutConfig.m_copy.key == "F8",
        "config roundtrip");
    // 分别删除方案字段与一个新增动作，真实反序列化必须逐字段回退默认。
    // 目录中的每一项都必须出现在持久化对象，防止设置页能录制但重启丢失。
    // 数量相等同时检查目录不存在重复字段；不依赖枚举的连续整数布局。
    ok &= require(json["shortcutConfig"].size() ==
                      MMM::Config::editorShortcutActions().size(),
                  "complete shortcut catalog");
    // JSON 对象按键比较，不把设置页的展示顺序当作持久化协议。
    // 包括默认禁用动作，未绑定入口同样必须能保存与恢复。
    for ( const auto& entry : MMM::Config::editorShortcutActions() ) {
        // 恢复后重新序列化单条绑定，比较全部修饰键与 enabled，不能只比主键。
        const nlohmann::json binding =
            restored.shortcutConfig.*(entry.m_binding);
        ok &= require(json["shortcutConfig"].contains(entry.m_id) &&
                          json["shortcutConfig"][entry.m_id] == binding,
                      "catalog binding roundtrip");
    }
    json.erase("interactionProfileId");
    json["shortcutConfig"].erase("copy");
    const auto legacy = json.get<MMM::Config::EditorSettings>();
    ok &= require(legacy.m_interactionProfileId == "classic-desktop" &&
                      legacy.shortcutConfig.m_copy.key == "C",
                  "legacy defaults");
    auto profile = std::make_unique<Profile>();
    // 注册表接管所有权后，只借用稳定对象检查生命周期次数。
    auto* observer = profile.get();
    ok &= require(Interaction::registerProfile(std::move(profile)),
                  "registration");
    // 重复注册不能替换原对象，否则已经借用的观察地址会立即悬空。
    ok &= require(!Interaction::registerProfile(std::make_unique<Profile>()),
                  "duplicate rejected");
    GestureGuard guard;
    Interaction::registerInteractionGuard(guard);
    // 输入空闲但旧控制器仍有事务时，也必须保持旧方案。
    ok &= require(!Interaction::synchronizeProfile("test-profile", true),
                  "controller transaction blocked");
    // 测试先结束控制器事务再撤销登记，不依赖物理按钮触发 guard 变化。
    guard.m_active = false;
    Interaction::unregisterInteractionGuard(guard);
    // 只观察物理释放的那一帧不允许切换，旧方案须先完成其收尾。
    ok &= require(!Interaction::synchronizeProfile("test-profile", false),
                  "pressed frame blocked");
    ok &= require(!Interaction::synchronizeProfile("test-profile", true),
                  "release frame preserved");
    ok &= require(Interaction::synchronizeProfile("test-profile", true),
                  "safe frame activation");
    ok &= require(
        observer->m_activations == 1 && Interaction::profileGeneration() == 1,
        "activation once");
    // 输入测试在已激活方案下建立自己的 ImGui 上下文，不借用编辑器窗口。
    ok &= testInput();
    ok &= require(
        Interaction::dispatchSurface({ Interaction::Surface::MainCanvas }),
        "main surface ownership");
    ok &= require(
        !Interaction::dispatchSurface({ Interaction::Surface::Timeline }),
        "other surface fallback");
    ok &= require(observer->m_surfaces == 2, "surface dispatch once");
    // 后续只验证动作协议，状态接收端不会建立隐藏窗口或访问实际工程。
    Sink                     sink;
    MMM::UI::MainMenuContext context{ sink };
    Action                   first("test-source"), second("test-destination");
    Toggle                   toggle;
    Interaction::registerAction(first);
    Interaction::registerAction(second);
    Interaction::registerAction(toggle);
    // 同一条语义激活只执行映射后的目标，禁用源或目标均不能绕过能力门禁。
    Interaction::dispatchAction(first, context, {});
    ok &= require(first.m_executions == 0 && second.m_executions == 1,
                  "semantic remapping");
    // 目标禁用时仍消费源动作；否则可能误执行原本准备替换的业务。
    second.m_enabled = false;
    Interaction::dispatchAction(first, context, {});
    ok &= require(first.m_executions == 0 && second.m_executions == 1,
                  "disabled remap target blocked");
    // 源动作禁用与目标动作禁用是两道独立检查，不能由成功目标掩盖源权限。
    second.m_enabled = true;
    first.m_enabled  = false;
    Interaction::dispatchAction(first, context, {});
    ok &= require(second.m_executions == 1, "disabled source blocked");
    // 方案直接调用最终业务入口时，也必须经过 toggle 的统一 perform。
    Interaction::executeAction("test-toggle", context, {});
    ok &= require(toggle.m_state && toggle.m_executions == 1, "toggle once");
    // 菜单派发和新方案直接调用必须共用同一个翻转步骤，不能预先修改状态。
    Interaction::dispatchAction(toggle, context, {});
    ok &= require(!toggle.m_state && toggle.m_executions == 2,
                  "dispatched toggle once");
    // 销毁或关闭菜单后身份应立即失效，不能留下可调用的悬空对象。
    Interaction::unregisterAction(toggle);
    ok &= require(!Interaction::executeAction("test-toggle", context, {}),
                  "closed action unavailable");
    // 命令测试走生产 EventBus，验证接管与正常转交没有重复发布。
    auto&      bus          = MMM::Event::EventBus::instance();
    int        commands     = 0;
    const auto subscription = bus.subscribe<MMM::Event::LogicCommandEvent>(
        [&](const auto&) { ++commands; });
    // Undo 由方案消费，Redo 交给总线；总计必须恰好一次，不能全部吞掉。
    Interaction::dispatchCommand(
        MMM::Event::LogicCommandEvent(MMM::Logic::CmdUndo{}));
    Interaction::dispatchCommand(
        MMM::Event::LogicCommandEvent(MMM::Logic::CmdRedo{}));
    ok &= require(commands == 1, "local command interception");
    // 发布完成后精确撤销本用例的订阅，不重置整个共享总线。
    bus.unsubscribe<MMM::Event::LogicCommandEvent>(subscription);
    // 先解除非拥有登记再离开栈作用域，真实菜单析构遵循相同顺序。
    Interaction::unregisterAction(first);
    Interaction::unregisterAction(second);
    // 未安装的方案运行时回退，持久化值由调用者保留以便恢复安装。
    ok &= require(Interaction::synchronizeProfile("missing-profile", true),
                  "unknown fallback");
    ok &= require(Interaction::activeProfile().id() == "classic-desktop" &&
                      observer->m_deactivations == 1,
                  "deactivation once");
    // 任一契约失败均保留非零退出码，CTest 不以是否打印日志判断结果。
    // 新方案只改变画笔意图，经典方案必须继续区分有无 Shift。
    const auto& classic = Interaction::activeProfile();
    // 默认方案不返回显式模式，画布必须继续读取经典物理 Ctrl。
    // 测试在真实注册表中切换方案，不能只验证一个独立的策略替身。
    // 即使收到模式设置请求，经典方案也不能留下会影响下一次点击的状态。
    Interaction::activeProfile().setSelectionOperation(
        MMM::Logic::SelectionOperation::Subtract);
    ok &= require(!classic.selectionOperation(),
                  "classic selection operation unchanged");
    // 新菜单随方案启用，经典右键移除框选的约定不应改变。
    ok &= require(!classic.selectionContextMenu(),
                  "classic right click unchanged");
    // 经典方案不保留擦除粒度，必须继续由真实 Shift 控制整条擦除。
    // 设置请求不能令经典查询变成显式 true，从而覆盖之后的物理按键。
    Interaction::activeProfile().setEraseWholePolyline(true);
    ok &=
        require(!classic.eraseWholePolyline(), "classic erase mode unchanged");
    ok &=
        require(!classic.extendBrush(Interaction::Surface::MainCanvas, false) &&
                    classic.extendBrush(Interaction::Surface::MainCanvas, true),
                "classic brush modifiers");
    Interaction::synchronizeProfile("mouse-only", true);
    Interaction::synchronizeProfile("mouse-only", true);
    const auto& mouse = Interaction::activeProfile();
    // 三种模式切换只改变独立意图，不能借修改键盘状态实现追加或减选。
    // 每次读取都应得到刚设置的值，起笔才能把它锁存在逻辑命令中。
    for ( const auto mode : { MMM::Logic::SelectionOperation::Replace,
                              MMM::Logic::SelectionOperation::Add,
                              MMM::Logic::SelectionOperation::Subtract } ) {
        Interaction::activeProfile().setSelectionOperation(mode);
        ok &= require(mouse.selectionOperation() == mode,
                      "mouse selection operation");
    }
    // 结束前恢复默认值，后续输入契约不应依赖本循环最后一次选择。
    Interaction::activeProfile().setSelectionOperation(
        MMM::Logic::SelectionOperation::Replace);
    ok &= require(mouse.selectionContextMenu(), "mouse-only selection menu");
    // 相机选项与画布修饰完全分离，不能把平移选择变成全局 Shift。
    // 四种组合按值来回设置，验证窗口展示的意图确实进入方案策略。
    // 此查询不依赖窗口或鼠标硬件，后续触控入口也应取得相同值语义。
    // 设置一组值不能重置另一字段，两个维度必须可以独立组合。
    for ( bool pan : { false, true } )
        for ( bool fine : { false, true } ) {
            Interaction::activeProfile().setCameraPointerOptions({ pan, fine });
            const auto options = mouse.cameraPointerOptions();
            ok &= require(
                options && options->m_pan == pan && options->m_fine == fine,
                "mouse camera options");
            ok &= require(!mouse.modifier(Interaction::Surface::MainCanvas,
                                          Interaction::Modifier::Shift,
                                          false),
                          "camera options do not synthesize shift");
        }
    // 设置只改变擦除语义，不可泄漏到绘制延伸、框选或物理修饰键。
    Interaction::activeProfile().setEraseWholePolyline(true);
    ok &= require(mouse.eraseWholePolyline() == true,
                  "whole polyline erase mode");
    Interaction::activeProfile().setEraseWholePolyline(false);
    ok &= require(mouse.eraseWholePolyline() == false, "segment erase mode");
    // 两个设置都只在当前方案对象中存储，不通过改写 ImGuiIO 制造修饰键。
    // setter 与查询成对验证，防止只在控件里显示勾选却没有影响策略结果。
    // 临时吸附状态只改变专用查询，不能成为选择、滚轮或平移的虚拟 Ctrl。
    Interaction::activeProfile().setBypassSnapping(true);
    Interaction::activeProfile().setTimingInterpolationDraw(true);
    ok &= require(mouse.bypassSnapping() &&
                      mouse.timingInterpolationDraw() == true &&
                      !mouse.modifier(Interaction::Surface::MainCanvas,
                                      Interaction::Modifier::Control,
                                      false),
                  "independent mouse intent");
    // 这里故意先留下启用状态再退出，比单独测试 false 默认值更严格。
    // 显式请求设置经典方案也必须是无副作用操作，不能留下共享全局状态。
    // 切出方案清理临时覆盖，但经典方案仍返回空绘制模式和关闭的临时开关。
    Interaction::synchronizeProfile("classic-desktop", true);
    Interaction::synchronizeProfile("classic-desktop", true);
    Interaction::activeProfile().setBypassSnapping(true);
    Interaction::activeProfile().setTimingInterpolationDraw(true);
    ok &=
        require(!classic.bypassSnapping() && !classic.timingInterpolationDraw(),
                "classic snap and timing unchanged");
    // 即使显式向经典方案设置，仍必须返回空，才能保留物理 Shift 的动态语义。
    Interaction::activeProfile().setCameraPointerOptions({ true, true });
    ok &= require(!classic.cameraPointerOptions(), "classic camera unchanged");
    Interaction::synchronizeProfile("mouse-only", true);
    Interaction::synchronizeProfile("mouse-only", true);
    ok &= require(!mouse.bypassSnapping(), "temporary snap reset");
    // 退出时清除相机临时状态；再次进入不能默认带着不可见的精细倍率。
    // 仍返回显式选项，不能因清理过程意外退回经典键盘模式。
    const auto resetCamera = mouse.cameraPointerOptions();
    ok &= require(resetCamera && !resetCamera->m_pan && !resetCamera->m_fine,
                  "temporary camera reset");
    Interaction::activeProfile().setTimingInterpolationDraw(false);
    // 下一段仍验证无 Shift 的折线意图，避免独立模式污染主画布绘制规则。
    // 把 Timing 恢复到点模式，后续镜像动作测试不依赖本轮设置顺序。
    // 方案注册必须能通过持久化身份查到，不能只在菜单中添加展示条目。
    // 连续安全帧沿用切换门禁，避免把尚未结束的画笔移交给新方案。
    // 此处不构造 ImGui 上下文，直接检验不依赖设备状态的策略契约。
    // 不把虚拟 Shift 泄漏给选择、滚动和时间点表面。
    // 主画布的绘制延伸与物理键盘完全独立，其他修饰查询仍返回真实状态。
    ok &= require(
        mouse.id() == "mouse-only" &&
            mouse.extendBrush(Interaction::Surface::MainCanvas, false) &&
            !mouse.extendBrush(Interaction::Surface::Timeline, false) &&
            !mouse.modifier(Interaction::Surface::MainCanvas,
                            Interaction::Modifier::Shift,
                            false),
        "mouse-only brush isolation");
    ok &= testMirrorActions();
    ok &= testLessonGesturePolicies();
    return ok ? 0 : 1;
}
