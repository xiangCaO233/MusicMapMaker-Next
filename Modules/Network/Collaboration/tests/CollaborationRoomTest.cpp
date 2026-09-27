#include "network/collaboration/CollaborationRoom.h"
#include "network/collaboration/CollaborationBuildFingerprint.h"
#include "network/collaboration/CollaborationDirectoryClient.h"
#include "network/collaboration_server/CollaborationSignalingServer.h"
#include "runtime/AppThreadPool.h"

#include "log/colorful-log.h"

#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using MMM::BeatMap;
using MMM::BeatmapMutationFlags;
using MMM::Network::Collaboration::CollaborationHostRoomConfig;
using MMM::Network::Collaboration::CollaborationJoinRoomConfig;
using MMM::Network::Collaboration::CollaborationLogEventType;
using MMM::Network::Collaboration::CollaborationRoom;
using MMM::Network::Collaboration::CollaborationRoomState;
using MMM::Network::Collaboration::CollaborationResourceBundle;
using MMM::Network::Collaboration::CollaborationServerEndpoint;
using MMM::Network::Collaboration::PeerId;
using MMM::Network::CollaborationServer::CollaborationSignalingServer;
using MMM::Network::CollaborationServer::CollaborationSignalingServerConfig;

/// @brief 本机 WebRTC 集成测试允许的最长等待时间。
///
/// @details
/// 本文件使用真实信令服务器和 WebRTC DataChannel，连接建立速度会受到
/// 本机调度、候选收集和后台线程启动时间影响。超时值只作为测试失败上限，
/// 轮询循环仍以较短间隔持续推进所有参与者，不把固定等待当作成功条件。
constexpr auto TEST_TIMEOUT = std::chrono::seconds(15);
/// @brief 公网部署探针允许的最长等待时间。
///
/// @details
/// 公网路径额外包含 DNS、TLS 和远端服务调度，因此允许比本机测试更长的
/// 收敛窗口。该上限仅用于显式启用的部署探针，不影响默认 CTest 用例。
constexpr auto EXTERNAL_TEST_TIMEOUT = std::chrono::seconds(30);
/// @brief 验收房间中的访客数量。
///
/// @note 当前测试以一个访客覆盖双向增量和断线清理；涉及旁观者广播的断言
///       保留数量判断，以便后续扩大参与者数量时自动启用对应验证。
constexpr std::size_t GUEST_COUNT = 1;

/// @brief 为真实 WebRTC 测试参与者生成确定的稳定身份。
/// @param ordinal 当前测试房间内的身份序号。
/// @return 满足协议格式的 32 字符小写十六进制标识。
///
/// @details
/// 协议把参与者身份视为跨重连稳定的十六进制字符串。测试使用序号编码而非
/// 随机数，使失败日志和信令消息可复现，同时用固定首字符避免生成全零身份。
/// 序号从字符串尾部按低位优先写入，足以区分同一测试创建的全部参与者。
[[nodiscard]] std::string makeRoomTestParticipantId(std::size_t ordinal)
{
    // 身份字符集与生产协议允许的小写十六进制格式保持一致，避免测试辅助逻辑
    // 绕过真实输入校验。
    constexpr std::string_view DIGITS = "0123456789abcdef";
    std::string                identity(32U, '0');
    // 固定非零前缀保证 ordinal 为零时仍不会退化为协议保留的空身份。
    identity.front() = 'd';
    // 从末尾编码序号可保留稳定且醒目的前缀，并使相邻测试身份只在尾部变化。
    for ( std::size_t index = identity.size(); index > 1U; --index ) {
        identity[index - 1U] = DIGITS[ordinal & 0xFU];
        ordinal >>= 4U;
    }
    return identity;
}

/// @brief 为真实 WebRTC 资源测试创建并清理隔离目录。
///
/// @details
/// 资源同步会真实落盘并重新读取校验，因此不能把输出写入源码树或共享测试
/// 资源目录。该守卫在系统临时目录下建立一次性根目录，并在所有提前返回路径
/// 上清理缓存、宿主资源和替换资源，保证失败用例也不会遗留测试数据。
///
/// @note 构造失败通过空路径表达，调用方必须在任何文件操作前检查 path()。
class ScopedRoomResourceDirectory
{
public:
    /// @brief 创建唯一测试目录。
    ///
    /// @details
    /// 时间戳后缀用于降低并行测试进程之间的目录碰撞概率。所有文件系统 API
    /// 均采用 error_code 重载，以符合项目禁用异常的约束；任一步失败都会把
    /// 路径清空，使调用方走统一失败分支。
    ScopedRoomResourceDirectory()
    {
        std::error_code error;
        auto            root = std::filesystem::temp_directory_path(error);
        // 临时目录不可用时不再拼接相对路径，避免意外写入当前工作目录。
        if ( error ) return;
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = root /
                 ("mmm-collaboration-room-resource-" + std::to_string(suffix));
        std::filesystem::create_directories(m_path, error);
        // 清空失败路径既是状态标记，也阻止析构函数删除并非本类创建的目录。
        if ( error ) m_path.clear();
    }

