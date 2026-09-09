#include "event/core/EventBus.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "logic/ProjectController.h"

/// @brief 验证入口随异步项目请求、旧画布关闭和替换请求正确传递，取消不残留。
/// @return 全部阶段通过时返回 0，1～6 分别定位失败的请求转换阶段。
/// @note 测试消费待处理动作，不实际打开这些路径或解压谱包。
/// @note 来源用于区分操作入口，不能在异步排队后丢失或沿用上一请求。
/// @note 用例顺序刻意共享控制器，覆盖连续请求间的状态转移而非独立字段赋值。
int main()
{
    using MMM::Event::ProjectOpenOrigin;
    auto& controller = MMM::Logic::ProjectController::instance();
    // 单例可能已有初始化状态，先清除待切换请求再建立本用例的起点。
    controller.cancelPendingProjectSwitch();
    // 无旧画布阻挡时，菜单请求的路径和入口应在同一次消费中一起交付。
    controller.requestOpenProject("/test/menu", ProjectOpenOrigin::FileMenu);
    // 不调用完成通知就先消费，覆盖不需要等待旧画布关闭的直接路径。
    auto action = controller.consumePendingProjectAction(false);
    if ( action.m_origin != ProjectOpenOrigin::FileMenu ||
         action.m_projectPathToOpen != "/test/menu" )
        return 1;
    controller.requestOpenProject("/test/shortcut",
                                  ProjectOpenOrigin::Shortcut);
    // true 模拟仍有旧画布需要关闭，不能提前交出新项目路径。
    action = controller.consumePendingProjectAction(true);
    // 此阶段只要求暂不打开；完整来源的最终归属由后面的替换请求断言验证。
    if ( !action.m_projectPathToOpen.empty() ) return 2;
    // 关闭流程尚未完成时用谱面拖入替换请求，检验来源跟随最新请求一起覆盖。
    controller.requestOpenProject("/test/chart.mmm",
                                  ProjectOpenOrigin::BeatmapDrop);
    controller.completePendingProjectSwitch();
    // 完成旧项目关闭后再消费，不能恢复之前已被替换的快捷键请求。
    action = controller.consumePendingProjectAction(false);
    if ( action.m_origin != ProjectOpenOrigin::BeatmapDrop ||
         action.m_projectPathToOpen != "/test/chart.mmm" )
        return 3;
    // 谱包通过事件入口进入控制器，与直接调用普通项目请求的路径不同。
    MMM::Event::OpenTemporaryProjectPackageEvent event;
    event.m_packagePath = "/test/package.osz";
    // 使用明确后缀表示谱包请求，但测试不依赖该文件存在或内容有效。
    event.m_origin = ProjectOpenOrigin::PackageDrop;
    // 控制器已初始化，事件处理应产生携带 PackageDrop 来源的待处理动作。
    MMM::Event::EventBus::instance().publish(event);
    action = controller.consumePendingProjectAction(false);
    // 同时验证来源和临时模式，避免只保留标签却误走普通项目打开流程。
    // 本断言不执行解包，也不验收文件系统层面的只读保护。
    if ( action.m_origin != ProjectOpenOrigin::PackageDrop ||
         action.m_projectOpenMode !=
             MMM::Logic::ProjectController::ProjectOpenMode::TemporaryPackage )
        return 4;
    controller.requestOpenProject("/test/cancel",
                                  ProjectOpenOrigin::FolderDrop);
    // 取消必须在下一次消费前清空请求，不能因来源为拖入而保留隐式重试。
    controller.cancelPendingProjectSwitch();
    if ( !controller.consumePendingProjectAction(false)
              .m_projectPathToOpen.empty() )
        return 5;
    // 省略入口参数模拟内部调用，必须回到 Unknown，而非继承刚取消的 FolderDrop。
    controller.requestOpenProject("/test/internal");
    if ( controller.consumePendingProjectAction(false).m_origin !=
         ProjectOpenOrigin::Unknown )
        return 6;
    // 成功结束前清理单例的待切换状态，不让本测试留下尚未完成的请求。
    controller.cancelPendingProjectSwitch();
    return 0;
}
