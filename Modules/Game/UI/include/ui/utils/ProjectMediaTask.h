#pragma once

#include "common/AudioInfoUtils.h"
#include "logic/ProjectController.h"
#include "runtime/AppThreadPool.h"
#include "ui/utils/ProjectResourceImport.h"

#include <cstdint>
#include <filesystem>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace MMM::UI::Utils
{

/// @brief 在文件线程完成的导入结果；相对路径按项目根解释。
/// @details 成功的路径可能指向已有文件，因此结果调用方不得在丢弃时删除路径。
/// 远端工作在本地授权副本上进行，结果仍只包含原工程的相对路径。
/// 读写均结束后发布结果，界面只把路径及标签绑定到当前草稿。
struct ProjectMediaImportResult {
    /// @brief 已存在或已经复制完成的项目相对文件路径。
    std::filesystem::path m_path;
    /// @brief 单次探测的标签和时长，向导不必再次访问文件。
    std::optional<::MMM::Utils::AudioInfo> m_audio;
    /// @brief 权限、复制或媒体探测错误。
    /// @note 错误非空时结果不包含可写回的文件名。
    /// operation_canceled 表示打开工程或窗口身份在开始复制前已无效。
    std::error_code m_error;
};

/// @brief 只把路径与媒体类别交给既有文件池，工作者不借用会话或界面。
/// @param root 入队时的可写工程根目录，后台不从活动会话读取。
/// @param source 文件对话框或拖放已提供的本地源路径。
/// @param audio 需要先验证可解码，并将探测出的标签随结果传回。
/// @param generation 工程每次打开时发布的身份，同目录重新打开也不重用。
/// @param lifetime 发起窗口持有的单次令牌，不延长窗口或草稿的寿命。
/// @return 文件线程不可用时返回空 future，由界面统一显示失败。
/// @details 音频首先以源文件验证其有效性，避免把无法读取的文件复制进工程。
/// 图片和视频沿用原有文件分类和复制规则，不提前解码或分配帧缓冲。
/// 无法中断已经交给系统的复制，因此完成后的文件属于当时确认的工程。
/// 最终路由再次检查原工程与谱面实例，不能用文件名替代身份确认。
/// @note 最终完成结果只能写入发起时的状态，调用方复核工程代次及会话。
/// 导入完成后的文件属于原工程，跨工程时不会被重新绑定或删除其他选择。
inline std::future<std::shared_ptr<ProjectMediaImportResult>>
queueProjectMediaImport(std::filesystem::path root,
                        std::filesystem::path source, bool audio,
                        std::uint64_t             generation,
                        std::weak_ptr<const bool> lifetime)
{
    // 使用独立文件池，通用池负责编码之外的逻辑与渲染准备任务。
    // 任务状态由 packaged_task 持有；未来值失效不会阻塞窗口关闭。
    auto* worker = Runtime::AppThreadPool::instance().getFileThreadPool();
    if ( !worker ) return {};
    return worker->enqueue([root   = std::move(root),
                            source = std::move(source),
                            audio,
                            generation,
                            lifetime = std::move(lifetime)] {
        // 工作线程只借用不可变项目快照。不能请求会话锁来判断项目是否已切换。
        // 检查弱令牌而不提升为共享所有权，使关闭窗口能在探测期间立即失效。
        const auto stillCurrent = [&] {
            const auto project =
                Logic::ProjectController::instance().uiSnapshot();
            return !lifetime.expired() && project &&
                   project->m_generation == generation &&
                   !project->m_isTemporaryProject;
        };
        auto result = std::make_shared<ProjectMediaImportResult>();
        if ( !stillCurrent() ) {
            result->m_error =
                std::make_error_code(std::errc::operation_canceled);
            return result;
        }
        // 探测失败前不写任何文件；标签和时长随复制结果一并交付。
        if ( audio ) {
            result->m_audio =
                ::MMM::Utils::AudioInfoUtils::probeAudioInfo(source);
            if ( !result->m_audio ) {
                result->m_error =
                    std::make_error_code(std::errc::invalid_argument);
                return result;
            }
        }
        // 慢速探测后再复核一次，关闭或切换时不开始复制。
        if ( !stillCurrent() ) {
            result->m_error =
                std::make_error_code(std::errc::operation_canceled);
            return result;
        }
        // 既有辅助函数保证普通文件检查、冲突后缀和配套标记的成对复制。
        // 如果输入本就在工程内，该函数仅返回相对表示，不再复制媒体。
        auto path = importProjectResource(root, source);
        if ( path )
            result->m_path = std::move(*path);
        else
            result->m_error = path.error();
        return result;
    });
}

}  // namespace MMM::UI::Utils
