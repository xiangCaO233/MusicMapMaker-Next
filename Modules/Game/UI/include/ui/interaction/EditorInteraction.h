#pragma once
#include "common/SelectionOperation.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

struct ImVec2;
namespace MMM::Event
{
struct LogicCommandEvent;
}
namespace MMM::UI
{
class IMainMenuItemActionHandler;
class UIManager;
struct MainMenuContext;
struct MainMenuItemActivation;
}  // namespace MMM::UI
namespace MMM::Config
{
struct ShortcutBinding;
}
namespace MMM::Canvas
{
struct PlanarObservationInputProjection;
}
namespace MMM::Common::Render
{
struct RenderSnapshot;
}
namespace MMM::UI::Interaction
{
/// @brief 输入属于哪个编辑表面；相同按键在不同表面可以有不同含义。
enum class Surface {
    Application,
    MainCanvas,
    Timeline,
    Preview,
    CameraLayout,
    AudioTool,
    BpmTool
};
/// @brief 指针修饰语义，保留 Ctrl 与 Command 的独立配置能力。
enum class Modifier { Control, Shift, Alt, Super };
/// @brief 相机指针意图独立于键盘修饰，供鼠标及后续触控入口共用。
struct CameraPointerOptions {
    /// @brief 中键导航沿观察平面平移；关闭时绕观察中心旋转。
    bool m_pan{ false };
    /// @brief 轴和旋转环采用十分之一增量，不改变目标相机的数值精度。
    bool m_fine{ false };
};
/// @brief 纯鼠标的定位与预览意图，不改变主画布缩放或全局修饰键。
struct NavigationPointerOptions {
    /// @brief 拖动 Timing 音频进度条时吸附到当前拍线网格。
    bool m_snapScrub{ false };
    /// @brief 预览滚轮采用两倍步长，范围仍由原有预览配置约束。
    bool m_fastPreviewZoom{ false };
};
/// @brief 供新方案替换整段交互的单帧上下文，不拥有视图或谱面。
/// @warning 仅在 UI 调用栈内借用，不能保存 snapshot 到下一帧。
struct SurfaceFrame {
    /// @brief 画布类型用于区分主谱面、时间点、预览和布局操作。
    Surface m_surface{ Surface::MainCanvas };
    /// @brief 会话相机稳定身份，多谱面方案必须按此隔离手势状态。
    std::string_view m_cameraId;
    /// @brief 不可变快照；没有谱面时可以为空，禁止转成可写会话。
    const Common::Render::RenderSnapshot* m_snapshot{};
    /// @brief 内容区屏幕原点和逻辑像素大小，不含菜单和窗口装饰。
    float m_x{}, m_y{}, m_width{}, m_height{};
    /// @brief 已通过现有模态和窗口归属检查的指针与焦点状态。
    bool m_hovered{}, m_focused{};
    /// @brief 当前 UI 管理器仅在这一帧借用，供新方案请求已有文件和视图动作。
    UIManager* m_sourceManager{};
    /// @brief 主画布的屏幕到谱面投影，只借用本帧矩阵，二维表面可为空。
    const Canvas::PlanarObservationInputProjection* m_projection{};
    /// @brief 动态顶点插值与命中框的偏移，供新方案沿用相同拾取坐标约定。
    float m_snapshotYOffset{};
    /// @brief 后台画布只允许检视，不能因方案切换获得编辑权限。
    bool m_readOnly{};
};

/// @brief 应用级操作方案；输入解释和完整表面交互均可替换。
/// @details 默认实现保留经典桌面操作。新方案可以只改绑定，也可以接管整个表面，
/// 使用既有逻辑命令执行编辑；不需要修改 Vulkan 或逻辑线程的工具实现。
/// @warning 所有方法只由 UI 线程调用；每帧路径禁止 IO、锁等待和按帧分配。
class IEditorInteractionProfile
{
public:
    /// @brief 生命周期由注册表持有，不能卸载活动方案或借出所有权副本。
    virtual ~IEditorInteractionProfile() = default;
    /// @brief 持久化的唯一英文身份，不随界面语言或显示名改变。
    virtual std::string_view id() const = 0;
    /// @brief 设置页使用的人类可读名称，返回存储必须与方案同寿命。
    /// @details 展示使用 ImGui 文本接口，名称必须以零结尾；身份仍独立使用 id。
    virtual std::string_view name() const = 0;
    /// @brief 返回该方案的快捷键解释；configured 保留用户持久化配置。
    /// @warning 每次快捷键查询调用；引用只能指向参数或方案的稳定成员。
    virtual const Config::ShortcutBinding& shortcut(
        std::string_view               action,
        const Config::ShortcutBinding& configured) const;
    /// @brief 将经典逻辑按钮映射到物理按钮；0 编辑、1 次操作、2 平移。
    /// @warning 每帧常量查询；按 surface 区分右键菜单与擦除等上下文。
    virtual int pointerButton(Surface surface, int logicalButton) const;
    /// @brief 解释修饰键；默认返回原值，独立于 ImGui 控件自身的文本编辑。
    /// @warning 只转换当前 UI 帧状态，不得反向改写 ImGuiIO。
    virtual bool modifier(Surface surface, Modifier modifier,
                          bool physical) const;
    /// @brief 解释画笔延伸意图，独立于选择、滚动等操作的 Shift 语义。
    /// @warning 每帧常量查询；起笔、拖动和滚动更新必须使用相同解释。
    virtual bool extendBrush(Surface surface, bool shift) const;
    /// @brief 是否在选中工具中启用无需键盘的右键编辑菜单。
    /// @warning 每帧只返回方案策略，不查询选择集合或访问会话锁。
    virtual bool selectionContextMenu() const;
    /// @brief 显式选择模式；经典方案返回空以保留物理修饰键语义。
    /// @warning 每帧只读取 UI 线程标量，不访问选择集合。
    virtual std::optional<Logic::SelectionOperation> selectionOperation() const;
    /// @brief 在未进行选择手势时切换纯鼠标选择模式。
    virtual void setSelectionOperation(Logic::SelectionOperation operation);
    /// @brief 显式折线擦除粒度；经典方案返回空以保留物理 Shift 的动态语义。
    /// @warning 每帧仅查询 UI 线程标量，不持有会话或修改键盘状态。
    virtual std::optional<bool> eraseWholePolyline() const;
    /// @brief 更新纯鼠标后续擦除的粒度，活动事务由画布锁存起笔值。
    virtual void setEraseWholePolyline(bool whole);
    /// @brief 是否临时绕过吸附；经典方案始终返回假，不改写原磁吸配置。
    /// @warning 每帧读取 UI 线程标量，不访问会话或伪造 Ctrl。
    virtual bool bypassSnapping() const;
    /// @brief 设置纯鼠标的临时吸附策略，切出方案后清除。
    virtual void setBypassSnapping(bool bypass);
    /// @brief Timing 是否按点按与拖动自动区分单点和插值段落。
    /// @warning 每帧仅查询方案能力，不改变真实 Shift 或分配手势状态。
    virtual bool automaticTimingDraw() const;
    /// @brief 显式相机指针意图；经典返回空以保留动态 Shift 手势。
    /// @warning 每帧按值读取两个标量，不分配资源或访问会话。
    virtual std::optional<CameraPointerOptions> cameraPointerOptions() const;
    /// @brief 只更新后续相机手势的意图，不修改物理键盘状态。
    virtual void setCameraPointerOptions(CameraPointerOptions options);
    /// @brief 显式时间滚动加速；经典返回空，继续动态读取真实 Shift。
    /// @warning 每帧只读取方案标量，不改变分拍、缩放或物理修饰键。
    virtual std::optional<bool> acceleratedScrolling() const;
    /// @brief 设置纯鼠标快速滚动意图，切出方案时清除。
    virtual void setAcceleratedScrolling(bool accelerated);
    /// @brief 显式定位与预览意图；经典返回空以保留物理 Shift 行为。
    /// @warning 每帧按值读取两个布尔值，不分配资源或访问会话。
    virtual std::optional<NavigationPointerOptions>
    navigationPointerOptions() const;
    /// @brief 更新纯鼠标导航意图，切出方案时恢复默认关闭状态。
    virtual void setNavigationPointerOptions(NavigationPointerOptions options);
    /// @brief 解释滚轮方向与倍率，横纵轴分开，不改变 ImGui 控件自身滚动。
    virtual float wheel(Surface surface, float physical, bool horizontal) const;
    /// @brief 解释键盘主键，支持整套键位语义替换；参数为 ImGui 命名键值。
    /// @warning 仅返回有效命名键或零，不能在此查询操作系统或分配对象。
    virtual int key(Surface surface, int physicalKey) const;
    /// @brief 新方案接管一个表面的完整操作；false 沿用经典状态机。
    /// @details true 时当前视图不再解释经典编辑手势；绘制和已有控件仍正常推进。
    /// @warning 每帧调用；多画布状态必须按 cameraId 隔离，不能访问可变
    /// SessionContext。
    virtual bool handleSurface(const SurfaceFrame& frame);
    /// @brief 拦截语义动作，true 表示方案已经处理，false 执行现有动作。
    /// @details 菜单、快捷键与新方案直接调用共享动作身份，不需要伪造键盘事件。
    virtual bool handleAction(std::string_view action, MainMenuContext& context,
                              const MainMenuItemActivation& activation);
    /// @brief 接管从 UI/画布发布的编辑命令，网络与逻辑线程命令不经过此入口。
    /// @warning 每帧可能调用；仅读取命令，不复制其资源或访问可变会话。
    virtual bool handleCommand(const Event::LogicCommandEvent& event);
    /// @brief 新方案持有触控等非鼠标手势时阻止切出，避免跨方案转移半次操作。
    virtual bool hasActiveGesture() const;
    /// @brief 切入方案时重置其临时状态；仅在安全帧边界执行。
    virtual void activate();
    /// @brief 切出方案时清理其本地状态；调用前旧方案已处理释放帧。
    virtual void deactivate();
};

/// @brief 获取稳定的已注册方案列表；注册后只允许在非交互初始化阶段增加。
/// @warning 设置页按引用读取，不能把 span 保存到下一次注册之后。
std::span<const std::unique_ptr<IEditorInteractionProfile>> profiles();
/// @brief 注册编译进程序的方案，拒绝空对象和重复身份，不覆盖现有方案。
/// @details 此入口在初始化或测试阶段使用，生产热路径禁止注册或卸载。
bool registerProfile(std::unique_ptr<IEditorInteractionProfile> profile);
/// @brief 获取当前方案的非拥有引用；默认经典方案始终存在。
/// @warning 每帧常量访问，不读取文件或复制 shared_ptr。
IEditorInteractionProfile& activeProfile();
/// @brief 在帧起点同步应用级选择；连续两个安全帧才允许切换。
/// @param safe 没有指针按下、活动控件、弹窗、拖放和快捷键录制。
/// @return 实际发生切换时返回 true；未知身份回退经典但不修改保存值。
/// @warning 每帧只比较短 ID；切换才调用生命周期回调，不做文件访问。
bool synchronizeProfile(std::string_view requested, bool safe);
/// @brief 当前方案切换代数，用于低频重建按视图持有的控制器。
/// @warning 单 UI 线程标量，不需要原子同步。
std::uint64_t profileGeneration();

/// @brief 将输入查询限定到一个表面；嵌套调用退出时恢复外层上下文。
class SurfaceScope
{
public:
    /// @brief 保存旧上下文并立即应用当前表面，不改变活动方案。
    explicit SurfaceScope(Surface surface);
    /// @brief 函数返回或提前退出时恢复旧上下文。
    ~SurfaceScope();
    /// @brief 作用域不能复制，避免重复恢复输入上下文。
    SurfaceScope(const SurfaceScope&) = delete;
    /// @brief 作用域不能赋值，避免覆盖待恢复的旧表面。
    SurfaceScope& operator=(const SurfaceScope&) = delete;

private:
    /// @brief 仅保存一个枚举，不持有视图或输入事件。
    Surface m_previous;
};
/// @brief 当前调用栈的输入表面，未进入画布时为 Application。
Surface currentSurface();
/// @brief 经典编辑按钮查询经过方案映射，ImGui 标准按钮仍由 ImGui 自行处理。
/// @warning 下列输入查询均为每帧热路径，只进行固定次数的虚调用与 ImGui 查询。
bool mouseDown(int button);
/// @brief 保留 repeat 参数，不能把按住当作每帧新起笔。
bool mouseClicked(int button, bool repeat = false);
/// @brief 双击仍遵循 ImGui 的时间和位置阈值。
bool mouseDoubleClicked(int button);
/// @brief 松开必须与起笔使用同一逻辑按钮。
bool mouseReleased(int button);
/// @brief 拖拽阈值沿用 ImGui 默认或调用方显式给定值。
bool mouseDragging(int button, float threshold = -1.0f);
/// @brief 对应逻辑按钮的按下原点，避免重映射后沿用另一按钮的拖拽起点。
ImVec2 mouseClickedPosition(int button);
/// @brief 映射后的累计拖拽位移，与按下原点使用相同物理按钮。
ImVec2 mouseDragDelta(int button = 0, float threshold = -1.0f);
/// @brief 仅重置对应映射按钮的累计位移，不能清除其他手势。
void resetMouseDragDelta(int button = 0);
/// @brief 读取经过方案解释的滚轮，参数选择横轴或纵轴。
float mouseWheel(bool horizontal = false);
/// @brief 获取方案解释后的修饰键，不影响文本控件使用的物理键。
bool modifier(Modifier modifier);
/// @brief 当前方案是否将画笔移动解释为持续段或折线延伸。
/// @warning UI 热路径只查询方案，不伪造物理按键或分配手势状态。
bool extendBrush();
/// @brief 主画布是否无需修饰键即可延伸，用于同一方案的教学提示与输入验收。
/// @warning 每帧只查询当前方案标量，不改写真实 Shift 或 Ctrl。
bool modifierFreeBrush();
/// @brief 验证教学画笔的修饰语义，实际路线和物件几何仍由画布与逻辑层验收。
/// @param extending 该练习是否需要拖动延伸；纯鼠标点按仍由移动量区分单键。
/// @warning 每帧常量查询，经典保留 Shift 与 Ctrl 的完整门禁。
bool lessonBrushModifiersMatch(bool extending);
/// @brief 独立的忽略吸附语义，组合物理 Ctrl 与当前方案临时开关。
/// @warning 每帧固定标量查询；不得用于选择切换、滚轮修饰或框选集合操作。
bool bypassSnapping();
/// @brief 只为时间滚轮与框选边缘滚动解析加速，不作为通用 Shift 使用。
/// @warning 每帧常量查询；经典沿用物理 Shift，纯鼠标使用显式开关。
bool acceleratedScroll();
/// @brief 仅为 Timing 音频进度条解析拍线吸附，不改变其它放置吸附策略。
/// @warning 每帧常量查询；经典仍动态读取 Shift，不伪造键盘状态。
bool snapTimelineScrub();
/// @brief 仅为预览滚轮解析两倍缩放步长，与时间滚动加速相互独立。
/// @warning 每帧常量查询，不改变滚轮数值或访问可变会话。
bool fastPreviewZoom();
/// @brief 查询主键边沿，保留旧调用者是否允许重复的语义。
bool keyPressed(int key, bool repeat = true);
/// @brief 查询主键保持状态，供连续编辑使用。
bool keyDown(int key);
/// @brief 查询主键释放状态，供手势收尾使用。
bool keyReleased(int key);
}  // namespace MMM::UI::Interaction

