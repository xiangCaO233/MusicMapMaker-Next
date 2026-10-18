#include "ui/imgui/manager/NewBeatmapWizard.h"

#include "config/Utf8Path.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "mmm/project/Project.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace MMM::UI
{
/// @brief 通过友元操作真实向导，不创建谱面或复制资源改名实现。
struct NewBeatmapWizardResourceRenameTestAccess {
    /// @brief 设置确认框新名称，测试输入遵守与 UI 相同的容量限制。
    static void name(NewBeatmapWizard& wizard, const char* value)
    {
        // 输入先清空再复制，短名称不会复用前一个请求缓冲尾部。
        wizard.m_renameNameBuffer.fill('\0');
        std::memcpy(
            wizard.m_renameNameBuffer.data(), value, std::strlen(value));
    }

    /// @brief 验证音轨 ID、图片共享绑定及失败请求的草稿隔离。
    /// @return 三类资源改名均同步向导，且已有测量结果保持时返回 true。
    /// @param wizard 尚未提交创建的真实向导实例。
    /// @param project 已打开的正式项目，只有资源而没有谱面会话。
    /// @details 音频只省略扩展名输入，验证文件类型和资源 ID 仍保持一致。
    /// 图片先从封面入口改名，再从背景入口改名，覆盖两个独立按钮的用途。
    /// 封面和背景共用一个文件，不能只修改被点击的向导字段。
    /// 已有目标和非法路径分别验证失败不改变当前绑定。
    /// 项目上下文变化通过冻结根目录不匹配模拟，不能继续提交陈旧请求。
    /// 所有断言使用真实事务结果，不通过直接修改路径模拟成功。
    static bool run(NewBeatmapWizard& wizard, Project& project)
    {
        wizard.m_selectedAudioPath    = "old.wav";
        wizard.m_selectedAudioTrackId = "old.wav";
        // 封面与背景刻意指向同一文件，能暴露只刷新一行的遗漏。
        wizard.m_selectedCoverImgPath = "cover.png";
        wizard.m_selectedCoverPath    = "cover.png";
        wizard.m_meta.title           = "Unsubmitted title";
        wizard.m_meta.cover_path      = "cover.png";
        wizard.m_meta.main_cover_path = "cover.png";
        wizard.m_bpm                  = 150.0;
        // 测量结果已存在，改名不能触发重新选音频的清空流程。
        wizard.m_measuredTimings.emplace_back();
        // 尚无打开谱面也必须支持改名，不能依赖设置页的活动会话身份。
        wizard.requestResourceRename(NewBeatmapWizard::ResourceTarget::Audio);
        // 音频输入不写扩展名，共享服务必须沿用 wav。
        name(wizard, "song");
        if ( !wizard.renameSelectedResource() ||
             wizard.m_selectedAudioPath != "song.wav" ||
             wizard.m_selectedAudioTrackId != "song.wav" ||
             project.m_audioResources[0].m_id != "song.wav" )
            return false;
        // 未写扩展名时沿用格式，不能误触发重新选音频而清空测量结果。
        if ( wizard.m_measuredTimings.size() != 1 || wizard.m_bpm != 150.0 )
            return false;
        wizard.requestResourceRename(NewBeatmapWizard::ResourceTarget::Cover);
        // 显式保留图片格式，确认视觉事务路径正常工作。
        name(wizard, "art.png");
        if ( !wizard.renameSelectedResource() ||
             wizard.m_selectedCoverImgPath != "art.png" ||
             wizard.m_selectedCoverPath != "art.png" ||
             wizard.m_meta.cover_path != "art.png" ||
             wizard.m_meta.main_cover_path != "art.png" )
            return false;
        // 从背景入口再次改名，同一个图片的封面绑定仍应一起迁移。
        wizard.requestResourceRename(
            NewBeatmapWizard::ResourceTarget::Background);
        name(wizard, "background");
        if ( !wizard.renameSelectedResource() ||
             wizard.m_selectedCoverPath != "background.png" ||
             wizard.m_selectedCoverImgPath != "background.png" )
            return false;
        wizard.requestResourceRename(NewBeatmapWizard::ResourceTarget::Audio);
        // 目标已在夹具目录存在，不能覆盖用户文件。
        name(wizard, "taken.wav");
        // 已存在目标不能覆盖，失败必须保留当前选择和可见原因。
        if ( wizard.renameSelectedResource() ||
             wizard.m_resourceRenameError.empty() ||
             wizard.m_selectedAudioPath != "song.wav" )
            return false;
        // 目录移动不是重命名，服务必须拒绝路径分隔符。
        name(wizard, "../escape.wav");
        if ( wizard.renameSelectedResource() ||
             wizard.m_selectedAudioPath != "song.wav" )
            return false;
        // 模拟确认期间切换项目，冻结请求不能自动跟随新根目录。
        wizard.m_renameProjectRoot /= "another-project";
        name(wizard, "unexpected.wav");
        // 确认框开始后的项目上下文改变，不能继续对新项目执行陈旧请求。
        return !wizard.renameSelectedResource() &&
               // 使用工程相对路径，与向导导入和下拉选择的真实存储一致。
               wizard.m_selectedAudioPath == "song.wav" &&
               // 模拟尚未提交的文字草稿，资源操作不能使其回退为默认值。
               wizard.m_meta.title == "Unsubmitted title" &&
               wizard.m_measuredTimings.size() == 1;
    }
};
}  // namespace MMM::UI

