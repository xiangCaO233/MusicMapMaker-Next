#include "ui/imgui/MainDockSpaceUI.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/GLFWNativeEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/glfw/window/adapters/IWindowFrameAdapter.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include <GLFW/glfw3.h>
#include <ImGuiFileDialog.h>
#include <concurrentqueue.h>
#include <filesystem>
#include <fmt/format.h>
#include <nfd.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MMM::UI
{

/// @brief 构造主窗口外壳并订阅原生窗口与音频导入事件。
/// @param name 视图注册名称，同时交给两个基础接口保存。
///
/// 事件回调只把外部状态复制到 UI 成员，不直接绘制控件。窗口最大化状态由
/// 原生层回报，音频导入请求则暂存路径并在下一 UI 帧打开类型选择弹窗。
/// @warning 回调可能位于主更新流程之外；不得捕获短生命周期局部对象，也不得
/// 在回调中执行纹理创建、文件选择或 ImGui 绘制。
MainDockSpaceUI::MainDockSpaceUI(const std::string& name)
    : IUIView(name), ITextureLoader(name)
{
    // 原生层完成最大化或还原后回报权威状态，菜单栏据此选择下一操作图标。
    Event::EventBus::instance().subscribe<Event::GLFWNativeEvent>(
        [&](Event::GLFWNativeEvent event) {
            if ( event.hasStateChange &&
                 event.type ==
                     Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE ) {
                m_isMaximized = event.isMaximized;
            }
        });

    // 菜单和其他 UI 入口统一发送导入触发事件，由此处汇聚到同一模态流程。
    Event::EventBus::instance().subscribe<Event::AudioImportTriggerEvent>(
        [&](Event::AudioImportTriggerEvent event) {
            m_pendingImportPath   = event.path;
            m_showImportTypeModal = true;
        });
}

/// @brief 销毁主窗口视图。
///
/// 资源成员使用 RAII 释放；事件总线订阅遵循视图既有生命周期约定。
MainDockSpaceUI::~MainDockSpaceUI() = default;

/// @brief 在皮肤切换后刷新工具栏依赖的调色板缓存。
///
/// Logo 纹理由 ITextureLoader 的重载流程处理，此处只转发业务调色板刷新。
void MainDockSpaceUI::refreshPaletteAfterSkinChange()
{
    m_toolbarView.refreshPaletteAfterSkinChange();
}

namespace
{
/// @brief 本文件私有实现分为跨线程通知、临时项目流程和无边框窗口几何三组。
///
/// 后台事件不得直接操作 ImGui，因此先复制到无锁队列；主 UI 帧统一消费并
/// 更新模态状态。文件对话框及窗口平台调用仍只发生在 UI 线程。

/// @brief 无边框窗口缩放热区基础宽度。
constexpr float NATIVE_FRAME_RESIZE_HIT_THICKNESS = 7.0f;

/// @brief 无边框窗口自绘边框基础宽度。
constexpr float NATIVE_FRAME_BORDER_THICKNESS = 1.0f;

/// @brief 无边框窗口自绘内阴影层数。
constexpr int NATIVE_FRAME_SHADOW_LAYERS = 5;

/// @brief 无边框窗口自绘圆角基础半径。
constexpr float NATIVE_FRAME_ROUNDING = 10.0f;

/// @brief 无边框窗口边缘命中结果。
struct NativeFrameHit {
    /// @brief 是否命中可缩放边缘。
    ///
    /// 为 false 时其余字段只是默认占位，调用方不得据此发起缩放。
    bool m_hit{ false };

    /// @brief 命中的缩放方向。
    ///
    /// 方向与平台 IWindowFrameAdapter 使用的边缘枚举保持一致。
    Graphic::WindowFrameResizeEdge m_edge{
        Graphic::WindowFrameResizeEdge::Right
    };

    /// @brief 命中方向对应的 ImGui 鼠标光标。
    ///
    /// 对角方向分别映射 NWSE 或 NESW，必须与 m_edge 语义匹配。
    ImGuiMouseCursor m_cursor{ ImGuiMouseCursor_Arrow };
};

/// @brief 临时项目路径提示载荷。
struct TemporaryProjectPromptPayload {
    /// @brief 原始谱面包路径。
    ///
    /// 用于向用户说明临时工程来自哪个只读包，不作为保存目标。
    std::string sourcePackagePath;

    /// @brief 临时项目缓存目录。
    ///
    /// 路径仅供提示和诊断；编辑权限仍由 EditorEngine 的临时状态决定。
    std::string cacheProjectPath;
};

/// @brief 临时项目保存结果载荷。
struct TemporaryProjectSaveResultPayload {
    /// @brief 是否保存成功。
    ///
    /// 成功时 savedProjectPath 有效，失败时优先展示 errorMessage。
    bool success{ false };

    /// @brief 保存成功后的正式项目目录。
    ///
    /// UI 用它替换缓存路径提示，不自行验证或再次复制目录。
    std::string savedProjectPath;

    /// @brief 失败时的错误信息。
    ///
    /// 允许为空；消费端会补充稳定的中文兜底说明。
    std::string errorMessage;
};

/// @brief 音频资源变更结果的跨线程 UI 载荷。
struct AudioResourceMutationResultPayload {
    /// @brief 本次资源操作类型。
    ///
    /// 决定成功提示文案，不改变失败原因的优先级。
    Event::AudioResourceMutationOperation operation{
        Event::AudioResourceMutationOperation::UpdateType
    };

    /// @brief 操作目标的稳定资源 ID。
    ///
    /// 即使资源已删除仍可用于定位原目标，因此不保存对象引用。
    std::string resourceId;

    /// @brief 操作是否成功。
    ///
    /// 失败通知会延长显示时间，便于阅读阻止路径列表。
    bool success{ false };

    /// @brief 阻止操作的全部谱面路径。
    ///
    /// 仅在存在引用约束时填充，通知构建时按原顺序逐项列出。
    std::vector<std::string> blockingBeatmapPaths;

    /// @brief 逻辑层返回的失败原因。
    ///
    /// 保留逻辑层上下文；空值由 UI 使用通用失败文案补齐。
    std::string errorMessage;
};

/// @brief 获取临时项目只读提示队列。
/// @return 进程期稳定存在的多生产者、单 UI 消费者队列。
/// @warning 事件回调可入队；仅 MainDockSpaceUI 的 UI 更新路径出队。
moodycamel::ConcurrentQueue<TemporaryProjectPromptPayload>&
temporaryProjectEditBlockedQueue()
{
    // 函数局部静态对象避免跨翻译单元初始化顺序依赖。
    static moodycamel::ConcurrentQueue<TemporaryProjectPromptPayload> queue;
    return queue;
}

/// @brief 获取临时项目关闭提示队列。
/// @return 进程期稳定存在的关闭确认通知队列。
/// @warning 载荷只携带值类型路径，不跨线程共享工程对象。
moodycamel::ConcurrentQueue<TemporaryProjectPromptPayload>&
temporaryProjectClosePromptQueue()
{
    // 独立队列保留只读拦截与关闭请求的不同 UI 语义。
    static moodycamel::ConcurrentQueue<TemporaryProjectPromptPayload> queue;
    return queue;
}

/// @brief 获取临时项目保存结果队列。
/// @return 进程期稳定存在的保存结果通知队列。
/// @warning 后台逻辑只写结果；后续关闭或退出动作由 UI 消费者决定。
moodycamel::ConcurrentQueue<TemporaryProjectSaveResultPayload>&
temporaryProjectSaveResultQueue()
{
    // 保存结果按到达顺序消费，确保最终 UI 状态对应最后完成的请求。
    static moodycamel::ConcurrentQueue<TemporaryProjectSaveResultPayload> queue;
    return queue;
}

/// @brief 获取音频资源变更结果队列。
/// @return 进程期稳定存在的音频资源操作结果队列。
/// @warning 阻止谱面路径已复制进载荷，不引用逻辑线程容器。
moodycamel::ConcurrentQueue<AudioResourceMutationResultPayload>&
audioResourceMutationResultQueue()
{
    // 通知文本在 UI 线程构建，事件回调只执行值复制和入队。
    static moodycamel::ConcurrentQueue<AudioResourceMutationResultPayload>
        queue;
    return queue;
}

/// @brief 获取协作访客本机项目打开拦截通知队列。
/// @return 只传递“需要显示提示”信号的队列。
/// @warning 信号值本身无业务含义，队列用于安全跨线程唤醒 UI。
moodycamel::ConcurrentQueue<bool>& collaborationProjectOpenBlockedQueue()
{
    // 三类协作限制分队列保存，避免后到信号覆盖先到原因。
    static moodycamel::ConcurrentQueue<bool> queue;
    return queue;
}

/// @brief 获取离线房间谱面编辑拦截通知队列。
/// @return 离线编辑拦截信号队列。
/// @warning 只允许 UI 更新路径消费。
moodycamel::ConcurrentQueue<bool>& collaborationOfflineEditBlockedQueue()
{
    // bool 仅作为轻量占位，实际弹窗类型由队列身份确定。
    static moodycamel::ConcurrentQueue<bool> queue;
    return queue;
}

/// @brief 获取协作细分权限编辑拦截通知队列。
/// @return 权限不足拦截信号队列。
/// @warning 队列不携带权限对象，避免跨线程生命周期耦合。
moodycamel::ConcurrentQueue<bool>& collaborationPermissionEditBlockedQueue()
{
    // 重复信号会在一帧内合并为同一个布尔弹窗请求。
    static moodycamel::ConcurrentQueue<bool> queue;
    return queue;
}

/// @brief 构建音频资源变更的完整用户提示。
/// @param result 待展示结果。
/// @return 包含目标资源及全部阻止谱面的多行提示。
///
/// 文案结构固定为操作结果、资源 ID、阻止谱面列表三段。成功结果由 operation
/// 决定首行；失败结果优先使用逻辑层原因。稳定 ID 和全部引用路径保留在后续
/// 行，便于用户复制诊断，同时避免 UI 再次查询已经变化的资源模型。
std::string buildAudioResourceMutationMessage(
    const AudioResourceMutationResultPayload& result)
{
    // 成功提示按操作类型说明实际完成的资源变更。
    std::string message;
    if ( result.success ) {
        switch ( result.operation ) {
        case Event::AudioResourceMutationOperation::UpdateType:
            message = "音频资源类型已更新";
            break;
        case Event::AudioResourceMutationOperation::Rename:
            message = "音频轨道与文件名已重命名";
            break;
        case Event::AudioResourceMutationOperation::Remove:
            message = "音频资源已删除";
            break;
        case Event::AudioResourceMutationOperation::MovePath:
            message = "音频资源路径已同步";
            break;
        }
    } else {
        // 逻辑层未提供原因时使用稳定兜底文本，保证通知不为空。
        message = result.errorMessage.empty() ? "音频资源操作失败"
                                              : result.errorMessage;
    }

    // 稳定资源 ID 帮助用户定位目标，不依赖可能已经变化的显示名称。
    if ( !result.resourceId.empty() ) {
        message += fmt::format("\n资源：{}", result.resourceId);
    }
    if ( !result.blockingBeatmapPaths.empty() ) {
        // 全量列出阻止路径，避免用户只能逐个重试才能发现剩余引用。
        message += "\n阻止操作的谱面：";
        for ( const auto& beatmapPath : result.blockingBeatmapPaths ) {
            message += fmt::format("\n- {}", beatmapPath);
        }
    }
    return message;
}

/// @brief 根据当前临时项目构建提示载荷。
/// @return 已转换为 UTF-8 值类型字符串的提示数据。
/// @warning 低频事件回调路径；读取引擎当前临时项目信息但不修改工程。
TemporaryProjectPromptPayload makeCurrentTemporaryProjectPayload()
{
    // 立即复制路径，避免队列中保存对 EditorEngine 内部状态的引用。
    const auto info =
        Logic::EditorEngine::instance().currentTemporaryProjectInfo();
    return TemporaryProjectPromptPayload{
        Config::pathToUtf8(info.m_sourcePackagePath),
        Config::pathToUtf8(info.m_cacheProjectPath),
    };
}

/// @brief 订阅临时项目及音频资源变更结果事件。
///
/// 订阅只建立一次。各回调把事件字段复制到对应队列，既不触碰 ImGui，也不
/// 决定弹窗之间的优先级；优先级统一由 UI 帧消费和渲染阶段处理。
/// @warning UI 热路径入口会调用本函数；首次之后只检查局部静态标志。
void ensureTemporaryProjectSubscriptions()
{
    static bool subscribed = false;
    // 重复调用不得重复注册，否则同一事件会产生多份弹窗信号。
    if ( subscribed ) return;

    // 先完成全部订阅，最后才置位，避免部分建立时被误判为已完成。
    auto& eventBus = Event::EventBus::instance();
    eventBus.subscribe<Event::TemporaryProjectEditBlockedEvent>(
        [](const Event::TemporaryProjectEditBlockedEvent& event) {
            // 事件已提供路径时直接复制，保留触发拦截时的准确上下文。
            temporaryProjectEditBlockedQueue().enqueue(
                TemporaryProjectPromptPayload{ event.m_sourcePackagePath,
                                               event.m_cacheProjectPath });
        });
    eventBus.subscribe<Event::TemporaryProjectClosePromptRequestedEvent>(
        [](const Event::TemporaryProjectClosePromptRequestedEvent&) {
            // 关闭事件不携带路径，因此在回调时抓取当前临时项目信息。
            temporaryProjectClosePromptQueue().enqueue(
                makeCurrentTemporaryProjectPayload());
        });
    eventBus.subscribe<Event::TemporaryProjectSaveResultEvent>(
        [](const Event::TemporaryProjectSaveResultEvent& event) {
            // 保存结果只携带后续状态机需要的成功、路径和错误文本。
            temporaryProjectSaveResultQueue().enqueue(
                TemporaryProjectSaveResultPayload{ event.m_success,
                                                   event.m_savedProjectPath,
                                                   event.m_errorMessage });
        });
    eventBus.subscribe<Event::AudioResourceMutationResultEvent>(
        [](const Event::AudioResourceMutationResultEvent& event) {
            // 阻止路径容器按值复制，UI 通知可以安全延后构建。
            audioResourceMutationResultQueue().enqueue(
                AudioResourceMutationResultPayload{
                    event.m_operation,
                    event.m_resourceId,
                    event.m_success,
                    event.m_blockingBeatmapPaths,
                    event.m_errorMessage,
                });
        });
    eventBus.subscribe<Event::CollaborationProjectOpenBlockedEvent>(
        [](const Event::CollaborationProjectOpenBlockedEvent&) {
            // 信号内容固定为 true，队列类型本身标识弹窗原因。
            collaborationProjectOpenBlockedQueue().enqueue(true);
        });
    eventBus.subscribe<Event::CollaborationOfflineEditBlockedEvent>(
        [](const Event::CollaborationOfflineEditBlockedEvent&) {
            collaborationOfflineEditBlockedQueue().enqueue(true);
        });
    eventBus.subscribe<Event::CollaborationPermissionEditBlockedEvent>(
        [](const Event::CollaborationPermissionEditBlockedEvent&) {
            collaborationPermissionEditBlockedQueue().enqueue(true);
        });

    // 只有全部订阅调用完成后才阻止后续重复进入。
    subscribed = true;
}

/// @brief 在当前内容区域内绘制可换行文本。
/// @param text 待绘制文本。
///
/// 输入为 string_view，因此显式传递起止指针，不要求末尾存在零字符。换行位置
/// 取当前游标加剩余宽度，适用于动态缩放的模态框内容区。
/// @warning UI 绘制路径：只设置 ImGui 文本换行位置并绘制文本。
void drawWrappedText(std::string_view text)
{
    // 空视图使用有效空字符串指针，避免对空 data() 的实现差异产生依赖。
    const char* textBegin = text.empty() ? "" : text.data();
    // 换行终点取当前可用宽度，适配 DPI 和弹窗动态尺寸。
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                           ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(textBegin, textBegin + text.size());
    ImGui::PopTextWrapPos();
}

/// @brief 绘制标签和可换行值。
/// @param label 标签文本。
/// @param value 值文本。
///
/// 标签保持在当前行起点，值从 SameLine 后的位置开始换行。该布局用于路径等
/// 长文本，既保留字段含义，也避免宽路径把模态框撑出视口。
/// @warning UI 绘制路径：只绘制 ImGui 文本。
void drawWrappedLabelValue(std::string_view label, std::string_view value)
{
    // 标签保持单行，值从同一行开始并在剩余内容区域内自动换行。
    const char* labelBegin = label.empty() ? "" : label.data();
    ImGui::TextUnformatted(labelBegin, labelBegin + label.size());
    ImGui::SameLine();
    drawWrappedText(value);
}

/// @brief 根据主视口和鼠标位置解析无边框窗口缩放命中。
/// @param viewport 主 ImGui 视口。
/// @param dpiScale 当前 DPI 缩放。
/// @return 缩放命中结果。
///
/// 命中规则先验证鼠标位于视口，再按 DPI 计算边缘厚度。四个角优先于四条边，
/// 因而角落能返回双轴缩放光标；中央区域返回默认未命中值。函数只做几何分类，
/// 不调用平台缩放 API，也不修改窗口状态。
/// @warning UI 热路径：每帧主窗口边缘检测调用；只做常量规模几何判断。
NativeFrameHit resolveNativeFrameHit(const ImGuiViewport& viewport,
                                     float                dpiScale)
{
    // ImGui 在窗口失焦或指针离开平台表面时可能报告无效鼠标坐标。
    if ( !ImGui::IsMousePosValid() ) {
        return {};
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 min   = viewport.Pos;
    const ImVec2 max   = { viewport.Pos.x + viewport.Size.x,
                           viewport.Pos.y + viewport.Size.y };
    // 只对主视口矩形内部命中，避免抢占相邻平台窗口的边缘交互。
    if ( mouse.x < min.x || mouse.y < min.y || mouse.x > max.x ||
         mouse.y > max.y ) {
        return {};
    }

    const float thickness = std::max(
        4.0f, std::floor(NATIVE_FRAME_RESIZE_HIT_THICKNESS * dpiScale));
    // 最小四像素保证低缩放下仍可命中，DPI 放大时同步扩大热区。
    const bool left   = mouse.x <= min.x + thickness;
    const bool right  = mouse.x >= max.x - thickness;
    const bool top    = mouse.y <= min.y + thickness;
    const bool bottom = mouse.y >= max.y - thickness;

    // 角点必须先于单边判断，否则左上区域会错误返回普通水平缩放。
    if ( top && left ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::TopLeft,
                 ImGuiMouseCursor_ResizeNWSE };
    }
    if ( top && right ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::TopRight,
                 ImGuiMouseCursor_ResizeNESW };
    }
    if ( bottom && left ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::BottomLeft,
                 ImGuiMouseCursor_ResizeNESW };
    }
    if ( bottom && right ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::BottomRight,
                 ImGuiMouseCursor_ResizeNWSE };
    }
    // 四个角均未命中后，再按单边方向选择平台缩放语义和鼠标形状。
    if ( left ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::Left,
                 ImGuiMouseCursor_ResizeEW };
    }
    if ( right ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::Right,
                 ImGuiMouseCursor_ResizeEW };
    }
    if ( top ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::Top,
                 ImGuiMouseCursor_ResizeNS };
    }
    if ( bottom ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::Bottom,
                 ImGuiMouseCursor_ResizeNS };
    }

    // 视口中央不属于缩放热区，返回默认未命中结果。
    return {};
}
}  // namespace

