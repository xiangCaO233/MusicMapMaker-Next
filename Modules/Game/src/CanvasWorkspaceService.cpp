#include "game/CanvasWorkspaceService.h"

#include "canvas/AnnotationTableWindow.h"
#include "canvas/Basic2DCanvas.h"
#include "canvas/PreviewCanvas.h"
#include "canvas/TimelineCanvas.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"

namespace MMM::Game
{

/// @brief 生成 UI 画布标签所需的会话工作区快照。
/// @param entries 接收与当前会话顺序一致的画布条目。
///
/// 标签元数据在结构变化时发布，输出只复制稳定相机 ID、Logo 占位标志和
/// 停靠恢复标志；拥有型快照允许本帧在逻辑线程更新时安全读取。
/// @warning UI 每帧调用：只复制轻量字段，不得等待 SessionRegistry 长锁。
void CanvasWorkspaceService::fillEntries(
    std::vector<UI::CanvasWorkspaceEntry>& entries)
{
    const auto snapshot =
        Logic::EditorEngine::instance().getSessionUiSnapshot();
    // vector 复用已有容量，快照在本次复制期间保持所有字符串有效。
    entries.resize(snapshot->entries.size());
    for ( std::size_t index = 0; index < snapshot->entries.size(); ++index ) {
        const auto& source              = snapshot->entries[index];
        auto&       target              = entries[index];
        target.cameraId                 = source.cameraId;
        target.isLogoPlaceholder        = source.isLogoPlaceholder;
        target.restoreDockFromWorkspace = source.restoreDockFromWorkspace;
    }
}

/// @brief 查询当前活动会话在工作区条目中的索引。
/// @return 活动索引；无活动会话时保留 EditorEngine 的哨兵语义。
/// @warning UI 更新路径：只读取引擎的当前索引，不缓存跨帧结果。
std::int32_t CanvasWorkspaceService::getActiveEntryIndex() const
{
    return Logic::EditorEngine::instance().getActiveSessionIndex();
}

/// @brief 查询项目控制器是否正在等待完成项目切换。
/// @return 存在待处理切换时返回 true。
///
/// UI 使用该状态阻止在切换事务中重复发起互相覆盖的打开操作。
bool CanvasWorkspaceService::hasPendingProjectSwitch() const
{
    return Logic::ProjectController::instance().hasPendingProjectSwitch();
}

/// @brief 请求 EditorEngine 保存当前项目及其工作区状态。
///
/// 服务不直接访问项目文件，保存路径、失败反馈和原子替换由逻辑层负责。
/// @warning 低频用户操作路径：可能执行文件 I/O，不得从每帧无条件调用。
void CanvasWorkspaceService::saveProject()
{
    Logic::EditorEngine::instance().saveProject();
}

/// @brief 创建没有谱面内容的 Logo 占位会话。
/// @param displayName 标签页显示名称。
///
/// nullptr 谱面与 true 占位标志共同区分欢迎页画布和真实编辑会话。
void CanvasWorkspaceService::createLogoPlaceholderSession(
    const std::string& displayName)
{
    Logic::EditorEngine::instance().createSession(nullptr, displayName, true);
}

/// @brief 关闭指定会话。
/// @param index 待关闭的工作区条目索引。
/// @param updateWorkspace 是否同步修正项目工作区持久状态。
///
/// 索引校验、活动会话选择和资源释放均由 EditorEngine 统一处理。
void CanvasWorkspaceService::closeSession(std::int32_t index,
                                          bool         updateWorkspace)
{
    Logic::EditorEngine::instance().closeSession(index, updateWorkspace);
}

/// @brief 查询当前会话条目数量。
/// @return EditorEngine 会话容器的当前元素数。
/// @warning UI 更新路径：返回瞬时快照，调用方不得据此长期假定索引有效。
std::int32_t CanvasWorkspaceService::getEntryCount() const
{
    return static_cast<std::int32_t>(
        Logic::EditorEngine::instance().getSessionUiSnapshot()->entries.size());
}

/// @brief 取出并清除逻辑层排队的会话聚焦请求。
/// @return 待聚焦索引；没有请求时保留 EditorEngine 的哨兵值。
///
/// consume 语义保证同一请求只由 UI 工作区处理一次。
std::int32_t CanvasWorkspaceService::consumePendingFocusIndex()
{
    return Logic::EditorEngine::instance().consumePendingFocusSessionIndex();
}

/// @brief 请求逻辑层在后续安全阶段聚焦指定会话。
/// @param index 目标工作区条目索引。
///
/// 函数只转发意图，不直接切换 UI 标签或持有会话引用。
void CanvasWorkspaceService::requestEntryFocus(std::int32_t index)
{
    Logic::EditorEngine::instance().requestSessionFocus(index);
}

/// @brief 为工作区条目创建主编辑画布。
/// @param entry 包含稳定相机 ID 的会话条目快照。
/// @param width 初始离屏目标宽度。
/// @param height 初始离屏目标高度。
/// @return 由调用方接管的 Basic2DCanvas 视图。
///
/// 相机 ID 同时选择同步缓冲并作为画布身份，二者必须保持一致。
std::unique_ptr<UI::IUIView> CanvasWorkspaceService::createMainCanvas(
    const UI::CanvasWorkspaceEntry& entry, std::uint32_t width,
    std::uint32_t height)
{
    return std::make_unique<Canvas::Basic2DCanvas>(
        entry.cameraId,
        width,
        height,
        Logic::EditorEngine::instance().getSyncBuffer(entry.cameraId),
        entry.cameraId);
}

/// @brief 创建共享 Preview 同步缓冲的谱面预览画布。
/// @param name UIManager 使用的稳定视图名。
/// @param width 初始离屏目标宽度。
/// @param height 初始离屏目标高度。
/// @return 由调用方接管的 PreviewCanvas 视图。
///
/// Preview 缓冲按固定逻辑键获取，不与主画布相机 ID 混用。
std::unique_ptr<UI::IUIView> CanvasWorkspaceService::createPreviewCanvas(
    const std::string& name, std::uint32_t width, std::uint32_t height)
{
    return std::make_unique<Canvas::PreviewCanvas>(
        name,
        width,
        height,
        Logic::EditorEngine::instance().getSyncBuffer("Preview"));
}

/// @brief 创建共享 Timeline 同步缓冲的时间轴画布。
/// @param name UIManager 使用的稳定视图名。
/// @param width 初始离屏目标宽度。
/// @param height 初始离屏目标高度。
/// @return 由调用方接管的 TimelineCanvas 视图。
///
/// 时间轴拥有独立固定同步键，生命周期随后由 UIManager 管理。
std::unique_ptr<UI::IUIView> CanvasWorkspaceService::createTimelineCanvas(
    const std::string& name, std::uint32_t width, std::uint32_t height)
{
    return std::make_unique<Canvas::TimelineCanvas>(
        name,
        width,
        height,
        Logic::EditorEngine::instance().getSyncBuffer("Timeline"));
}

/// @brief 创建独立的批注表窗口。
/// @param name UIManager 使用的稳定视图名。
/// @return 由调用方接管的 AnnotationTableWindow。
///
/// 批注表不依赖 TimelineCanvas 实例，可在时间轴关闭后继续存在。
std::unique_ptr<UI::IUIView>
CanvasWorkspaceService::createAnnotationTableWindow(const std::string& name)
{
    return std::make_unique<Canvas::AnnotationTableWindow>(name);
}

}  // namespace MMM::Game
