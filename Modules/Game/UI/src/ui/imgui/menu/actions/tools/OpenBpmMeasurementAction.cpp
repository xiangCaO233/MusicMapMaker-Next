#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <memory>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 打开 BPM 测量工具动作。
/// @details 复用已注册工具视图；首次打开时创建实例并移交给 SourceManager。
class OpenBpmMeasurementAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在已有项目时允许打开 BPM 测量工具。
    /// @param context 单帧主菜单上下文，本判断无需读取。
    /// @return 当前存在项目时返回 true。
    /// @warning UI 热路径：只读取当前项目指针，不得启动测量或访问文件系统。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().getCurrentProject() != nullptr;
    }

    /// @brief 注册并打开 BPM 测量工具视图。
    /// @param context 提供工具视图注册与查找入口。
    /// @param activation 激活来源，不改变工具打开方式。
    /// @note 空音轨参数表示由工具视图根据当前项目选择输入。
    /// @warning 该低频动作可能创建视图，不得在逐帧更新中自动执行。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 无视图管理器时无法安全注册工具，保持动作无副作用退出。
        if ( !context.sourceManager ) return;

        // 稳定注册名确保重复打开复用同一个工具实例。
        std::string viewName = "BpmMeasurementTool";
        auto*       tool =
            context.sourceManager->getView<BpmMeasurementToolView>(viewName);
        // 返回指针仅用于本次 UI 调用，所有权始终留在 SourceManager。
        const bool wasOpen = tool && tool->isOpen();
        if ( !tool ) {
            // 首次使用才分配视图，随后由 SourceManager 接管独占所有权。
            auto toolView = std::make_unique<BpmMeasurementToolView>(
                TR("ui.tools.bpm_measure").data());
            tool = toolView.get();
            context.sourceManager->registerView(viewName, std::move(toolView));
        }
        if ( tool ) {
            // 注册成功后统一走视图公开入口，避免菜单层复制测量初始化逻辑。
            tool->openWithAudioTrack("");
            // 使用打开前快照区分首次显示和对已有窗口的重复请求。
            if ( !wasOpen ) {
                // 已打开窗口再次聚焦时不重复播放弹窗音效。
                ::MMM::UI::PlayPopupOpenFeedback();
            }
        }
    }
};
}  // namespace

/// @brief 创建打开 BPM 测量工具动作处理器。
/// @return 独占所有权的无状态处理器。
/// @note 工具视图本身由 SourceManager 管理，动作不持有其生命周期。
/// @warning 必须在可提供有效 MainMenuContext 的 UI 线程调用。
std::unique_ptr<IMainMenuItemActionHandler> createOpenBpmMeasurementAction()
{
    return std::make_unique<OpenBpmMeasurementAction>();
}

}  // namespace MMM::UI