/// @brief 更新主 DockSpace、顶部菜单、全局文件对话框和应用级模态弹窗。
/// @param sourceManager 当前 UI 管理器。
///
/// 每帧顺序固定为：消费跨线程信号、推进反馈状态、计算固定区域几何、绘制
/// 菜单/DockSpace/状态栏/工具栏、处理文件选择器、处理模态状态机，最后绘制
/// 延迟菜单弹窗。顺序保证新到通知能在本帧打开，同时让所有弹窗位于主界面之上。
/// 文件选择器只有用户显式打开后才可能执行平台阻塞调用。
///
/// 布局不变量：
/// - 菜单栏和状态栏高度一致，并分别贴合主视口上下边缘；
/// - 固定侧栏和固定工具栏从中央 DockSpace 宽度中预先扣除；
/// - 可停靠工具栏不占外部宽度，由 DockBuilder 为其建立节点；
/// - 所有皮肤尺寸先乘 DPI，再按像素取整；
/// - 客户端边框最后绘制到前景层，不参与中央窗口排版。
///
/// 模态不变量：
/// - 外部事件只设置成员请求位，不在回调内调用 OpenPopup；
/// - 请求位在调用 FeedbackOpenPopup 后立即清除；
/// - 异步保存通过队列返回结果，不在按钮回调内等待；
/// - GLFW 关闭标志可被临时工程或未保存工程流程拦截；
/// - 主菜单延迟弹窗使用不可见宿主维持合法 ImGui 上下文。
///
/// 线程边界：
/// - EventBus 回调只复制轻量值并写入 ConcurrentQueue；
/// - UI 帧是所有弹窗布尔状态和临时路径成员的唯一写入者；
/// - 逻辑操作通过 LogicCommandEvent 或 EditorEngine 命令队列提交；
/// - Vulkan 字体和纹理重建只设置请求，不在普通控件回调中执行；
/// - 会话互斥锁仅用于低频退出确认框中的谱面名称读取；
/// - 常规菜单、状态栏和 DockSpace 绘制不获取会话锁。
/// - 文件对话框结果先转换为值类型路径，再离开其内部选择状态；
/// - 平台窗口句柄只在当前帧使用，不保存到 MainDockSpaceUI 成员；
/// - 所有通知文本在 UI 线程组装，后台生产者不访问皮肤与翻译资源。
/// @warning UI 热路径：每帧执行；除用户明确触发的文件选择器和窗口关闭确认外，
/// 禁止加入文件系统扫描、阻塞等待或完整数据重建。
void MainDockSpaceUI::update(UIManager* sourceManager)
{
    // 先确保事件桥已建立，再把后台结果归并到 UI 线程成员状态。
    ensureTemporaryProjectSubscriptions();
    consumeTemporaryProjectQueues();

    const float deltaSeconds = ImGui::GetIO().DeltaTime;
    // 三类反馈都只推进本地计时或菜单状态，不查询工程文件系统。
    m_statusMessageService.update(deltaSeconds);
    m_beatmapLoadDiagnosticFeedback.update();
    m_mainMenuview.update(sourceManager, m_statusMessageService);

    // 本帧复用引擎、皮肤、视口和 DPI 引用，避免各布局块采样到不同配置。
    auto&                engine   = Logic::EditorEngine::instance();
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    ImGuiViewport*       viewport = ImGui::GetMainViewport();
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    // IGFD 翻译当前受库封装限制；此处不在热路径重复写入静态文案表。

    if ( auto* nativeWindow =
             sourceManager ? sourceManager->getNativeWindow() : nullptr ) {
        // 每帧从平台窗口校正状态，覆盖可能延迟或丢失的事件通知。
        m_isMaximized = nativeWindow->isWindowMaximized();
    }

    const auto& editorSettings =
        Config::AppConfig::instance().getEditorConfig().settings;
    const auto& aesthetics = editorSettings.aesthetics;

    float windowPaddingVal = std::floor(aesthetics.windowPadding * dpiScale);

    // 侧栏和固定工具栏宽度都包含两侧窗口内边距。
    float sidebarWidth =
        SideBarUI::GetSidebarWidth(dpiScale) + 2.0f * windowPaddingVal;
    float toolbarWidth = std::floor(32.0f * dpiScale) + 2.0f * windowPaddingVal;
    float toolbarLayoutWidth =
        editorSettings.fixedToolWindow ? toolbarWidth : 0.0f;

    float       extraPaddingY = std::floor(4.0f * dpiScale);
    ImGuiStyle& style         = ImGui::GetStyle();

    // 把皮肤审美参数转换为整数像素并同步到本帧全局 ImGui 样式。
    float windowRound     = std::floor(aesthetics.windowRounding * dpiScale);
    float frameRound      = std::floor(aesthetics.frameRounding * dpiScale);
    style.WindowRounding  = windowRound;
    style.ChildRounding   = windowRound;
    style.FrameRounding   = frameRound;
    style.PopupRounding   = frameRound;
    style.TabRounding     = frameRound;
    style.ItemSpacing     = { std::floor(aesthetics.itemSpacing * dpiScale),
                              std::floor(aesthetics.itemSpacing * dpiScale) };
    style.WindowPadding   = { windowPaddingVal, windowPaddingVal };
    style.FrameBorderSize = 0.0f;

    // 菜单栏与状态栏等高，形成对称的顶部和底部固定区域。
    float menuBarHeight =
        ImGui::GetFontSize() + (style.FramePadding.y + extraPaddingY) * 2.0f;
    float statusBarHeight = menuBarHeight;

    // 在提交窗口控件前设置边缘鼠标形状，避免被中央内容覆盖。
    handleNativeWindowFrameInteraction(sourceManager, dpiScale);

    // 顶部菜单栏占据主视口第一条固定带。
    renderMenuBar(sourceManager,
                  menuBarHeight,
                  sidebarWidth,
                  toolbarLayoutWidth,
                  dpiScale);

    // 中央 DockSpace 使用扣除四周固定区域后的剩余矩形。
    renderDockingSpace(sourceManager,
                       menuBarHeight,
                       statusBarHeight,
                       sidebarWidth,
                       toolbarLayoutWidth);

    // 状态栏在 DockSpace 之后绘制，以覆盖底部边界接缝。
    renderStatusBar(sourceManager, statusBarHeight, dpiScale);

    // 工具栏可固定在右侧，也可作为 DockSpace 内的普通停靠窗口。
    {
        float floatGap = std::floor(aesthetics.windowGap * dpiScale);
        if ( editorSettings.fixedToolWindow ) {
            // 固定模式显式覆盖位置和尺寸，不读取上一次 ImGui 停靠布局。
            ImGui::SetNextWindowPos(
                ImVec2(viewport->WorkPos.x + viewport->WorkSize.x -
                           toolbarWidth - floatGap,
                       viewport->WorkPos.y + menuBarHeight + floatGap),
                ImGuiCond_Always);
            ImGui::SetNextWindowSize(
                ImVec2(toolbarWidth,
                       viewport->WorkSize.y - menuBarHeight - statusBarHeight -
                           2.0f * floatGap));
        }
        // 浮动模式只指定视口，窗口位置由 DockBuilder 或用户布局决定。
        ImGui::SetNextWindowViewport(viewport->ID);
        m_toolbarView.update(sourceManager);
    }

    // 前景边框在所有主工作区窗口后绘制，确保无边框轮廓不被遮盖。
    renderNativeWindowFrameOverlay(sourceManager, dpiScale);

    // Unified 模式的所有文件选择器都由同一 ImGuiFileDialog 实例逐帧驱动。
    if ( editorSettings.filePickerStyle == Config::FilePickerStyle::Unified ) {
        // 选择器共同约定：
        // - 每个用途使用独立稳定 key，不能复用同一打开状态；
        // - IsOpened 时才预设居中位置，避免影响其他普通窗口；
        // - Display 返回完成态后读取 IsOk 区分确认与取消；
        // - 完成后无条件 Close，清理 IGFD 内部选择状态；
        // - 工程和音频选择会保存最近浏览目录；
        // - 字体选择只更新对应路径并请求延迟重建。
        // - 对话框尺寸使用逻辑像素，由居中工具按 DPI 和视口修正；
        // - NoSavedSettings 防止临时模态尺寸污染普通窗口布局；
        // - NoCollapse/NoResize 保证确认按钮与当前路径始终可见；
        // - 所有业务命令都在确认分支发布，取消不产生副作用。
        // 项目目录选择器接受目录本身，不要求用户选中具体文件。
        {
            // RAII scope 只负责本对话框的居中模态样式。
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened(
                     "ProjectFolderPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "ProjectFolderPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    // 某些平台目录模式只返回 CurrentPath，空结果时显式回退。
                    std::string folderPath =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    if ( folderPath.empty() ) {
                        folderPath =
                            ImGuiFileDialog::Instance()->GetCurrentPath();
                    }

                    // 记录浏览位置供下一次选择器启动，不把最终目标误作浏览目录。
                    auto config = engine.getEditorConfig();
                    config.settings.lastFilePickerPath =
                        ImGuiFileDialog::Instance()->GetCurrentPath();
                    engine.setEditorConfig(config);

                    // UTF-8 字符串在 UI 边界转换为项目统一文件系统路径。
                    MenuUtil::submitProjectFolderSelection(
                        Config::utf8ToPath(folderPath));
                }
                // 无论确认还是取消，Display 完成后都关闭该命名实例。
                ImGuiFileDialog::Instance()->Close();
            }
        }

        // 临时项目保存目录选择器与普通打开目录使用独立 ID 和后续命令。
        {
            // 对话框只决定正式目录；是否关闭工程或退出由 afterSaveAction 保留。
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened(
                     "TemporaryProjectSaveFolderPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "TemporaryProjectSaveFolderPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    // 目录选择的空路径回退规则与普通工程选择保持一致。
                    std::string folderPath =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    if ( folderPath.empty() ) {
                        folderPath =
                            ImGuiFileDialog::Instance()->GetCurrentPath();
                    }

                    // 保存成功前只更新最近浏览路径，不提前改变当前工程根目录。
                    auto config = engine.getEditorConfig();
                    config.settings.lastFilePickerPath =
                        ImGuiFileDialog::Instance()->GetCurrentPath();
                    engine.setEditorConfig(config);

                    // 先置进行中标志禁用重复按钮，再向逻辑线程发布保存命令。
                    m_temporaryProjectSaveInProgress = true;
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdSaveTemporaryProject{ folderPath }));
                }
                // 取消选择不会改变保存中标志，也不会清除原后续动作。
                ImGuiFileDialog::Instance()->Close();
            }
        }

        // 音频选择器只收集文件路径，轨道类型由后续专用模态框决定。
        {
            // 类型选择与文件选择分离，使同一音频路径可明确导入主轨或音效轨。
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("AudioImportPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "AudioImportPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    // 文件模式直接使用 GetFilePathName，不接受空目录回退。
                    std::string filePath =
                        ImGuiFileDialog::Instance()->GetFilePathName();

                    // 最近浏览目录独立于待导入文件路径保存。
                    auto config = engine.getEditorConfig();
                    config.settings.lastFilePickerPath =
                        ImGuiFileDialog::Instance()->GetCurrentPath();
                    engine.setEditorConfig(config);

                    // 暂存路径并延迟打开类型选择，避免在文件对话框栈内嵌套弹窗。
                    m_pendingImportPath   = filePath;
                    m_showImportTypeModal = true;
                }
                // 关闭 IGFD 后，m_showImportTypeModal
                // 会在后续代码中打开新模态框。
                ImGuiFileDialog::Instance()->Close();
            }
        }

        // ASCII 字体选择器更新配置后请求渲染器在安全时机重建字体图集。
        {
            // ASCII 与 CJK 分开配置，允许皮肤覆盖时保留用户字体回退策略。
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("AsciiFontPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "AsciiFontPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    // 配置保存原始 UTF-8 路径，加载器负责后续有效性检查。
                    std::string filePath =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    auto config = engine.getEditorConfig();
                    config.settings.preferredAsciiFont = filePath;
                    engine.setEditorConfig(config);
                    // Vulkan 上下文可能正在关闭；不可用时只保留配置变更。
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().requestFontRebuild();
                }
                // 取消时不修改配置，也不请求字体图集重建。
                ImGuiFileDialog::Instance()->Close();
            }
        }

        // CJK 字体使用独立配置键，但共享同一延迟字体重建机制。
        {
            // CJK 字体通常体积更大，仍只置重建请求而不阻塞当前模态帧。
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("CjkFontPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "CjkFontPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    // 不在文件选择回调中直接创建 GPU 字体资源。
                    std::string filePath =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    auto config                      = engine.getEditorConfig();
                    config.settings.preferredCjkFont = filePath;
                    engine.setEditorConfig(config);
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().requestFontRebuild();
                }
                // 路径有效性和字体解析错误由字体资源重载流程统一反馈。
                ImGuiFileDialog::Instance()->Close();
            }
        }
    }

    // 事件或文件选择器只置请求位，实际 OpenPopup 必须发生在当前 ImGui 帧。
    if ( m_showImportTypeModal ) {
        ::MMM::UI::FeedbackOpenPopup("AudioImportTypeModal");
        m_showImportTypeModal = false;
    }

    {
        // 类型选择模态与文件选择器分帧衔接，避免两个模态生命周期冲突。
        Utils::CenteredModalPopupScope importModalScope(dpiScale);
        if ( importModalScope.begin("AudioImportTypeModal") ) {
            // 三个按钮共享同一待导入路径，只有轨道类型和是否发布命令不同。
            ImGui::Text("%s", TR("ui.audio_import.type_hint").data());
            ImGui::Spacing();

            if ( ::MMM::UI::FeedbackButton(TR("ui.audio_track.main").data(),
                                           { 120, 0 }) ) {
                // 主轨和音效轨只改变命令枚举，使用同一暂存文件路径。
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdImportAudio{
                        m_pendingImportPath, AudioTrackType::Main }));
                // 命令已按值复制路径，关闭弹窗后无需延长 UI 字符串生命周期。
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.audio_track.effect").data(),
                                           { 120, 0 }) ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdImportAudio{
                        m_pendingImportPath, AudioTrackType::Effect }));
                // 音效轨导入遵循同一逻辑队列，不在 UI 线程读取音频文件。
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           { 80, 0 }) ) {
                // 取消不发布命令；暂存路径可留到下一次请求覆盖。
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // 覆盖确认由请求位驱动，回调仅在用户明确确认时执行一次。
    if ( m_showOverwriteModal ) {
        ::MMM::UI::FeedbackOpenPopup("OverwriteConfirmModal");
        m_showOverwriteModal = false;
    }

    {
        // 路径文本用于确认目标，具体覆盖逻辑仍由请求方回调封装。
        Utils::CenteredModalPopupScope overwriteModalScope(dpiScale);
        if ( overwriteModalScope.begin("OverwriteConfirmModal") ) {
            // 弹窗展示的是请求时冻结的路径，避免调用方状态变化后目标不明确。
            ImGui::Text("%s", TR("ui.file.overwrite.title").data());
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("%s",
                        TR_FMT("ui.file.overwrite.msg", m_pendingOverwritePath)
                            .c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if ( ::MMM::UI::FeedbackButton(TR("ui.common.confirm").data(),
                                           { 120, 0 }) ) {
                // 回调允许为空；弹窗无论是否存在回调都在确认后关闭。
                if ( m_onOverwriteConfirm ) m_onOverwriteConfirm();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           { 120, 0 }) ) {
                // 取消不调用覆盖回调，原目标文件保持不变。
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // 安全限制先于临时项目提示绘制，两者内部各自维护稳定弹窗 ID。
    renderCollaborationSafetyPopups(dpiScale);
    renderTemporaryProjectPopups(dpiScale, viewport);
    if ( m_exitAfterTemporaryProjectSave && viewport->PlatformHandle ) {
        // 保存成功事件先落到 UI 队列，本帧再向 GLFW 提交最终退出标志。
        m_temporaryProjectExitConfirmed = true;
        glfwSetWindowShouldClose(
            static_cast<GLFWwindow*>(viewport->PlatformHandle), GLFW_TRUE);
        m_exitAfterTemporaryProjectSave = false;
        // 请求提交后立即清除一次性标志，避免后续帧重复设置关闭状态。
    }

    // GLFW 关闭标志是平台请求；存在临时项目或未保存内容时需要先拦截。
    if ( viewport->PlatformHandle ) {
        // 关闭决策优先级：
        // - 已确认临时项目退出时不再拦截；
        // - 未确认且临时项目打开时必须询问正式保存；
        // - 普通工程有未保存内容时显示保存确认；
        // - 其他情况保留平台关闭标志并退出主循环。
        // PlatformHandle 由 ImGui 主视口提供，只在本帧内转换为 GLFWwindow。
        GLFWwindow* nativeWin = (GLFWwindow*)viewport->PlatformHandle;
        if ( glfwWindowShouldClose(nativeWin) ) {
            if ( !m_temporaryProjectExitConfirmed &&
                 engine.isTemporaryProjectOpen() ) {
                // 临时项目优先于普通未保存判断，因为它还涉及正式目录选择。
                glfwSetWindowShouldClose(nativeWin, GLFW_FALSE);
                const auto info = engine.currentTemporaryProjectInfo();
                // 冻结源包和缓存路径，关闭确认期间不再依赖引擎内部对象。
                m_temporaryProjectSourcePath =
                    Config::pathToUtf8(info.m_sourcePackagePath);
                m_temporaryProjectCachePath =
                    Config::pathToUtf8(info.m_cacheProjectPath);
                m_temporaryProjectSaveError.clear();
                m_temporaryProjectAfterSaveAction =
                    TemporaryProjectAfterSaveAction::ExitApp;
                // 记录退出意图，保存成功后由队列消费路径恢复关闭请求。
                m_showTemporaryProjectCloseModal = true;
            } else if ( !m_temporaryProjectExitConfirmed &&
                        engine.hasUnsavedChanges() ) {
                // 普通工程拦截关闭请求，并使用稳定 ### ID 打开确认框。
                glfwSetWindowShouldClose(nativeWin, GLFW_FALSE);
                const std::string exitPopupName = fmt::format(
                    "{}###ExitConfirmation", TR("ui.exit.confirm_title"));
                ::MMM::UI::FeedbackOpenPopup(exitPopupName.c_str());
            }
            // 已确认的普通关闭请求保持 GLFW_TRUE，让平台循环自然退出。
        }
    }

    const std::string exitPopupName =
        fmt::format("{}###ExitConfirmation", TR("ui.exit.confirm_title"));
    {
        // 可见标题允许随语言变化，### 后缀保证 ImGui 弹窗身份稳定。
        Utils::CenteredModalPopupScope exitModalScope(dpiScale);
        if ( exitModalScope.begin(exitPopupName.c_str()) ) {
            // 锁只覆盖弹窗当前帧的会话名称读取和控件提交。
            // 这是低频确认路径，不允许扩展到常规主界面绘制。
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            // 只在退出弹窗可见的低频路径加锁，读取当前谱面显示名。
            auto        session = engine.getActiveSession();
            std::string mapName = "Unknown";
            if ( session && session->getContext().currentBeatmap ) {
                // 谱面显示名按值复制，避免格式化文本持有模型内部引用。
                mapName = session->getContext()
                              .currentBeatmap->m_baseMapMetadata.name;
            }

            ImGui::TextUnformatted(
                TR_FMT("ui.exit.confirm_msg_fmt", mapName).c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if ( ::MMM::UI::FeedbackButton(TR("ui.file.save").data(),
                                           ImVec2(120 * dpiScale, 0)) ) {
                // 保存命令先入逻辑队列，再设置 GLFW 关闭标志维持既有顺序。
                engine.pushCommand(Logic::CmdSaveBeatmap{});
                // 当前逻辑命令模型按顺序处理保存和退出；此处不阻塞等待完成。
                if ( viewport->PlatformHandle ) {
                    glfwSetWindowShouldClose(
                        (GLFWwindow*)viewport->PlatformHandle, GLFW_TRUE);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.exit.dont_save").data(),
                                           ImVec2(120 * dpiScale, 0)) ) {
                // 放弃保存直接恢复平台关闭请求，不向逻辑队列追加保存命令。
                if ( viewport->PlatformHandle ) {
                    glfwSetWindowShouldClose(
                        (GLFWwindow*)viewport->PlatformHandle, GLFW_TRUE);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.help.cancel").data(),
                                           ImVec2(120 * dpiScale, 0)) ) {
                // 取消只关闭模态框，之前拦截的 GLFW 标志继续保持 false。
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // 延迟菜单弹窗需要一个活跃 ImGui 窗口作为宿主，但宿主本身不可见不可交互。
    {
        constexpr ImGuiWindowFlags popupHostFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f), ImGuiCond_Always);
        // 固定一像素宿主不占布局空间，弹窗仍由 ImGui 定位到主视口。
        if ( ImGui::Begin("MainMenuPopupHost", nullptr, popupHostFlags) ) {
            // PGO 同意窗口与菜单动作延迟弹窗共享宿主，但各自维护独立 Popup ID。
            m_pgoUploadConsentWindow.render(dpiScale);
            m_mainMenuview.renderDeferredPopups(
                sourceManager, dpiScale, m_statusMessageService);
        }
        // Begin 即使返回 false 也必须配对 End。
        ImGui::End();
    }
}

/// @brief 在普通帧和文件操作占位帧中统一消费、绘制保存反馈。
/// @param fileOperationBusy 当前是否由文件操作占据主 UI。
///
/// 保存结果与主 DockSpace 绘制解耦，即使工程切换暂时隐藏常规窗口，也要继续
/// 推进反馈计时并显示结果，防止成功或失败提示被操作遮蔽。
/// @warning UI 热路径：不读取会话状态，不等待文件操作。
void MainDockSpaceUI::updateSaveFeedback(bool fileOperationBusy)
{
    // update 先消费结果并推进计时，render 再按同一 busy 状态选择表现层级。
    m_saveResultFeedback.update(
        ImGui::GetIO().DeltaTime, m_statusMessageService, fileOperationBusy);
    m_saveResultFeedback.render(
        Config::AppConfig::instance().getWindowContentScale(),
        fileOperationBusy);
}

/// @brief 保持暂时隐藏的谱面与工具窗口的停靠树，不访问会话。
///
/// 文件操作期间常规 DockSpace 可能不绘制内容；KeepAliveOnly 保留节点、标签页
/// 归属和用户布局，待操作结束后可无损恢复。
/// @warning UI 热路径：操作期间每帧只提交一次停靠节点保活。
void MainDockSpaceUI::keepFileOperationDockSpaceAlive()
{
    // 零 ID 表示根 DockSpace 尚未创建，此时没有需要保活的节点树。
    if ( s_mainDockId != 0 ) {
        ImGui::DockSpace(
            s_mainDockId, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
    }
}

/// @brief 处理临时项目提示、保存结果及音频资源变更结果队列。
///
/// 本函数是跨线程事件进入 ImGui 状态的唯一汇合点。每个队列会在本帧排空，
/// 同类重复通知折叠到最终成员状态；保存结果仍逐项执行后续关闭或退出动作。
///
/// 临时项目状态机：
/// - 编辑拦截进入只读提示，afterSaveAction 为 None；
/// - 关闭请求进入保存提示，afterSaveAction 为 CloseProject；
/// - 应用退出拦截由 update 设置 afterSaveAction 为 ExitApp；
/// - 保存失败回到原提示并保留错误，不执行后续动作；
/// - 保存成功按动作发布关闭工程事件或设置延迟退出标志；
/// - 每个完成结果最终把 afterSaveAction 重置为 None。
/// @warning UI 热路径：每帧执行非阻塞 try_dequeue；不得等待生产者。
void MainDockSpaceUI::consumeTemporaryProjectQueues()
{
    TemporaryProjectPromptPayload prompt;
    // 编辑拦截表示保留当前临时工程，只提示用户先选择正式保存位置。
    while ( temporaryProjectEditBlockedQueue().try_dequeue(prompt) ) {
        m_temporaryProjectSourcePath = prompt.sourcePackagePath;
        m_temporaryProjectCachePath  = prompt.cacheProjectPath;
        m_temporaryProjectSaveError.clear();
        m_temporaryProjectAfterSaveAction =
            TemporaryProjectAfterSaveAction::None;
        // 弹窗请求位在渲染阶段消费，队列处理阶段不调用 ImGui。
        m_showTemporaryProjectReadOnlyModal = true;
    }

    // 关闭提示需要记住“保存后关闭工程”的后续动作。
    while ( temporaryProjectClosePromptQueue().try_dequeue(prompt) ) {
        // 队列载荷覆盖旧提示路径，保证弹窗对应最后一次关闭请求。
        m_temporaryProjectSourcePath = prompt.sourcePackagePath;
        m_temporaryProjectCachePath  = prompt.cacheProjectPath;
        m_temporaryProjectSaveError.clear();
        m_temporaryProjectAfterSaveAction =
            TemporaryProjectAfterSaveAction::CloseProject;
        // 仅设置显示请求，实际 Modal 在本帧后续渲染阶段打开。
        m_showTemporaryProjectCloseModal = true;
    }

    TemporaryProjectSaveResultPayload saveResult;
    // 保存完成解除按钮禁用状态，并依据结果推进临时项目状态机。
    while ( temporaryProjectSaveResultQueue().try_dequeue(saveResult) ) {
        m_temporaryProjectSaveInProgress = false;
        if ( !saveResult.success ) {
            // 空错误使用稳定兜底文本；失败后回到触发保存的原弹窗。
            m_temporaryProjectSaveError = saveResult.errorMessage.empty()
                                              ? "保存临时项目失败"
                                              : saveResult.errorMessage;
            if ( m_temporaryProjectAfterSaveAction ==
                 TemporaryProjectAfterSaveAction::None ) {
                // 编辑拦截发起的保存失败后，重新显示只读说明。
                m_showTemporaryProjectReadOnlyModal = true;
            } else {
                // 关闭或退出流程失败后，重新显示带错误的保存确认框。
                m_showTemporaryProjectCloseModal = true;
            }
            continue;
        }

        // 成功后清除源包提示，并把正式保存目录作为当前路径反馈。
        m_temporaryProjectSaveError.clear();
        m_temporaryProjectSourcePath.clear();
        m_temporaryProjectCachePath = saveResult.savedProjectPath;
        if ( m_temporaryProjectAfterSaveAction ==
             TemporaryProjectAfterSaveAction::CloseProject ) {
            // 关闭工程仍走统一项目事件，不能在 UI 内直接销毁会话。
            Event::EventBus::instance().publish(
                Event::ProjectCloseRequestedEvent{});
        } else if ( m_temporaryProjectAfterSaveAction ==
                    TemporaryProjectAfterSaveAction::ExitApp ) {
            // 退出标志延迟到 update 持有有效 GLFW 视口时再提交。
            m_exitAfterTemporaryProjectSave = true;
        }
        m_temporaryProjectAfterSaveAction =
            TemporaryProjectAfterSaveAction::None;
        // 重置后续动作发生在每个成功结果末尾，后到结果不会继承旧意图。
    }

    AudioResourceMutationResultPayload mutationResult;
    // 音频结果转换为中心通知；失败停留更久以便阅读完整阻止列表。
    while ( audioResourceMutationResultQueue().try_dequeue(mutationResult) ) {
        if ( auto context = Graphic::VKContext::get() ) {
            context->get().showCenterNotification(
                buildAudioResourceMutationMessage(mutationResult),
                mutationResult.success ? 3.0F : 10.0F);
        }
    }

    // 三类协作限制分别置位，实际每帧只打开一个安全提示模态框。
    bool collaborationSafetySignal = false;
    while ( collaborationProjectOpenBlockedQueue().try_dequeue(
        collaborationSafetySignal) ) {
        // 队列中多个同类信号合并为一个待显示布尔状态。
        m_showCollaborationProjectOpenBlockedModal = true;
    }
    while ( collaborationOfflineEditBlockedQueue().try_dequeue(
        collaborationSafetySignal) ) {
        m_showCollaborationOfflineEditBlockedModal = true;
    }
    while ( collaborationPermissionEditBlockedQueue().try_dequeue(
        collaborationSafetySignal) ) {
        m_showCollaborationPermissionEditBlockedModal = true;
    }
    // collaborationSafetySignal 只承接队列 API 输出，弹窗原因由队列决定。
}

/// @brief 请求选择临时项目正式保存位置。
///
/// Native 模式立即调用平台目录选择器；Unified 模式只打开 IGFD，并由 update
/// 后续帧处理结果。两条路径最终都发布 CmdSaveTemporaryProject，逻辑层负责
/// 复制工程内容及返回保存结果。
/// @warning 用户触发的低频路径；Native 选择器可能阻塞 UI 直到用户关闭。
void MainDockSpaceUI::requestTemporaryProjectSaveFolder()
{
    // 新一次选择开始时移除旧错误，避免错误文本与当前请求混淆。
    m_temporaryProjectSaveError.clear();

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    if ( editorSettings.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 平台对话框不经过 ImGui Popup，显式播放一次弹出反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t* outPath = nullptr;
        nfdresult_t  result  = NativeFileDialog::pickFolder(&outPath, nullptr);
        if ( result == NFD_OKAY ) {
            // 只有确认路径后才进入保存中状态；取消不发布逻辑命令。
            m_temporaryProjectSaveInProgress = true;
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSaveTemporaryProject{ outPath }));
            NFD_FreePathU8(outPath);
        }
        // Native 路径已完整处理，不再打开 Unified 对话框。
        return;
    }

    // Unified 对话框从最近浏览目录启动，并限制为单一目录结果。
    IGFD::FileDialogConfig fdConfig;
    fdConfig.path              = editorSettings.lastFilePickerPath;
    fdConfig.countSelectionMax = 1;
    fdConfig.flags             = ImGuiFileDialogFlags_Modal;
    const bool wasOpen         = ImGuiFileDialog::Instance()->IsOpened(
        "TemporaryProjectSaveFolderPicker");
    // OpenDialog 可重复调用；wasOpen 用于确保打开反馈只播放一次。
    ImGuiFileDialog::Instance()->OpenDialog(
        "TemporaryProjectSaveFolderPicker", "保存临时项目", nullptr, fdConfig);
    if ( !wasOpen && ImGuiFileDialog::Instance()->IsOpened(
                         "TemporaryProjectSaveFolderPicker") ) {
        // 只有关闭到打开的状态跃迁需要播放弹出反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 渲染临时项目只读和关闭确认弹窗。
/// @param dpiScale 当前窗口内容缩放。
/// @param viewport 主视口，用于“不保存并退出”时恢复 GLFW 关闭标志。
///
/// 只读弹窗对应被拦截的编辑操作；关闭弹窗对应关闭工程或退出应用。两者共享
/// 保存目录选择与异步结果状态，但通过 afterSaveAction 区分成功后的下一步。
///
/// 交互约束：
/// - 保存按钮在异步保存期间禁用，防止并发写入同一缓存工程；
/// - 只读提示允许用户暂不保存并继续浏览，不改变临时工程状态；
/// - 关闭提示的“不保存”立即执行原关闭或退出意图；
/// - 取消关闭清除 afterSaveAction，使后续保存不误触发退出；
/// - 保存错误显示在原路径上下文内，不自动关闭弹窗；
/// - viewport 无平台句柄时“不保存并退出”只清理本地决策状态。
/// @warning UI 热路径：每帧提交固定数量模态控件；文件选择仅由按钮触发。
void MainDockSpaceUI::renderTemporaryProjectPopups(float          dpiScale,
                                                   ImGuiViewport* viewport)
{
    // 请求位只消费一次，避免弹窗可见期间重复 OpenPopup 重置焦点。
    if ( m_showTemporaryProjectReadOnlyModal ) {
        ::MMM::UI::FeedbackOpenPopup(
            "临时项目只读###TemporaryProjectReadOnlyModal");
        m_showTemporaryProjectReadOnlyModal = false;
    }

    {
        // 稳定 ### ID 让中文标题变化时仍保留同一个 ImGui 弹窗身份。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin("临时项目只读###TemporaryProjectReadOnlyModal",
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(560.0f * dpiScale, 0.0f)) ) {
            drawWrappedText(
                "当前打开的是临时项目。要修改谱面或项目资源，请先选择正式保存位"
                "置。");
            // 同时显示来源与缓存目录，帮助用户确认数据仍位于临时工作区。
            ImGui::Spacing();
            drawWrappedLabelValue("打开文件：", m_temporaryProjectSourcePath);
            drawWrappedLabelValue("缓存项目：", m_temporaryProjectCachePath);
            if ( !m_temporaryProjectSaveError.empty() ) {
                // 保存失败保留弹窗上下文，并以危险色显示逻辑层原因。
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                   "%s",
                                   m_temporaryProjectSaveError.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 保存进行中禁用重复目录选择，“继续只读”仍可关闭提示。
            ImGui::BeginDisabled(m_temporaryProjectSaveInProgress);
            if ( ::MMM::UI::FeedbackButton("选择保存位置",
                                           ImVec2(140.0f * dpiScale, 0.0f)) ) {
                m_temporaryProjectAfterSaveAction =
                    TemporaryProjectAfterSaveAction::None;
                // 该入口保存成功后只解除临时状态，不自动关闭工程。
                requestTemporaryProjectSaveFolder();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton("继续只读",
                                           ImVec2(120.0f * dpiScale, 0.0f)) ) {
                // 继续只读不清空路径和错误，下一次编辑拦截仍可提供上下文。
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    if ( m_showTemporaryProjectCloseModal ) {
        // 关闭确认同样通过请求位打开一次，失败结果可以重新置位。
        ::MMM::UI::FeedbackOpenPopup(
            "保存临时项目###TemporaryProjectCloseModal");
        m_showTemporaryProjectCloseModal = false;
    }

    {
        // 关闭弹窗展示源包与缓存路径，明确“不保存”放弃的临时状态。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin("保存临时项目###TemporaryProjectCloseModal",
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(560.0f * dpiScale, 0.0f)) ) {
            drawWrappedText("该项目为临时项目，是否保存项目？");
            // 路径信息在用户作出不可逆的“不保存”决定前保持可见。
            ImGui::Spacing();
            drawWrappedLabelValue("打开文件：", m_temporaryProjectSourcePath);
            drawWrappedLabelValue("缓存项目：", m_temporaryProjectCachePath);
            if ( !m_temporaryProjectSaveError.empty() ) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                   "%s",
                                   m_temporaryProjectSaveError.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 异步保存未结束前禁止再次提交，但保留其他决策按钮。
            ImGui::BeginDisabled(m_temporaryProjectSaveInProgress);
            if ( ::MMM::UI::FeedbackButton("保存项目",
                                           ImVec2(120.0f * dpiScale, 0.0f)) ) {
                // afterSaveAction 已由关闭或退出入口设置，此处不覆盖它。
                requestTemporaryProjectSaveFolder();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton("不保存",
                                           ImVec2(120.0f * dpiScale, 0.0f)) ) {
                if ( m_temporaryProjectAfterSaveAction ==
                     TemporaryProjectAfterSaveAction::ExitApp ) {
                    // 退出分支恢复 GLFW 标志，并标记已通过临时项目确认。
                    if ( viewport && viewport->PlatformHandle ) {
                        m_temporaryProjectExitConfirmed = true;
                        glfwSetWindowShouldClose(
                            static_cast<GLFWwindow*>(viewport->PlatformHandle),
                            GLFW_TRUE);
                    }
                } else {
                    // 非退出分支统一请求关闭当前工程，由上层执行资源清理。
                    Event::EventBus::instance().publish(
                        Event::ProjectCloseRequestedEvent{});
                }
                m_temporaryProjectAfterSaveAction =
                    TemporaryProjectAfterSaveAction::None;
                // 决策完成后清除后续动作，防止下次提示继承旧意图。
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.help.cancel").data(),
                                           ImVec2(120.0f * dpiScale, 0.0f)) ) {
                // 取消关闭流程，同样清除保存后的关闭或退出动作。
                m_temporaryProjectAfterSaveAction =
                    TemporaryProjectAfterSaveAction::None;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
}

/// @brief 渲染协作模式下的离线、工程打开和细分权限拦截提示。
/// @param dpiScale 当前窗口内容缩放。
///
/// 三类提示使用独立稳定 ID 和翻译文本。请求位按离线、打开工程、权限不足的
/// 顺序每帧最多打开一个弹窗，避免多个 Modal 同帧竞争焦点；未消费请求保留到
/// 后续帧。提示仅解释限制，不在 UI 层修改协作连接或权限状态。
///
/// 优先顺序体现可恢复范围：离线状态影响全部编辑，访客工程限制影响工程边界，
/// 细分权限只影响具体操作。逐帧展示能让同时到达的原因分别被用户确认，不把
/// 多个限制压缩成含义不清的通用错误。
/// @warning UI 热路径：每帧提交三个固定模态宿主，不进行网络访问。
void MainDockSpaceUI::renderCollaborationSafetyPopups(float dpiScale)
{
    // 可见标题来自翻译，### 后缀使语言切换不改变弹窗内部 ID。
    const std::string offlinePopupId =
        TR("ui.collaboration.offline_edit.title").toString() +
        "###CollaborationOfflineEditBlockedModal";
    const std::string projectPopupId =
        TR("ui.collaboration.project_open_blocked.title").toString() +
        "###CollaborationProjectOpenBlockedModal";
    const std::string permissionPopupId =
        TR("ui.collaboration.permission_edit.title").toString() +
        "###CollaborationPermissionEditBlockedModal";
    // else-if 保证一帧只请求打开一个 Modal，维持明确的焦点归属。
    if ( m_showCollaborationOfflineEditBlockedModal ) {
        FeedbackOpenPopup(offlinePopupId.c_str());
        m_showCollaborationOfflineEditBlockedModal = false;
    } else if ( m_showCollaborationProjectOpenBlockedModal ) {
        FeedbackOpenPopup(projectPopupId.c_str());
        m_showCollaborationProjectOpenBlockedModal = false;
    } else if ( m_showCollaborationPermissionEditBlockedModal ) {
        FeedbackOpenPopup(permissionPopupId.c_str());
        m_showCollaborationPermissionEditBlockedModal = false;
    }

    {
        // 离线提示说明房间状态限制，确认按钮只关闭说明。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(offlinePopupId.c_str(),
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0F * dpiScale, 0.0F)) ) {
            ImGui::TextWrapped(
                "%s", TR("ui.collaboration.offline_edit.message").data());
            ImGui::Spacing();
            if ( FeedbackButton(TR("ui.common.confirm").data(),
                                ImVec2(120.0F * dpiScale, 0.0F)) ) {
                // 连接恢复由协作模块负责，此处不发起隐式重连。
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    {
        // 访客打开本机工程会破坏房间上下文，因此只提供确认关闭。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(projectPopupId.c_str(),
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0F * dpiScale, 0.0F)) ) {
            ImGui::TextWrapped(
                "%s",
                TR("ui.collaboration.project_open_blocked.message").data());
            ImGui::Spacing();
            if ( FeedbackButton(TR("ui.common.confirm").data(),
                                ImVec2(120.0F * dpiScale, 0.0F)) ) {
                // 被拦截动作不会在关闭提示后自动重放。
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    {
        // 细分权限提示与离线提示分离，帮助用户判断应联系房主还是重连。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(permissionPopupId.c_str(),
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0F * dpiScale, 0.0F)) ) {
            ImGui::TextWrapped(
                "%s", TR("ui.collaboration.permission_edit.message").data());
            ImGui::Spacing();
            if ( FeedbackButton(TR("ui.common.confirm").data(),
                                ImVec2(120.0F * dpiScale, 0.0F)) ) {
                // 权限变更由服务器状态同步，本弹窗不乐观更新本地权限。
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

/// @brief 根据鼠标所在边缘更新无边框窗口的缩放光标。
/// @param sourceManager UI 管理器，用于取得平台窗口框架适配器。
/// @param dpiScale 当前窗口内容缩放。
///
/// 只有平台声明支持客户端框架请求且窗口未最大化时才检测边缘。此函数仅更新
/// 光标形状；真正的缩放开始由平台适配器的命中测试和原生事件流程负责。
/// 不读取鼠标按钮，也不在这里调用 beginResize，避免 ImGui 与平台层同时抢占
/// 指针手势。最大化状态来自原生窗口回报，是是否存在缩放边缘的权威判断。
/// @warning UI 热路径：每帧执行常量次数指针检查和几何判断。
void MainDockSpaceUI::handleNativeWindowFrameInteraction(
    UIManager* sourceManager, float dpiScale)
{
    // 最大化窗口没有可拖动缩放边缘；管理器缺失时也无法访问平台能力。
    if ( !sourceManager || m_isMaximized ) {
        return;
    }

    auto* frameAdapter = sourceManager->getWindowFrameAdapter();
    // 不支持客户端框架请求的平台应完全沿用系统窗口边框行为。
    if ( !frameAdapter || !frameAdapter->supportsClientFrameRequests() ) {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // 启动或关闭阶段主视口可能尚未存在，直接等待下一帧。
    if ( !viewport ) {
        return;
    }

    const NativeFrameHit hit = resolveNativeFrameHit(*viewport, dpiScale);
    // 中央内容区域不覆盖其控件设置的普通鼠标形状。
    if ( !hit.m_hit ) {
        return;
    }

    // ImGui 光标与平台缩放方向来自同一个命中结果，保持视觉语义一致。
    ImGui::SetMouseCursor(hit.m_cursor);
}

/// @brief 在主视口前景层绘制客户端窗口边框和非最大化内阴影。
/// @param sourceManager UI 管理器，用于查询平台是否采用客户端边框。
/// @param dpiScale 当前窗口内容缩放。
///
/// Overlay 不参与命中测试，只补足无系统装饰时的视觉边界。最大化状态取消
/// 圆角和阴影，但保留一像素边框；普通状态从外向内叠加固定层数的弱阴影。
///
/// 绘制约束：
/// - 使用 ForegroundDrawList，确保轮廓覆盖所有主视口窗口；
/// - 使用 viewport Pos/Size，包含菜单栏、状态栏和中央工作区；
/// - 边框宽度至少一像素，圆角随 DPI 放大；
/// - 阴影层数固定，透明度随靠近内容区域逐渐提高；
/// - 每层 inset 同时收缩矩形和圆角，不在视口外绘制；
/// - 最大化时跳过阴影，避免屏幕边缘产生不必要的暗带。
/// @warning UI 热路径：每帧最多提交固定 NATIVE_FRAME_SHADOW_LAYERS 次矩形。
void MainDockSpaceUI::renderNativeWindowFrameOverlay(UIManager* sourceManager,
                                                     float      dpiScale) const
{
    // 未启用客户端 Overlay 的平台由系统负责边框，本函数不重复绘制。
    auto* frameAdapter =
        sourceManager ? sourceManager->getWindowFrameAdapter() : nullptr;
    if ( !frameAdapter || !frameAdapter->usesClientFrameOverlay() ) {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // 视口或前景 DrawList 在生命周期边界可能为空，允许无边框跳过一帧。
    if ( !viewport ) {
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    if ( !drawList ) {
        return;
    }

    // 绘制范围使用平台视口完整矩形，而不是扣除菜单栏后的 WorkRect。
    const ImVec2 min = viewport->Pos;
    const ImVec2 max = { viewport->Pos.x + viewport->Size.x,
                         viewport->Pos.y + viewport->Size.y };
    const float  borderThickness =
        std::max(1.0f, std::floor(NATIVE_FRAME_BORDER_THICKNESS * dpiScale));
    // 最大化窗口必须使用直角，避免屏幕边缘露出背景像素。
    const float rounding =
        m_isMaximized
            ? 0.0f
            : std::floor(NATIVE_FRAME_ROUNDING * std::max(1.0f, dpiScale));

    if ( !m_isMaximized ) {
        // 阴影从内层到外层计算透明度，越靠近内容轮廓越明显。
        for ( int layer = NATIVE_FRAME_SHADOW_LAYERS; layer > 0; --layer ) {
            const float inset = static_cast<float>(layer);
            const float alpha =
                0.035f *
                (static_cast<float>(NATIVE_FRAME_SHADOW_LAYERS - layer + 1) /
                 static_cast<float>(NATIVE_FRAME_SHADOW_LAYERS));
            const ImU32 shadowColor =
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, alpha));
            // inset 同时收缩矩形和圆角，防止内层曲线越过外层轮廓。
            drawList->AddRect({ min.x + inset, min.y + inset },
                              { max.x - inset, max.y - inset },
                              shadowColor,
                              std::max(0.0f, rounding - inset),
                              0,
                              borderThickness);
        }
    }

    // 最终边框沿用皮肤颜色，但设置最低不透明度保证任何背景下可见。
    ImVec4 border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
    border.w      = std::max(border.w, 0.55f);
    drawList->AddRect({ min.x + 0.5f, min.y + 0.5f },
                      { max.x - 0.5f, max.y - 0.5f },
                      ImGui::GetColorU32(border),
                      rounding,
                      0,
                      borderThickness);
}

/// @brief 消费纹理重载请求。
/// @return 本次调用前是否存在待处理重载请求。
///
/// exchange 同时读取并清除标志，确保一次皮肤变更只触发一次资源重建。
/// 调用方只有在返回 true 时进入 reloadTextures；若重载期间再次置位，新请求会
/// 留待下次检查，而不是在本函数内递归执行资源操作。
/// 标志由 UI 生命周期管理路径写入并由渲染资源路径消费，不承载纹理对象本身。
/// @warning 渲染资源检查路径调用；不执行实际 GPU 操作。
bool MainDockSpaceUI::needReload()
{
    return std::exchange(m_needReload, false);
}

/// @brief 按当前皮肤和 DPI 重新创建菜单栏 Logo 纹理。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param cmdPool 用于上传纹理的命令池。
/// @param queue 用于执行上传命令的队列。
///
/// 资源路径由 SkinManager 解析，目标像素尺寸随当前内容缩放变化。旧纹理由
/// unique_ptr 替换时自动释放，所有权继续保存在 MainDockSpaceUI。
/// 设备、命令池和队列由渲染资源管理流程提供，本函数不缓存其引用；返回后
/// 菜单栏只读取已经完成创建的 VKTexture 描述符。
/// @warning 低频资源重载路径：可能分配并提交 GPU 上传，不得在普通 UI 帧调用。
void MainDockSpaceUI::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                     vk::Device&         logicalDevice,
                                     vk::CommandPool& cmdPool, vk::Queue& queue)
{
    // 在实际重载时重新读取 DPI，避免请求产生后窗口已切换显示器。
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();
    // 轻微灰色调制保持 Logo 与标题栏文本层级一致。
    m_logo_texture = loadTextureResource(
        Config::SkinManager::instance().getAssetPath("logo"),
        static_cast<uint32_t>(24 * dpiScale),
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue,
        { { .83f, .83f, .83f, .83f } });
}

};  // namespace MMM::UI
