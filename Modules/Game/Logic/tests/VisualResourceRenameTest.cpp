/// @file
/// @brief 验证图片与视频文件改名、谱面引用提交和失败时的数据保护。
/// 测试只操作构建树中的独立项目目录，不读取或改写源码资源。
/// 两张谱面同时引用同一图片，覆盖跨谱面同步和同图双字段引用。
/// 视频夹具只验证文件身份、路径和类型语义，不要求媒体可解码。
/// 冲突场景在正式文件修改前触发，验证错误路径不污染项目。
#include "logic/VisualResourceRenameService.h"

#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace
{

/// @brief 在测试项目内写入仅供文件身份验证的资源字节。
/// @param path 构建树中的资源路径。
/// @return 父目录创建且文件完整写入时返回 true。
/// @note 字节用于判定文件身份，不参与图片或视频解码。
/// @note 避免把任何测试输出写进 Git LFS 管理的源码资源目录。
bool writeResource(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if ( error ) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "visual-resource-fixture";
    return output.good();
}

/// @brief 保存一张含有指定视觉资源字段的原生谱面。
/// @param path 项目谱面路径。
/// @param background 项目相对背景路径。
/// @param cover 项目相对封面路径。
/// @param type 背景媒体类型。
/// @return 原生谱面成功保存时返回 true。
/// @note 先由正式保存器产生有效谱面，再由改名服务读取它。
/// @note charts 与 media 是不同目录，用来验证项目相对引用。
/// @note 名称和版本是非资源字段，回读时可发现误覆盖。
bool writeBeatmap(const std::filesystem::path& path,
                  const std::filesystem::path& background,
                  const std::filesystem::path& cover, MMM::CoverType type)
{
    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.name            = "Visual rename fixture";
    beatmap.m_baseMapMetadata.version         = "Hard";
    beatmap.m_baseMapMetadata.map_path        = path;
    beatmap.m_baseMapMetadata.main_cover_path = background;
    beatmap.m_baseMapMetadata.cover_path      = cover;
    beatmap.m_baseMapMetadata.cover_type      = type;
    // 其余字段保持默认值，用名称和版本在回读时检查非资源数据未丢失。
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    return !error && beatmap.saveToFile(path);
}

/// @brief 构建只登记本测试生成谱面的项目资源表。
/// @param root 独立测试项目根目录。
/// @return 含两个受管谱面入口的正式项目。
/// @note 项目没有单独的视觉资源列表，谱面入口决定扫描范围。
/// @note 夹具不依赖目录扫描服务，保持测试焦点在改名事务。
MMM::Project makeProject(const std::filesystem::path& root)
{
    MMM::Project project;
    project.m_projectRoot = root;
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "one", "charts/one.mmm", "" });
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "two", "charts/two.mmm", "" });
    return project;
}

/// @brief 检查图片改名同步两个谱面中的封面和背景引用。
/// @param root 独立测试项目根目录。
/// @return 文件和谱面元数据均符合预期时返回 true。
/// @details
/// 第一张谱面同时以封面和背景引用 art.png，第二张只用它作背景。
/// 两张谱面位于 charts，图片位于 media，保存后仍须保留 media 前缀。
/// 第二张谱面的独立封面不能因为共享背景被替换。
/// 还检查旧实体消失、新实体存在，排除只改谱面文本的假成功。
/// @note 测试从源文件加载新谱面，防止错误地断言夹具内存值。
/// @note 不要求资源字节可解析，改名职责不包含媒体解码。
bool testImageRename(const std::filesystem::path& root)
{
    const auto oldPath = root / "media/art.png";
    const auto onePath = root / "charts/one.mmm";
    const auto twoPath = root / "charts/two.mmm";
    if ( !writeResource(oldPath) ||
         !writeBeatmap(onePath,
                       "media/art.png",
                       "media/art.png",
                       MMM::CoverType::IMAGE) ||
         !writeBeatmap(twoPath,
                       "media/art.png",
                       "media/other.jpg",
                       MMM::CoverType::IMAGE) ) {
        XERROR("Cannot create image rename test project");
        return false;
    }
    // 服务接收正式项目对象，与 EditorEngine 的实际委托链相同。
    const auto project = makeProject(root);
    const auto result  = MMM::Logic::VisualResourceRenameService::rename(
        project, oldPath, "renamed.png");
    if ( !result.m_errorMessage.empty() ) {
        XERROR("Image rename failed: {}", result.m_errorMessage);
        return false;
    }
    // 回读正式谱面，验证物理文件与持久化文本均已提交。
    const auto      one = MMM::BeatMap::loadFromFile(onePath);
    const auto      two = MMM::BeatMap::loadFromFile(twoPath);
    std::error_code error;
    // 同图同时作为封面与背景时两个字段都改；另一张谱面的独立封面保持原值。
    return !std::filesystem::exists(oldPath, error) && !error &&
           std::filesystem::exists(root / "media/renamed.png", error) &&
           !error &&
           one.m_baseMapMetadata.main_cover_path == "media/renamed.png" &&
           one.m_baseMapMetadata.cover_path == "media/renamed.png" &&
           two.m_baseMapMetadata.main_cover_path == "media/renamed.png" &&
           two.m_baseMapMetadata.cover_path == "media/other.jpg" &&
           one.m_baseMapMetadata.name == "Visual rename fixture";
}

