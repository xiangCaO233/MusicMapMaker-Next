#include "canvas/CanvasTabTitle.h"
#include "canvas/Basic2DCanvas.h"
#include "common/render/RenderSnapshot.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/IParallelUiPreparable.h"

#include <memory>
#include <string>

namespace MMM::Canvas
{
/// @brief 对隐藏编辑器使用真实快照池验证标题状态刷新，不创建 GPU 或 ImGui
/// 窗口。
struct CanvasTabSnapshotTestAccess {
    /// @brief 验证后台标签消费替换谱面及关闭谱面的状态而不改变可见性。
    /// @return 两次发布都被调度器消费且保持后台状态时为 true。
    /// @note 标题、dirty 与 hasBeatmap 来自同一代快照，不能部分保留旧值。
    /// 发布与消费在此串行模拟，但调用入口与运行时线程边界一致。
    /// 不模拟点击标签；刷新必须在可见性始终为 false 时完成。
    static bool checkHiddenSnapshotRefresh()
    {
        auto buffer = std::make_shared<Common::Render::RenderSnapshotBuffer>();
        // 构造阶段不初始化离屏目标，测试不能依赖图形设备可用性。
        // 同步缓冲由测试持有，确保整轮读取槽生命周期稳定。
        Basic2DCanvas canvas("TitleSnapshotTest", 1, 1, buffer);
        canvas.m_isCanvasVisible = false;
        UI::UiFrameSnapshot frame;
        // 没有音频分析窗口或焦点请求，隐藏标签也必须参与轻量准备。
        // 空音频分析 ID 是关键前提，旧调度只为可见或分析目标准备快照。
        // 若人为设置相同 cameraId，会掩盖后台普通标签不刷新的回归。
        const auto consume = [&]() {
            if ( !canvas.needsParallelUiPrepare(frame) ) return false;
            canvas.prepareUiFrameData(frame);
            canvas.swapPreparedUiFrameData();
            // isDirty 控制离屏绘制，标题刷新不能反向恢复后台渲染。
            return !canvas.isDirty() && !canvas.m_isCanvasVisible;
        };
        // 首份旧项目状态用于建立真实读取基线，而不是直接写画布成员。
        // 旧值刻意标脏，后续项目替换必须同时清除这一标志。
        auto* initial = buffer->getWorkingSnapshot();
        if ( !initial ) return false;
        initial->hasBeatmap  = true;
        initial->beatmapName = "Old project";
        initial->isDirty     = true;
        buffer->pushWorkingSnapshot();
        if ( !consume() ) return false;
        // 新项目快照必须替换整个读取槽；旧谱名与脏标志不能残留。
        // 写入仍通过生产者入口，能覆盖准备调度遗漏导致的队列积压。
        // 消费者不调用引擎锁或直接读取 SessionContext。
        auto* replacement = buffer->getWorkingSnapshot();
        if ( !replacement ) return false;
        replacement->hasBeatmap  = true;
        replacement->beatmapName = "New project";
        replacement->isDirty     = false;
        buffer->pushWorkingSnapshot();
        if ( !consume() || !canvas.m_currentSnapshot ||
             canvas.m_currentSnapshot->beatmapName != "New project" ||
             canvas.m_currentSnapshot->isDirty )
            return false;
        // 同一谱面内部名称变化仍发布完整元数据，不能只在切项目时刷新。
        // 重命名后标脏模拟实际元数据命令，不要求标签取得任何焦点。
        // 断言最终完整标题，同时覆盖名称刷新与前置星号的组合。
        // 仅检查快照指针变化不足以保证标签使用了正确的业务字段。
        // 此处仍保持原画布对象，不借重建窗口偶然获得正确名称。
        auto* renamed = buffer->getWorkingSnapshot();
        if ( !renamed ) return false;
        renamed->hasBeatmap  = true;
        renamed->beatmapName = "Renamed map";
        renamed->isDirty     = true;
        buffer->pushWorkingSnapshot();
        if ( !consume() ||
             makeCanvasTabTitle("Editor",
                                canvas.m_currentSnapshot->hasBeatmap,
                                canvas.m_currentSnapshot->beatmapName,
                                canvas.m_currentSnapshot->isDirty) !=
                 "* Renamed map" )
            return false;
        // 关闭项目后仍保留编辑器窗口，空谱面快照不能停留在后台队列中。
        // clear 同时重置谱名与 dirty，不从上次工作槽继承测试赋值。
        // 此场景与生产端关闭后发布无谱面快照使用相同数据契约。
        auto* closed = buffer->getWorkingSnapshot();
        if ( !closed ) return false;
        closed->clear();
        buffer->pushWorkingSnapshot();
        // 先确认准备与交换仍执行，再验证标题回退，不直接断言窗口焦点。
        // 空谱面回退不得泄漏上一项目名称或重命名后的脏标志。
        return consume() && canvas.m_currentSnapshot &&
               !canvas.m_currentSnapshot->hasBeatmap &&
               makeCanvasTabTitle("Editor",
                                  canvas.m_currentSnapshot->hasBeatmap,
                                  canvas.m_currentSnapshot->beatmapName,
                                  canvas.m_currentSnapshot->isDirty) ==
                   "Editor";
    }
};
}  // namespace MMM::Canvas

