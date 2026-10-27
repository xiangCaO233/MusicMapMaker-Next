#include "ui/interaction/EditorInteraction.h"
#include "config/EditorSettings.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"
#include <algorithm>
#include <imgui.h>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::UI::Interaction
{
namespace
{
/// @brief 经典方案保留修饰键控制的桌面行为。
class ClassicDesktopProfile final : public IEditorInteractionProfile
{
public:
    /// @brief 稳定身份与旧版没有方案字段的配置兼容。
    std::string_view id() const override { return "classic-desktop"; }
    /// @brief 初期名称不依赖皮肤或翻译初始化顺序。
    std::string_view name() const override { return "经典桌面"; }
};
/// @brief 纯鼠标方案的画笔直接延伸，其他操作暂时沿用现有入口。
/// @details 仅声明绘制意图，不改写全局 Shift，避免影响框选和滚动。
class MouseOnlyProfile final : public IEditorInteractionProfile
{
public:
    /// @brief 稳定身份供设置页保存，经典方案仍为旧配置默认值。
    std::string_view id() const override { return "mouse-only"; }
    /// @brief 与经典方案并列显示，用户可在交互设置中切换。
    std::string_view name() const override { return "纯鼠标"; }
    /// @brief 选中工具提供右键菜单，绘制工具仍保留右键擦除。
    /// @warning 每帧常量策略，不持有弹窗或画布状态。
    bool selectionContextMenu() const override { return true; }
    /// @brief 返回应用级纯鼠标模式，默认替换选择。
    /// @warning 每帧只读枚举成员，不复制选择或配置容器。
    std::optional<Logic::SelectionOperation> selectionOperation() const override
    {
        return m_selectionOperation;
    }
    /// @brief 菜单只改 UI 意图，起笔命令负责向逻辑线程锁存本次模式。
    void setSelectionOperation(Logic::SelectionOperation operation) override
    {
        m_selectionOperation = operation;
    }
    /// @brief 纯鼠标默认局部擦除，不依赖 Shift 决定是否删除整条折线。
    /// @warning 仅返回 UI 本地标量，画布起笔后不再重读该设置。
    std::optional<bool> eraseWholePolyline() const override
    {
        return m_eraseWholePolyline;
    }
    /// @brief 只改变下一次擦除意图，不模拟 Shift 或持久化键盘配置。
    void setEraseWholePolyline(bool whole) override
    {
        m_eraseWholePolyline = whole;
    }
    /// @brief 临时吸附只属于纯鼠标方案，不写入软件配置。
    /// @warning 每帧返回本地标量，无跨线程同步。
    bool bypassSnapping() const override { return m_bypassSnapping; }
    /// @brief 修改后由下一次输入查询立即采用，不推迟本地交互。
    void setBypassSnapping(bool bypass) override { m_bypassSnapping = bypass; }
    /// @brief 点模式为默认，插值模式只用于 Timing 表面。
    /// @warning 每次起笔只读取标量，不复制曲线或快照。
    std::optional<bool> timingInterpolationDraw() const override
    {
        return m_timingInterpolation;
    }
    /// @brief 活动手势由画布锁存，设置只影响下一笔。
    void setTimingInterpolationDraw(bool interpolation) override
    {
        m_timingInterpolation = interpolation;
    }
    /// @brief 切出时清除临时忽略吸附，重新进入不会继承隐藏开关。
    void deactivate() override { m_bypassSnapping = false; }
    /// @brief 主画布无需修饰键即可延伸画笔，其他表面保留原语义。
    /// @warning 连续绘制只做枚举判断，不持有会话或跨帧指针。
    bool extendBrush(Surface surface, bool shift) const override
    {
        // 点按仍从 NOTE 起笔，只有移动才由原有工具升级形状。
        // 保留逻辑层的折线能力门禁、取消和撤销事务，不在方案中重复实现。
        return surface == Surface::MainCanvas || shift;
    }

private:
    /// @brief 纯鼠标擦除粒度，跨工具切换保留，但不影响经典方案。
    bool m_eraseWholePolyline{ false };
    /// @brief 不持久化的吸附覆盖值，与键盘修饰和磁吸偏好独立。
    bool m_bypassSnapping{ false };
    /// @brief Timing 后续绘制选择，主画布仍沿用原有单键与折线策略。
    bool m_timingInterpolation{ false };
    /// @brief 模式仅由 UI 线程读写，不参与每帧配置保存。
    Logic::SelectionOperation m_selectionOperation{
        Logic::SelectionOperation::Replace
    };
};
/// @brief 注册表只拥有方案；活动指针在 vector 扩容后仍指向独立对象。
struct Registry {
    /// @brief 动作身份指向菜单对象，登记和移除都在低频视图生命周期内完成。
    std::unordered_map<std::string_view, IMainMenuItemActionHandler*> m_actions;
    /// @brief 控制器观察表只在视图创建和销毁时改变，方案切换时读取。
    std::vector<const IInteractionGuard*> m_guards;
    /// @brief 默认方案先登记，未知配置始终有可用回退。
    std::vector<std::unique_ptr<IEditorInteractionProfile>> m_entries;
    /// @brief UI 线程的非拥有引用，生命周期由 entries 保证。
    IEditorInteractionProfile* m_active{};
    /// @brief 连续安全帧门禁确保旧方案先消费松开再切换。
    bool m_previousSafe{ false };
    /// @brief 只在真实切换时递增，供视图同步控制器身份。
    std::uint64_t m_generation{};
    /// @brief 初始化阶段分配默认对象，常态 UI 帧不创建方案。
    Registry()
    {
        m_entries.push_back(std::make_unique<ClassicDesktopProfile>());
        m_entries.push_back(std::make_unique<MouseOnlyProfile>());
        m_active = m_entries.front().get();
    }
};
/// @brief 函数静态注册表保证第一次使用时默认方案已经存在。
/// @warning 常态 UI
/// 帧只访问已经初始化的注册表；首次构造发生在菜单或管理器创建阶段。
Registry& registry()
{
    static Registry value;
    return value;
}
/// @brief 输入上下文只属于渲染线程，无需跨线程同步或 TLS 分配。
Surface inputSurface{ Surface::Application };
/// @brief UI 栈的观察指针只在 UiFrameScope 生效时存在。
UIManager* frameManager{};
/// @brief 将非法按钮转换为禁用查询，防止第三方方案触发 ImGui 断言。
/// @param button 经典逻辑按钮；负数和超出五键范围的值必须禁用。
/// @warning 每帧只做范围检查和一次方案虚调用，不生成新的物理输入事件。
int mappedButton(int button)
{
    // 非法逻辑按钮不能借助自定义映射重新变成有效的点击。
    if ( button < 0 || button >= 5 ) return -1;
    const int mapped = activeProfile().pointerButton(inputSurface, button);
    return mapped >= 0 && mapped < 5 ? mapped : -1;
}
/// @brief 无效键或方案返回零时停用查询，不误触发 ImGui 内部键空间。
/// @param key 来自 UI 适配层的命名键，不接受平台原生扫描码。
/// @warning 每帧只做映射和边界检查，不能扫描系统键盘状态。
ImGuiKey mappedKey(int key)
{
    const int mapped = activeProfile().key(inputSurface, key);
    return mapped >= ImGuiKey_NamedKey_BEGIN && mapped < ImGuiKey_NamedKey_END
               ? static_cast<ImGuiKey>(mapped)
               : ImGuiKey_None;
}
}  // namespace

/// @brief 默认使用原有配置引用，保证用户已经录制的快捷键不丢失。
/// @param configured 用户已经录制的稳定存储，默认方案只借用而不复制。
/// @warning 快捷键热路径不得创建临时绑定后返回其引用。
const Config::ShortcutBinding& IEditorInteractionProfile::shortcut(
    std::string_view, const Config::ShortcutBinding& configured) const
{
    return configured;
}
/// @brief 经典方案的逻辑按钮与鼠标按钮一一对应。
/// @param button 输入解释中的逻辑按钮，默认与物理按钮一一对应。
/// @warning 每帧查询不维护第二份按钮状态，也不改变平台后端。
int IEditorInteractionProfile::pointerButton(Surface, int button) const
{
    return button;
}
/// @brief 默认不改变 Ctrl、Shift、Alt 和 Command 的解释。
/// @param physical 当前帧聚合左右修饰键后的物理值。
/// @warning 每帧返回值解释，不得改写 ImGui 或逻辑线程状态。
bool IEditorInteractionProfile::modifier(Surface, Modifier, bool physical) const
{
    return physical;
}
/// @brief 默认画笔延伸跟随原有 Shift 解释，保证旧方案行为不变。
/// @warning UI 热路径只返回标量，不访问逻辑线程状态。
bool IEditorInteractionProfile::extendBrush(Surface, bool shift) const
{
    return shift;
}
/// @brief 经典方案保留既有右键行为，新菜单由明确选择的新方案启用。
/// @warning 每帧常量返回，不改变输入事件消费状态。
bool IEditorInteractionProfile::selectionContextMenu() const
{
    return false;
}
/// @brief 默认不接管选择语义，避免改变经典点击切换和 Ctrl 框选。
/// @warning 每帧返回空标量，不创建任何输入状态。
std::optional<Logic::SelectionOperation>
IEditorInteractionProfile::selectionOperation() const
{
    return std::nullopt;
}
/// @brief 经典方案忽略模式设置，不引入隐藏的输入状态。
void IEditorInteractionProfile::setSelectionOperation(Logic::SelectionOperation)
{
}
/// @brief 经典方案无显式擦除粒度，仍由每帧真实 Shift 决定。
/// @warning 查询不分配，不读取输入或配置容器。
std::optional<bool> IEditorInteractionProfile::eraseWholePolyline() const
{
    return std::nullopt;
}
/// @brief 经典方案忽略纯鼠标设置，不留下影响物理修饰键的隐藏状态。
void IEditorInteractionProfile::setEraseWholePolyline(bool) {}
/// @brief 经典模式没有额外吸附覆盖，继续使用原物理修饰键。
/// @warning 纯常量查询，不访问配置或输入设备。
bool IEditorInteractionProfile::bypassSnapping() const
{
    return false;
}
/// @brief 经典方案忽略纯鼠标临时设置，不能产生跨方案污染。
void IEditorInteractionProfile::setBypassSnapping(bool) {}
/// @brief 空模式保留原有 Shift 创建和维持插值范围的规则。
/// @warning 仅返回空标量，不创建或销毁任何手势。
std::optional<bool> IEditorInteractionProfile::timingInterpolationDraw() const
{
    return std::nullopt;
}
/// @brief 经典模式不保存纯鼠标 Timing 选择。
void IEditorInteractionProfile::setTimingInterpolationDraw(bool) {}
/// @brief 吸附覆盖与修饰键组合仅用于放置、移动，不影响选择语义。
/// @warning 常量时间查询，不能把结果当作通用 Ctrl 状态。
bool bypassSnapping()
{
    return activeProfile().bypassSnapping() || modifier(Modifier::Control);
}

/// @brief 将当前表面及修饰键解释传给方案，避免画布识别具体方案 ID。
/// @warning 起笔和连续更新使用同一入口，无额外手势分配或等待。
bool extendBrush()
{
    return activeProfile().extendBrush(currentSurface(),
                                       modifier(Modifier::Shift));
}
/// @brief 默认保留系统滚轮的增量和方向，不额外累计小数。
/// @param physical 保留触控板的小数增量，具体工具负责其步进累计。
/// @warning 每帧不重新查询设备或累计跨表面的滚动残量。
float IEditorInteractionProfile::wheel(Surface, float physical, bool) const
{
    return physical;
}
/// @brief 默认保留原有主键身份。
/// @param physical 平台适配层已经转换成的命名键。
/// @warning 每帧仅映射身份，重复和释放仍由同一输入快照解释。
int IEditorInteractionProfile::key(Surface, int physical) const
{
    return physical;
}
/// @brief 默认交还给经典控制器，不提前消费指针或编辑命令。
/// @return false 明确选择经典状态机，而非表示操作成功或失败。
/// @warning 每个表面的实际输入帧调用，不能启动同步文件操作。
bool IEditorInteractionProfile::handleSurface(const SurfaceFrame&)
{
    return false;
}
/// @brief 经典方案交还原有语义动作，不改变菜单业务流程。
/// @return true 表示已接管请求，即使重映射目标不可用也不应误回退。
/// @warning 只在动作激活时调用；文件操作由已有动作自己的确认流程执行。
bool IEditorInteractionProfile::handleAction(std::string_view, MainMenuContext&,
                                             const MainMenuItemActivation&)
{
    return false;
}
/// @brief 经典方案保留原事件通道，命令负载不在这里复制。
/// @param event 由当前 UI 调用者拥有的完整命令，仅在此调用内借用。
/// @warning 连续输入可能逐帧发布，禁止复制大型命令或等待会话锁。
bool IEditorInteractionProfile::handleCommand(const Event::LogicCommandEvent&)
{
    return false;
}
/// @brief 默认方案没有独立于画布控制器的手势状态。
/// @return 方案自身是否仍持有平台鼠标状态之外的触控或笔事务。
/// @warning 切换门禁只读取 UI 状态，不等待另一线程释放手势。
bool IEditorInteractionProfile::hasActiveGesture() const
{
    return false;
}
/// @brief 无状态经典方案不需要额外激活资源。
/// @details 仅在安全帧边界被调用，新方案可在此建立长期控制状态。
void IEditorInteractionProfile::activate() {}
/// @brief 默认不改变工程、选择集或持久化视图。
/// @details 此时所有事务均已结束，不能把半次笔刷交给下一个方案。
void IEditorInteractionProfile::deactivate() {}
/// @brief 暴露注册列表的只读视图，不复制所有权。
/// @warning 设置页只借用 span；注册造成扩容后必须重新取得视图。
std::span<const std::unique_ptr<IEditorInteractionProfile>> profiles()
{
    return registry().m_entries;
}
/// @brief 校验身份后才转移所有权，失败不能替换当前方案。
/// @param profile 所有权只在校验成功后进入注册表，重复身份不会替换活动对象。
/// @warning 初始化期低频入口，禁止在表面逐帧回调里注册新方案。
bool registerProfile(std::unique_ptr<IEditorInteractionProfile> profile)
{
    // 空对象和空身份没有可持久化的选择语义，必须在登记前拒绝。
    if ( !profile || profile->id().empty() ) return false;
    auto& entries = registry().m_entries;
    if ( std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
             return entry->id() == profile->id();
         }) )
        return false;
    entries.push_back(std::move(profile));
    return true;
}
/// @brief 活动引用直接借用，不建立每帧共享所有权。
/// @warning 每帧借用稳定对象，不复制 unique_ptr 或建立共享所有权。
IEditorInteractionProfile& activeProfile()
{
    return *registry().m_active;
}
/// @brief 未知方案只回退运行行为，保留配置以支持以后再次安装方案。
/// @param requested 来自应用设置的稳定身份，未知名称保留于配置但运行时回退。
/// @param safe 调用者已经检查控件、弹窗、录制与物理输入的归属。
/// @return 只有生命周期真正发生切换时返回 true，同一目标不重复激活。
/// @warning UI 每帧边界不执行 IO，不使用固定等待窗口，不等待逻辑线程。
bool synchronizeProfile(std::string_view requested, bool safe)
{
    auto& state = registry();
    // 隐藏画布也可能仍持有尚未收尾的旧控制器，不能只观察物理鼠标是否松开。
    safe                 = safe && !state.m_active->hasActiveGesture() &&
                           std::none_of(state.m_guards.begin(),
                                        state.m_guards.end(),
                                        [](const auto* guard) {
                            return guard->hasActivePointerGesture();
                                        });
    const bool canSwitch = safe && state.m_previousSafe;
    state.m_previousSafe = safe;
    if ( !canSwitch ) return false;
    auto found = std::find_if(
        state.m_entries.begin(), state.m_entries.end(), [&](const auto& entry) {
            return entry->id() == requested;
        });
    auto* next = found == state.m_entries.end() ? state.m_entries.front().get()
                                                : found->get();
    // 未变化或未知名称已经回退到当前对象时，不重复清空控制器状态。
    if ( next == state.m_active ) return false;
    // 旧方案已处理完整释放帧；再切出可避免将半次拖拽交给另一种解释。
    state.m_active->deactivate();
    state.m_active = next;
    ++state.m_generation;
    state.m_active->activate();
    return true;
}
/// @brief 状态代数只跨真实方案切换变化。
/// @warning 每帧只读单 UI 线程计数，不能将它扩展为跨线程原子状态。
std::uint64_t profileGeneration()
{
    return registry().m_generation;
}
/// @brief 栈式切换保证弹窗或嵌套视图不会污染外层表面。
/// @details 仅切换当前调用栈的输入域，不创建配置或控制器副本。
SurfaceScope::SurfaceScope(Surface surface) : m_previous(inputSurface)
{
    inputSurface = surface;
}
/// @brief 提前返回同样恢复输入上下文。
/// @details 嵌套表面、弹窗或提前返回都遵循相同的恢复顺序。
SurfaceScope::~SurfaceScope()
{
    inputSurface = m_previous;
}
/// @brief 默认应用上下文不依赖当前 ImGui 窗口标题。
/// @warning 每帧只读当前 UI 栈，不查询焦点窗口或解析动态标题。
Surface currentSurface()
{
    return inputSurface;
}
/// @brief 按住状态通过映射按钮读取。
/// @param button 逻辑按钮，映射禁用时返回 false。
/// @warning 每帧查询只读同一 ImGui 输入快照，不加锁或触发事件。
bool mouseDown(int button)
{
    const int mapped = mappedButton(button);
    return mapped >= 0 && ImGui::IsMouseDown(mapped);
}
/// @brief 边沿查询保留重复选项。
/// @param repeat 是否沿用按住重复；编辑起笔通常只使用边沿。
/// @warning 每帧保持调用者的重复选项，不自行轮询或消抖。
bool mouseClicked(int button, bool repeat)
{
    const int mapped = mappedButton(button);
    return mapped >= 0 && ImGui::IsMouseClicked(mapped, repeat);
}
/// @brief 双击阈值继续由平台输入层统一维护。
/// @warning 每帧复用平台双击阈值，不生成另一套计时状态。
bool mouseDoubleClicked(int button)
{
    const int mapped = mappedButton(button);
    return mapped >= 0 && ImGui::IsMouseDoubleClicked(mapped);
}
/// @brief 释放与按下使用相同映射，不维护第二套物理按键状态。
/// @warning 每帧与按下使用同一映射，方案不能在按钮按住时切换。
bool mouseReleased(int button)
{
    const int mapped = mappedButton(button);
    return mapped >= 0 && ImGui::IsMouseReleased(mapped);
}
/// @brief 连续拖拽复用平台累计位移和阈值。
/// @param threshold 负值使用 ImGui 默认阈值，显式值保持原调用契约。
/// @warning 每帧不重新计算或缓存设备位移，不延迟本地拖动反馈。
bool mouseDragging(int button, float threshold)
{
    const int mapped = mappedButton(button);
    return mapped >= 0 && ImGui::IsMouseDragging(mapped, threshold);
}
/// @brief 起点来自映射后的同一物理按钮，无效映射没有有效落点。
/// @return 逻辑按钮对应的物理起点；禁用映射返回不可用坐标。
/// @warning 每帧借用同一输入快照，不把屏幕点当作谱面坐标。
ImVec2 mouseClickedPosition(int button)
{
    const int mapped = mappedButton(button);
    return mapped >= 0 ? ImGui::GetIO().MouseClickedPos[mapped]
                       : ImVec2{ -1.0e20f, -1.0e20f };
}
/// @brief 拖拽位移与平台阈值保持一致，不重新计算屏幕距离。
/// @param threshold 与拖动判断相同的触发阈值，避免起点和状态分离。
/// @warning 每帧只读取累计量，具体工具决定何时提交编辑。
ImVec2 mouseDragDelta(int button, float threshold)
{
    const int mapped = mappedButton(button);
    return mapped >= 0 ? ImGui::GetMouseDragDelta(mapped, threshold) : ImVec2{};
}
/// @brief 只重置映射有效的手势；禁用按钮不能误清除默认左键。
/// @warning 连续手势入口只重置对应物理按钮，不影响其他正在进行的操作。
void resetMouseDragDelta(int button)
{
    const int mapped = mappedButton(button);
    if ( mapped >= 0 ) ImGui::ResetMouseDragDelta(mapped);
}
/// @brief 滚轮解释不写入 ImGuiIO，标准控件仍看到原始滚动。
/// @param horizontal 为真时查询横向轴，否则查询纵向轴。
/// @warning 每帧不消费或修改原始 IO，标准控件仍能处理自己的滚动。
float mouseWheel(bool horizontal)
{
    const auto& io = ImGui::GetIO();
    return activeProfile().wheel(
        inputSurface, horizontal ? io.MouseWheelH : io.MouseWheel, horizontal);
}
/// @brief 四个修饰键分别解释，调用者可继续组合成主修饰键。
/// @warning 每帧只读聚合状态，再由当前表面解释语义，不做系统查询。
bool modifier(Modifier requested)
{
    const auto& io       = ImGui::GetIO();
    bool        physical = false;
    switch ( requested ) {
    case Modifier::Control: physical = io.KeyCtrl; break;
    case Modifier::Shift: physical = io.KeyShift; break;
    case Modifier::Alt: physical = io.KeyAlt; break;
    case Modifier::Super: physical = io.KeySuper; break;
    }
    return activeProfile().modifier(inputSurface, requested, physical);
}
/// @brief 先检查映射范围，禁用键不调用 ImGui 查询。
/// @param repeat 决定是否继续沿用当前控件的重复输入语义。
/// @warning 每帧只有有效映射才查询 ImGui，不产生模拟键盘事件。
bool keyPressed(int key, bool repeat)
{
    const auto mapped = mappedKey(key);
    return mapped != ImGuiKey_None && ImGui::IsKeyPressed(mapped, repeat);
}
/// @brief 保持状态不产生重复边沿。
/// @warning 每帧只读保持状态，不将按住转换成新的按下边沿。
bool keyDown(int key)
{
    const auto mapped = mappedKey(key);
    return mapped != ImGuiKey_None && ImGui::IsKeyDown(mapped);
}
/// @brief 释放查询与按下保持相同映射。
/// @warning 每帧使用与按下相同的有效键身份，保证手势收尾一致。
bool keyReleased(int key)
{
    const auto mapped = mappedKey(key);
    return mapped != ImGuiKey_None && ImGui::IsKeyReleased(mapped);
}
/// @brief 编辑命令的单一出口，方案只接管来自本机 UI 的请求。
/// @param event 事件和负载由调用栈拥有，方案消费结束后立即失效。
/// @warning 连续编辑热路径不复制命令集合，也不等待逻辑执行完成。
void dispatchCommand(const Event::LogicCommandEvent& event)
{
    if ( !activeProfile().handleCommand(event) )
        Event::EventBus::instance().publish(event);
}
/// @brief 已存在同名动作时保留最早登记，避免第二个菜单覆盖借用生命周期。
/// @param action 由菜单拥有的稳定对象，必须在析构前撤销登记。
/// @warning 低频菜单创建入口，不属于每帧绘制或快捷键检查。
void registerAction(IMainMenuItemActionHandler& action)
{
    registry().m_actions.try_emplace(action.operationId(), &action);
}
/// @brief 对照观察指针再删除，同名对象不能移除别人的动作。
/// @details 同名菜单对象只允许删除属于自己的观察地址，避免影响其他窗口。
void unregisterAction(IMainMenuItemActionHandler& action)
{
    auto&      actions = registry().m_actions;
    const auto found   = actions.find(action.operationId());
    if ( found != actions.end() && found->second == &action )
        actions.erase(found);
}
/// @brief 能力检查先于方案，禁用动作不能从快捷键或另一个方案绕过。
/// @param activation 保留菜单、快捷键或新方案来源，业务焦点路由不丢失。
/// @warning 仅在命中时执行；权限检查与业务动作复用同一上下文。
bool dispatchAction(IMainMenuItemActionHandler&   action,
                    MainMenuContext&              context,
                    const MainMenuItemActivation& activation)
{
    if ( !action.isEnabled(context) ) return false;
    // 方案消费请求不代表原动作已执行，调用者不能提前确认教程或原业务结果。
    if ( activeProfile().handleAction(
             action.operationId(), context, activation) )
        return false;
    action.perform(context, activation);
    return true;
}
/// @brief ID 查找只发生在实际动作请求，失败明确返回而不执行相似名称。
/// @return 未注册或禁用返回 false，不退回名称相似的其他动作。
/// @details 方案自己选择目标后直接执行业务，不重新进入同一重映射器。
bool executeAction(std::string_view id, MainMenuContext& context,
                   const MainMenuItemActivation& activation)
{
    const auto found = registry().m_actions.find(id);
    if ( found == registry().m_actions.end() ||
         !found->second->isEnabled(context) )
        return false;
    // 新方案直接复用业务实现，避免重新进入自身的动作拦截器。
    found->second->perform(context, activation);
    return true;
}
/// @brief 栈式帧上下文只保存指针，允许无 UI 的测试保持空管理器。
/// @details 管理器只有当前栈拥有生命周期保证，不能保存为未来帧的调用上下文。
UiFrameScope::UiFrameScope(UIManager* manager) : m_previous(frameManager)
{
    frameManager = manager;
}
/// @brief 退出更新后立即恢复外层，不把帧状态泄漏给其他调用线程。
/// @details 恢复指针后，已结束帧的表面不能继续请求主界面业务。
UiFrameScope::~UiFrameScope()
{
    frameManager = m_previous;
}
/// @brief 表面只借用当前 UI 调用栈，不建立全局视图所有权。
/// @param frame 固定大小视图信息，快照、投影与管理器都只在此帧借用。
/// @warning 每帧不复制谱面、持有会话或创建动态视图。
bool dispatchSurface(SurfaceFrame frame)
{
    // 独立派发表面同样限定输入域，新控制器不能读到外层窗口的按钮解释。
    const SurfaceScope scope(frame.m_surface);
    if ( !frame.m_sourceManager ) frame.m_sourceManager = frameManager;
    return activeProfile().handleSurface(frame);
}
}  // namespace MMM::UI::Interaction

namespace MMM::UI::Interaction
{
/// @brief 登记只保存观察地址，不能把跨线程会话锁引入方案切换。
/// @details 观察表只在控制器生命周期变化时扩容，切换检查不分配。
void registerInteractionGuard(const IInteractionGuard& guard)
{
    auto& guards = registry().m_guards;
    if ( std::find(guards.begin(), guards.end(), &guard) == guards.end() )
        guards.push_back(&guard);
}
/// @brief 按地址精确移除，构造失败或重复移除保持无副作用。
/// @details 缺少登记也安全，不基于别的视图身份批量删除。
void unregisterInteractionGuard(const IInteractionGuard& guard)
{
    std::erase(registry().m_guards, &guard);
}
}  // namespace MMM::UI::Interaction