    /// @brief 删除测试目录。
    ///
    /// @details
    /// 析构阶段不能传播清理错误，测试结论只由同步与内容断言决定。使用
    /// remove_all 的 error_code 重载可覆盖目录内的宿主文件和访客缓存，同时
    /// 保持析构函数 noexcept 语义。
    ~ScopedRoomResourceDirectory()
    {
        // 空路径表示构造未取得目录所有权，此时不得执行递归删除。
        if ( m_path.empty() ) return;
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    ScopedRoomResourceDirectory(const ScopedRoomResourceDirectory&) = delete;
    ScopedRoomResourceDirectory& operator=(const ScopedRoomResourceDirectory&) =
        delete;

    /// @brief 返回测试目录。
    /// @return 已创建目录的绝对路径；创建失败时为空路径。
    /// @warning 返回引用只在守卫对象生命周期内有效。
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    /// @brief 测试目录路径及其递归清理所有权标记。
    std::filesystem::path m_path;
};

/// @brief 构造覆盖物件、时间线、采样和元数据的协作谱面。
/// @param noteTimestamp 根 Note 的可变时间戳，用于区分同步版本。
/// @param author 谱面作者，用于区分元数据增量版本。
/// @return 已完成派生数据同步、可直接提交给 CollaborationRoom 的谱面。
///
/// @details
/// 固定字段覆盖协作文档编解码中的主要数据族：普通 Note、Hold、Polyline
/// 子物件、Timing、音频采样绑定以及各类扩展元数据。测试场景只改变传入的
/// 时间戳或作者，从而能明确判断 Objects 与 Metadata 标志是否保持窄更新。
///
/// @note 所有协作 ID 都显式指定，以验证并发合并按稳定身份而非容器位置工作。
///
/// @par 字段覆盖
/// 构造结果有意采用非默认值，分别承担以下回归探针职责：
///
/// - 六条普通轨道和一条 BGM 轨道验证基础布局元数据；
/// - 根 Note 的可变时间戳代表 Objects 版本差异；
/// - Note 的采样绑定验证 optional 嵌套对象；
/// - 独立 Hold 的持续时间验证区间型物件；
/// - Polyline 同时引用 Hold 与 Flick，验证异构子物件顺序；
/// - Timing 的 BPM、beat length 和效果参数验证时间线字段；
/// - AudioSample 的负偏移验证有符号整数不会被错误转换；
/// - 三类扩展元数据验证映射键和值能跨端保持；
/// - 作者参数代表 Metadata 版本差异，且不会改动物件内容。
std::shared_ptr<BeatMap> makeBeatmap(double noteTimestamp, std::string author)
{
    // 基础元数据同时包含文本、轨道数量和浮点 BPM，覆盖不同 JSON 值类型。
    auto beatmap                               = std::make_shared<BeatMap>();
    beatmap->m_baseMapMetadata.name            = "Collaboration Test";
    beatmap->m_baseMapMetadata.title           = "Transport Integrity";
    beatmap->m_baseMapMetadata.author          = std::move(author);
    beatmap->m_baseMapMetadata.track_count     = 6;
    beatmap->m_baseMapMetadata.bgm_track_count = 1;
    beatmap->m_baseMapMetadata.preference_bpm  = 150.0;

    // 根 Note 携带采样绑定和自定义元数据，用于验证嵌套字段不会在传输中丢失。
    auto& note             = beatmap->m_noteData.notes.emplace_back();
    note.m_timestamp       = noteTimestamp;
    note.m_track           = 2;
    note.m_collaborationId = "room-note-root";
    note.m_sampleBinding   = MMM::AudioSampleBinding{
        .m_audioResourceId = "tap.wav",
        .m_volume          = 0.45F,
    };
    note.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["transport"] =
        "preserved";

    // 独立 Hold 覆盖持续时间字段，并与下方 Polyline 子 Hold 保持不同身份。
    auto& hold             = beatmap->m_noteData.holds.emplace_back();
    hold.m_timestamp       = 2000.0;
    hold.m_duration        = 600.0;
    hold.m_track           = 3;
    hold.m_collaborationId = "room-hold-root";

    // Polyline 的子物件同时保存在按类型容器与引用序列中，编解码后必须重建
    // 引用关系，不能仅比较表层数量。
    auto& subHold              = beatmap->m_noteData.holds.emplace_back();
    subHold.m_timestamp        = 2800.0;
    subHold.m_duration         = 400.0;
    subHold.m_track            = 1;
    subHold.m_isSubNote        = true;
    subHold.m_collaborationId  = "room-polyline-sub-hold";
    auto& subFlick             = beatmap->m_noteData.flicks.emplace_back();
    subFlick.m_timestamp       = 3200.0;
    subFlick.m_track           = 1;
    subFlick.m_dtrack          = 2;
    subFlick.m_isSubNote       = true;
    subFlick.m_collaborationId = "room-polyline-sub-flick";
    auto& polyline             = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_timestamp       = subHold.m_timestamp;
    polyline.m_track           = subHold.m_track;
    polyline.m_collaborationId = "room-polyline-root";
    polyline.m_subHolds.emplace_back(subHold);
    polyline.m_subFlicks.emplace_back(subFlick);
    polyline.m_subNotes.emplace_back(subHold);
    polyline.m_subNotes.emplace_back(subFlick);

    // Timing 同时覆盖标准 BPM 字段、效果枚举以及 Malody 扩展元数据。
    auto& timing                   = beatmap->m_timings.emplace_back();
    timing.m_timestamp             = 0.0;
    timing.m_bpm                   = 150.0;
    timing.m_beat_length           = 400.0;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = 150.0;
    timing.m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["transport"] =
        "preserved";

    // 音频采样使用负偏移和非默认音量，避免默认值掩盖序列化字段缺失。
    auto& sample             = beatmap->m_audioSamples.emplace_back();
    sample.m_timestamp       = 500.0;
    sample.m_offsetMs        = -12;
    sample.m_track           = 6;
    sample.m_audioResourceId = "kick.wav";
    sample.m_volume          = 0.75F;
    sample.m_metadata
        .sample_properties[MMM::SampleMetadataType::MMM]["transport"] =
        "preserved";
    // 协作提交前刷新派生索引，确保测试输入与编辑器正常提交路径一致。
    beatmap->sync();
    return beatmap;
}

/// @brief 驱动房主和全部访客直到条件满足或超时。
/// @tparam Predicate 无参数且返回可转为 bool 的收敛判定器。
/// @param server 本机信令服务器，由当前线程显式推进。
/// @param host 房主房间状态机。
/// @param guests 已创建的全部访客房间状态机。
/// @param predicate 每轮完整推进后检查的目标条件。
/// @return 条件在超时前满足时返回 true，否则返回 false。
///
/// @details
/// 测试没有独立信令事件循环，因此必须依次推进服务器、房主和访客。末尾再次
/// 更新房主，可在同一轮处理访客刚发出的应答或增量，减少对线程调度偶然性的
/// 依赖。条件始终在一轮状态推进之后检查，避免观察到半处理状态。
///
/// @warning 该函数只用于集成测试轮询；5ms sleep 是测试让出 CPU 的方式，
///          不得复制到生产逻辑、渲染或 UI 热路径。
///
/// @par 推进顺序约束
/// 信令服务器必须先消费上一轮客户端输出；房主随后处理服务端事件并可能生成
/// 对访客的消息；全部访客各推进一次后，房主再消费访客即时应答。若删去最后
/// 一次房主更新，逻辑仍可能最终成功，但每个协议往返会多占一轮并放大测试
/// 对调度延迟的敏感性。
///
/// @par 谓词约束
/// predicate 应只读取状态，不应主动发送消息或修改房间。这样每个循环周期都
/// 保持“推进后观察”的单向结构，超时日志对应的也是最后一个完整处理轮次。
template<typename Predicate>
bool pumpUntil(CollaborationSignalingServer& server, CollaborationRoom& host,
               std::vector<std::unique_ptr<CollaborationRoom>>& guests,
               Predicate                                        predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        // 保持固定推进顺序，使信令请求、房主响应和访客消费形成可复现的一轮。
        server.update();
        host.update();
        for ( auto& guest : guests ) guest->update();
        host.update();
        // 仅在全部参与者都完成本轮更新后观察跨端不变量。
        if ( predicate() ) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/// @brief 统计指定类型的协作日志条数。
/// @param room 待读取的房间；测试线程保证其日志此时不被并发修改。
/// @param type 需要匹配的结构化日志事件类型。
/// @return 日志中类型完全相等的条目数量。
///
/// @details 日志数量用于验证协议阶段只发生一次或至少发生预期次数，而不依赖
///          面向诊断的详情文本。
std::size_t countLogs(const CollaborationRoom&  room,
                      CollaborationLogEventType type)
{
    return static_cast<std::size_t>(std::count_if(
        room.logs().begin(), room.logs().end(), [type](const auto& entry) {
            return entry.type == type;
        }));
}

/// @brief 统计指定详情的协作日志条数。
/// @param room 待读取的房间。
/// @param detail 需要精确匹配的稳定诊断标识。
/// @return 详情字段完全相等的条目数量。
///
/// @details 该辅助函数只用于断言特定失败没有出现，例如无效文档或本地提交
///          失败；它不把普通说明文本当作协议状态。
std::size_t countLogDetails(const CollaborationRoom& room,
                            std::string_view         detail)
{
    return static_cast<std::size_t>(std::count_if(
        room.logs().begin(), room.logs().end(), [detail](const auto& entry) {
            return entry.detail == detail;
        }));
}

/// @brief 判断谱面是否包含指定同步结果。
/// @param beatmap 待检查的不可变谱面快照。
/// @param noteTimestamp 根 Note 预期时间戳。
/// @param author 预期作者元数据。
/// @return 所有顶层数量、关键字段和嵌套引用均符合基准时返回 true。
///
/// @details
/// 该断言有意同时检查稳定字段与每轮可变字段：若对象更新意外覆盖元数据，
/// 或元数据更新丢失物件，都会在同一收敛检查中暴露。浮点字段采用小容差，
/// 其余协议字段和扩展元数据要求精确保持。
///
/// @par 判定层次
/// 检查顺序从结构安全逐步深入到业务语义：
///
/// - 先验证指针和所有顶层容器基数，保证后续 front() 安全；
/// - 再验证根 Note 的坐标、采样绑定、音量和扩展元数据；
/// - 验证独立 Hold 的时间区间及轨道；
/// - 验证 Polyline 的子容器数量、引用顺序、子物件标记和动态类型；
/// - 验证 Timing 的数值、效果枚举和 Malody 扩展字段；
/// - 验证音频采样的资源 ID、偏移、轨道、音量和扩展字段；
/// - 最后验证谱面基础元数据以及调用方指定的作者版本。
///
/// 这种完整比较使测试能把“状态最终看似可用但部分字段已丢失”识别为失败，
/// 而不是只用一个时间戳作为同步成功的替代指标。
bool hasExpectedState(const std::shared_ptr<const BeatMap>& beatmap,
                      double noteTimestamp, std::string_view author)
{
    // 先校验容器基数再访问 front()，既表达文档完整性也避免失败路径越界。
    if ( !beatmap || beatmap->m_noteData.notes.size() != 1U ||
         beatmap->m_noteData.holds.size() != 2U ||
         beatmap->m_noteData.flicks.size() != 1U ||
         beatmap->m_noteData.polylines.size() != 1U ||
         beatmap->m_timings.size() != 1U ||
         beatmap->m_audioSamples.size() != 1U ) {
        return false;
    }
    // 取出各数据族的代表元素，后续断言覆盖传输时最容易遗漏的嵌套字段。
    const auto& note     = beatmap->m_noteData.notes.front();
    const auto& hold     = beatmap->m_noteData.holds.front();
    const auto& timing   = beatmap->m_timings.front();
    const auto& sample   = beatmap->m_audioSamples.front();
    const auto& polyline = beatmap->m_noteData.polylines.front();
    // Polyline 的引用顺序和动态类型同样属于协作文档语义，不能只比较根容器。
    return std::abs(note.m_timestamp - noteTimestamp) < 1e-6 &&
           note.m_track == 2U && note.m_sampleBinding &&
           note.m_sampleBinding->m_audioResourceId == "tap.wav" &&
           std::abs(note.m_sampleBinding->m_volume - 0.45F) < 1e-6F &&
           note.m_metadata.note_properties.at(MMM::NoteMetadataType::MMM)
                   .at("transport") == "preserved" &&
           std::abs(hold.m_timestamp - 2000.0) < 1e-6 &&
           std::abs(hold.m_duration - 600.0) < 1e-6 && hold.m_track == 3U &&
           polyline.m_subNotes.size() == 2U &&
           polyline.m_subHolds.size() == 1U &&
           polyline.m_subFlicks.size() == 1U &&
           polyline.m_subNotes[0].get().m_isSubNote &&
           polyline.m_subNotes[0].get().m_type == MMM::NoteType::HOLD &&
           polyline.m_subNotes[1].get().m_isSubNote &&
           polyline.m_subNotes[1].get().m_type == MMM::NoteType::FLICK &&
           std::abs(timing.m_timestamp) < 1e-6 &&
           std::abs(timing.m_bpm - 150.0) < 1e-6 &&
           std::abs(timing.m_beat_length - 400.0) < 1e-6 &&
           timing.m_timingEffect == MMM::TimingEffect::BPM &&
           timing.m_metadata.timing_properties
                   .at(MMM::TimingMetadataType::MALODY)
                   .at("transport") == "preserved" &&
           std::abs(sample.m_timestamp - 500.0) < 1e-6 &&
           sample.m_offsetMs == -12 && sample.m_track == 6U &&
           sample.m_audioResourceId == "kick.wav" &&
           std::abs(sample.m_volume - 0.75F) < 1e-6F &&
           sample.m_metadata.sample_properties.at(MMM::SampleMetadataType::MMM)
                   .at("transport") == "preserved" &&
           beatmap->m_baseMapMetadata.name == "Collaboration Test" &&
           beatmap->m_baseMapMetadata.title == "Transport Integrity" &&
           beatmap->m_baseMapMetadata.author == author &&
           beatmap->m_baseMapMetadata.track_count == 6 &&
           beatmap->m_baseMapMetadata.bgm_track_count == 1 &&
           std::abs(beatmap->m_baseMapMetadata.preference_bpm - 150.0) < 1e-6;
}

/// @brief 判断谱面根 Note 是否包含全部指定时间戳。
/// @param beatmap 待检查的不可变谱面快照。
/// @param timestamps 期望出现且数量完全相同的根 Note 时间戳集合。
/// @return 数量相等且每个期望时间戳均能找到时返回 true。
///
/// @details 并发合并不承诺容器顺序，因此按集合语义查找；先要求数量相等，
///          防止重复或额外 Note 被“至少包含”式判断遗漏。
bool hasRootNoteTimestamps(const std::shared_ptr<const BeatMap>& beatmap,
                           std::initializer_list<double>         timestamps)
{
    if ( !beatmap || beatmap->m_noteData.notes.size() != timestamps.size() ) {
        return false;
    }
    return std::all_of(timestamps.begin(), timestamps.end(), [&](double value) {
        return std::any_of(beatmap->m_noteData.notes.begin(),
                           beatmap->m_noteData.notes.end(),
                           [value](const MMM::Note& note) {
                               return std::abs(note.m_timestamp - value) < 1e-6;
                           });
    });
}

/// @brief 判断房主和访客连续写入的稳定 ID 物件是否全部收敛。
/// @param beatmap 待检查的合并后谱面。
/// @param count 房主与访客各自提交的突发 Note 数量。
/// @return 基准 Note 与两侧全部稳定 ID 均存在时返回 true。
///
/// @details
/// 时间戳可验证数据但不能可靠充当协作身份，因此本断言专门按 collaborationId
/// 检查每一轮双向写入。期望总数包含一个基准 Note 和此前两条并发 Note，
/// 再加本轮房主与访客各 count 条，借此同时发现丢失和重复应用。
bool hasConcurrentBurstNotes(const std::shared_ptr<const BeatMap>& beatmap,
                             std::size_t                           count)
{
    if ( !beatmap || beatmap->m_noteData.notes.size() != 3U + count * 2U ) {
        return false;
    }
    for ( std::size_t index = 0; index < count; ++index ) {
        // 每个序号必须同时拥有房主和访客版本，任一方向丢包都会立即失败。
        const auto contains = [&](std::string_view prefix) {
            const std::string identity =
                std::string(prefix) + std::to_string(index);
            return std::any_of(beatmap->m_noteData.notes.begin(),
                               beatmap->m_noteData.notes.end(),
                               [&identity](const MMM::Note& note) {
                                   return note.m_collaborationId == identity;
                               });
        };
        if ( !contains("host-burst-") || !contains("guest-burst-") ) {
            return false;
        }
    }
    return true;
}

/// @brief 覆盖中心信令、真实 WebRTC、初始快照和双向增量收敛。
/// @return 所有连接、同步、并发合并、视口广播和断线清理断言通过时返回 true。
///
/// @details
/// 本用例是 CollaborationRoom 的端到端主流程验收，实际启动本机信令服务并
/// 建立 WebRTC DataChannel，而非使用传输替身。验证顺序刻意贴近真实会话：
///
/// - 房主发布房间并在访客加入前提交初始快照；
/// - 访客经过人工审批后取得参与者列表和完整谱面；
/// - 房主、访客分别提交增量，并验证窄 mutation flags；
/// - 双端同时新增带稳定 ID 的物件，验证重放与重基合并；
/// - 连续突发更新验证序列确认不会让旧快照覆盖本地新状态；
/// - 房主发布视口后断开，验证访客清空全部会话态。
///
/// @note 任何阶段失败都会保留尽可能具体的结构化日志，便于区分信令、P2P、
///       文档合并与清理问题。
///
/// @par 初始同步不变量
/// 房主在访客出现前提交完整谱面。访客获准后，只有在以下条件同时成立时才
/// 进入增量阶段：
///
/// - 房主参与者列表包含自身和全部访客；
/// - 每个访客处于 Connected，而非停留在信令已连状态；
/// - 每个访客参与者列表与房主观察一致；
/// - 每个访客模型通过完整字段比较；
/// - 房主本地提交没有触发多余的本地应用回调。
///
/// @par 并发编辑不变量
/// 双端编辑都先更新本地模型再提交，刻意模拟 UI 的即时反馈。协作层必须把
/// 两个基于同一旧版本的 Objects 变更按稳定 ID 合并，并满足：
///
/// - 两端最终包含双方新增的物件；
/// - 远端应用标志仍为 Objects；
/// - 对象差量在多访客配置下包含双方新增 ID；
/// - 同步确认后，两端以同一合并结果作为下一阶段基线。
///
/// @par 突发与序列不变量
/// 连续提交不等待单次网络往返，因此会同时存在多份在途版本。最终状态必须
/// 包含每个序号对应的 host-burst 与 guest-burst ID，且访客最后本地序列必须
/// 被快照 included sequence 或独立 acknowledge 覆盖。测试还要求中间过程没有
/// 记录 invalid_beatmap_document，避免最终快照掩盖临时损坏。
///
/// @par 断线清理不变量
/// 房主断开后，访客不仅要离开 Connected，还必须清除 peer ID、参与者列表、
/// 视口缓存和权限。保留非空 lastError 与唯一 HostDisconnected 日志，使上层
/// 可以向用户说明会话终止原因，同时不会把同一事件重复上报。
bool testPublicDirectoryWebRtcRoom()
{
    // 使用系统分配端口避免并行 CTest 实例争抢固定端口；回环地址保证该用例
    // 不依赖外网或防火墙配置。
    CollaborationSignalingServerConfig serverConfig;
    serverConfig.port        = 0;
    serverConfig.bindAddress = "127.0.0.1";
    // 信令服务器只负责发现和配对，后续谱面数据仍通过真实 P2P 通道传输。
    CollaborationSignalingServer server;
    if ( !server.start(std::move(serverConfig)) ) return false;
    // 给服务器后台监听线程一次启动机会；成功仍由后续发布与状态断言决定。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // 客户端端点必须使用服务器实际绑定的动态端口。
    CollaborationServerEndpoint endpoint;
    endpoint.address       = "127.0.0.1";
    endpoint.signalingPort = server.listeningPort();
    endpoint.useTls        = false;

    // 第一阶段：房主注册稳定身份并等待服务端返回公开房间 ID。
    CollaborationRoom           host;
    CollaborationHostRoomConfig hostConfig;
    hostConfig.creator       = "Host Creator";
    hostConfig.participantId = makeRoomTestParticipantId(1);
    hostConfig.roomName      = "Public WebRTC Test";
    hostConfig.endpoint      = endpoint;
    if ( !host.startHost(hostConfig) ) return false;
    // 访客使用独立房间实例模拟不同客户端；unique_ptr 保证 vector 扩容时实例
    // 地址稳定，避免底层回调捕获的 this 失效。
    std::vector<std::unique_ptr<CollaborationRoom>> guests;
    if ( !pumpUntil(
             server, host, guests, [&]() { return !host.roomId().empty(); }) ) {
        return false;
    }

    // 第二阶段：安装编辑器侧应用回调，并在访客加入前建立房主权威快照。
    // hostModel 模拟 UI/会话层当前持有的不可变文档版本。
    std::shared_ptr<const BeatMap> hostModel;
    std::size_t                    hostApplyCount = 0;
    host.setApplyBeatmapCallback(
        [&hostModel, &hostApplyCount](std::shared_ptr<const BeatMap> beatmap,
                                      BeatmapMutationFlags,
                                      std::uint64_t,
                                      std::uint64_t,
                                      std::optional<std::vector<std::string>>) {
            // 回调只记录真正由协作层应用的快照；本地提交不应反向回调自身。
            hostModel = std::move(beatmap);
            ++hostApplyCount;
        });
    // 初始文档在访客连接前提交，用于验证晚加入者会收到当前权威状态。
    auto initial = makeBeatmap(1000.0, "Host Creator");
    hostModel    = initial;
    static_cast<void>(
        host.onBeatmapMutated(*initial, BeatmapMutationFlags::All));
    host.update();
    // 本地模型由调用方直接更新，因此协作房间不得重复应用并增加计数。
    if ( !hasExpectedState(hostModel, 1000.0, "Host Creator") ||
         hostApplyCount != 0U ) {
        return false;
    }

    // 第三阶段：为每个访客分别记录快照、更新类型、对象 ID 增量和序列确认。
    // 这些并行数组按 guest index 对齐，回调只捕获稳定索引。
    std::vector<std::shared_ptr<const BeatMap>> guestModels(GUEST_COUNT);
    std::vector<std::size_t>                    guestApplyCounts(GUEST_COUNT);
    std::vector<std::vector<BeatmapMutationFlags>> guestApplyFlags(GUEST_COUNT);
    std::vector<std::vector<std::optional<std::vector<std::string>>>>
                                            guestApplyObjectDeltas(GUEST_COUNT);
    std::vector<std::vector<std::uint64_t>> guestApplyMutationSequences(
        GUEST_COUNT);
    std::vector<std::vector<std::uint64_t>> guestAcknowledgedMutationSequences(
        GUEST_COUNT);
    // required sequence 是 UI 层防止旧远端快照覆盖本地未确认编辑的水位线。
    std::vector<std::uint64_t> guestRequiredMutationSequences(GUEST_COUNT);
    guests.reserve(GUEST_COUNT);
    for ( std::size_t index = 0; index < GUEST_COUNT; ++index ) {
        // 每个访客在 join 前安装回调，确保初始快照也进入测试模型。
        auto guest = std::make_unique<CollaborationRoom>();
        guest->setApplyBeatmapCallback(
            [&guestModels,
             &guestApplyCounts,
             &guestApplyFlags,
             &guestApplyObjectDeltas,
             &guestApplyMutationSequences,
             &guestRequiredMutationSequences,
             index](std::shared_ptr<const BeatMap> beatmap,
                    BeatmapMutationFlags           flags,
                    std::uint64_t includedLocalMutationSequence,
                    std::uint64_t,
                    std::optional<std::vector<std::string>>
                        objectDeltaIdentities) {
                // 模拟编辑器的防回退策略：只接受已经包含当前本地水位线的快照。
                if ( includedLocalMutationSequence <
                     guestRequiredMutationSequences[index] ) {
                    return;
                }
                // 同时保存 flags、对象差量和序列，后续可验证协作层没有扩大刷新
                // 范围，也没有遗漏本地操作确认。
                guestModels[index] = std::move(beatmap);
                ++guestApplyCounts[index];
                guestApplyFlags[index].push_back(flags);
                guestApplyObjectDeltas[index].push_back(
                    std::move(objectDeltaIdentities));
                guestApplyMutationSequences[index].push_back(
                    includedLocalMutationSequence);
            });
        // 某些操作无需重新下发完整文档，仅通过确认回调推进本地水位线，因此
        // 测试把 apply 与 acknowledge 两种路径共同纳入收敛判断。
        guest->setLocalMutationAcknowledgedCallback(
            [&guestAcknowledgedMutationSequences,
             &guestRequiredMutationSequences,
             index](std::uint64_t sequence) {
                guestAcknowledgedMutationSequences[index].push_back(sequence);
                // 水位线只能单调前进，乱序确认不得允许更旧快照重新生效。
                guestRequiredMutationSequences[index] =
                    std::max(guestRequiredMutationSequences[index], sequence);
            });
        // 参与者身份按索引确定生成，创建者名称只用于参与者列表和日志辨识。
        CollaborationJoinRoomConfig guestConfig;
        guestConfig.creator       = "Guest " + std::to_string(index + 1);
        guestConfig.participantId = makeRoomTestParticipantId(index + 2);
        guestConfig.roomId        = host.roomId();
        guestConfig.roomName      = host.roomName();
        guestConfig.endpoint      = endpoint;
        if ( !guest->join(guestConfig) ) return false;
        guests.push_back(std::move(guest));
        // 加入必须先停在 AwaitingApproval；若直接
        // Connected，会绕过房主准入契约。
        if ( !pumpUntil(
                 server,
                 host,
                 guests,
                 [&]() {
                     return guests.back()->state() ==
                                CollaborationRoomState::AwaitingApproval &&
                            !host.pendingJoinRequests().empty();
                 }) ||
             !host.approveJoinRequest(
                 host.pendingJoinRequests().front().requestId) ) {
            return false;
        }
        // 审批后等待真实 SDP/ICE/DataChannel 握手完成，而非只观察信令响应。
        if ( !pumpUntil(server, host, guests, [&]() {
                 return guests.back()->state() ==
                        CollaborationRoomState::Connected;
             }) ) {
            // 分阶段连接失败通常与候选交换或通道状态有关，输出两端和服务器
            // 状态可避免只有一个布尔断言而无法定位根因。
            XERROR(
                "Guest {} failed during staged connection: state={}, error={}",
                index + 1U,
                static_cast<int>(guests.back()->state()),
                guests.back()->lastError());
            for ( const auto& entry : guests.back()->logs() ) {
                XERROR("Guest {} staged log type={} detail={}",
                       index + 1U,
                       static_cast<int>(entry.type),
                       entry.detail);
            }
            XERROR("Server clients={}, rooms={}, host participants={}",
                   server.clientCount(),
                   server.roomCount(),
                   host.participants().size());
            for ( const auto& entry : host.logs() ) {
                XERROR("Host staged log type={} detail={}",
                       static_cast<int>(entry.type),
                       entry.detail);
            }
            return false;
        }
    }

    // 第四阶段：连接成功还不够，参与者视图和访客初始文档必须同时收敛。
    const bool connected = pumpUntil(server, host, guests, [&]() {
        // 房主列表包含自身，故期望值为访客数量加一。
        if ( host.participants().size() != GUEST_COUNT + 1U ) return false;
        for ( std::size_t index = 0; index < guests.size(); ++index ) {
            if ( guests[index]->state() != CollaborationRoomState::Connected ||
                 guests[index]->participants().size() != GUEST_COUNT + 1U ||
                 !hasExpectedState(
                     guestModels[index], 1000.0, "Host Creator") ) {
                return false;
            }
        }
        return true;
    });
    if ( !connected ) {
        // 超时诊断逐访客输出状态、peer ID、参与者数量和协议日志，区分单端
        // DataChannel 未建立与初始文档未到达两类失败。
        XERROR(
            "Collaboration test connection timeout: host participants={}, "
            "state={}, error={}",
            host.participants().size(),
            static_cast<int>(host.state()),
            host.lastError());
        for ( std::size_t index = 0; index < guests.size(); ++index ) {
            XERROR("Guest {} state={}, peer={}, participants={}, error={}",
                   index + 1U,
                   static_cast<int>(guests[index]->state()),
                   guests[index]->localPeerId(),
                   guests[index]->participants().size(),
                   guests[index]->lastError());
            for ( const auto& entry : guests[index]->logs() ) {
                XERROR("Guest {} log type={} detail={}",
                       index + 1U,
                       static_cast<int>(entry.type),
                       entry.detail);
            }
        }
        return false;
    }
    // 通知协作层上层已经应用初始快照，并清空观测记录，为后续增量断言建立
    // 干净基线；这不会改变房间内的权威文档。
    for ( std::size_t index = 0; index < guests.size(); ++index ) {
        guests[index]->onBeatmapSynchronized(*guestModels[index]);
        guestApplyFlags[index].clear();
        guestApplyObjectDeltas[index].clear();
        guestApplyMutationSequences[index].clear();
        guestAcknowledgedMutationSequences[index].clear();
    }

    // 第五阶段：房主和访客从同一基线各新增一个物件，并在尚未交换增量时连续
    // 提交，验证按 collaborationId 合并而不是后到版本整表覆盖。
    auto  hostConcurrent      = makeBeatmap(1000.0, "Host Creator");
    auto& hostAddedNote       = hostConcurrent->m_noteData.notes.emplace_back();
    hostAddedNote.m_timestamp = 3000.0;
    hostAddedNote.m_track     = 0;
    hostAddedNote.m_collaborationId = "host-concurrent-note";
    hostConcurrent->sync();
    auto  guestConcurrent = makeBeatmap(1000.0, "Host Creator");
    auto& guestAddedNote  = guestConcurrent->m_noteData.notes.emplace_back();
    guestAddedNote.m_timestamp       = 4000.0;
    guestAddedNote.m_track           = 1;
    guestAddedNote.m_collaborationId = "guest-concurrent-note";
    guestConcurrent->sync();
    // 模拟两端 UI 先立即采用各自本地编辑，再把变更交给协作层发送。
    hostModel           = hostConcurrent;
    guestModels.front() = guestConcurrent;
    static_cast<void>(
        host.onBeatmapMutated(*hostConcurrent, BeatmapMutationFlags::Objects));
    static_cast<void>(guests.front()->onBeatmapMutated(
        *guestConcurrent, BeatmapMutationFlags::Objects));
    // 两端最终都必须保留基准 Note 和两条并发新增 Note，顺序不作要求。
    const bool concurrentEditsConverged =
        pumpUntil(server, host, guests, [&]() {
            return hasRootNoteTimestamps(hostModel,
                                         { 1000.0, 3000.0, 4000.0 }) &&
                   hasRootNoteTimestamps(guestModels.front(),
                                         { 1000.0, 3000.0, 4000.0 });
        });
    if ( !concurrentEditsConverged ) {
        XERROR(
            "Concurrent host and guest object additions overwrote each other");
        return false;
    }
    // 并发重基仍然只涉及 Objects；若扩成 All，会造成无关元数据和时间线刷新。
    if ( guestApplyFlags.front().empty() ||
         std::any_of(guestApplyFlags.front().begin(),
                     guestApplyFlags.front().end(),
                     [](BeatmapMutationFlags flags) {
                         return flags != BeatmapMutationFlags::Objects;
                     }) ) {
        XERROR("Concurrent object rebase widened its replacement flags");
        return false;
    }
    // 多访客配置下，未参与编辑的后台访客必须一次观察到两个稳定 ID 的差量。
    // 当前单访客配置跳过此分支，但保留契约便于扩大覆盖规模。
    const bool backgroundDeltaDelivered =
        GUEST_COUNT < 2U ||
        std::any_of(
            guestApplyObjectDeltas[1].begin(),
            guestApplyObjectDeltas[1].end(),
            [](const auto& delta) {
                return delta &&
                       std::find(delta->begin(),
                                 delta->end(),
                                 "host-concurrent-note") != delta->end() &&
                       std::find(delta->begin(),
                                 delta->end(),
                                 "guest-concurrent-note") != delta->end();
            });
    if ( !backgroundDeltaDelivered ) {
        XERROR("Remote object updates did not expose background ID deltas");
        return false;
    }
    // 双端确认已经采用合并结果，后续 burst 从同一权威版本继续。
    host.onBeatmapSynchronized(*hostModel);
    guests.front()->onBeatmapSynchronized(*guestModels.front());

    // 第六阶段：连续交错提交 32 组物件，覆盖发送队列合并、序列确认和远端
    // 重基压力，同时保持测试运行时间可控。
    constexpr std::size_t CONCURRENT_BURST_COUNT = 32;
    auto                  hostBurst = makeBeatmap(1000.0, "Host Creator");
    // 先补回上一阶段的两条并发结果，使突发编辑基线与房间权威文档一致。
    auto& hostBurstEarlier       = hostBurst->m_noteData.notes.emplace_back();
    hostBurstEarlier.m_timestamp = 3000.0;
    hostBurstEarlier.m_track     = 0;
    hostBurstEarlier.m_collaborationId = "host-concurrent-note";
    auto& hostBurstGuestEarlier = hostBurst->m_noteData.notes.emplace_back();
    hostBurstGuestEarlier.m_timestamp       = 4000.0;
    hostBurstGuestEarlier.m_track           = 1;
    hostBurstGuestEarlier.m_collaborationId = "guest-concurrent-note";
    // 访客从完全相同的三 Note 基线出发，之后两端各维护自己的本地增长版本。
    auto guestBurst                      = makeBeatmap(1000.0, "Host Creator");
    guestBurst->m_noteData.notes         = hostBurst->m_noteData.notes;
    std::uint64_t lastGuestBurstSequence = 0;
    for ( std::size_t index = 0; index < CONCURRENT_BURST_COUNT; ++index ) {
        // 每轮先提交房主版本，再立即提交尚未收到该版本的访客版本，持续制造
        // 合法并发而不是串行等待每次同步完成。
        auto& hostNote             = hostBurst->m_noteData.notes.emplace_back();
        hostNote.m_timestamp       = 5000.0 + static_cast<double>(index);
        hostNote.m_track           = static_cast<std::uint32_t>(index % 6U);
        hostNote.m_collaborationId = "host-burst-" + std::to_string(index);
        hostBurst->sync();
        static_cast<void>(
            host.onBeatmapMutated(*hostBurst, BeatmapMutationFlags::Objects));

        auto& guestNote       = guestBurst->m_noteData.notes.emplace_back();
        guestNote.m_timestamp = 6000.0 + static_cast<double>(index);
        guestNote.m_track     = static_cast<std::uint32_t>(index % 6U);
        guestNote.m_collaborationId = "guest-burst-" + std::to_string(index);
        guestBurst->sync();
        // 只需保存访客最后一个序列；它隐含此前全部有序本地提交的确认水位线。
        lastGuestBurstSequence = guests.front()->onBeatmapMutated(
            *guestBurst, BeatmapMutationFlags::Objects);
    }
    // 循环中协作回调可能尚未运行，先让上层模型反映各自最后一次本地编辑。
    hostModel                              = hostBurst;
    guestModels.front()                    = guestBurst;
    guestRequiredMutationSequences.front() = lastGuestBurstSequence;
    // 收敛要求同时满足：最终文档完整、序列有效且两端没有记录无效文档。
    // 后一条件可捕获中间合并产生损坏但最终又被后续快照覆盖的隐蔽问题。
    if ( lastGuestBurstSequence == 0 ||
         !pumpUntil(server,
                    host,
                    guests,
                    [&]() {
                        return hasConcurrentBurstNotes(
                                   hostModel, CONCURRENT_BURST_COUNT) &&
                               hasConcurrentBurstNotes(guestModels.front(),
                                                       CONCURRENT_BURST_COUNT);
                    }) ||
         countLogDetails(host, "invalid_beatmap_document") != 0U ||
         countLogDetails(*guests.front(), "invalid_beatmap_document") != 0U ) {
        XERROR("Concurrent object burst failed to converge without errors");
        return false;
    }
    host.onBeatmapSynchronized(*hostModel);
    guests.front()->onBeatmapSynchronized(*guestModels.front());

    // 第七阶段：双方同时把突发新增全部删除，验证并发删除同样能收敛回基准。
    // 若 tombstone 或版本水位线处理错误，旧 Note 会残留或重新出现。
    auto hostReset      = makeBeatmap(1000.0, "Host Creator");
    auto guestReset     = makeBeatmap(1000.0, "Host Creator");
    hostModel           = hostReset;
    guestModels.front() = guestReset;
    static_cast<void>(
        host.onBeatmapMutated(*hostReset, BeatmapMutationFlags::Objects));
    static_cast<void>(guests.front()->onBeatmapMutated(
        *guestReset, BeatmapMutationFlags::Objects));
    if ( !pumpUntil(server, host, guests, [&]() {
             return hasExpectedState(hostModel, 1000.0, "Host Creator") &&
                    hasExpectedState(
                        guestModels.front(), 1000.0, "Host Creator");
         }) ) {
        XERROR("Concurrent object cleanup did not converge");
        return false;
    }
    host.onBeatmapSynchronized(*hostModel);
    guests.front()->onBeatmapSynchronized(*guestModels.front());

    // 第八阶段：访客修改根 Note 时间戳，重点验证提交序列通过 apply 或 ack
    // 至少一种路径得到确认，旧远端文档不会覆盖访客即时本地视图。
    const auto guestApplyCountBeforeLocalEdit = guestApplyCounts.front();
    auto       guestEdit                 = makeBeatmap(1250.0, "Host Creator");
    guestModels.front()                  = guestEdit;
    const auto guestEditMutationSequence = guests.front()->onBeatmapMutated(
        *guestEdit, BeatmapMutationFlags::Objects);
    guestRequiredMutationSequences.front() = guestEditMutationSequence;
    const bool guestEditConverged = pumpUntil(server, host, guests, [&]() {
        // 房主先采用访客编辑，证明上行增量已经穿过 DataChannel 并提交。
        if ( !hasExpectedState(hostModel, 1250.0, "Host Creator") ) {
            return false;
        }
        // 完整快照回调可能携带 included sequence；若无需回放快照，则独立 ack
        // 回调也可以确认同一操作，因此取两者较大值。
        const auto appliedSequence =
            guestApplyMutationSequences.front().empty()
                ? 0
                : *std::max_element(guestApplyMutationSequences.front().begin(),
                                    guestApplyMutationSequences.front().end());
        const auto acknowledgedSequence =
            guestAcknowledgedMutationSequences.front().empty()
                ? 0
                : *std::max_element(
                      guestAcknowledgedMutationSequences.front().begin(),
                      guestAcknowledgedMutationSequences.front().end());
        // 除序列水位线外，全部访客模型也必须已经采用相同业务内容。
        return std::max(appliedSequence, acknowledgedSequence) >=
                   guestEditMutationSequence &&
               std::all_of(guestModels.begin(),
                           guestModels.end(),
                           [](const auto& model) {
                               return hasExpectedState(
                                   model, 1250.0, "Host Creator");
                           });
    });
    // 超时后重新计算最终观测水位线，用于给失败日志提供精确序列证据。
    const auto latestGuestAppliedMutationSequence = std::max(
        guestApplyMutationSequences.front().empty()
            ? 0
            : *std::max_element(guestApplyMutationSequences.front().begin(),
                                guestApplyMutationSequences.front().end()),
        guestAcknowledgedMutationSequences.front().empty()
            ? 0
            : *std::max_element(
                  guestAcknowledgedMutationSequences.front().begin(),
                  guestAcknowledgedMutationSequences.front().end()));
    if ( !guestEditConverged || guestEditMutationSequence == 0 ||
         latestGuestAppliedMutationSequence < guestEditMutationSequence ) {
        XERROR(
            "Guest edit failed to converge or acknowledge its local view: "
            "participants={}, applyBefore={}, applyAfter={}, mutation={}, "
            "included={}",
            host.participants().size(),
            guestApplyCountBeforeLocalEdit,
            guestApplyCounts.front(),
            guestEditMutationSequence,
            latestGuestAppliedMutationSequence);
        return false;
    }

    // 第九阶段：房主只修改作者元数据，验证 Metadata 标志的下行同步不会丢失
    // 已由访客提交的对象时间戳。
    auto hostEdit = makeBeatmap(1250.0, "Host Revised");
    hostModel     = hostEdit;
    static_cast<void>(
        host.onBeatmapMutated(*hostEdit, BeatmapMutationFlags::Metadata));
    const bool hostEditConverged = pumpUntil(server, host, guests, [&]() {
        if ( !hasExpectedState(hostModel, 1250.0, "Host Revised") ) {
            return false;
        }
        return std::all_of(
            guestModels.begin(), guestModels.end(), [](const auto& model) {
                return hasExpectedState(model, 1250.0, "Host Revised");
            });
    });
    // 应用次数使用下界而非严格值，因为真实通道可按时序拆分合法中间快照；
    // 非编辑访客的严格分支仅在未来 GUEST_COUNT 扩大时生效。
    if ( !hostEditConverged || hostApplyCount < 3U ||
         guestApplyCounts.front() < 4U ||
         !std::all_of(
             guestApplyCounts.begin() + 1,
             guestApplyCounts.end(),
             [](std::size_t count) { return count == 3U; }) ||
         countLogs(host, CollaborationLogEventType::OperationCommitted) < 3U ) {
        XERROR(
            "Host edit validation failed: converged={}, hostApply={}, "
            "firstGuestApply={}, hostLogs={}",
            hostEditConverged,
            hostApplyCount,
            guestApplyCounts.front(),
            countLogs(host, CollaborationLogEventType::OperationCommitted));
        return false;
    }

    // 第十阶段：先发布房主视口并确认访客缓存，再验证断线会清除该临时状态。
    const PeerId hostPeerId = host.localPeerId();
    host.publishLocalViewport({
        .playbackTime          = 1.25,
        .visualTime            = 1.5,
        .visibleTimeStart      = 0.0,
        .visibleTimeEnd        = 4.0,
        .horizontalOffsetRatio = 0.2,
    });
    if ( !pumpUntil(server, host, guests, [&]() {
             return std::all_of(
                 guests.begin(), guests.end(), [hostPeerId](const auto& guest) {
                     return guest->participantViewports().contains(hostPeerId);
                 });
         }) ) {
        XERROR("Guests did not receive the host viewport before disconnect");
        return false;
    }

    // 房主主动断开应让所有访客回到可重新加入的 Idle 状态，并撤销 peer ID、
    // 权限、参与者和视口缓存；HostDisconnected 日志必须恰好记录一次。
    host.disconnect();
    return pumpUntil(server, host, guests, [&]() {
        return std::all_of(guests.begin(), guests.end(), [](const auto& guest) {
            return guest->state() == CollaborationRoomState::Idle &&
                   !guest->isActive() && guest->localPeerId() == 0 &&
                   guest->participants().empty() &&
                   guest->participantViewports().empty() &&
                   guest->localPermissions() == 0U &&
                   !guest->lastError().empty() &&
                   countLogs(*guest,
                             CollaborationLogEventType::HostDisconnected) == 1U;
        });
    });
}

/// @brief 验证房主可以拒绝待审批访客并移出已经获准的访客。
/// @return 拒绝、批准、移除及移除后静默提交均符合契约时返回 true。
///
/// @details
/// 准入控制包含两个容易混淆的状态转换。本用例先拒绝一个仍处于审批队列的
/// 访客，要求其进入 Error 并得到稳定错误码；随后创建全新访客并批准加入，
/// 等真实连接和初始文档传输完成后由房主主动移除。被移除端应像房主断开一样
/// 清空会话临时状态，但之后本地编辑不应再产生“提交失败”噪声日志。
///
/// @note 两个访客使用不同稳定身份，避免前一次拒绝留下的服务端状态影响第二
///       次批准流程。
///
/// @par 拒绝路径
/// 待审批访客不应提前进入参与者集合。房主按 requestId 拒绝后，请求必须从
/// 队列移除，访客进入 Error，并以 host_rejected 作为稳定原因；随后显式断开
/// 该访客，确保它不再参与后续事件循环。
///
/// @par 移除路径
/// 获准访客必须先接收至少一次业务文档提交，证明它确实越过审批和 P2P 建链。
/// 房主再按真实 peer ID 移除该成员。完成状态要求房主只剩自身，访客回到 Idle，
/// 并清除连接身份、参与者、视口与权限。
///
/// @par 离线编辑契约
/// 被移除后调用 onBeatmapMutated 仍是合法的上层行为，因为用户可以继续编辑
/// 本地项目。房间已经不活跃时，该调用应被视为无需联网提交，而不是协议错误；
/// 因此测试比较调用前后的 local_operation_submit_failed 日志数量必须相等。
bool testHostAdmissionControl()
{
    // 准入测试使用独立服务端，避免主流程用例的房间和连接残留影响结果。
    CollaborationSignalingServerConfig serverConfig;
    serverConfig.port        = 0;
    serverConfig.bindAddress = "127.0.0.1";
    CollaborationSignalingServer server;
    if ( !server.start(std::move(serverConfig)) ) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 动态监听端口只有 start 成功后才确定，随后复制进所有房间配置。
    CollaborationServerEndpoint endpoint;
    endpoint.address       = "127.0.0.1";
    endpoint.signalingPort = server.listeningPort();
    endpoint.useTls        = false;

    // 房主先完成房间发布，之后才创建访客，确保审批请求属于有效公开房间。
    CollaborationRoom           host;
    CollaborationHostRoomConfig hostConfig;
    hostConfig.creator       = "Admission Host";
    hostConfig.participantId = makeRoomTestParticipantId(1);
    hostConfig.roomName      = "Admission Test";
    hostConfig.endpoint      = endpoint;
    if ( !host.startHost(hostConfig) ) return false;
    std::vector<std::unique_ptr<CollaborationRoom>> guests;
    // 所有阶段共用诊断出口，记录当前阶段和两端关键状态，保持主断言紧凑。
    const auto failAdmission = [&host, &guests](std::string_view phase) {
        const CollaborationRoom* guest =
            guests.empty() ? nullptr : guests.back().get();
        // guest 可能尚未成功构造或 join，因此诊断读取必须允许空指针。
        XERROR(
            "Admission control test failed at {}: host_state={}, "
            "host_participants={}, pending={}, guest_state={}, guest_error={}",
            phase,
            static_cast<int>(host.state()),
            host.participants().size(),
            host.pendingJoinRequests().size(),
            guest ? static_cast<int>(guest->state()) : -1,
            guest ? guest->lastError() : std::string{});
        return false;
    };
    if ( !pumpUntil(
             server, host, guests, [&]() { return !host.roomId().empty(); }) ) {
        return failAdmission("publish");
    }

    // 访客身份从二开始，序号一保留给房主；每次调用都会消费新身份。
    std::size_t nextGuestIdentity = 2;
    auto        makeGuest         = [&](std::string creator) {
        // 工厂统一绑定同一房间和端点，两个子场景只改变名称与稳定身份。
        auto guest = std::make_unique<CollaborationRoom>();
        CollaborationJoinRoomConfig config;
        config.creator       = std::move(creator);
        config.participantId = makeRoomTestParticipantId(nextGuestIdentity++);
        config.roomId        = host.roomId();
        config.roomName      = host.roomName();
        config.endpoint      = endpoint;
        // 用空 unique_ptr 表达同步 join 启动失败，调用方在加入容器后统一检查。
        if ( !guest->join(std::move(config)) ) {
            return std::unique_ptr<CollaborationRoom>{};
        }
        return guest;
    };

    // 第一子场景：请求必须进入待审批队列，且在批准前不能增加参与者数量。
    guests.push_back(makeGuest("Rejected Guest"));
    if ( !guests.back() ||
         !pumpUntil(server,
                    host,
                    guests,
                    [&]() {
                        return guests.back()->state() ==
                                   CollaborationRoomState::AwaitingApproval &&
                               host.pendingJoinRequests().size() == 1U;
                    }) ||
         host.participants().size() != 1U ||
         // 房主使用服务端分配的 requestId 拒绝，验证真实请求关联而非伪造 ID。
         !host.rejectJoinRequest(
             host.pendingJoinRequests().front().requestId) ||
         !pumpUntil(server, host, guests, [&]() {
             return guests.back()->state() == CollaborationRoomState::Error &&
                    guests.back()->lastError() == "host_rejected" &&
                    host.pendingJoinRequests().empty();
         }) ) {
        return failAdmission("reject");
    }
    // 显式释放被拒绝端的连接状态，再清空容器，避免其继续参与下一阶段 pump。
    guests.back()->disconnect();
    guests.clear();

    // 第二子场景：新的访客走完整批准和 P2P 建链路径。
    guests.push_back(makeGuest("Approved Guest"));
    if ( !guests.back() ||
         !pumpUntil(server,
                    host,
                    guests,
                    [&]() {
                        return guests.back()->state() ==
                                   CollaborationRoomState::AwaitingApproval &&
                               host.pendingJoinRequests().size() == 1U;
                    }) ||
         !host.approveJoinRequest(
             host.pendingJoinRequests().front().requestId) ||
         !pumpUntil(server, host, guests, [&]() {
             return guests.back()->state() ==
                        CollaborationRoomState::Connected &&
                    host.participants().size() == 2U;
         }) ) {
        return failAdmission("approve");
    }

    // 建链后提交一份文档，确保获准访客的 DataChannel 已可承载业务消息，
    // 而不仅是信令状态显示 Connected。
    auto sharedBeatmap = makeBeatmap(1000.0, "Admission Host");
    static_cast<void>(
        host.onBeatmapMutated(*sharedBeatmap, BeatmapMutationFlags::All));
    if ( !pumpUntil(server, host, guests, [&]() {
             return countLogs(*guests.back(),
                              CollaborationLogEventType::OperationCommitted) >=
                    1U;
         }) ) {
        return failAdmission("initial_document");
    }

    // 使用访客真实分配的 peer ID 执行移除；零值表示连接身份尚未建立。
    const PeerId guestPeerId = guests.back()->localPeerId();
    if ( guestPeerId == 0 || !host.removeParticipant(guestPeerId) ) {
        return failAdmission("remove_start");
    }
    // 移除完成后检查所有会话派生状态，避免残留权限或视口泄漏到后续重连。
    if ( !pumpUntil(server, host, guests, [&]() {
             return host.participants().size() == 1U &&
                    guests.back()->state() == CollaborationRoomState::Idle &&
                    !guests.back()->isActive() &&
                    guests.back()->participants().empty() &&
                    guests.back()->participantViewports().empty() &&
                    guests.back()->localPermissions() == 0U &&
                    !guests.back()->lastError().empty() &&
                    countLogs(*guests.back(),
                              CollaborationLogEventType::HostDisconnected) ==
                        1U;
         }) ) {
        return failAdmission("remove_complete");
    }

    // 被移除端仍可能继续编辑本地谱面。此时 CollaborationRoom 应静默忽略发送，
    // 不能把正常离线编辑记录为 local_operation_submit_failed。
    const auto submitFailuresBefore =
        countLogDetails(*guests.back(), "local_operation_submit_failed");
    sharedBeatmap->m_noteData.notes.front().m_timestamp = 1250.0;
    sharedBeatmap->sync();
    static_cast<void>(guests.back()->onBeatmapMutated(
        *sharedBeatmap, BeatmapMutationFlags::Objects));
    // 推进一次房间状态机，覆盖可能延迟到 update 中产生的错误日志。
    guests.back()->update();
    if ( countLogDetails(*guests.back(), "local_operation_submit_failed") !=
         submitFailuresBefore ) {
        return failAdmission("post_disconnect_mutation");
    }
    return true;
}

/// @brief 验证资源清单和多分块通过真实 DataChannel 到达访客并完成校验。
/// @return 初次资源和在线替换资源均逐字节一致时返回 true。
///
/// @details
/// 本用例构造大于单个传输块的二进制资源，通过房主项目清单发现引用，再由
/// 真实 DataChannel 分块发送到访客缓存。除了校验回调产生的 Project DTO，
/// 还会重新读取落盘文件逐字节比较，确保分块顺序、长度、摘要校验与最终重命名
/// 都正确。连接保持期间再次发布不同项目，验证资源清单版本更新能触发第二轮
/// 下载，而不会复用旧缓存内容。
///
/// @warning 测试会创建临时文件，但 ScopedRoomResourceDirectory 保证所有提前
///          返回路径最终都递归清理，不会写入 tests/data。
///
/// @par 首轮清单契约
/// 宿主 Project 声明一个主音频资源，BeatMap 通过 ID 引用它。访客收到的
/// CollaborationResourceBundle 必须包含重定位后的项目根和一条资源记录；
/// 宿主恰好记录一次 ResourceManifest，访客恰好记录一次 ResourceCompleted。
///
/// @par 文件完整性契约
/// 生成内容采用确定性字节模式和非整块长度。验收先比较落盘长度，再读取完整
/// 缓冲并逐字节比较，因此可以分别发现：
///
/// - 最后一个分块被截断；
/// - 分块顺序错误或重复写入；
/// - 临时文件未完整提交就触发完成回调；
/// - 访客项目仍错误引用宿主源路径；
/// - 摘要相同判断或缓存复用选择了错误资源。
///
/// @par 在线替换契约
/// 第二轮项目使用不同根目录、文件名、资源 ID、长度和字节模式。收到新 bundle
/// 后，日志累计次数必须精确增加一次，返回项目只能描述 replacement-main-id，
/// 且新落盘内容与第二轮模式一致。这确保 prepareHostResources 在活动房间内
/// 表达“发布新清单”，而非只在建房前生效。
bool testOneGuestResourceSync()
{
    // 与其他集成场景隔离信令服务，使用回环地址和系统分配端口。
    CollaborationSignalingServerConfig serverConfig;
    serverConfig.port        = 0;
    serverConfig.bindAddress = "127.0.0.1";
    CollaborationSignalingServer server;
    if ( !server.start(std::move(serverConfig)) ) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CollaborationServerEndpoint endpoint;
    endpoint.address       = "127.0.0.1";
    endpoint.signalingPort = server.listeningPort();
    endpoint.useTls        = false;

    // 所有宿主源文件与访客缓存都置于同一临时根，便于生命周期统一管理。
    ScopedRoomResourceDirectory directory;
    if ( directory.path().empty() ) return false;
    // 宿主文件使用嵌套相对路径，验证项目资源路径在清单中得到正确保留。
    const auto      projectRoot = directory.path() / "host";
    const auto      audioPath   = projectRoot / "audio/main.bin";
    std::error_code error;
    std::filesystem::create_directories(audioPath.parent_path(), error);
    if ( error ) return false;
    // 150003 字节有意超过常规块大小且不整除，末块边界必须被正确处理。
    std::vector<std::uint8_t> expected(150003U);
    for ( std::size_t index = 0; index < expected.size(); ++index ) {
        // 确定性非恒定模式可发现错序、重复块和零填充，而无需保存测试资源。
        expected[index] = static_cast<std::uint8_t>((index * 73U) % 251U);
    }
    // 关闭输出流后再检查状态，保证数据已刷新到磁盘再交给资源扫描器读取。
    std::ofstream output(audioPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(expected.data()),
                 static_cast<std::streamsize>(expected.size()));
    output.close();
    if ( !output ) return false;

    // 项目只声明一个主音频资源，谱面采样通过资源 ID 建立实际引用。
    // 未被谱面引用的文件不属于本用例的同步集合。
    MMM::Project project;
    project.m_projectRoot = projectRoot;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id   = "main-id",
        .m_path = "audio/main.bin",
        .m_type = MMM::AudioTrackType::Main,
    });
    BeatMap beatmap;
    beatmap.m_audioSamples.emplace_back().m_audioResourceId = "main-id";

