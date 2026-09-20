#include "ui/walkthrough/WalkthroughService.h"
#include "BuiltinWalkthrough.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapCreateInteractionEvent.h"
#include "event/project/ProjectOpenInteractionEvent.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include <algorithm>
#include <concurrentqueue.h>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

/// @file WalkthroughService.cpp
/// @brief 演练主题加载、业务事件适配、可信操作注册和进度持久化实现。
/// @details 服务不依赖欢迎页是否可见，由 UIManager 在整个 UI 生命周期内更新；
/// 这样业务结果即使发生在演练页面关闭期间也能正确记录。
/// 服务维护三类不同来源的数据：
/// - 编译期生成的内置章节和主题 JSON；
/// - 自定义目录中受大小和数量限制的主题 JSON；
/// - 专用进度路径中的用户完成记录。
/// 模型解析负责内容校验，服务负责来源优先级、目录排序和持久化生命周期；
/// 页面层只读取验证后的结果，不直接接触这些文件。
/// 跨线程业务事件先入队，进度对象只在 UI 更新线程修改。
///
/// 新建谱面事件适配约定：
/// - 向导打开、主音频选择和创建完成分别映射为 dialog、audio、ready；
/// - FileMenu 与 Shortcut 使用互不替代的信号命名空间；
/// - 入口枚举由业务命令携带，服务不根据当前焦点或可见菜单猜测来源；
/// - 未知入口不会推进教程，但事件仍可被其他业务订阅；
/// - 取消向导没有完成事件，因此不会伪造任何后续步骤；
/// - 创建失败不会发布 ready，已选择音频也不能单独完成主题；
/// - 同一阶段重复到达由 Progress 的集合语义幂等处理；
/// - 每次到达仍更新易失信号序号，路线重放不受历史完成度影响；
/// - 易失序号只存在于当前进程，不写入用户学习进度文件；
/// - 路线启动时记录当前基线，只有之后到达的同名信号可以推进；
/// - allSignals 步骤要求每个配置信号都在本轮基线后重新出现；
/// - 跨线程回调只复制轻量事件到队列，不写进度文件；
/// - UI update 一帧统一消费两类队列，并把多次变化合并为一次保存；
/// - 事件中的谱面路径用于诊断，不进入学习记录键；
/// - 主题配置仍决定信号对应哪些步骤，服务不硬编码主题索引；
/// - 自定义主题可以复用公开信号，但不能覆盖同 ID 的内置主题；
/// - 项目上下文门禁属于页面职责，不在事件到达时丢弃历史实操结果；
/// - 服务析构必须分别解除项目与谱面订阅，防止队列悬空访问；
/// - 两个队列均只承载低频用户流程，不用于渲染或逻辑帧同步；
/// - 业务枚举到字符串的转换集中在 update，资产无需依赖 C++ 枚举值；
/// - 新阶段若扩展，必须同时补充配置解析测试与来源隔离测试；
/// - signal 保持稳定可持久化语义，翻译和界面标题变化不得改名。