/// @brief 检查视频背景改名保留媒体类型和独立图片封面。
/// @param root 独立测试项目根目录。
/// @return 未指定新扩展名时仍使用原视频扩展名且引用已更新。
/// @details
/// 背景使用 UI 白名单中的 webm 容器，封面保留已改名的 png 图片。
/// 调用时仅给基础名，服务须复用 .webm 扩展名而不触碰媒体字节。
/// cover_type 是用户选择的视频语义，不能因文件重命名而变成图片。
/// @note 独立封面的路径检查可发现视频背景改名误改其他媒体字段。
/// @note 保存器回读后的类型断言覆盖持久化语义。
bool testVideoRename(const std::filesystem::path& root)
{
    const auto oldPath = root / "media/motion.webm";
    const auto mapPath = root / "charts/video.mmm";
    if ( !writeResource(oldPath) || !writeBeatmap(mapPath,
                                                  "media/motion.webm",
                                                  "media/renamed.png",
                                                  MMM::CoverType::VIDEO) ) {
        XERROR("Cannot create video rename test project");
        return false;
    }
    // 只登记自己的视频谱面，不重复扫描图片场景产生的候选。
    MMM::Project project;
    project.m_projectRoot = root;
    project.m_beatmaps.push_back(
        MMM::Project::BeatmapEntry{ "video", "charts/video.mmm", "" });
    const auto result = MMM::Logic::VisualResourceRenameService::rename(
        project, oldPath, "motion-new");
    // 读取正式谱面，确保更新实际保存而不是只改变返回对象。
    const auto loaded = MMM::BeatMap::loadFromFile(mapPath);
    // 服务仅重映射路径；封面图片和 VIDEO 语义绝不能随文件改名漂移。
    return result.m_errorMessage.empty() &&
           result.m_newPath == root / "media/motion-new.webm" &&
           loaded.m_baseMapMetadata.main_cover_path ==
               "media/motion-new.webm" &&
           loaded.m_baseMapMetadata.cover_path == "media/renamed.png" &&
           loaded.m_baseMapMetadata.cover_type == MMM::CoverType::VIDEO;
}

/// @brief 验证目标冲突和暂存冲突均在修改任何正式文件前拒绝。
/// @param root 已完成图片改名的项目目录。
/// @return 已有资源、谱面和原始字节仍保持不变。
/// @details
/// 目标已存在时禁止覆盖；改变扩展名不代表转码，必须拒绝。
/// 带 ../ 的输入不能移动资源目录；临时项目始终只读。
/// 最后占用固定暂存路径，模拟上次异常中断留下恢复证据。
/// 服务应拒绝覆盖该证据，同时保持资源实体和谱面引用。
bool testFailureProtection(const std::filesystem::path& root)
{
    const auto source  = root / "media/renamed.png";
    const auto mapPath = root / "charts/one.mmm";
    const auto project = makeProject(root);
    if ( !writeResource(root / "media/taken.png") ) return false;
    const auto occupied = MMM::Logic::VisualResourceRenameService::rename(
        project, source, "taken.png");
    // 前四项无副作用拒绝均不能移走后续暂存冲突需要的源文件。
    const auto wrongExtension = MMM::Logic::VisualResourceRenameService::rename(
        project, source, "converted.jpg");
    const auto invalidName = MMM::Logic::VisualResourceRenameService::rename(
        project, source, "../escaped.png");
    auto readOnlyProject                 = project;
    readOnlyProject.m_isTemporaryProject = true;
    const auto readOnly = MMM::Logic::VisualResourceRenameService::rename(
        readOnlyProject, source, "blocked.png");

    // 预先占用该谱面的暂存路径，验证事务不覆盖上次操作可能留下的证据。
    // 不依赖权限失败模拟回滚，避免平台和运行身份改变测试结果。
    const auto staged = root / "charts/one.mmm.mmm-visual-rename.mmm";
    if ( !writeResource(staged) ) return false;
    const auto blocked = MMM::Logic::VisualResourceRenameService::rename(
        project, source, "blocked.png");
    // 核对正式文件、既有旁路和谱面字段，不仅凭错误字符串判断安全性。
    const auto      loaded = MMM::BeatMap::loadFromFile(mapPath);
    std::error_code error;
    return !occupied.m_errorMessage.empty() &&
           !wrongExtension.m_errorMessage.empty() &&
           !invalidName.m_errorMessage.empty() &&
           !readOnly.m_errorMessage.empty() &&
           !blocked.m_errorMessage.empty() &&
           std::filesystem::exists(source, error) && !error &&
           std::filesystem::exists(staged, error) && !error &&
           loaded.m_baseMapMetadata.cover_path == "media/renamed.png";
}

