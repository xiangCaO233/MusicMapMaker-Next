#include "ui/project/ProjectDropRouter.h"
#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <vector>

namespace MMM::UI
{
/// @brief ProjectDropRouter 的事件订阅与单帧待消费投放队列。
/// @note PImpl 避免公开头引入 GLFW 投放事件及其容器定义。
struct ProjectDropRouter::Impl {
    Event::SubscriptionID m_subscription{};  ///< 系统投放订阅令牌。
    std::vector<Event::GLFWDropEvent>
        m_drops;  ///< GLFW 回调和消费均在渲染线程。
};
/// @brief 创建主窗口拖放路由并订阅 GLFW 文件投放事件。
/// @warning 订阅回调捕获 this，析构时必须先解除订阅。
ProjectDropRouter::ProjectDropRouter() : m_impl(std::make_unique<Impl>())
{
    // 事件回调只收集值语义事件，具体路径判断延迟到 UI 更新阶段。
    m_impl->m_subscription =
        Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
            [this](const auto& event) { m_impl->m_drops.push_back(event); });
}
/// @brief 解除 GLFW 投放订阅。
ProjectDropRouter::~ProjectDropRouter()
{
    // 令牌与构造时的订阅严格配对，防止回调访问已析构路由。
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(
        m_impl->m_subscription);
}
/// @brief 消费本帧待处理投放并发布项目打开事件。
/// @param enabled 当前 UI 是否允许接受主窗口投放。
/// @warning UI 热路径：空队列时常数时间返回；仅真实投放时查询文件系统。
/// @details
/// 只接受主视口内的单一路径，目录按项目打开，支持的压缩包按临时项目打开。
/// 投放处理保持以下约束：
/// - 模态弹窗打开时不延迟执行用户此前的投放；
/// - 每个 GLFW 事件只消费一次；
/// - 多路径事件整体忽略，不猜测打开顺序；
/// - 视口外投放不路由给主窗口；
/// - 目录优先于扩展名判断；
/// - 文件系统错误按非目录处理且不抛出异常；
/// - 不支持的普通文件保持静默；
/// - 发布事件携带明确来源供上层选择导入策略。
void ProjectDropRouter::update(bool enabled)
{
    // 常规帧没有投放时不访问 ImGui 视口或文件系统。
    if ( m_impl->m_drops.empty() ) return;
    if ( !enabled || ImGui::GetTopMostPopupModal() ) {
        // 禁用或模态交互期间丢弃旧投放，避免稍后意外补执行。
        m_impl->m_drops.clear();
        return;
    }
    for ( const auto& drop : m_impl->m_drops ) {
        // 多路径投放语义不明确，避免一次触发多个项目切换。
        if ( drop.paths.size() != 1U ) continue;
        // 只接受主视口客户区内的坐标，忽略其他平台窗口的投放。
        const auto* viewport = ImGui::GetMainViewport();
        if ( drop.pos.x < 0 || drop.pos.y < 0 ||
             drop.pos.x >= viewport->Size.x || drop.pos.y >= viewport->Size.y )
            continue;
        // GLFW 路径为 UTF-8，文件系统操作前转换为平台原生路径。
        const auto      path = Config::utf8ToPath(drop.paths.front());
        std::error_code error;
        if ( std::filesystem::is_directory(path, error) && !error ) {
            // 目录投放沿用正常项目打开事件，并标记来源供上层策略判断。
            Event::OpenProjectEvent event;
            event.m_projectPath = path;
            event.m_origin      = Event::ProjectOpenOrigin::FolderDrop;
            Event::EventBus::instance().publish(event);
            continue;
        }
        // 非目录按不区分大小写的扩展名识别支持的谱包格式。
        auto extension = Config::pathToUtf8(path.extension());
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if ( extension == ".osz" || extension == ".mcz" ||
             extension == ".mpk" || extension == ".zip" ||
             extension == ".7z" ) {
            // 包投放进入临时项目解包流程，不把压缩包当作普通目录打开。
            Event::OpenTemporaryProjectPackageEvent event;
            event.m_packagePath = path;
            event.m_origin      = Event::ProjectOpenOrigin::PackageDrop;
            Event::EventBus::instance().publish(event);
        }
    }
    // 所有事件至多消费一次，包括无效或不支持的路径。
    m_impl->m_drops.clear();
}
}  // namespace MMM::UI