namespace MMM::UI::Walkthrough
{
namespace
{
/// @brief 有大小上限的低频文件读取，失败返回空文本。
/// @param path 待读取的主题或进度文件路径。
/// @return 不超过 1 MiB 的完整二进制文本；查询或打开失败时返回空。
/// @warning 低频文件路径：只在服务构造加载自定义主题或恢复进度时调用。
std::string readFile(const std::filesystem::path& path)
{
    // 先通过无异常接口验证大小，防止意外读取超大自定义文件。
    std::error_code error;
    const auto      size = std::filesystem::file_size(path, error);
    if ( error || size > 1024 * 1024 ) return {};
    // 二进制模式保持 JSON 文本字节不被平台换行转换。
    std::ifstream stream(path, std::ios::binary);
    // 流打开失败时迭代器区间为空，统一返回空字符串交给解析器报告。
    return { std::istreambuf_iterator<char>(stream),
             std::istreambuf_iterator<char>() };
}
}  // namespace
/// @brief Service 的稳定目录、进度、事件队列和动作注册表实现。
/// @details PImpl 隔离
/// concurrentqueue、事件类型和持久化细节，公开头只暴露模型接口。
struct Service::Impl {
    std::vector<Chapter> m_chapters;  ///< 已排序章节，保留空章节入口区域。
    std::vector<Topic>   m_topics;    ///< 通过校验的主题。
    Progress             m_progress;  ///< 独立的学习状态。
    std::uint64_t        m_signalSequence{ 0 };  ///< 本进程业务信号单调序号。
    std::map<std::string, std::uint64_t, std::less<>>
        m_signalRevisions;          ///< 每个稳定信号最近一次到达序号。
    std::filesystem::path m_path;   ///< 专用进度文件。
    std::string           m_error;  ///< 最近错误。
    bool                  m_writable{ true };  ///< 损坏进度文件禁止自动覆盖。
    Event::SubscriptionID m_projectSubscription{};  ///< 项目交互订阅。
    Event::SubscriptionID m_beatmapSubscription{};  ///< 新建谱面交互订阅。
    moodycamel::ConcurrentQueue<Event::ProjectOpenInteractionEvent>
        m_projectEvents;  ///< 跨线程仅传递低频项目结果。
    moodycamel::ConcurrentQueue<Event::BeatmapCreateInteractionEvent>
        m_beatmapEvents;  ///< 跨线程仅传递低频新建谱面阶段。
    std::map<std::string, std::function<void()>, std::less<>>
        m_actions;  ///< 可信操作注册表。

