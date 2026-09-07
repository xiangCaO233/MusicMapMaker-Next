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
struct ProjectDropRouter::Impl {
    Event::SubscriptionID m_subscription{};  ///< 系统投放订阅令牌。
    std::vector<Event::GLFWDropEvent>
        m_drops;  ///< GLFW 回调和消费均在渲染线程。
};
ProjectDropRouter::ProjectDropRouter() : m_impl(std::make_unique<Impl>())
{
    m_impl->m_subscription =
        Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
            [this](const auto& event) { m_impl->m_drops.push_back(event); });
}
ProjectDropRouter::~ProjectDropRouter()
{
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(
        m_impl->m_subscription);
}
void ProjectDropRouter::update(bool enabled)
{
    if ( m_impl->m_drops.empty() ) return;
    if ( !enabled || ImGui::GetTopMostPopupModal() ) {
        m_impl->m_drops.clear();
        return;
    }
    for ( const auto& drop : m_impl->m_drops ) {
        if ( drop.paths.size() != 1U ) continue;
        const auto* viewport = ImGui::GetMainViewport();
        if ( drop.pos.x < 0 || drop.pos.y < 0 ||
             drop.pos.x >= viewport->Size.x || drop.pos.y >= viewport->Size.y )
            continue;
        const auto      path = Config::utf8ToPath(drop.paths.front());
        std::error_code error;
        if ( std::filesystem::is_directory(path, error) && !error ) {
            Event::OpenProjectEvent event;
            event.m_projectPath = path;
            event.m_origin      = Event::ProjectOpenOrigin::FolderDrop;
            Event::EventBus::instance().publish(event);
            continue;
        }
        auto extension = Config::pathToUtf8(path.extension());
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if ( extension == ".osz" || extension == ".mcz" ||
             extension == ".mpk" || extension == ".zip" ||
             extension == ".7z" ) {
            Event::OpenTemporaryProjectPackageEvent event;
            event.m_packagePath = path;
            event.m_origin      = Event::ProjectOpenOrigin::PackageDrop;
            Event::EventBus::instance().publish(event);
        }
    }
    m_impl->m_drops.clear();
}
}  // namespace MMM::UI