namespace MMM::UI::Interaction
{
/// @brief 将短生命周期命令交给操作方案，再发送到原有逻辑事件通道。
/// @warning UI 热路径不获取会话锁，原事件总线保持原有线程归属。
void dispatchCommand(const Event::LogicCommandEvent& event);
/// @brief 登记菜单持有的稳定动作，所有权仍属于主菜单。
void registerAction(IMainMenuItemActionHandler& action);
/// @brief 菜单销毁时只移除属于自身的登记，不影响其他菜单实例。
void unregisterAction(IMainMenuItemActionHandler& action);
/// @brief 用户操作先经过方案，方案未接管时执行默认动作。
/// @return 只有原动作确实被派发时为 true，方案接管不能伪造原教程目标完成。
/// @warning 只在激活或快捷键命中时调用，执行前仍检查动作能力。
bool dispatchAction(IMainMenuItemActionHandler&   action,
                    MainMenuContext&              context,
                    const MainMenuItemActivation& activation);
/// @brief 新交互通过稳定动作 ID 执行现有业务，不模拟菜单点击或快捷键。
/// @details 此入口直接执行目标动作，避免方案重映射递归调用自己。
bool executeAction(std::string_view id, MainMenuContext& context,
                   const MainMenuItemActivation& activation);
}  // namespace MMM::UI::Interaction

