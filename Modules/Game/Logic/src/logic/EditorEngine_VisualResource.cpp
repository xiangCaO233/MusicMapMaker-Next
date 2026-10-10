#include "logic/EditorEngine.h"

#include "logic/BeatmapSession.h"
#include "logic/ProjectController.h"
#include "logic/VisualResourceRenameService.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

namespace MMM::Logic
{

/// @brief 原地重命名谱面图片或视频资源并同步全部打开会话。
/// @details 注册表锁串行化谱面保存与资源提交。磁盘事务先成功，随后每个会话
/// 只迁移自己的元数据路径及历史快照，其他尚未保存的物件、时间线和元数据
/// 均保持原值。磁盘阶段失败时服务恢复文件，会话状态尚未改变。
/// @param oldPath 项目内资源路径，可以是绝对路径或项目相对路径。
/// @param newFileName 单个新文件名，未写扩展名时保留原格式。
/// @return 成功返回空字符串；拒绝或提交失败时返回可展示原因。
/// @note 项目没有独立的图片或视频资源 ID 表，下拉框按目录扫描文件。
/// @note 背景尺寸是此入口唯一需要主动刷新的视觉派生缓存。
/// @note 本入口不保存会话中的其他编辑，已有脏状态保持原状。
/// @note 更名是外部资源操作，不添加到普通谱面 Undo 栈。
/// @note 元数据命令的改名代次由会话记录，避免旧草稿稍后恢复失效路径。
/// @note 封面字段改名不需要背景尺寸探测，背景字段改变才重新计算。
/// @note 同一资源可被多个打开画布引用，因此遍历全部会话发布新路径。
/// @warning 低频文件操作路径：读取并重写受管谱面，禁止从每帧调用。
std::string EditorEngine::renameBeatmapVisualResource(
    const std::filesystem::path& oldPath, const std::string& newFileName)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    // 注册表锁覆盖预检、物理改名和会话发布，逻辑线程不观察半提交状态。
    auto* project = ProjectController::instance().currentProject();
    if ( !project ) return "当前没有可重命名视觉资源的项目";
    if ( project->m_isTemporaryProject )
        return "临时只读项目不能重命名视觉资源";

    auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( const auto& entry : sessions ) {
        // 后台保存的独立快照可能携带旧文件名；future 完成后仍需等待
        // 会话处理保存结果，才能提交资源改名。
        if ( entry.session && entry.session->hasInFlightBeatmapSave() ) {
            // 保存线程持有改名前的独立谱面快照，必须等它完成后才能提交资源改名。
            return "谱面正在后台保存，请稍后重试视觉资源重命名";
        }
    }

    const auto result =
        VisualResourceRenameService::rename(*project, oldPath, newFileName);
    // 服务在失败时负责恢复磁盘；此处尚未改动会话元数据或撤销历史。
    if ( !result.m_errorMessage.empty() ) return result.m_errorMessage;
    if ( result.m_oldPath == result.m_newPath ) return {};

    std::error_code rootError;
    // 服务返回规范化绝对路径，项目根也采用相同基准计算持久化引用。
    const auto projectRoot =
        std::filesystem::weakly_canonical(project->m_projectRoot, rootError);
    // 服务已验证根目录；此处仅用同一规范化路径构造会话中的相对引用。
    const auto& root = rootError ? project->m_projectRoot : projectRoot;
    const auto  oldProjectPath = result.m_oldPath.lexically_relative(root);
    const auto  newProjectPath = result.m_newPath.lexically_relative(root);
    for ( auto& entry : sessions ) {
        // 占位画布和空谱面没有资源字段，只需处理真实打开的谱面会话。
        if ( entry.isLogoPlaceholder || !entry.session ) continue;
        auto& session = *entry.session;
        auto& context = session.getContextMutable();
        if ( !context.currentBeatmap ) continue;

        // 先迁移历史和已排队输入，使下一次 Undo 或命令消费无法恢复旧文件名。
        context.actionStack.remapResourcePaths(oldProjectPath, newProjectPath);
        // 历史谱面可能保存绝对路径，也可能以谱面目录为相对基准。
        // 三种身份都定向迁移，不改写历史中其他背景选择。
        context.actionStack.remapResourcePaths(result.m_oldPath,
                                               result.m_newPath);
        const auto mapDirectory =
            context.currentBeatmap->m_baseMapMetadata.map_path.is_absolute()
                ? context.currentBeatmap->m_baseMapMetadata.map_path
                      .parent_path()
                : root / context.currentBeatmap->m_baseMapMetadata.map_path
                             .parent_path();
        context.actionStack.remapResourcePaths(
            result.m_oldPath.lexically_relative(mapDirectory),
            result.m_newPath.lexically_relative(mapDirectory));
        session.recordResourceRename(oldProjectPath, newProjectPath);
        // 改名前已入队的元数据草稿稍后消费时，必须看见新路径身份。

        auto&      metadata = context.currentBeatmap->m_baseMapMetadata;
        const auto previousBackground = metadata.main_cover_path;
        // 只替换确实指向源文件的字段，未保存谱面的其他编辑保持原值。
        if ( VisualResourceRenameService::remapMetadata(
                 metadata, root, result.m_oldPath, result.m_newPath) &&
             previousBackground != metadata.main_cover_path ) {
            // 只在背景路径改变时刷新尺寸；cover_type 保留，视频仍走视频探针。
            SessionUtils::updateBackgroundSize(context, metadata, project);
            // 图片和视频继续由原有 cover_type 选择各自的尺寸探针。
        }
    }
    return {};
}

}  // namespace MMM::Logic