namespace
{

/// @brief 验证恢复稳定画布 ID 后，无窗口尺寸事件也能发布谱名。
/// @return 新建及占位复用两种路径均立即发布主画布元数据时为 true。
/// @note 不创建窗口，不投递 Resize，模拟欢迎页保持前台的恢复过程。
/// @note 固定 ID 对应工作区保存身份，但谱名必须取自本次载入的元数据。
bool testRestoredHiddenSessionPublishesTitle()
{
    auto& engine = MMM::Logic::EditorEngine::instance();
    // 清理单例条目，防止之前的缓存相机意外掩盖初始化缺口。
    while ( engine.getSessionCount() > 0 ) {
        engine.closeSession(engine.getSessionCount() - 1, false);
    }
    const auto check = [&](bool reusePlaceholder) {
        const std::string cameraId =
            reusePlaceholder ? "HiddenRestoreReuse" : "HiddenRestoreNew";
        // 工作区恢复使用固定 ID；两条路径都没有任何已登记的视口尺寸。
        // 占位从未显示或更新，复用不能假定此前已建立主相机。
        if ( reusePlaceholder ) {
            engine.createSession(nullptr, "Editor", true, cameraId, true);
        }
        auto map                    = std::make_shared<MMM::BeatMap>();
        map->m_baseMapMetadata.name = "Restored hidden beatmap";
        engine.createSession(
            map, map->m_baseMapMetadata.name, false, cameraId, true);
        auto session = engine.getActiveSession();
        // 创建失败不能用空缓冲的回退标题当作成功，先确认真实会话存在。
        if ( !session ) return false;
        // 只推进正常逻辑更新，不借读取 SessionContext 伪造 UI 标题。
        // 生产者必须自行为隐藏主画布生成第一份真实快照。
        session->update(0.0, engine.getEditorConfig(), true);
        auto        buffer   = engine.getSyncBuffer(cameraId);
        const auto* snapshot = buffer->pullLatestSnapshot();
        // 只读取主画布专属缓冲；共享 Preview 有数据并不代表编辑器可刷新。
        // 同时检查 hasBeatmap，避免旧占位或空工作槽偶然保留同名字符串。
        const bool matches =
            snapshot && snapshot->hasBeatmap &&
            snapshot->beatmapName == map->m_baseMapMetadata.name;
        // 每条路径独立清理，避免缓存尺寸或占位复用改变下一次创建条件。
        engine.closeSession(engine.getSessionCount() - 1, false);
        return matches;
    };
    // 不共享初始化条件：分别覆盖新会话及既有占位复用的命令投递分支。
    return check(false) && check(true);
}

/// @brief 验证长标题的脏标志固定在最前方，不会成为尾部省略内容。
/// @return 标题保留前置脏标志且不追加尾部标志时返回 true。
bool testDirtyLongTitleKeepsLeadingMarker()
{
    // 使用足够长的真实风格名称，模拟停靠标签发生尾部截断的场景。
    const std::string beatmapName = "[mc] Monochrome City [4] 4K Inferno Lv.10";
    const std::string title       = MMM::Canvas::makeCanvasTabTitle(
        "canvas.editor", true, beatmapName, true);
    // 同时验证完整字符串、前缀和不存在旧版尾缀三项契约。
    return title == "* " + beatmapName && title.starts_with("* ") &&
           !title.ends_with(" *");
}

/// @brief 验证未修改谱面不显示脏标志。
/// @return 标题与谱面名称完全一致时返回 true。
bool testCleanTitleHasNoMarker()
{
    // hasBeatmap=true 且 isDirty=false 时不得引入任何装饰字符。
    return MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", true, "Clean Beatmap", false) ==
           "Clean Beatmap";
}

/// @brief 验证未命名脏谱面仍使用回退标题并保留前置标志。
/// @return 回退标题前存在脏标志时返回 true。
bool testDirtyUnnamedBeatmapKeepsMarker()
{
    // 空谱面名回退到稳定标题，但仍属于有谱面的脏会话。
    return MMM::Canvas::makeCanvasTabTitle("canvas.editor", true, "", true) ==
           "* canvas.editor";
}

/// @brief 验证欢迎占位标签不会继承无效的脏状态。
/// @return 不含谱面时只显示回退标题。
bool testPlaceholderIgnoresDirtyState()
{
    // hasBeatmap=false 的占位页不采信外部残留的 isDirty 标志。
    return MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", false, "Old map", true) == "canvas.editor";
}

/// @brief 验证协作状态位于谱面名称前，脏标志仍固定在整个标签最前方。
/// @return 在线与离线标签均按预期组合时返回 true。
///
/// 两个状态标签使用不同脏状态，覆盖协作前缀与脏前缀的组合顺序。
bool testCollaborationStatusPrecedesBeatmapName()
{
    // 在线状态不带脏标志，离线状态与脏标志同时存在以验证前缀次序。
    return MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", true, "Online Map", false, "(在线)") ==
               "(在线) Online Map" &&
           MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", true, "Offline Map", true, "(离线)") ==
               "* (离线) Offline Map";
}

/// @brief 验证协作画布在欢迎页复用、断线和关闭过程中的标记生命周期。
/// @return 远端谱面正确标记、断线保留且回到欢迎页后清除时返回 true。
///
/// 状态输入模拟房间启动、Session 复用、断线和额外本地谱面四类来源。
bool testCollaborationCanvasStateLifecycle()
{
    // 回到欢迎占位页后必须清除历史协作标记。
    const bool offlinePlaceholder =
        MMM::Canvas::resolveCollaborationCanvasState(
            true, true, false, false, false, true);
    const bool joiningPlaceholder =
        // 房间正在加入但仍是占位页时不能提前显示协作标签。
        MMM::Canvas::resolveCollaborationCanvasState(
            false, true, true, true, false, true);
    const bool joinedThroughReusedPlaceholder =
        // 同一 Session 从欢迎页复用为远端谱面时建立协作标记。
        MMM::Canvas::resolveCollaborationCanvasState(
            false, false, true, true, true, true);
    const bool joinedDirectly = MMM::Canvas::resolveCollaborationCanvasState(
        // 非复用路径在房间生命周期首次激活时也应建立标记。
        false,
        false,
        false,
        true,
        false,
        true);
    const bool disconnectedBeatmap =
        // 房间断开后真实谱面仍保留协作来源，供 UI 展示离线状态。
        MMM::Canvas::resolveCollaborationCanvasState(
            true, false, false, false, true, true);
    const bool inactiveOfflineBeatmap =
        // 非活动画布同样保留已有标记，不依赖当前焦点。
        MMM::Canvas::resolveCollaborationCanvasState(
            true, false, false, false, true, false);
    const bool additionalLocalBeatmap =
        // 房间稳定期间新建的本地谱面不能被误标为协作画布。
        MMM::Canvas::resolveCollaborationCanvasState(
            false, false, false, false, false, true);
    // 七种生命周期状态共同约束建立、保留和清除三个方向。
    return !offlinePlaceholder && !joiningPlaceholder &&
           joinedThroughReusedPlaceholder && joinedDirectly &&
           disconnectedBeatmap && inactiveOfflineBeatmap &&
           !additionalLocalBeatmap;
}

}  // namespace

/// @brief 覆盖主画布标签的标题和脏标志布局规则。
/// @return 所有标签规则满足时返回 0。
///
/// 这里只检查可见标题；固定 ImGui ID 由实际窗口调用方负责。
int main()
{
    // 标题文本与协作状态归约分别测试，再组合为统一退出码。
    return testDirtyLongTitleKeepsLeadingMarker() &&
                   testCleanTitleHasNoMarker() &&
                   testDirtyUnnamedBeatmapKeepsMarker() &&
                   testPlaceholderIgnoresDirtyState() &&
                   testCollaborationStatusPrecedesBeatmapName() &&
                   testCollaborationCanvasStateLifecycle() &&
                   testRestoredHiddenSessionPublishesTitle() &&
                   MMM::Canvas::CanvasTabSnapshotTestAccess::
                       checkHiddenSnapshotRefresh()
               // 任一标题或生命周期规则失败都返回非零。
               ? 0
               : 1;
}
