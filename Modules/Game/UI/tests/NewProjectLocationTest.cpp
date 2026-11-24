#include "ui/imgui/manager/NewProjectWizard.h"

#include "logic/ProjectStorage.h"
#include "runtime/AppThreadPool.h"
#include <ice/thread/ThreadPool.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#if !defined(_WIN32)
#    include <unistd.h>
#endif

namespace MMM::UI
{
/// @brief 驱动向导实际路径探测，避免用窗口像素判断文件查询结果。
struct NewProjectLocationTestAccess {
    /// @brief 测试直接填写公开界面的等价字段，不修改探测结果。
    /// @param wizard 用正常构造创建的测试向导。
    /// @param parent 构建树内的父目录，未与用户设置共用。
    /// @param folder 模拟输入控件产生的文件夹名。
    /// @details 按 UI 的先写控件再更新验证顺序驱动同一个入口。
    static void setInput(NewProjectWizard&            wizard,
                         const std::filesystem::path& parent,
                         const char*                  folder)
    {
        // 位置步骤走生产页脚的可用性门禁，不受前两页的默认设置影响。
        wizard.m_currentStep     = NewProjectWizard::Step::Location;
        wizard.m_parentDirectory = parent;
        // 使用实际的固定缓冲复制约定，某一次过短写入不能意外使用旧名称。
        wizard.copyToBuffer(
            wizard.m_folderNameBuf, sizeof(wizard.m_folderNameBuf), folder);
        wizard.updateLocationValidation();
    }
    /// @brief 等待一轮测试查询结束，超时视为夹具或线程调度失败。
    /// @note 等待只存在测试，不进入生产 UI 路径。
    /// @param wizard 当前正在消费结果的向导。
    /// @return 当前输入的可用、无效或已占用结论到达为真。
    /// @details 上限只覆盖测试里的线程调度，不以定时器实现生产确认。
    static bool awaitResult(NewProjectWizard& wizard)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            wizard.updateLocationValidation();
            // 无效也是完成结果，权限测试不能靠永久等不到可用状态通过。
            if ( wizard.m_locationResult ) return true;
            std::this_thread::yield();
        } while ( std::chrono::steady_clock::now() < deadline );
        return false;
    }
    /// @brief 文件池忙时修改目录；旧结果不得使新目录的提交按钮可用。
    /// @param wizard 待验证的向导实例。
    /// @param root 已准备真实项目配置的隔离目录。
    /// @param files 应用提供给 UI 的文件线程池，所有测试门闩在退出前释放。
    /// @return 旧结果、权限错误和提交前变化均被拒绝时返回真。
    static bool exercise(NewProjectWizard&            wizard,
                         const std::filesystem::path& root,
                         ice::ThreadPool&             files)
    {
        // 文件池实际固定四个工作者，gate 使四个任务都不能越过探测时序。
        // 四份阻塞工作由同一门闩释放，不依赖它们各自获得线程的顺序。
        std::promise<void>               release;
        const auto                       gate = release.get_future().share();
        std::array<std::future<void>, 4> blockers;
        // 阻塞任务必须先于第一次探测排队，保证结果在检查前不可能完成。
        for ( auto& blocker : blockers )
            blocker = files.enqueue([gate] { gate.wait(); });

        // 第一份结论会允许创建。若被误用于下一份输入，生产按钮将错误启用。
        setInput(wizard, root, "available-old");
        const bool pending =
            wizard.m_locationJob.valid() && !wizard.canAdvance();
        // 故意保留同一父目录，只有文件夹名变化也必须作废前一个请求。
        setInput(wizard, root, "existing-new");
        // 尚未释放工作者，所以下面循环期间生产代码必须继续完成每帧检查。
        // 记住输入变化后的序号。重复绘制帧不应把相同键再次识别成变更。
        const auto waiting = wizard.m_locationRevision;
        for ( int frame = 0; frame < 32; ++frame )
            wizard.updateLocationValidation();
        const bool idle =
            !wizard.canAdvance() && wizard.m_locationRevision == waiting;
        // 先判定 UI 已返回，再允许查询启动。测试不会让后台任务长期占有应用池。
        release.set_value();
        // 回收占位任务；接着通过向导自己的轮询取得当前键结果。
        for ( auto& blocker : blockers ) blocker.get();
        if ( !pending || !idle || !awaitResult(wizard) ||
             wizard.m_locationResult->status !=
                 NewProjectWizard::LocationStatus::ExistingProject )
            return false;

        // 换回有效新目标再实际点击确认，避免测试只覆盖已被禁用的按钮。
        setInput(wizard, root, "creating");
        if ( !awaitResult(wizard) || !wizard.canAdvance() ) return false;
        // 检查查询已完成的静置帧，不能仅验证排队期间不会无限提交。
        // 此时没有在途任务，检查静置不会向已经释放的工作线程发出新探测。
        const auto steady = wizard.m_locationRevision;
        for ( int frame = 0; frame < 32; ++frame ) {
            wizard.updateLocationValidation();
            // 用任务句柄和可用状态双重检查，避免丢弃结果后重新排队也通过。
            if ( wizard.m_locationJob.valid() || !wizard.canAdvance() ||
                 wizard.m_locationRevision != steady )
                return false;
        }
        // 已通过探测后在目标创建项目，再从实际确认入口重新探测。
        std::error_code error;
        // 使用存储层的标准清单名；路径占用与 JSON 是否能加载无关。
        // 这一步必须在第一次验证完成后发生，才能模拟真实的外部变化。
        const auto existing =
            Logic::ProjectStorage::manifestPath(root / "creating");
        std::filesystem::create_directories(existing.parent_path(), error);
        if ( error || !(std::ofstream(existing) << "{}") ) return false;
        // 使用生产确认入口。中途出现的项目不能仅因已有可用快照就被覆盖。
        wizard.submitCreateRequest();
        // 复核任务所需的权限不保证与第一次一致；完成前按钮必须保持禁用。
        if ( !wizard.m_confirmLocation || wizard.canAdvance() ||
             !awaitResult(wizard) ||
             wizard.m_locationResult->status !=
                 NewProjectWizard::LocationStatus::ExistingProject )
            return false;

#if !defined(_WIN32)
        // root 可以绕过目录权限位，因此该环境无法用于证明访问拒绝。
        if ( geteuid() != 0 ) {
            // 中间目录可见但不可搜索，目标查询必须拒绝而不是假定尚未创建。
            // 顶层父目录仍可搜索，只有中间目录的权限会使目标查询失败。
            const auto denied = root / "denied";
            std::filesystem::create_directories(denied, error);
            std::filesystem::permissions(
                denied, std::filesystem::perms::none, error);
            if ( error ) return false;
            // 顶层父目录仍可访问，确保错误来自目标查询而非更早的输入检查。
            // 无法查询的父级并不意味着下游路径可用于创建。
            setInput(wizard, root, "denied/project");
            // 需要收到明确的拒绝结果，而不是仅观察测试尚未得到结果。
            const bool rejected = awaitResult(wizard) && !wizard.canAdvance() &&
                                  wizard.m_locationResult->status ==
                                      NewProjectWizard::LocationStatus::Invalid;
            // 先恢复构建树权限，后续重复运行可以清理夹具。
            std::filesystem::permissions(
                denied, std::filesystem::perms::owner_all, error);
            if ( !rejected || error ) return false;
        }
#endif
        return true;
    }
};
}  // namespace MMM::UI