/// @brief 在构建树生成最小资源，验证向导复用真实资源改名事务。
/// @param argc 参数数量，测试输出根由 CMake 明确提供。
/// @param argv 第二个参数为构建树内的绝对输出目录。
/// @return 所有文件与草稿绑定验证通过时返回零。
/// @details 输出根由 CMake 传入，不使用源码当前目录生成资源。
/// 每个测试运行生成唯一子目录，失败现场可保留供检查。
/// 项目先在空目录打开，再登记夹具资源，避免依赖扫描器的音频探测。
/// 夹具文件内容不参与解码，测试聚焦文件改名和项目引用事务。
/// 控制器关闭后向导析构解除事件订阅，不留下下一个场景可见的项目状态。
/// 音频 ID、两张视觉选择、标题和测量 Timing 都由同一向导实例断言。
/// 文件存在性另作检查，不能以 UI 文本变化代替实际文件已改名。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    // 先解码绝对输出路径，禁止以用户当前工作目录作为可写目标。
    const auto base = MMM::Config::utf8ToPath(argv[1]);
    // 缺少明确输出根时直接拒绝测试，避免误写项目源码。
    if ( !base.is_absolute() ) return 1;
    // 每次使用独立目录，不递归清理旧输出或触碰 tests/data 源资源。
    const auto root =
        base / std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if ( error ) return 1;
    MMM::Logic::EditorEngine::instance();
    auto& controller = MMM::Logic::ProjectController::instance();
    // 打开空正式项目，不能靠预先创建的谱面会话满足 UI 入口。
    if ( !controller.openProject(root).m_opened ) return 1;
    // 文件内容不会解码，本次只验证已有资源的文件事务和身份迁移。
    for ( const auto* file : { "old.wav", "cover.png", "taken.wav" } ) {
        std::ofstream output(root / file, std::ios::binary);
        output << "rename fixture";
        if ( !output.good() ) return 1;
    }
    auto* project = controller.currentProject();
    // 用完整项目资源模型验证 ID 改名，而非只改一个路径字符串。
    MMM::AudioResource resource;
    resource.m_id   = "old.wav";
    resource.m_path = "old.wav";
    resource.m_type = MMM::AudioTrackType::Main;
    project->m_audioResources.push_back(resource);
    MMM::UI::NewBeatmapWizard wizard;
    // 草稿与物理文件必须同时验证，选择路径变化不能代替磁盘提交。
    const bool passed = MMM::UI::NewBeatmapWizardResourceRenameTestAccess::run(
                            wizard, *project) &&
                        std::filesystem::exists(root / "song.wav") &&
                        std::filesystem::exists(root / "background.png") &&
                        !std::filesystem::exists(root / "old.wav") &&
                        !std::filesystem::exists(root / "cover.png");
    // 统一关闭项目，失败结果同样不能泄漏到后续测试。
    controller.closeProject();
    // 保留构建树内的夹具供失败诊断，不把测试输出写入源码资源目录。
    return passed ? 0 : 1;
}
