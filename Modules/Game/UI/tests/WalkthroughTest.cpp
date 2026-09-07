#include "BuiltinWalkthrough.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectOpenInteractionEvent.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include "ui/walkthrough/WalkthroughService.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

/// @brief 验证分支隔离、每步手动了解、信号判定、配置校验和无窗口持久化。
int main(int argc, char** argv)
{
    using namespace MMM::UI::Walkthrough;
    if ( argc != 2 ) return 1;
    const auto topic = parseTopic(BUILTIN_WALKTHROUGH);
    if ( !topic || topic->m_branches.size() != 5 || !topic->m_anyBranch )
        return 2;
    Progress progress;
    if ( !progress.acknowledge(*topic, topic->m_branches[4].m_steps[1]) ||
         progress.completed(*topic, topic->m_branches[4].m_steps[0]) )
        return 3;
    if ( !progress.signal(*topic, "project.shortcut.picker") ||
         progress.completed(*topic, topic->m_branches[1].m_steps[1]) ||
         progress.completed(*topic, topic->m_branches[0].m_steps[0]) )
        return 4;
    if ( !progress.signal(*topic, "project.shortcut.ready") ||
         !progress.completed(*topic, topic->m_branches[1].m_steps[1]) )
        return 5;
    Progress restored;
    if ( !restored.restore(progress.serialize()) ||
         !restored.completed(*topic, topic->m_branches[4].m_steps[1]) )
        return 6;
    if ( restored.restore("broken") ||
         !restored.completed(*topic, topic->m_branches[4].m_steps[1]) )
        return 7;
    restored.reset(*topic);
    if ( restored.completed(*topic, topic->m_branches[4].m_steps[1]) ) return 8;
    auto dependency = parseTopic(
        R"({"id":"test","title":"T","completion":"all","branches":[{"id":"a","title":"A","steps":[{"id":"first","title":"First"},{"id":"second","title":"Second","requires":["first"],"match":"all","signals":["one","two"]}]}]})");
    if ( !dependency || dependency->m_anyBranch ) return 9;
    const auto& first  = dependency->m_branches[0].m_steps[0];
    const auto& second = dependency->m_branches[0].m_steps[1];
    progress.signal(*dependency, "one");
    progress.signal(*dependency, "two");
    if ( progress.completed(*dependency, second) ) return 10;
    progress.acknowledge(*dependency, first);
    progress.signal(*dependency, "");
    if ( !progress.completed(*dependency, second) ) return 11;
    if (
        parseTopic(
            R"({"id":"x","title":"X","branches":[{"id":"b","title":"B","steps":[{"id":"s","title":"S","requires":["s"]}]}]})") )
        return 12;
    if (
        parseTopic(
            R"({"id":"x","title":"X","branches":[{"id":"b","title":"B","steps":[{"id":"s","title":"S"},{"id":"s","title":"S"}]}]})") )
        return 13;
    const auto directory =
        std::filesystem::path(argv[1]) /
        ("walkthrough-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "progress.json";
    {
        Service service(path, directory / "walkthroughs");
        MMM::Event::ProjectOpenInteractionEvent event;
        event.m_origin    = MMM::Event::ProjectOpenOrigin::PackageDrop;
        event.m_completed = true;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        if ( service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[4].m_steps[0]) )
            return 14;
        event.m_readOnly = true;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        if ( !service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[4].m_steps[1]) )
            return 15;
        event.m_origin = MMM::Event::ProjectOpenOrigin::BeatmapDrop;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        if ( service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[3].m_steps[1]) )
            return 16;
        event.m_beatmapOpened = true;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        if ( !service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[3].m_steps[1]) )
            return 17;
        int invoked = 0;
        service.registerAction("test", [&] { ++invoked; });
        service.execute("unregistered");
        service.execute("test");
        if ( invoked != 1 ) return 18;
    }
    {
        Service service(path, directory / "walkthroughs");
        if ( !service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[4].m_steps[1]) )
            return 19;
        service.reset(service.topics()[0]);
    }
    {
        Service service(path, directory / "walkthroughs");
        if ( service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[4].m_steps[1]) )
            return 20;
    }
    return 0;
}