/// @brief 用隔离工程夹具覆盖静置、过期结果、复核和目录查询错误。
int main()
{
    // 输出目录位于构建树，不接触用户设置、谱面和上一次运行的真实工程。
    namespace fs    = std::filesystem;
    const auto root = fs::current_path() / "test_output/NewProjectLocationTest";
    std::error_code error;
    fs::remove_all(root, error);
    if ( error ) return 1;
    // 使用真实的分片清单位置，禁止用测试专有标记代替现有工程入口。
    const auto existing =
        MMM::Logic::ProjectStorage::manifestPath(root / "existing-new");
    fs::create_directories(existing.parent_path(), error);
    // 缺少可识别的清单会反转旧结果测试，提前拒绝不完整夹具。
    if ( error || !(std::ofstream(existing) << "{}") ) return 2;
    // 文件池与应用由同一入口启动，回归必须检测真实任务排队契约。
    auto& pools = MMM::Runtime::AppThreadPool::instance();
    pools.init();
    // 同一个向导连续处理多个输入，用它验证旧结果不会跨表单阶段泄漏。
    MMM::UI::NewProjectWizard wizard;
    // 使用应用实际交给界面的文件池，不用专门的测试线程替代调度。
    auto* files = pools.getFileThreadPool();
    // 若文件池无法创建，不能退回到同步探测并误判通过。
    const bool passed =
        files &&
        MMM::UI::NewProjectLocationTestAccess::exercise(wizard, root, *files);
    // exercise 已释放全部门闩，回收线程只发生在测试退出边界。
    pools.shutdown();
    return passed ? 0 : 3;
}