/// @brief 给四个持久媒体字段放入同一路径，便于逐字段验证身份迁移。
/// @param metadata 要更新的谱面元数据。
/// @param path 测试中的媒体文件身份。
/// @note 实际谱面可以分别选择不同文件；共用值使遗漏任何字段立即可见。
void setResourceFields(MMM::BaseMapMeta&            metadata,
                       const std::filesystem::path& path)
{
    metadata.main_audio_path = path;
    metadata.song_file_hint  = path;
    metadata.cover_path      = path;
    metadata.main_cover_path = path;
}

/// @brief 检查音频、歌曲提示、封面、背景均指向指定资源。
/// @param metadata 待验证的会话元数据。
/// @param path 期望资源身份。
/// @return 四个路径字段都符合预期时为 true。
bool hasResourceFields(const MMM::BaseMapMeta&      metadata,
                       const std::filesystem::path& path)
{
    return metadata.main_audio_path == path &&
           metadata.song_file_hint == path && metadata.cover_path == path &&
           metadata.main_cover_path == path;
}

/// @brief 验证元数据命令只迁移入队后发生的资源改名。
/// @return 旧草稿被迁移而新选择的同名文件不被旧别名覆盖时为 true。
/// @details 代次在生产者入队时记录，消费路径不做文件系统身份探测。
/// @note 第二条命令故意选择旧名字，模拟改名后新增的同名资源。
/// @note 此场景同时覆盖音频和视觉资源共用的四个元数据字段。
/// @note 不创建真实媒体，避免文件是否存在掩盖代次判断的正确性。
/// @note 使用真实 BeatmapSession 命令队列，而不是直接调用处理器。
/// @note 首条输入在资源改名前进入队列，用于模拟 UI 草稿滞后。
/// @note 资源改名在下一轮 update 前发布，代表低频提交已完成。
/// @note 第二条输入在新代次下产生，路径文本相同但身份已不同。
bool testQueuedMetadataGeneration()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    auto                       beatmap = std::make_shared<MMM::BeatMap>();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(beatmap) } });
    session.update(0.0, config, false);
    if ( !session.getContext().currentBeatmap ) return false;

    // 简化相对路径与项目存盘路径同形，但本测试不依赖真实项目对象。
    constexpr auto oldPath = "media/old.png";
    constexpr auto newPath = "media/new.png";
    auto oldDraft = session.getContext().currentBeatmap->m_baseMapMetadata;
    setResourceFields(oldDraft, oldPath);
    // 旧草稿已排队，但物理改名先于下一轮逻辑消费完成。
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateBeatmapMetadata{ .baseMeta = oldDraft } });
    // 改名提交发生在命令入队之后，这一先后关系是回归核心。
    session.recordResourceRename(oldPath, newPath);
    session.update(0.0, config, false);
    // 四字段任一遗漏都会使断言失败，尤其包括旧音轨与歌曲提示。
    if ( !hasResourceFields(
             session.getContext().currentBeatmap->m_baseMapMetadata,
             newPath) ) {
        XERROR("Queued metadata kept stale resource paths");
        return false;
    }

    // 新命令持有新的代次；虽然文本碰巧等于旧文件名，不能强行映射它。
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateBeatmapMetadata{ .baseMeta = oldDraft } });
    session.update(0.0, config, false);
    // 新资源重用旧名字并不表示要回到已改名的上一代资源。
    return hasResourceFields(
        session.getContext().currentBeatmap->m_baseMapMetadata, oldPath);
}

