#include "event/core/EventBus.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "logic/ProjectController.h"

/// @brief 验证入口随异步项目请求、旧画布关闭和替换请求正确传递，取消不残留。
int main()
{
    using MMM::Event::ProjectOpenOrigin;
    auto& controller = MMM::Logic::ProjectController::instance();
    controller.cancelPendingProjectSwitch();
    controller.requestOpenProject("/test/menu", ProjectOpenOrigin::FileMenu);
    auto action = controller.consumePendingProjectAction(false);
    if ( action.m_origin != ProjectOpenOrigin::FileMenu ||
         action.m_projectPathToOpen != "/test/menu" )
        return 1;
    controller.requestOpenProject("/test/shortcut",
                                  ProjectOpenOrigin::Shortcut);
    action = controller.consumePendingProjectAction(true);
    if ( !action.m_projectPathToOpen.empty() ) return 2;
    controller.requestOpenProject("/test/chart.mmm",
                                  ProjectOpenOrigin::BeatmapDrop);
    controller.completePendingProjectSwitch();
    action = controller.consumePendingProjectAction(false);
    if ( action.m_origin != ProjectOpenOrigin::BeatmapDrop ||
         action.m_projectPathToOpen != "/test/chart.mmm" )
        return 3;
    MMM::Event::OpenTemporaryProjectPackageEvent event;
    event.m_packagePath = "/test/package.osz";
    event.m_origin      = ProjectOpenOrigin::PackageDrop;
    MMM::Event::EventBus::instance().publish(event);
    action = controller.consumePendingProjectAction(false);
    if ( action.m_origin != ProjectOpenOrigin::PackageDrop ||
         action.m_projectOpenMode !=
             MMM::Logic::ProjectController::ProjectOpenMode::TemporaryPackage )
        return 4;
    controller.requestOpenProject("/test/cancel",
                                  ProjectOpenOrigin::FolderDrop);
    controller.cancelPendingProjectSwitch();
    if ( !controller.consumePendingProjectAction(false)
              .m_projectPathToOpen.empty() )
        return 5;
    controller.requestOpenProject("/test/internal");
    if ( controller.consumePendingProjectAction(false).m_origin !=
         ProjectOpenOrigin::Unknown )
        return 6;
    controller.cancelPendingProjectSwitch();
    return 0;
}
