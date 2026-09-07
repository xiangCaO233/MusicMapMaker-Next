#include "ui/walkthrough/WalkthroughService.h"
#include "BuiltinWalkthrough.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectOpenInteractionEvent.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include <concurrentqueue.h>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

namespace MMM::UI::Walkthrough
{
namespace
{
/// @brief 有大小上限的低频文件读取，失败返回空文本。
std::string readFile(const std::filesystem::path& path)
{
    std::error_code error;
    const auto      size = std::filesystem::file_size(path, error);
    if ( error || size > 1024 * 1024 ) return {};
    std::ifstream stream(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream),
             std::istreambuf_iterator<char>() };
}
}  // namespace
struct Service::Impl {
    std::vector<Topic>    m_topics;            ///< 通过校验的主题。
    Progress              m_progress;          ///< 独立的学习状态。
    std::filesystem::path m_path;              ///< 专用进度文件。
    std::string           m_error;             ///< 最近错误。
    bool                  m_writable{ true };  ///< 损坏进度文件禁止自动覆盖。
    Event::SubscriptionID m_subscription{};    ///< 业务结果订阅。
    moodycamel::ConcurrentQueue<Event::ProjectOpenInteractionEvent>
        m_events;  ///< 跨线程仅传递低频结果。
    std::map<std::string, std::function<void()>, std::less<>>
        m_actions;  ///< 可信操作注册表。

    /// @brief 校验单个主题后加入目录，不覆盖同 ID 内置或已有主题。
    void add(std::string_view input)
    {
        auto topic = parseTopic(input);
        if ( !topic ) {
            m_error = topic.error();
            return;
        }
        for ( const auto& existing : m_topics )
            if ( existing.m_id == topic->m_id ) {
                m_error = "演练主题 ID 重复：" + topic->m_id;
                return;
            }
        m_topics.push_back(std::move(*topic));
    }
    /// @brief 使用临时文件替换进度；Windows 回退保留旧文件以便恢复。
    /// @warning 仅进度变化时执行文件 I/O，不在空闲帧重试。
    void save()
    {
        if ( !m_writable ) return;
        std::error_code ec;
        std::filesystem::create_directories(m_path.parent_path(), ec);
        if ( ec ) {
            m_error = ec.message();
            return;
        }
        auto temporary = m_path;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << m_progress.serialize();
            stream.close();
            if ( !stream ) {
                m_error = "无法写入演练进度临时文件";
                return;
            }
        }
        std::filesystem::rename(temporary, m_path, ec);
        if ( !ec ) return;
        auto backup = m_path;
        backup += ".previous";
        if ( std::filesystem::exists(backup, ec) ) {
            m_error = "存在演练恢复文件，请先检查 " + backup.string();
            return;
        }
        ec.clear();
        std::filesystem::rename(m_path, backup, ec);
        if ( ec ) {
            m_error = ec.message();
            return;
        }
        std::filesystem::rename(temporary, m_path, ec);
        if ( ec ) {
            m_error = ec.message();
            std::error_code rollbackError;
            std::filesystem::rename(backup, m_path, rollbackError);
            return;
        }
        std::filesystem::remove(backup, ec);
    }
};

Service::Service(const std::filesystem::path& progressPath,
                 const std::filesystem::path& customDirectory)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->m_path = progressPath;
    m_impl->add(BUILTIN_WALKTHROUGH);
    std::error_code error;
    if ( std::filesystem::is_directory(customDirectory, error) ) {
        std::filesystem::directory_iterator it(customDirectory, error), end;
        for ( ; !error && it != end && m_impl->m_topics.size() < 64;
              it.increment(error) ) {
            if ( it->path().extension() == ".json" )
                m_impl->add(readFile(it->path()));
        }
    }
    error.clear();
    auto loadPath = progressPath;
    auto backup   = progressPath;
    backup += ".previous";
    if ( !std::filesystem::exists(loadPath, error) &&
         std::filesystem::exists(backup, error) ) {
        std::filesystem::rename(backup, loadPath, error);
        if ( error ) loadPath = backup;
    }
    if ( std::filesystem::exists(loadPath, error) &&
         !m_impl->m_progress.restore(readFile(loadPath)) ) {
        m_impl->m_writable = false;
        m_impl->m_error    = "演练进度损坏，已保留原文件；重置进度前请备份。";
    }
    m_impl->m_subscription =
        Event::EventBus::instance()
            .subscribe<Event::ProjectOpenInteractionEvent>(
                [this](const auto& event) { m_impl->m_events.enqueue(event); });
}
Service::~Service()
{
    Event::EventBus::instance().unsubscribe<Event::ProjectOpenInteractionEvent>(
        m_impl->m_subscription);
}
void Service::update()
{
    Event::ProjectOpenInteractionEvent event;
    bool                               changed = false;
    while ( m_impl->m_events.try_dequeue(event) ) {
        std::string signal;
        switch ( event.m_origin ) {
        case Event::ProjectOpenOrigin::FileMenu:
            signal = event.m_completed ? "project.menu.ready"
                                       : "project.menu.picker";
            break;
        case Event::ProjectOpenOrigin::Shortcut:
            signal = event.m_completed ? "project.shortcut.ready"
                                       : "project.shortcut.picker";
            break;
        case Event::ProjectOpenOrigin::FolderDrop:
            if ( event.m_completed ) signal = "project.folder_drop.ready";
            break;
        case Event::ProjectOpenOrigin::BeatmapDrop:
            if ( event.m_completed && event.m_beatmapOpened )
                signal = "project.beatmap_drop.ready";
            break;
        case Event::ProjectOpenOrigin::PackageDrop:
            if ( event.m_completed && event.m_readOnly )
                signal = "project.package_drop.ready";
            break;
        default: break;
        }
        if ( !signal.empty() )
            for ( const auto& topic : m_impl->m_topics )
                changed |= m_impl->m_progress.signal(topic, signal);
    }
    if ( changed ) m_impl->save();
}
const std::vector<Topic>& Service::topics() const
{
    return m_impl->m_topics;
}
const Progress& Service::progress() const
{
    return m_impl->m_progress;
}
const std::string& Service::error() const
{
    return m_impl->m_error;
}
void Service::acknowledge(const Topic& topic, const Step& step)
{
    if ( m_impl->m_progress.acknowledge(topic, step) ) {
        m_impl->m_progress.signal(topic, "");
        m_impl->save();
    }
}
void Service::reset(const Topic& topic)
{
    m_impl->m_progress.reset(topic);
    m_impl->save();
}
void Service::registerAction(std::string id, std::function<void()> action)
{
    m_impl->m_actions.insert_or_assign(std::move(id), std::move(action));
}
bool Service::hasAction(std::string_view id) const
{
    return m_impl->m_actions.contains(id);
}
void Service::execute(std::string_view id)
{
    if ( auto it = m_impl->m_actions.find(id); it != m_impl->m_actions.end() )
        it->second();
}
}  // namespace MMM::UI::Walkthrough