/// @brief 验证真实元数据替换动作的两个历史方向都迁移媒体引用。
/// @return Undo 与 Redo 都不会恢复改名前资源身份时为 true。
/// @details 替换动作的 redo 原本清空遗留 main_audio_path，其余三字段保留。
/// @note 先创建正式动作，再按引擎改名顺序迁移历史快照和当前元数据。
/// @note 不额外建立专用测试动作，直接覆盖现有动作栈执行链。
/// @note 来源谱面只改变名称，避免其他领域变化模糊资源字段断言。
/// @note 背景和封面共用旧路径，分别检验两个快照的映射。
/// @note 动作执行前后的元数据不同，Undo 确实走 before 快照。
/// @note 再次执行 Redo 确实走 after 快照，不仅检查栈大小。
/// @note 改名自身不是撤销动作，不应给历史栈额外增加一步。
bool testReplaceMetadataHistory()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    auto                       initial = std::make_shared<MMM::BeatMap>();
    constexpr auto             oldPath = "media/old.png";
    constexpr auto             newPath = "media/new.png";
    setResourceFields(initial->m_baseMapMetadata, oldPath);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(initial) } });
    session.update(0.0, config, false);
    if ( !session.getContext().currentBeatmap ) return false;

    // 采用现有用户操作生成真正的 ReplaceBeatmapDataAction 历史快照。
    auto replacement                    = std::make_shared<MMM::BeatMap>();
    replacement->m_baseMapMetadata.name = "Imported metadata";
    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdReplaceBeatmapData{
            .sourceBeatmap   = std::move(replacement),
            .replaceMetadata = true } });
    session.update(0.0, config, false);
    auto& context = session.getContextMutable();
    if ( context.actionStack.getUndoStackSize() != 1U ||
         context.currentBeatmap->m_baseMapMetadata.name !=
             "Imported metadata" ) {
        XERROR("Metadata replacement did not enter undo history");
        return false;
    }

    // 模拟成功改名后的内存发布：历史快照与当前元数据均变成新身份。
    context.actionStack.remapResourcePaths(oldPath, newPath);
    setResourceFields(context.currentBeatmap->m_baseMapMetadata, newPath);
    // 如果 before 快照仍持有旧名，撤销后首先暴露四字段回退。
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdUndo{} });
    session.update(0.0, config, false);
    if ( !hasResourceFields(context.currentBeatmap->m_baseMapMetadata,
                            newPath) ) {
        XERROR("Undo restored stale resource paths");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdRedo{} });
    session.update(0.0, config, false);
    const auto& redone = context.currentBeatmap->m_baseMapMetadata;
    // 重新应用后读取真实谱面，不直接窥视动作的私有快照。
    // ReplaceBeatmapDataAction 的既有语义清空旧单音轨字段，不是改名丢失。
    return redone.main_audio_path.empty() && redone.song_file_hint == newPath &&
           redone.cover_path == newPath && redone.main_cover_path == newPath;
}

}  // namespace

/// @brief 在构建树输出目录执行全部视觉资源重命名回归场景。
/// @param argc 参数数量。
/// @param argv 第二项为独立测试输出目录。
/// @return 全部断言通过时为零。
/// @note 每次运行只清理调用方明确传入的测试专属目录。
/// @note 本用例验证资源事务，完整工程构建由外层流程负责。
int main(int argc, char** argv)
{
    if ( argc != 2 ) {
        XERROR("VisualResourceRenameTest requires an output directory");
        return 1;
    }
    const auto      root = MMM::Config::utf8ToPath(argv[1]);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if ( error ) {
        XERROR("Cannot prepare visual rename test directory: {}",
               error.message());
        return 1;
    }
    // 场景串行共享图片改名结果，视频使用独立路径，不依赖测试运行次序。
    const bool passed = testImageRename(root) && testVideoRename(root) &&
                        testFailureProtection(root) &&
                        testQueuedMetadataGeneration() &&
                        testReplaceMetadataHistory();
    if ( !passed ) {
        XERROR("Visual resource rename regression failed");
        return 1;
    }
    return 0;
}