namespace MMM::UI::Interaction
{
/// @brief 为整个 UI 更新调用栈提供当前管理器，退出后不保留悬空帧指针。
class UiFrameScope
{
public:
    /// @brief 只借用管理器，不能复制所有权或访问会话锁。
    explicit UiFrameScope(UIManager* manager);
    /// @brief 嵌套 UI 更新退出时恢复外层管理器。
    ~UiFrameScope();
    /// @brief 作用域不可复制，避免重复恢复帧上下文。
    UiFrameScope(const UiFrameScope&) = delete;
    /// @brief 作用域不可赋值，保留待恢复的外层身份。
    UiFrameScope& operator=(const UiFrameScope&) = delete;

private:
    /// @brief 原管理器可能为空，不延长视图生命周期。
    UIManager* m_previous{};
};
/// @brief 补齐借用的应用上下文后把整段表面操作交给方案。
/// @warning 每帧固定大小值复制，不能在此读取工程或分配控制器。
bool dispatchSurface(SurfaceFrame frame);
/// @brief 从表面手势执行已有菜单业务，保持能力检查并标记方案来源。
/// @details 未注册动作或缺少主界面时返回 false，不模拟按键或绕过保存确认。
bool executeAction(std::string_view id, UIManager* manager);
/// @brief 从新方案显式指定业务目标，例如独立 BPM 工具的播放与历史。
bool executeAction(std::string_view id, UIManager* manager,
                   const MainMenuItemActivation& activation);
}  // namespace MMM::UI::Interaction

namespace MMM::UI::Interaction
{
/// @brief 经典控制器只暴露手势是否结束，不让运行时读取可变会话。
class IInteractionGuard
{
public:
    /// @brief 观察接口不拥有控制器，销毁前必须撤销注册。
    virtual ~IInteractionGuard() = default;
    /// @brief UI 线程读取本地状态，禁止加锁、IO 或遍历谱面。
    virtual bool hasActivePointerGesture() const = 0;
};
/// @brief 控制器构造时登记，不在每帧创建或复制拥有关系。
void registerInteractionGuard(const IInteractionGuard& guard);
/// @brief 控制器销毁时移除观察指针，不能留到下一次方案切换。
void unregisterInteractionGuard(const IInteractionGuard& guard);
}  // namespace MMM::UI::Interaction