    /// @brief 校验单个主题后加入目录，不覆盖同 ID 内置或已有主题。
    /// @param input 待解析的 UTF-8 JSON 文本。
    /// @post 成功时追加一个主题；失败时保留原目录并更新错误文本。
    void add(std::string_view input)
    {
        // 所有内置和自定义主题共享同一严格解析与模型校验入口。
        auto topic = parseTopic(input);
        if ( !topic ) {
            // 保留最近解析错误供欢迎页显示，不插入部分模型。
            m_error = topic.error();
            return;
        }
        for ( const auto& existing : m_topics )
            if ( existing.m_id == topic->m_id ) {
                // 首个声明获胜，防止自定义文件覆盖可信内置教程。
                m_error = "演练主题 ID 重复：" + topic->m_id;
                return;
            }
        // 完整验证后移动主题，避免复制多语言正文和步骤容器。
        m_topics.push_back(std::move(*topic));
    }
    /// @brief 使用临时文件替换进度；Windows 回退保留旧文件以便恢复。
    /// @warning 仅进度变化时执行文件 I/O，不在空闲帧重试。
    /// @details 优先在同目录写完整临时文件再原子 rename；平台无法覆盖已有
    /// 目标时，先把旧文件改名为 .previous，替换失败则尝试回滚。
    void save()
    {
        // 损坏进度被加载时禁止自动覆盖，必须由用户先备份或明确重置。
        if ( !m_writable ) return;
        std::error_code ec;
        // 进度目录可能首次使用，保存前递归创建。
        std::filesystem::create_directories(m_path.parent_path(), ec);
        if ( ec ) {
            m_error = ec.message();
            return;
        }
        auto temporary = m_path;
        temporary += ".tmp";
        {
            // 临时文件采用截断写，确保没有旧 JSON 尾部残留。
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << m_progress.serialize();
            stream.close();
            if ( !stream ) {
                // 写入或关闭失败时不触碰现有正式进度文件。
                m_error = "无法写入演练进度临时文件";
                return;
            }
        }
        // POSIX 同文件系统 rename 可直接替换，成功即完成提交。
        std::filesystem::rename(temporary, m_path, ec);
        if ( !ec ) return;
        auto backup = m_path;
        backup += ".previous";
        if ( std::filesystem::exists(backup, ec) ) {
            // 既有恢复文件可能包含用户唯一可用进度，不自动覆盖。
            m_error = "存在演练恢复文件，请先检查 " + backup.string();
            return;
        }
        ec.clear();
        // Windows 无法覆盖目标时先把旧正式文件移到恢复路径。
        std::filesystem::rename(m_path, backup, ec);
        if ( ec ) {
            m_error = ec.message();
            return;
        }
        // 第二次 rename 把已完整写入的临时文件提升为正式进度。
        std::filesystem::rename(temporary, m_path, ec);
        if ( ec ) {
            m_error = ec.message();
            std::error_code rollbackError;
            // 最终替换失败时尽力恢复旧文件，原始错误仍保留给用户。
            std::filesystem::rename(backup, m_path, rollbackError);
            return;
        }
        // 新文件成功后删除备份；删除失败不影响已提交的新进度。
        std::filesystem::remove(backup, ec);
    }
};

/// @brief 加载演练目录和进度，并订阅项目打开交互结果。
/// @param progressPath 专用进度 JSON 路径。
/// @param customDirectory 用户自定义主题目录。
/// @warning 构造低频路径：会读取内置文本、扫描最多 64 个主题并访问进度文件。
Service::Service(const std::filesystem::path& progressPath,
                 const std::filesystem::path& customDirectory)
    : m_impl(std::make_unique<Impl>())
{
    // 进度路径固定在服务生命周期内，所有保存和恢复都使用同一位置。
    m_impl->m_path = progressPath;
    if ( auto chapters = parseChapters(BUILTIN_CHAPTERS) )
        // 章节目录可包含没有主题的入口区域，保持声明顺序与排序信息。
        m_impl->m_chapters = std::move(*chapters);
    else
        // 章节失败不阻止内置主题加载，错误留给 UI 诊断。
        m_impl->m_error = chapters.error();
    // 核心教程优先加入，后续同 ID 自定义主题不能覆盖。
    m_impl->add(BUILTIN_WALKTHROUGH);
    // 三个创建主题分别接收项目、空白谱面和模板谱面事件，不依赖欢迎页是否可见。
    m_impl->add(BUILTIN_CREATE_PROJECT_WALKTHROUGH);
    m_impl->add(BUILTIN_CREATE_BEATMAP_WALKTHROUGH);
    m_impl->add(BUILTIN_CREATE_BEATMAP_TEMPLATE_WALKTHROUGH);
    for ( const auto* placeholder : BUILTIN_PLACEHOLDERS )
        // 占位主题沿用相同解析规则，保证模型结构一致。
        m_impl->add(placeholder);
    std::error_code error;
    if ( std::filesystem::is_directory(customDirectory, error) ) {
        // 使用 error_code 迭代目录，不让损坏条目抛出异常终止启动。
        std::filesystem::directory_iterator it(customDirectory, error), end;
        for ( ; !error && it != end && m_impl->m_topics.size() < 64;
              it.increment(error) ) {
            if ( it->path().extension() == ".json" )
                // 只有 JSON 文件进入主题解析，其他资源可供正文图片引用。
                m_impl->add(readFile(it->path()));
        }
    }
    // 仅加载时排序；相同阶段保留声明顺序，旧主题和未知章节仍可访问。
    std::stable_sort(
        m_impl->m_topics.begin(),
        m_impl->m_topics.end(),
        [](const Topic& a, const Topic& b) { return a.m_order < b.m_order; });
    for ( const auto& topic : m_impl->m_topics ) {
        // 自定义或旧版本主题可引用内置章节目录中未知的章节 ID。
        if ( std::none_of(m_impl->m_chapters.begin(),
                          m_impl->m_chapters.end(),
                          [&](const Chapter& chapter) {
                              return chapter.m_id == topic.m_chapter;
                          }) ) {
            // 为未知章节合成可访问入口，避免有效主题从目录中消失。
            Chapter chapter;
            chapter.m_id = topic.m_chapter;
            // other 使用本地化友好名称，其余未知 ID 直接作为回退标题。
            chapter.m_title.m_translations["en_us"] =
                topic.m_chapter == "other" ? "Other" : topic.m_chapter;
            chapter.m_title.m_translations["zh_cn"] =
                topic.m_chapter == "other" ? "其他" : topic.m_chapter;
            // 合成章节排在显式配置章节之后，并保持首次出现顺序。
            chapter.m_order = 100000;
            m_impl->m_chapters.push_back(std::move(chapter));
        }
    }
    error.clear();
    // 正常启动优先读正式文件；缺失时尝试恢复上次替换留下的备份。
    auto loadPath = progressPath;
    auto backup   = progressPath;
    backup += ".previous";
    if ( !std::filesystem::exists(loadPath, error) &&
         std::filesystem::exists(backup, error) ) {
        // 能恢复时把 previous 提升回正式路径，失败仍可直接读取备份。
        std::filesystem::rename(backup, loadPath, error);
        if ( error ) loadPath = backup;
    }
    if ( std::filesystem::exists(loadPath, error) &&
         !m_impl->m_progress.restore(readFile(loadPath)) ) {
        // 损坏内容保持原样并锁定自动保存，防止用户恢复材料被覆盖。
        m_impl->m_writable = false;
        m_impl->m_error    = "演练进度损坏，已保留原文件；重置进度前请备份。";
    }
    // 业务事件可能从逻辑线程到达，回调只入无锁队列，不修改进度文件。
    m_impl->m_projectSubscription =
        Event::EventBus::instance()
            .subscribe<Event::ProjectOpenInteractionEvent>(
                [this](const auto& event) {
                    m_impl->m_projectEvents.enqueue(event);
                });
    m_impl->m_beatmapSubscription =
        Event::EventBus::instance()
            .subscribe<Event::BeatmapCreateInteractionEvent>(
                [this](const auto& event) {
                    m_impl->m_beatmapEvents.enqueue(event);
                });
}
/// @brief 解除项目打开交互事件订阅。
/// @warning EventBus 必须比 UIManager 管理的服务存活更久。
Service::~Service()
{
    // 避免服务析构后订阅回调继续访问 m_impl。
    Event::EventBus::instance().unsubscribe<Event::ProjectOpenInteractionEvent>(
        m_impl->m_projectSubscription);
    Event::EventBus::instance()
        .unsubscribe<Event::BeatmapCreateInteractionEvent>(
            m_impl->m_beatmapSubscription);
}
/// @brief 返回按目录顺序排列的章节列表。
/// @return 服务拥有的只读章节容器。
const std::vector<Chapter>& Service::chapters() const
{
    return m_impl->m_chapters;
}
/// @brief 消费业务结果事件并更新自动完成进度。
/// @warning UI 热路径：空队列时不分配、不访问文件；状态改变后才保存。
void Service::update()
{
    Event::ProjectOpenInteractionEvent event;
    bool                               changed = false;
    while ( m_impl->m_projectEvents.try_dequeue(event) ) {
        // 每个业务来源与阶段映射为教程模型使用的稳定 signal 字符串。
        std::string signal;
        switch ( event.m_origin ) {
        case Event::ProjectOpenOrigin::FileMenu:
            // 菜单流程区分打开选择器和最终完成两个学习步骤。
            signal = event.m_completed ? "project.menu.ready"
                                       : "project.menu.picker";
            break;
        case Event::ProjectOpenOrigin::Shortcut:
            // 快捷键流程使用独立 signal，避免与菜单步骤互相代替。
            signal = event.m_completed ? "project.shortcut.ready"
                                       : "project.shortcut.picker";
            break;
        case Event::ProjectOpenOrigin::FolderDrop:
            // 文件夹拖放只有成功完成时才计入教程进度。
            if ( event.m_completed ) signal = "project.folder_drop.ready";
            break;
        case Event::ProjectOpenOrigin::BeatmapDrop:
            // 谱面拖放必须同时完成项目流程并真正打开谱面。
            if ( event.m_completed && event.m_beatmapOpened )
                signal = "project.beatmap_drop.ready";
            break;
        case Event::ProjectOpenOrigin::PackageDrop:
            // 谱包教程要求以只读临时项目方式成功打开。
            if ( event.m_completed && event.m_readOnly )
                signal = "project.package_drop.ready";
            break;
        case Event::ProjectOpenOrigin::CreateFileMenu:
            // 新建菜单入口先记录向导唤出，项目真正加载后再完成第二步。
            signal = event.m_completed ? "project.create.menu.ready"
                                       : "project.create.menu.dialog";
            break;
        case Event::ProjectOpenOrigin::CreateShortcut:
            // 快捷键分支保持独立，不能由菜单创建结果代替实操。
            signal = event.m_completed ? "project.create.shortcut.ready"
                                       : "project.create.shortcut.dialog";
            break;
        default: break;
        }
        if ( !signal.empty() ) {
            // 易失序号记录每次真实到达，不能被持久化进度的幂等结果吞掉。
            m_impl->m_signalRevisions.insert_or_assign(
                signal, ++m_impl->m_signalSequence);
            // 一个业务 signal 可能推进多个主题，逐主题合并 changed 标志。
            for ( const auto& topic : m_impl->m_topics )
                changed |= m_impl->m_progress.signal(topic, signal);
        }
    }
    Event::BeatmapCreateInteractionEvent beatmapEvent;
    while ( m_impl->m_beatmapEvents.try_dequeue(beatmapEvent) ) {
        // 新建谱面按入口和实际阶段生成独立信号，取消与失败没有完成事件。
        std::string signal;
        const char* origin = nullptr;
        switch ( beatmapEvent.m_origin ) {
        case Logic::BeatmapCreateOrigin::FileMenu: origin = "menu"; break;
        case Logic::BeatmapCreateOrigin::Shortcut: origin = "shortcut"; break;
        default: break;
        }
        if ( origin ) {
            const char* stage  = nullptr;
            const char* family = beatmapEvent.m_fromTemplate
                                     ? "beatmap.template"
                                     : "beatmap.create";
            switch ( beatmapEvent.m_stage ) {
            case Event::BeatmapCreateInteractionStage::WizardOpened:
                // 两个主题共用同一弹窗入口，来源尚未选择时使用通用信号。
                signal = std::string("beatmap.create.") + origin + ".dialog";
                break;
            case Event::BeatmapCreateInteractionStage::TemplateSelected:
                // 非模板路径不会产生此阶段，仍防御错误构造的事件。
                if ( beatmapEvent.m_fromTemplate ) stage = "selected";
                break;
            case Event::BeatmapCreateInteractionStage::AudioSelected:
                stage = "audio";
                break;
            case Event::BeatmapCreateInteractionStage::TimingMeasured:
                // 模板流程保留源 Timing，不使用单独的自动测量步骤。
                if ( !beatmapEvent.m_fromTemplate ) stage = "timing";
                break;
            case Event::BeatmapCreateInteractionStage::Completed:
                stage = "ready";
                break;
            }
            if ( stage )
                signal = std::string(family) + "." + origin + "." + stage;
        }
        if ( !signal.empty() ) {
            // 重复创建流程仍有独立序号，供正在运行的路线识别本轮动作。
            m_impl->m_signalRevisions.insert_or_assign(
                signal, ++m_impl->m_signalSequence);
            for ( const auto& topic : m_impl->m_topics )
                changed |= m_impl->m_progress.signal(topic, signal);
        }
    }
    // 一帧内多个事件只触发一次持久化写入。
    if ( changed ) m_impl->save();
}
/// @brief 返回已解析并验证的主题目录。
/// @return 服务拥有的只读主题容器。
const std::vector<Topic>& Service::topics() const
{
    return m_impl->m_topics;
}
/// @brief 返回当前学习进度。
/// @return 服务拥有的只读进度对象。
const Progress& Service::progress() const
{
    return m_impl->m_progress;
}

/// @brief 返回步骤任一业务信号在当前进程中的最近到达序号。
/// @param step 待查询步骤；历史持久化记录不会产生本轮序号。
/// @return 所有步骤信号对应序号的最大值，均未出现时返回 0。
std::uint64_t Service::latestSignalRevision(const Step& step) const
{
    std::uint64_t latest = 0;
    for ( const auto& signal : step.m_signals ) {
        const auto it = m_impl->m_signalRevisions.find(signal);
        if ( it != m_impl->m_signalRevisions.end() )
            latest = std::max(latest, it->second);
    }
    return latest;
}

/// @brief 判断步骤需要的业务信号是否在路线步骤启动后重新到达。
/// @param step 待判断步骤；allSignals 为 true 时要求每个信号都重新到达。
/// @param revision 路线步骤启动时记录的信号序号。
/// @return 满足当前步骤信号组合语义时返回 true。
/// @warning UI 热路径：只查询当前步骤的小型信号数组，不分配或访问文件。
bool Service::receivedSignalAfter(const Step&   step,
                                  std::uint64_t revision) const
{
    if ( step.m_signals.empty() ) return false;
    const auto received = [&](const std::string& signal) {
        const auto it = m_impl->m_signalRevisions.find(signal);
        return it != m_impl->m_signalRevisions.end() && it->second > revision;
    };
    if ( step.m_allSignals )
        return std::all_of(
            step.m_signals.begin(), step.m_signals.end(), received);
    return std::any_of(step.m_signals.begin(), step.m_signals.end(), received);
}
/// @brief 返回最近一次加载、解析或保存错误。
/// @return 空字符串表示当前没有记录错误。
const std::string& Service::error() const
{
    return m_impl->m_error;
}
/// @brief 手动完成指定主题步骤并保存进度。
/// @param topic 步骤所属主题。
/// @param step 待确认步骤。
void Service::acknowledge(const Topic& topic, const Step& step)
{
    if ( m_impl->m_progress.acknowledge(topic, step) ) {
        // 空 signal 触发依赖传播，让后续步骤可用性同步更新。
        m_impl->m_progress.signal(topic, "");
        // 只有首次确认产生变化时才写文件。
        m_impl->save();
    }
}
/// @brief 重置一个主题的全部学习记录并保存。
/// @param topic 待重置主题，其他主题不受影响。
void Service::reset(const Topic& topic)
{
    // 明确用户操作可覆盖该主题的手动和自动完成记录。
    m_impl->m_progress.reset(topic);
    m_impl->save();
}
/// @brief 注册或替换一个可信本地操作。
/// @param id 主题步骤引用的稳定操作 ID。
/// @param action 由 C++ 提供的无参操作函数。
void Service::registerAction(std::string id, std::function<void()> action)
{
    // 数据文件只能选择 ID，不能携带可执行脚本或函数地址。
    m_impl->m_actions.insert_or_assign(std::move(id), std::move(action));
}
/// @brief 查询可信操作注册表是否包含指定 ID。
/// @param id 待查询操作 ID。
/// @return 已注册时返回 true。
bool Service::hasAction(std::string_view id) const
{
    return m_impl->m_actions.contains(id);
}
/// @brief 执行指定可信操作；未知 ID 保持无操作。
/// @param id 主题步骤声明的操作 ID。
/// @warning UI 用户触发路径：被调函数的具体副作用由注册方定义。
void Service::execute(std::string_view id)
{
    if ( auto it = m_impl->m_actions.find(id); it != m_impl->m_actions.end() )
        // 只调用注册表内函数，未知或恶意数据 ID 不会执行任意内容。
        it->second();
}
}  // namespace MMM::UI::Walkthrough