    // prepareHostResources 必须在发布房间前调用，使首位访客连接后立即收到清单。
    CollaborationRoom           host;
    CollaborationHostRoomConfig hostConfig;
    hostConfig.creator       = "Resource Host";
    hostConfig.participantId = makeRoomTestParticipantId(1);
    hostConfig.roomName      = "Resource Test";
    hostConfig.endpoint      = endpoint;
    host.prepareHostResources(project, beatmap);
    if ( !host.startHost(hostConfig) ) return false;
    std::vector<std::unique_ptr<CollaborationRoom>> guests;
    if ( !pumpUntil(
             server, host, guests, [&]() { return !host.roomId().empty(); }) ) {
        return false;
    }

    // 回调持有最终资源包，optional 同时作为异步完成信号和结果容器。
    std::optional<CollaborationResourceBundle> receivedBundle;
    auto guest = std::make_unique<CollaborationRoom>();
    guest->setResourceBundleCallback(
        [&receivedBundle](CollaborationResourceBundle bundle) {
            receivedBundle = std::move(bundle);
        });
    // 为访客指定独立缓存根，防止其直接读取宿主源路径而产生伪成功。
    CollaborationJoinRoomConfig joinConfig;
    joinConfig.creator           = "Resource Guest";
    joinConfig.participantId     = makeRoomTestParticipantId(2);
    joinConfig.roomId            = host.roomId();
    joinConfig.roomName          = host.roomName();
    joinConfig.endpoint          = endpoint;
    joinConfig.resourceCacheRoot = directory.path() / "cache";
    if ( !guest->join(joinConfig) ) return false;
    guests.push_back(std::move(guest));
    // 资源同步仍受房主审批保护，访客在批准前不得收到项目内容。
    if ( !pumpUntil(server,
                    host,
                    guests,
                    [&]() {
                        return guests.front()->state() ==
                                   CollaborationRoomState::AwaitingApproval &&
                               !host.pendingJoinRequests().empty();
                    }) ||
         !host.approveJoinRequest(
             host.pendingJoinRequests().front().requestId) ) {
        return false;
    }
    // Connected 与 bundle 回调必须同时成立：前者证明会话建成，后者证明资源
    // 清单、分块和校验已完整结束。
    if ( !pumpUntil(server, host, guests, [&]() {
             return receivedBundle.has_value() &&
                    guests.front()->state() ==
                        CollaborationRoomState::Connected;
         }) ) {
        // 失败时输出资源阶段，可区分等待清单、下载块、校验与发布回调超时。
        XERROR(
            "Resource collaboration timeout: host participants={}, guest "
            "state={}, error={}, resource phase={}",
            host.participants().size(),
            static_cast<int>(guests.front()->state()),
            guests.front()->lastError(),
            static_cast<int>(guests.front()->resourceProgress().phase));
        for ( const auto& entry : guests.front()->logs() ) {
            XERROR("Resource guest log type={} detail={}",
                   static_cast<int>(entry.type),
                   entry.detail);
        }
        return false;
    }
    // 首轮同步应各产生一次宿主清单日志和访客完成日志，防止重复发布回调。
    if ( !receivedBundle->project ||
         receivedBundle->project->m_audioResources.size() != 1U ||
         countLogs(host, CollaborationLogEventType::ResourceManifest) != 1U ||
         countLogs(*guests.front(),
                   CollaborationLogEventType::ResourceCompleted) != 1U ) {
        return false;
    }
    // 从回调返回的访客项目根重新定位资源，不能沿用宿主路径进行校验。
    const auto& receivedResource =
        receivedBundle->project->m_audioResources.front();
    std::ifstream input(
        receivedBundle->project->m_projectRoot / receivedResource.m_path,
        std::ios::binary | std::ios::ate);
    // 先在文件尾比较精确长度，再分配读取缓冲，避免截断文件被部分读取掩盖。
    if ( !input ||
         input.tellg() != static_cast<std::streamoff>(expected.size()) ) {
        return false;
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> actual(expected.size());
    input.read(reinterpret_cast<char*>(actual.data()),
               static_cast<std::streamsize>(actual.size()));
    // 同时要求完整读取和内容相等，区分 I/O 短读与传输数据损坏。
    if ( input.gcount() != static_cast<std::streamsize>(actual.size()) ||
         actual != expected ) {
        return false;
    }

    // 第二轮使用全新的宿主项目根、资源 ID、路径、大小和字节模式，确保成功
    // 不能来自首轮缓存命中或旧 bundle 残留。
    const auto replacementRoot = directory.path() / "replacement-host";
    const auto replacementPath = replacementRoot / "audio/replacement.bin";
    std::filesystem::create_directories(replacementPath.parent_path(), error);
    if ( error ) return false;
    // 同样选择非整块长度，但与首轮尺寸不同，覆盖在线替换后的末块处理。
    std::vector<std::uint8_t> replacementExpected(90017U);
    for ( std::size_t index = 0; index < replacementExpected.size(); ++index ) {
        replacementExpected[index] =
            static_cast<std::uint8_t>((index * 41U + 17U) % 253U);
    }
    std::ofstream replacementOutput(replacementPath,
                                    std::ios::binary | std::ios::trunc);
    replacementOutput.write(
        reinterpret_cast<const char*>(replacementExpected.data()),
        static_cast<std::streamsize>(replacementExpected.size()));
    replacementOutput.close();
    if ( !replacementOutput ) return false;

    // 新谱面只引用新资源 ID，资源清单应完整替换而非与旧清单累加。
    MMM::Project replacementProject;
    replacementProject.m_projectRoot = replacementRoot;
    replacementProject.m_audioResources.push_back(MMM::AudioResource{
        .m_id   = "replacement-main-id",
        .m_path = "audio/replacement.bin",
        .m_type = MMM::AudioTrackType::Main,
    });
    BeatMap replacementBeatmap;
    replacementBeatmap.m_audioSamples.emplace_back().m_audioResourceId =
        "replacement-main-id";
    // 清空旧结果后再发布，使等待谓词只可能由第二轮回调满足。
    receivedBundle.reset();
    host.prepareHostResources(replacementProject, replacementBeatmap);
    if ( !pumpUntil(
             server,
             host,
             guests,
             [&]() {
                 return receivedBundle && receivedBundle->project &&
                        receivedBundle->project->m_audioResources.size() ==
                            1U &&
                        receivedBundle->project->m_audioResources.front()
                                .m_id == "replacement-main-id";
             }) ||
         countLogs(host, CollaborationLogEventType::ResourceManifest) != 2U ||
         countLogs(*guests.front(),
                   CollaborationLogEventType::ResourceCompleted) != 2U ) {
        return false;
    }

    // 第二轮日志计数应累计到二，并再次从返回的访客项目根读取内容。
    const auto& replacementResource =
        receivedBundle->project->m_audioResources.front();
    std::ifstream replacementInput(
        receivedBundle->project->m_projectRoot / replacementResource.m_path,
        std::ios::binary | std::ios::ate);
    if ( !replacementInput ||
         replacementInput.tellg() !=
             static_cast<std::streamoff>(replacementExpected.size()) ) {
        return false;
    }
    replacementInput.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> replacementActual(replacementExpected.size());
    replacementInput.read(
        reinterpret_cast<char*>(replacementActual.data()),
        static_cast<std::streamsize>(replacementActual.size()));
    // 最终返回值同时覆盖短读和逐字节一致性，完成在线替换验收。
    return replacementInput.gcount() ==
               static_cast<std::streamsize>(replacementActual.size()) &&
           replacementActual == replacementExpected;
}

/// @brief 驱动公网目录、房主和访客，直到条件满足或超时。
/// @tparam Predicate 无参数且返回可转为 bool 的公网收敛判定器。
/// @param host 部署探针创建的房主房间。
/// @param guest 部署探针创建的单个访客房间。
/// @param directory 独立的公开房间目录客户端。
/// @param predicate 每轮完整推进后检查的目标条件。
/// @return 条件在公网超时上限前满足时返回 true。
///
/// @details
/// 公网探针除两端 CollaborationRoom 外还需要推进独立目录客户端，以覆盖房间
/// 发布后的列表发现。房主在访客更新后再推进一次，可及时消费访客刚发送的
/// 审批或连接消息。该函数不把网络延迟假设编码进断言，只设置最终截止时间。
///
/// @warning 轮询和 sleep 只属于显式部署测试，不代表生产事件循环实现。
///
/// @par 与本机泵的差异
/// 部署模式没有进程内 CollaborationSignalingServer 可推进，信令由远端服务
/// 自己运行；本地只需额外更新 CollaborationDirectoryClient。测试固定只有一名
/// 访客，因此函数直接接收 guest 引用，不使用访客容器，也避免把公网探针扩大
/// 为并发负载测试。
template<typename Predicate>
bool pumpExternalUntil(
    CollaborationRoom& host, CollaborationRoom& guest,
    MMM::Network::Collaboration::CollaborationDirectoryClient& directory,
    Predicate                                                  predicate)
{
    const auto deadline =
        std::chrono::steady_clock::now() + EXTERNAL_TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        // 先刷新目录，使房间发布或移除能在本轮被测试条件观察。
        directory.update();
        // 随后按房主、访客、房主顺序推进双向信令和 P2P 消息。
        host.update();
        guest.update();
        host.update();
        // 谓词只读取本轮处理后的稳定快照，不在更新过程中穿插观察。
        if ( predicate() ) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/// @brief 验证部署后的房间发布、目录发现、加入配对和 DataChannel 建立。
/// @param endpoint 用户显式提供的公网信令端点。
/// @return 目录发现、审批连接和访客移除均完成时返回 true。
///
/// @details
/// 该用例不会由默认 CTest 自动访问公网，只在命令行选择 external 模式时运行。
/// 它用三个独立客户端对象验证部署闭环：目录客户端负责公开列表，房主负责
/// 发布与审批，访客按目录返回的信息加入。成功标准要求双方参与者列表均包含
/// 两人，证明不仅 WebSocket 信令在线，实际 P2P DataChannel 也已建立。
///
/// @note 失败日志分别保留目录状态、两端房间状态和协议事件，便于判断部署证书、
///       反向代理、信令服务或 ICE 连通性问题。
///
/// @par 部署探针边界
/// 该测试只验证应用协议可用性，不尝试诊断公网基础设施本身。调用方负责提供
/// 已知可访问的地址、端口和 TLS 选择；函数通过现有客户端接口观察结果，不会
/// 修改服务端配置、证书、DNS 或防火墙。
///
/// @par 成功路径
/// 成功必须依次满足以下可观察条件：
///
/// - 目录客户端进入 Connected；
/// - 房主获得非空 roomId；
/// - 目录列表出现同一 roomId；
/// - 访客使用目录返回的名称和 ID 发起 join；
/// - 访客停在 AwaitingApproval，房主收到请求；
/// - 房主批准后双方都看到两名参与者；
/// - 访客断开后房主参与者列表恢复为一人。
///
/// @par 清理保证
/// 访客移除等待无论成功与否，房主和目录客户端都会在返回前主动断开。这样
/// 手动运行失败的探针也不会长时间占用公开房间条目或保留客户端连接。
bool testExternalPublicDirectoryWebRtcRoom(CollaborationServerEndpoint endpoint)
{
    using MMM::Network::Collaboration::CollaborationDirectoryClient;
    using MMM::Network::Collaboration::CollaborationDirectoryState;

    // 首先单独验证目录客户端能与部署端点完成连接，避免后续房间错误掩盖基础
    // WebSocket 或 TLS 配置问题。
    CollaborationDirectoryClient directory;
    if ( !directory.connect(endpoint) ) return false;

    // 房主和访客在同一进程中模拟两个客户端，但各自拥有独立传输和状态机。
    CollaborationRoom host;
    CollaborationRoom guest;
    if ( !pumpExternalUntil(host, guest, directory, [&]() {
             return directory.state() == CollaborationDirectoryState::Connected;
         }) ) {
        // 目录启动失败时还没有房间上下文，只报告目录自己的稳定状态和错误。
        XERROR("External directory bootstrap failed: state={}, error={}",
               static_cast<int>(directory.state()),
               directory.lastError());
        return false;
    }

    // 目录连接成功后再发布房间，确保后续列表变化来自本次探针。
    CollaborationHostRoomConfig hostConfig;
    hostConfig.creator       = "Deployment Host";
    hostConfig.participantId = makeRoomTestParticipantId(1);
    hostConfig.roomName      = "Public Deployment Probe";
    hostConfig.endpoint      = endpoint;
    if ( !host.startHost(hostConfig) ) return false;

    // 房主必须先获得服务端分配的 roomId，目录列表随后应出现同一 ID。
    // 只按 ID 匹配，避免部署环境存在同名房间造成误判。
    const bool roomDiscovered =
        pumpExternalUntil(host, guest, directory, [&]() {
            if ( host.roomId().empty() ||
                 directory.state() != CollaborationDirectoryState::Connected ) {
                return false;
            }
            return std::any_of(directory.rooms().begin(),
                               directory.rooms().end(),
                               [&host](const auto& room) {
                                   return room.roomId == host.roomId();
                               });
        });
    if ( !roomDiscovered ) {
        // 同时输出房主与目录状态，可区分“发布失败”和“发布成功但列表未刷新”。
        XERROR(
            "External room discovery failed: host state={}, host error={}, "
            "directory state={}, directory error={}",
            static_cast<int>(host.state()),
            host.lastError(),
            static_cast<int>(directory.state()),
            directory.lastError());
        return false;
    }

    // 从目录条目而非房主对象复制加入信息，验证公开发现结果本身足以发起连接。
    const auto roomIterator = std::find_if(
        directory.rooms().begin(),
        directory.rooms().end(),
        [&host](const auto& room) { return room.roomId == host.roomId(); });
    if ( roomIterator == directory.rooms().end() ) return false;
    CollaborationJoinRoomConfig guestConfig;
    guestConfig.creator       = "Deployment Guest";
    guestConfig.participantId = makeRoomTestParticipantId(2);
    guestConfig.roomId        = roomIterator->roomId;
    guestConfig.roomName      = roomIterator->roomName;
    guestConfig.endpoint      = endpoint;
    if ( !guest.join(guestConfig) ) return false;

    // 公网加入同样必须经过 AwaitingApproval，确认部署服务没有绕过准入阶段。
    if ( !pumpExternalUntil(
             host,
             guest,
             directory,
             [&]() {
                 return guest.state() ==
                            CollaborationRoomState::AwaitingApproval &&
                        !host.pendingJoinRequests().empty();
             }) ||
         !host.approveJoinRequest(
             host.pendingJoinRequests().front().requestId) ) {
        return false;
    }

    // 双方参与者列表都达到二才算完整连接；单侧 Connected 可能只是暂态。
    const bool connected = pumpExternalUntil(host, guest, directory, [&]() {
        return guest.state() == CollaborationRoomState::Connected &&
               host.participants().size() == 2U &&
               guest.participants().size() == 2U;
    });
    if ( !connected ) {
        // 连接诊断展开两端协议日志，保留 SDP、ICE 和 DataChannel 阶段线索。
        XERROR(
            "External P2P connection failed: host state={}, participants={}, "
            "error={}; guest state={}, participants={}, error={}",
            static_cast<int>(host.state()),
            host.participants().size(),
            host.lastError(),
            static_cast<int>(guest.state()),
            guest.participants().size(),
            guest.lastError());
        for ( const auto& entry : host.logs() ) {
            XERROR("External host log type={} detail={}",
                   static_cast<int>(entry.type),
                   entry.detail);
        }
        for ( const auto& entry : guest.logs() ) {
            XERROR("External guest log type={} detail={}",
                   static_cast<int>(entry.type),
                   entry.detail);
        }
        return false;
    }

    // 主动断开访客后，房主列表必须恢复为仅自身，验证部署服务的离开广播。
    guest.disconnect();
    const bool guestRemoved = pumpExternalUntil(host, guest, directory, [&]() {
        return host.participants().size() == 1U;
    });
    // 不论访客移除结果如何，都在返回前关闭房主和目录客户端，避免探针房间
    // 留在公网目录中等待服务端超时清理。
    host.disconnect();
    directory.disconnect();
    return guestRemoved;
}

}  // namespace

/// @brief 根据命令行模式执行本机 P2P、资源同步或公网部署验收。
/// @param argc 参数数量。
/// @param argv 参数数组；第一个业务参数为 p2p、resource 或 external。
/// @return 0 表示所选场景通过，1/2 表示场景失败，3 表示参数无效，4 表示
///         协作构建指纹初始化失败。
///
/// @details
/// 所有场景共享 AppThreadPool 和异步构建指纹。指纹必须在创建房间前进入 Ready，
/// 因为真实握手会把它作为兼容性约束。external 模式额外解析地址、端口和 TLS
/// 布尔值，并只在该分支初始化日志器，避免默认测试污染全局日志配置。
///
/// @note 无论选择何种模式，函数都在返回前关闭 AppThreadPool，保证后台任务不会
///       跨越测试进程退出边界。
///
/// @par 命令行契约
/// 支持的精确形式为：
///
/// - `CollaborationRoomTest p2p`：本机协作与准入控制；
/// - `CollaborationRoomTest resource`：本机资源同步与在线替换；
/// - `CollaborationRoomTest external ADDRESS PORT TLS`：公网部署探针。
///
/// PORT 必须是完整十进制文本且位于 1..65535；TLS 只能为小写 true 或 false。
/// 多余、缺少或无法解析的参数统一返回 3，不尝试猜测调用方意图。
///
/// @par 初始化顺序
/// AppThreadPool 先启动，构建指纹初始化随后投递后台工作。只有状态达到 Ready
/// 且结果通过协议格式校验后才分派测试模式。此顺序保证所有真实握手都使用
/// 确定且有效的兼容性指纹，失败则以退出码 4 与网络链路失败区分。
///
/// @par 退出码分层
/// p2p 与 external 的业务失败返回 1，资源链路失败返回 2，调用格式错误返回 3，
/// 指纹前置条件失败返回 4。CTest 可据此展示失败，但详细阶段仍以场景内日志为
/// 准；退出码不承载具体协议错误，避免把实现内部枚举固化为进程接口。
///
/// @par 生命周期保证
/// 模式函数中的房间和服务器均在返回 main 前析构，随后统一 shutdown 线程池。
/// external 分支的日志器则在探针返回后立即关闭。该次序确保房间析构仍可使用
/// 后台执行设施，而进程退出时不再遗留协作任务或日志后台线程。
int main(int argc, char** argv)
{
    // 至少需要一个模式参数；空 argv[1] 也按无效调用处理。
    if ( argc < 2 || !argv[1] ) return 3;
    // 构建指纹计算会投递后台任务，因此线程池必须先于初始化入口启动。
    auto& appThreadPool = MMM::Runtime::AppThreadPool::instance();
    appThreadPool.init();
    if ( !MMM::Network::Collaboration::
             startCollaborationBuildFingerprintInitialization() ) {
        return 4;
    }
    // 初始化入口允许计算已经由同进程其他路径启动，初态可为 Calculating 或
    // Ready，但任何失败或未初始化状态都不满足真实协作握手前置条件。
    const auto initialFingerprintState =
        MMM::Network::Collaboration::collaborationBuildFingerprintState();
    if ( initialFingerprintState !=
             MMM::Network::Collaboration::CollaborationBuildFingerprintState::
                 Calculating &&
         initialFingerprintState !=
             MMM::Network::Collaboration::CollaborationBuildFingerprintState::
                 Ready ) {
        return 4;
    }
    // 指纹需要遍历构建产物，独立使用较宽超时；轮询只等待后台任务，不推进
    // 房间状态机，因为此时尚未创建任何连接。
    constexpr auto FINGERPRINT_TIMEOUT = std::chrono::seconds(30);
    const auto     fingerprintDeadline =
        std::chrono::steady_clock::now() + FINGERPRINT_TIMEOUT;
    while ( MMM::Network::Collaboration::collaborationBuildFingerprintState() ==
                MMM::Network::Collaboration::
                    CollaborationBuildFingerprintState::Calculating &&
            std::chrono::steady_clock::now() < fingerprintDeadline ) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 超时或计算失败都使用专用退出码，避免被误报为 P2P 或资源同步失败。
    if ( MMM::Network::Collaboration::collaborationBuildFingerprintState() !=
         MMM::Network::Collaboration::CollaborationBuildFingerprintState::
             Ready ) {
        return 4;
    }
    // Ready 只表示任务结束，仍需验证结果满足协议格式和非空约束。
    if ( !MMM::Network::Collaboration::isValidCollaborationBuildFingerprint(
             MMM::Network::Collaboration::collaborationBuildFingerprint()) ) {
        return 4;
    }
    // 默认保持参数错误退出码，只有精确匹配模式及其参数数量才执行场景。
    const std::string_view mode(argv[1]);
    int                    result = 3;
    if ( mode == "p2p" && argc == 2 ) {
        // 本机主模式连续覆盖协作收敛与准入控制，两者共享成功退出码。
        result = testPublicDirectoryWebRtcRoom() && testHostAdmissionControl()
                     ? 0
                     : 1;
    } else if ( mode == "resource" && argc == 2 ) {
        // 资源模式单独返回 2，便于 CTest 区分资源链路与普通协作失败。
        result = testOneGuestResourceSync() ? 0 : 2;
    } else if ( mode == "external" && argc == 5 ) {
        // external 参数依次为地址、十进制端口和严格小写 TLS 布尔值。
        std::uint32_t          port = 0;
        const std::string_view portText(argv[3]);
        const auto [end, error] = std::from_chars(
            portText.data(), portText.data() + portText.size(), port);
        // 必须消费完整字符串并限制到 TCP 端口范围，拒绝后缀字符和零端口。
        if ( error != std::errc{} || end != portText.data() + portText.size() ||
             port == 0 || port > 65535 ) {
            result = 3;
        } else {
            const std::string_view tlsText(argv[4]);
            // 仅接受明确 true/false，避免隐式真值让部署探针连接错误协议。
            if ( tlsText == "true" || tlsText == "false" ) {
                CollaborationServerEndpoint endpoint;
                endpoint.address       = argv[2];
                endpoint.signalingPort = static_cast<std::uint16_t>(port);
                endpoint.useTls        = tlsText == "true";
                // 公网失败需要持久诊断信息，日志器生命周期严格包围探针调用。
                XLogger::init("CollaborationRoomTest");
                const bool success =
                    testExternalPublicDirectoryWebRtcRoom(std::move(endpoint));
                XLogger::shutdown();
                result = success ? 0 : 1;
            }
        }
    }
    // 即使参数无效或场景失败，也统一等待后台任务退出后再返回结果。
    appThreadPool.shutdown();
    return result;
}
