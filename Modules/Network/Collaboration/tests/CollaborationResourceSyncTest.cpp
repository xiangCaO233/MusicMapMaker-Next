#include "network/collaboration/CollaborationResourceSync.h"

#include "CollaborationResourceCipher.h"

#include "config/Utf8Path.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
// 测试只引入资源同步场景需要的领域类型，避免用冗长限定名遮挡状态流。
using MMM::AudioResource;
using MMM::AudioTrackConfig;
using MMM::AudioTrackType;
using MMM::BeatMap;
using MMM::Project;
using MMM::Network::Collaboration::ByteBuffer;
using MMM::Network::Collaboration::CollaborationResourceBundle;
using MMM::Network::Collaboration::CollaborationResourceSync;
using MMM::Network::Collaboration::CollaborationResourceSyncEvent;
using MMM::Network::Collaboration::CollaborationResourceSyncPhase;
using MMM::Network::Collaboration::ResourceChunk;
using MMM::Network::Collaboration::ResourceManifest;
using MMM::Network::Collaboration::ResourceRequest;

/// @brief 后台资源状态机测试允许的最长等待时间。
/// @note 超时只保护测试进程；生产同步路径不依赖该固定窗口推进状态。
constexpr auto TEST_TIMEOUT = std::chrono::seconds(10);
/// @brief 资源请求测试使用的访客标识。
/// @note 房主传输路由使用默认槽位一，因此单访客固定为槽位二。
constexpr MMM::Network::Collaboration::PeerId GUEST_ID = 2;

/// @brief 为资源测试创建并清理隔离目录。
/// @details
/// 每个场景在系统临时目录下拥有独立根目录，房主源文件、访客缓存、解密结果
/// 和故障注入产生的中间文件都限制在其中。析构阶段递归删除该目录，避免测试
/// 之间共享缓存或在用户项目中留下资源。构造失败以空路径表示，不抛出异常，
/// 由各测试入口显式返回失败。
/// @note 类型禁止复制，确保只有一个对象负责目录生命周期和最终清理。
class ScopedResourceDirectory
{
public:
    /// @brief 创建唯一资源目录。
    /// @details 使用 steady_clock 计数生成进程内低碰撞后缀，并以 error_code
    /// 接收文件系统错误，遵守项目无异常约束。
    ScopedResourceDirectory()
    {
        // 先查询系统临时根；失败时保持 m_path 为空供调用方识别。
        std::error_code error;
        auto            root = std::filesystem::temp_directory_path(error);
        if ( error ) return;
        // 单调时钟不受系统时间回拨影响，适合作为短生命周期测试目录后缀。
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path =
            root / ("mmm-collaboration-resource-" + std::to_string(suffix));
        // 只有目录真正创建成功才向后续场景暴露非空路径。
        std::filesystem::create_directories(m_path, error);
        if ( error ) m_path.clear();
    }

    /// @brief 删除测试拥有的目录。
    /// @details 清理失败不会从析构函数传播；测试断言只关注同步逻辑，系统级
    /// 临时目录清理错误不应导致终止过程抛出异常。
    ~ScopedResourceDirectory()
    {
        // 空路径代表构造阶段未取得所有权，禁止对其执行递归删除。
        if ( m_path.empty() ) return;
        std::error_code error;
        // m_path 始终是本对象创建的具体子目录，不会指向临时根本身。
        std::filesystem::remove_all(m_path, error);
    }

    /// @brief 禁止复制目录所有权，避免双重递归清理。
    ScopedResourceDirectory(const ScopedResourceDirectory&) = delete;
    /// @brief 禁止通过赋值转移或复制目录清理责任。
    ScopedResourceDirectory& operator=(const ScopedResourceDirectory&) = delete;

    /// @brief 返回测试目录。
    /// @return 本对象生命周期内稳定的隔离目录路径；构造失败时为空。
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    /// @brief 测试目录路径。
    std::filesystem::path m_path;
};

/// @brief 将字节完整写入测试文件。
/// @details
/// 辅助函数先创建父目录，再以二进制截断模式写入全部字节。它用于构造房主源
/// 资源以及覆盖已有文件的变更场景，因此不能使用追加模式。所有文件系统和流
/// 失败都折叠为 false，调用方保留具体场景的失败语义。
/// @param path 隔离目录内的目标文件。
/// @param bytes 要写入的完整内容。
/// @return 父目录建立、写入和关闭均成功时返回 true。
bool writeBytes(const std::filesystem::path& path, const ByteBuffer& bytes)
{
    // 资源路径包含 audio 或 images 子目录，测试不预先逐个创建。
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if ( error ) return false;
    // trunc 保证变更场景不会遗留旧文件尾部。
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if ( !output ) return false;
    // ByteBuffer 数据按原始字节写入，不进行文本编码或换行转换。
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    // 关闭后的流状态包含延迟写入失败，可作为完整落盘结果。
    return static_cast<bool>(output);
}

/// @brief 读取测试文件全部字节。
/// @details
/// 先定位文件末尾取得精确长度，再一次性分配缓冲并回到起点读取。该函数用于
/// 比较访客物化资源、检查加密容器以及执行密文故障注入，不对内容作任何解释。
/// @param path 要读取的普通文件。
/// @return 成功时返回完整字节；打开、测长或读取失败时返回 std::nullopt。
std::optional<ByteBuffer> readBytes(const std::filesystem::path& path)
{
    // ate 只用于测长，后续仍显式 seek 到文件开头。
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if ( !input ) return std::nullopt;
    const auto size = input.tellg();
    // 负位置表示流无法提供可靠长度，禁止转换为超大 size_t。
    if ( size < 0 ) return std::nullopt;
    ByteBuffer bytes(
        static_cast<std::size_t>(static_cast<std::streamoff>(size)));
    input.seekg(0, std::ios::beg);
    // 空文件无需调用 read，避免依赖空 vector 的 data 指针行为。
    if ( !bytes.empty() ) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    // 精确读取到文件尾允许 eof 状态，其他流错误均视为读取失败。
    if ( !input && !input.eof() ) return std::nullopt;
    return bytes;
}

/// @brief 生成可跨多个 64 KiB 分块验证的确定性内容。
/// @details
/// 周期 251 的位置模式比单一填充值更容易发现分块偏移、拼接顺序和截断错误。
/// seed 让同尺寸资源仍有不同内容，避免错误映射在字节比较中碰巧通过。
/// @param size 生成的字节数。
/// @param seed 每个资源独有的模式起点。
/// @return 长度固定且可重复生成的测试内容。
ByteBuffer patternedBytes(std::size_t size, std::uint8_t seed)
{
    ByteBuffer bytes(size);
    // 乘数 131 与模数 251 互质，使相邻位置具有明显变化。
    for ( std::size_t index = 0; index < size; ++index ) {
        bytes[index] = static_cast<std::uint8_t>(
            seed + static_cast<std::uint8_t>((index * 131U) % 251U));
    }
    return bytes;
}

/// @brief 构造覆盖全部音轨配置字段的测试配置。
/// @details
/// offset 让主音轨与效果音轨在每个数值字段上不同，防止序列化过程交换资源
/// 时仅凭默认值通过。EQ 数组包含多个元素，可同时覆盖变长浮点数组往返。
/// @param offset 叠加到代表性浮点字段的资源差异量。
/// @return 包含音量、变速、变调、静音和均衡器设置的完整配置。
AudioTrackConfig makeAudioConfig(float offset)
{
    AudioTrackConfig config;
    config.volume        = 0.35F + offset;
    config.playbackSpeed = 0.85F + offset;
    config.playbackPitch = -2.0F + offset;
    config.muted         = offset > 0.1F;
    config.eqEnabled     = true;
    config.eqPreset      = 2;
    config.eqBandGains   = { -1.5F + offset, 2.25F + offset };
    config.eqBandQs      = { 0.7F + offset, 1.1F + offset };
    return config;
}

/// @brief 判断两个音轨配置逐字段完全一致。
/// @details
/// 测试数据未经过有损转换，所有 float 都由同一序列化值还原，因此应要求精确
/// 相等。遗漏任一字段都会让资源清单恢复中的配置丢失无法被发现。
/// @param lhs 实际收到的音轨配置。
/// @param rhs 房主项目中的源配置。
/// @return 所有标量和 EQ 数组均相等时返回 true。
bool sameAudioConfig(const AudioTrackConfig& lhs, const AudioTrackConfig& rhs)
{
    return lhs.volume == rhs.volume && lhs.playbackSpeed == rhs.playbackSpeed &&
           lhs.playbackPitch == rhs.playbackPitch && lhs.muted == rhs.muted &&
           lhs.eqEnabled == rhs.eqEnabled && lhs.eqPreset == rhs.eqPreset &&
           lhs.eqBandGains == rhs.eqBandGains && lhs.eqBandQs == rhs.eqBandQs;
}

/// @brief 构造资源项目和谱面引用。
/// @details
/// 项目声明三项音频资源，但谱面只引用主音频和效果音频；未引用项用于证明
/// 清单按实际依赖裁剪。封面通过元数据路径引用，效果音频同时出现在普通子音符、
/// Polyline 子集合和采样表中，以覆盖资源引用收集时的去重。
/// @param root 房主项目根目录。
/// @param project 接收音频资源表的项目对象。
/// @param beatmap 接收元数据、音符和采样引用的谱面对象。
void makeProjectAndBeatmap(const std::filesystem::path& root, Project& project,
                           BeatMap& beatmap)
{
    // 资源表顺序稳定，后续接收包按同一顺序比较类型和音频配置。
    project.m_projectRoot    = root;
    project.m_audioResources = {
        AudioResource{
            .m_id     = "main-id",
            .m_path   = "audio/main.bin",
            .m_type   = AudioTrackType::Main,
            .m_config = makeAudioConfig(0.0F),
        },
        AudioResource{
            .m_id     = "effect-id",
            .m_path   = "audio/effect.wav",
            .m_type   = AudioTrackType::Effect,
            .m_config = makeAudioConfig(0.2F),
        },
        AudioResource{
            .m_id     = "unused-id",
            .m_path   = "audio/unused.ogg",
            .m_type   = AudioTrackType::Effect,
            .m_config = makeAudioConfig(0.3F),
        },
    };
    beatmap.m_baseMapMetadata.main_audio_path = "audio/main.bin";
    beatmap.m_baseMapMetadata.song_file_hint  = "main-id";
    beatmap.m_baseMapMetadata.main_cover_path = "images/cover.png";
    // 独立 Flick 以资源 ID 绑定效果音频，并携带非默认采样音量。
    auto& child           = beatmap.m_noteData.flicks.emplace_back();
    child.m_isSubNote     = true;
    child.m_sampleBinding = MMM::AudioSampleBinding{
        .m_audioResourceId = "effect-id",
        .m_volume          = 0.75F,
    };
    auto& polyline = beatmap.m_noteData.polylines.emplace_back();
    // 同一子音符进入两个 Polyline 容器，资源收集仍只能生成一项清单记录。
    polyline.m_subFlicks.emplace_back(child);
    polyline.m_subNotes.emplace_back(child);
    // 全局采样再次引用主音频，覆盖按资源 ID 与元数据路径合并的情况。
    auto& sample             = beatmap.m_audioSamples.emplace_back();
    sample.m_audioResourceId = "main-id";
    // 同步谱面内部派生状态后再交给资源扫描器。
    beatmap.sync();
}

/// @brief 等待并取出指定同步器的下一事件。
/// @details
/// 资源哈希、加密和物化由 AppThreadPool 异步完成，测试线程通过 pollEvent
/// 非阻塞观察结果。yield 只让出当前时间片，不规定后台任务完成所需的固定延迟；
/// 十秒截止用于在实现不再产出事件时让测试确定失败。
/// @param sync 要观察的资源同步状态机。
/// @param event 接收第一条可用事件。
/// @return 截止时间前取得事件时返回 true。
bool waitEvent(CollaborationResourceSync&      sync,
               CollaborationResourceSyncEvent& event)
{
    // 使用单调时钟，避免墙上时间调整延长或缩短测试保护窗口。
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        if ( sync.pollEvent(event) ) return true;
        // 不持有同步器锁，仅提示调度器运行后台工作线程。
        std::this_thread::yield();
    }
    return false;
}

/// @brief 等待房主清单准备完成。
/// @details
/// startHost 成功启动并不代表清单已经完成，因为文件哈希和加密快照在后台执行。
/// 本函数要求首个事件就是 ManifestReady，并进一步验证消息变体类型，避免把
/// Error 或其他发送事件误当作可用清单。
/// @param host 已调用 startHost 的房主同步器。
/// @return 成功生成的清单副本；事件缺失或类型不符时返回空值。
std::optional<ResourceManifest> waitManifest(CollaborationResourceSync& host)
{
    CollaborationResourceSyncEvent event;
    if ( !waitEvent(host, event) ||
         event.type != CollaborationResourceSyncEvent::Type::ManifestReady ) {
        return std::nullopt;
    }
    const auto* manifest = std::get_if<ResourceManifest>(&event.message);
    return manifest ? std::optional<ResourceManifest>(*manifest) : std::nullopt;
}

/// @brief 一次资源传输的观测结果。
/// @details
/// 该 DTO 汇总完成状态、错误详情、最终资源包和请求覆盖面，使场景能在一次
/// 手工路由后同时断言协议结果与传输效率。requestedFiles 使用集合去重，
/// requestCount 则保留真实请求次数，两者可区分正常分块请求与重复请求。
struct TransferResult {
    /// @brief 是否收到 BundleReady 终态。
    bool success = false;
    /// @brief 是否收到 Error 终态。
    bool error = false;
    /// @brief Error 事件携带的稳定诊断标识。
    std::string errorDetail;
    /// @brief 成功时转移出的访客资源包及生命周期所有权。
    CollaborationResourceBundle bundle;
    /// @brief 访客实际发出的分块请求总数。
    std::size_t requestCount = 0;
    /// @brief 至少请求过一次的资源索引集合。
    std::unordered_set<std::uint32_t> requestedFiles;
};

/// @brief 手工路由房主和访客资源事件，便于逐分块校验与篡改注入。
/// @details
/// 真实应用由 Peer 和传输层转发 ResourceRequest、ResourceChunk；本测试直接
/// 搬运两端事件，以便准确记录请求覆盖面，并可在第一块到达访客前修改一个字节。
/// 每轮先排空访客事件，再排空房主事件，直至 BundleReady、Error 或超时。
/// @param host 已持有不可变资源快照的房主同步器。
/// @param guest 已指定缓存根目录的访客同步器。
/// @param manifest 要交给访客解析的房主清单。
/// @param corruptFirstChunk 是否篡改第一项非空分块以触发摘要校验。
/// @return 包含终态、资源包和请求统计的传输结果。
TransferResult transferResources(CollaborationResourceSync& host,
                                 CollaborationResourceSync& guest,
                                 const ResourceManifest&    manifest,
                                 bool corruptFirstChunk = false)
{
    // 清单是访客创建会话目录和初始请求队列的唯一入口。
    guest.receiveManifest(manifest);
    TransferResult result;
    // corrupted 保证故障注入只修改一块，便于预期错误稳定落在 SHA-256 校验。
    bool       corrupted = false;
    const auto deadline  = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        CollaborationResourceSyncEvent event;
        // progressed 区分本轮真正搬运了消息还是应主动让出 CPU。
        bool progressed = false;
        // 访客事件可能连续产生多个分块请求，必须在轮转到房主前全部收集。
        while ( guest.pollEvent(event) ) {
            progressed = true;
            if ( event.type ==
                 CollaborationResourceSyncEvent::Type::SendRequest ) {
                const auto* request =
                    std::get_if<ResourceRequest>(&event.message);
                if ( !request ) return result;
                // 同时统计次数和资源索引，便于判断每项资源是否实际走过网络。
                ++result.requestCount;
                result.requestedFiles.insert(request->resourceIndex);
                host.receiveRequest(GUEST_ID, *request);
            } else if ( event.type ==
                        CollaborationResourceSyncEvent::Type::BundleReady ) {
                // BundleReady 是成功终态，移动资源包以保留临时目录生命周期。
                result.success = true;
                result.bundle  = std::move(event.bundle);
                return result;
            } else if ( event.type ==
                        CollaborationResourceSyncEvent::Type::Error ) {
                // 错误详情使用稳定标识，场景可精确区分摘要失败和格式失败。
                result.error       = true;
                result.errorDetail = event.detail;
                return result;
            }
        }
        // 房主只应产生响应分块或自身后台错误，不会产生访客请求。
        while ( host.pollEvent(event) ) {
            progressed = true;
            if ( event.type ==
                 CollaborationResourceSyncEvent::Type::SendChunk ) {
                auto* chunk = std::get_if<ResourceChunk>(&event.message);
                if ( !chunk ) return result;
                // 在加密传输正文进入访客状态机前翻转一位，模拟链路篡改。
                if ( corruptFirstChunk && !corrupted &&
                     !chunk->payload.empty() ) {
                    chunk->payload.front() ^= 0xFFU;
                    corrupted = true;
                }
                // 移动分块避免测试路由额外复制较大的 64 KiB 负载。
                guest.receiveChunk(std::move(*chunk));
            } else if ( event.type ==
                        CollaborationResourceSyncEvent::Type::Error ) {
                result.error       = true;
                result.errorDetail = event.detail;
                return result;
            }
        }
        // 没有事件时后台线程可能仍在哈希、加密或物化文件。
        if ( !progressed ) std::this_thread::yield();
    }
    return result;
}

/// @brief 校验资源包中的路径、内容和音频配置均无损。
/// @details
/// 访客项目必须只包含谱面实际引用的两项音频资源，并通过 pathRemap 找到三项
/// 物化文件：主音频、效果音频和封面。验证同时比较资源 ID、类型、全部音频配置
/// 和文件字节，避免只校验摘要却遗漏项目对象重建错误。
/// @param bundle 访客完成同步后持有的资源包。
/// @param sourceProject 房主构造清单时的项目元数据。
/// @param mainBytes 期望主音频快照内容。
/// @param effectBytes 期望效果音频快照内容。
/// @param coverBytes 期望封面快照内容。
/// @return 引用裁剪、路径重映射、配置和文件内容全部正确时返回 true。
bool verifyInitialBundle(const CollaborationResourceBundle& bundle,
                         const Project&                     sourceProject,
                         const ByteBuffer&                  mainBytes,
                         const ByteBuffer&                  effectBytes,
                         const ByteBuffer&                  coverBytes)
{
    // 未引用音频不能进入访客项目，也不能出现在路径重映射表。
    if ( !bundle.project || bundle.project->m_audioResources.size() != 2U ||
         bundle.pathRemap.size() != 3U ||
         bundle.pathRemap.contains("audio/unused.ogg") ) {
        return false;
    }
    // 资源表顺序与清单顺序稳定，可逐索引对照源项目的前两项。
    for ( std::size_t index = 0;
          index < bundle.project->m_audioResources.size();
          ++index ) {
        const auto& received = bundle.project->m_audioResources[index];
        const auto& source   = sourceProject.m_audioResources[index];
        // 路径会被重映射，其他资源元数据与音频处理配置必须无损。
        if ( received.m_id != source.m_id || received.m_type != source.m_type ||
             !sameAudioConfig(received.m_config, source.m_config) ) {
            return false;
        }
        const auto expected = index == 0 ? mainBytes : effectBytes;
        // 通过接收项目根和重写后的相对路径读取实际物化明文。
        const auto actual =
            readBytes(bundle.project->m_projectRoot / received.m_path);
        if ( !actual || *actual != expected ) return false;
    }
    // 封面不属于音频资源表，只通过原始路径到接收路径映射验证。
    const auto cover = bundle.pathRemap.find("images/cover.png");
    if ( cover == bundle.pathRemap.end() ) return false;
    const auto actualCover =
        readBytes(bundle.project->m_projectRoot / cover->second);
    return actualCover && *actualCover == coverBytes;
}

/// @brief 覆盖完整传输、加密落盘、会话隔离和资源变更后的重新同步。
/// @details
/// 场景先创建跨三个 64 KiB 分块的主音频、小型效果音频和封面，并保留一项
/// 未引用资源。房主生成清单后再次 startHost，确认内容未变时 generation 和
/// CBOR 载荷稳定；随后修改源文件，证明当前房间仍发送首次哈希时冻结的快照。
/// 第一名访客完成传输后，测试检查明文包、进度计数、加密容器无明文片段，
/// 以及资源包 shared_ptr 对临时 session 目录的生命周期所有权。
/// 第二名访客使用同一缓存根仍建立隔离会话并完整请求资源，防止跨会话复用
/// 加密材料。最后修改主音频并重启房主，要求 generation 更新且新访客取得新内容。
/// @par 房主快照不变量
/// - 清单 generation 同时标识元数据和已冻结的资源字节。
/// - 清单发布后修改源文件不得改变该 generation 的响应内容。
/// - 源内容变化并重新启动房主时必须产生新的 generation。
/// - 未被谱面引用的项目资源不得进入清单或传输统计。
/// @par 访客会话不变量
/// - 每次 startGuest 都创建独立 session 目录和资源密钥。
/// - 加密容器只能在该会话内物化为重映射后的明文路径。
/// - 同步器释放不应提前删除仍由 Project 使用的会话目录。
/// - 最后一个项目所有者释放后必须自动删除会话目录。
/// @return 三轮同步及全部生命周期、摘要与进度断言通过时返回 true。
bool testRoundTripCacheAndIncrementalChanges()
{
    // 所有源文件和访客会话都位于本场景的隔离临时根下。
    ScopedResourceDirectory directory;
    if ( directory.path().empty() ) return false;
    const auto projectRoot = directory.path() / "host";
    const auto cacheRoot   = directory.path() / "cache";
    const auto mainPath    = projectRoot / "audio/main.bin";
    const auto effectPath  = projectRoot / "audio/effect.wav";
    const auto coverPath   = projectRoot / "images/cover.png";
    const auto unusedPath  = projectRoot / "audio/unused.ogg";
    ByteBuffer mainBytes   = patternedBytes(150003U, 17U);
    // 三字节 abc 具有公开 SHA-256 测试向量，可验证清单摘要实现。
    const ByteBuffer effectBytes{ 'a', 'b', 'c' };
    const ByteBuffer coverBytes = patternedBytes(4097U, 91U);
    if ( !writeBytes(mainPath, mainBytes) ||
         !writeBytes(effectPath, effectBytes) ||
         !writeBytes(coverPath, coverBytes) ||
         !writeBytes(unusedPath, patternedBytes(2048U, 33U)) ) {
        return false;
    }

    // 房主扫描谱面引用并在后台冻结资源快照、计算清单摘要。
    Project project;
    BeatMap beatmap;
    makeProjectAndBeatmap(projectRoot, project, beatmap);
    CollaborationResourceSync host;
    host.startHost(project, beatmap);
    const auto manifest = waitManifest(host);
    if ( !manifest ) return false;

    const auto manifestJson =
        nlohmann::json::from_cbor(manifest->payload, true, false);
    // 直接解析 CBOR 以独立验证清单文件数量和已知 abc 摘要。
    bool knownHashFound = false;
    if ( manifestJson.is_discarded() || !manifestJson.is_object() ) {
        return false;
    }
    const auto files = manifestJson.find("files");
    if ( files == manifestJson.end() || !files->is_array() ||
         files->size() != 3U ) {
        return false;
    }
    for ( const auto& file : *files ) {
        // 以唯一的三字节长度定位效果音频清单项。
        if ( file.value("size", 0U) == 3U ) {
            knownHashFound = file.value("sha256", std::string{}) ==
                             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb"
                             "410ff61f20015ad";
        }
    }
    if ( !knownHashFound ) return false;

    // 源项目未变时重复 startHost 应复用同一代次和规范化清单内容。
    host.startHost(project, beatmap);
    const auto stableManifest = waitManifest(host);
    if ( !stableManifest ||
         stableManifest->generation != manifest->generation ||
         stableManifest->payload != manifest->payload ) {
        return false;
    }

    // 清单完成后修改项目源文件，当前房间仍必须发送已哈希的不可变快照。
    if ( !writeBytes(mainPath, patternedBytes(mainBytes.size(), 99U)) ) {
        return false;
    }

    CollaborationResourceSync firstGuest;
    // 新访客在独立 session 子目录接收三项被引用资源。
    firstGuest.startGuest(cacheRoot);
    auto first = transferResources(host, firstGuest, *stableManifest, false);
    if ( !first.success || first.error || first.requestedFiles.size() != 3U ||
         !verifyInitialBundle(
             first.bundle, project, mainBytes, effectBytes, coverBytes) ) {
        return false;
    }
    const auto firstProgress = firstGuest.progress();
    // Ready 终态的完成数和传输字节必须与清单引用资源总量一致。
    if ( firstProgress.phase != CollaborationResourceSyncPhase::Ready ||
         firstProgress.completedFiles != 3U ||
         firstProgress.transferredBytes !=
             mainBytes.size() + effectBytes.size() + coverBytes.size() ) {
        return false;
    }

    const auto firstSessionRoot =
        first.bundle.project->m_projectRoot.parent_path();
    // 接收项目根位于 session 目录下，pathRemap 提供其内部明文相对路径。
    const auto effectRemap = first.bundle.pathRemap.find("audio/effect.wav");
    if ( effectRemap == first.bundle.pathRemap.end() ||
         firstSessionRoot.parent_path() != cacheRoot ||
         !firstSessionRoot.filename().string().starts_with("session-") ) {
        return false;
    }
    const auto encryptedEffect =
        firstSessionRoot / "encrypted" /
        MMM::Config::utf8ToPath(effectRemap->second + ".mmrsc");
    const auto encryptedBytes = readBytes(encryptedEffect);
    // GCM 容器包含头、随机量和认证标签，且不能出现连续 abc 明文。
    if ( !encryptedBytes || encryptedBytes->size() <= effectBytes.size() ||
         std::search(encryptedBytes->begin(),
                     encryptedBytes->end(),
                     effectBytes.begin(),
                     effectBytes.end()) != encryptedBytes->end() ) {
        return false;
    }
    firstGuest.reset();
    std::error_code lifetimeError;
    // 同步器销毁后资源包仍持有会话目录租约，因此目录必须继续存在。
    if ( !std::filesystem::exists(firstSessionRoot, lifetimeError) ||
         lifetimeError ) {
        return false;
    }
    first.bundle.project.reset();
    // 最后一个项目所有者释放后，会话目录应立即由租约清理。
    if ( std::filesystem::exists(firstSessionRoot, lifetimeError) ||
         lifetimeError ) {
        return false;
    }

    CollaborationResourceSync cachedGuest;
    // 同一 cacheRoot 不代表同一会话；密钥隔离要求重新传输全部资源。
    cachedGuest.startGuest(cacheRoot);
    const auto cached =
        transferResources(host, cachedGuest, *stableManifest, false);
    const auto cachedProgress = cachedGuest.progress();
    if ( !cached.success || cached.requestedFiles.size() != 3U ||
         cachedProgress.cachedFiles != 0U ||
         cachedProgress.transferredBytes !=
             mainBytes.size() + effectBytes.size() + coverBytes.size() ) {
        return false;
    }

    mainBytes[mainBytes.size() / 2U] ^= 0x5AU;
    // 修改文件中部确保大小不变，仅内容摘要和代次发生变化。
    if ( !writeBytes(mainPath, mainBytes) ) return false;
    host.startHost(project, beatmap);
    const auto changedManifest = waitManifest(host);
    if ( !changedManifest ||
         changedManifest->generation == manifest->generation ) {
        return false;
    }
    CollaborationResourceSync changedGuest;
    changedGuest.startGuest(cacheRoot);
    const auto changed =
        transferResources(host, changedGuest, *changedManifest, false);
    const auto changedProgress = changedGuest.progress();
    // 新代次仍应完整传输三项，并物化修改后的主音频字节。
    if ( !changed.success || changed.requestedFiles.size() != 3U ||
         changedProgress.cachedFiles != 0U ||
         changedProgress.transferredBytes !=
             mainBytes.size() + effectBytes.size() + coverBytes.size() ||
         !verifyInitialBundle(
             changed.bundle, project, mainBytes, effectBytes, coverBytes) ) {
        return false;
    }
    return true;
}

/// @brief 覆盖清单篡改、代次错配和分块篡改的拒绝路径。
/// @details
/// 第一条路径翻转清单 CBOR 末字节，要求访客在创建传输计划前报告格式错误。
/// 第二条只修改外层 generation，使其与已签入载荷的代次不一致，同样必须拒绝。
/// 第三条保留合法清单，但翻转第一块密文载荷，要求最终 SHA-256 校验失败。
/// 错误结束后缓存根可以保留，但任何 `.part-` 临时文件都必须删除，防止下次
/// 同步把不完整资源误当成可恢复内容。
/// @par 失败终态不变量
/// - 无效清单不能产生 ResourceRequest。
/// - 错误 generation 不能建立另一套未认证传输计划。
/// - 分块摘要失败不能发布 BundleReady。
/// - 部分密文不能重命名为可用缓存容器。
/// - Error 事件必须提供可稳定断言的机器可读详情。
/// @return 三种篡改均被准确拒绝且临时分块清理完成时返回 true。
bool testTamperRejection()
{
    ScopedResourceDirectory directory;
    if ( directory.path().empty() ) return false;
    const auto       projectRoot = directory.path() / "host";
    const auto       mainBytes   = patternedBytes(70001U, 7U);
    const ByteBuffer effectBytes{ 'a', 'b', 'c' };
    const auto       coverBytes = patternedBytes(513U, 71U);
    if ( !writeBytes(projectRoot / "audio/main.bin", mainBytes) ||
         !writeBytes(projectRoot / "audio/effect.wav", effectBytes) ||
         !writeBytes(projectRoot / "images/cover.png", coverBytes) ||
         !writeBytes(projectRoot / "audio/unused.ogg",
                     patternedBytes(64U, 3U)) ) {
        return false;
    }
    Project project;
    BeatMap beatmap;
    makeProjectAndBeatmap(projectRoot, project, beatmap);
    CollaborationResourceSync host;
    host.startHost(project, beatmap);
    const auto manifest = waitManifest(host);
    if ( !manifest ) return false;

    // 清单载荷变更但没有重新生成对应认证信息，解析必须失败。
    ResourceManifest tamperedManifest = *manifest;
    tamperedManifest.payload.back() ^= 0x01U;
    CollaborationResourceSync tamperedGuest;
    tamperedGuest.startGuest(directory.path() / "tampered-manifest-cache");
    tamperedGuest.receiveManifest(std::move(tamperedManifest));
    CollaborationResourceSyncEvent event;
    if ( !waitEvent(tamperedGuest, event) ||
         event.type != CollaborationResourceSyncEvent::Type::Error ||
         event.detail != "invalid_resource_manifest" ) {
        return false;
    }

    ResourceManifest wrongGeneration = *manifest;
    // 外层 generation 与 CBOR 内记录不一致，不能接受为另一合法代次。
    ++wrongGeneration.generation;
    CollaborationResourceSync generationGuest;
    generationGuest.startGuest(directory.path() / "generation-cache");
    generationGuest.receiveManifest(std::move(wrongGeneration));
    if ( !waitEvent(generationGuest, event) ||
         event.type != CollaborationResourceSyncEvent::Type::Error ||
         event.detail != "invalid_resource_manifest" ) {
        return false;
    }

    CollaborationResourceSync chunkGuest;
    // 合法清单进入传输后，只篡改第一项非空分块以命中端到端摘要检查。
    const auto chunkCache = directory.path() / "chunk-cache";
    chunkGuest.startGuest(chunkCache);
    const auto corrupted = transferResources(host, chunkGuest, *manifest, true);
    if ( !corrupted.error || corrupted.success ||
         corrupted.errorDetail != "resource_sha256_mismatch" ) {
        return false;
    }
    std::error_code error;
    // 状态机可保留缓存根，但失败资源的临时组装文件必须全部回收。
    if ( !std::filesystem::exists(chunkCache, error) || error ) return false;
    for ( const auto& entry :
          std::filesystem::recursive_directory_iterator(chunkCache, error) ) {
        if ( error || entry.path().filename().string().contains(".part-") ) {
            return false;
        }
    }
    return !error;
}

/// @brief 验证清单不会读取项目根目录之外的相对资源。
/// @details
/// 项目资源路径使用 `../outside.bin` 指向隔离目录中真实存在、但不属于项目根的
/// 文件。房主扫描必须按规范化路径执行根目录边界检查，并把该引用报告为缺失，
/// 不能因为目标可读就将项目外数据加入协作清单。
/// @return 收到对应资源 ID 的 host_resource_missing 错误时返回 true。
bool testHostProjectBoundary()
{
    ScopedResourceDirectory directory;
    if ( directory.path().empty() ) return false;
    const auto      projectRoot = directory.path() / "host";
    std::error_code error;
    std::filesystem::create_directories(projectRoot, error);
    if ( error || !writeBytes(directory.path() / "outside.bin",
                              patternedBytes(128U, 0x44U)) ) {
        return false;
    }

    Project project;
    // 路径在词法规范化后逃逸 host 根目录，是本场景唯一非法条件。
    project.m_projectRoot = projectRoot;
    project.m_audioResources.push_back(AudioResource{
        .m_id   = "outside-id",
        .m_path = "../outside.bin",
        .m_type = AudioTrackType::Effect,
    });
    BeatMap beatmap;
    beatmap.m_audioSamples.emplace_back().m_audioResourceId = "outside-id";

    CollaborationResourceSync host;
    // 错误由后台清单构建产生，因此仍通过事件队列等待。
    host.startHost(project, beatmap);
    CollaborationResourceSyncEvent event;
    return waitEvent(host, event) &&
           event.type == CollaborationResourceSyncEvent::Type::Error &&
           event.detail == "host_resource_missing:outside-id";
}

/// @brief 验证资源容器可认证解密，并拒绝被篡改的 GCM 密文。
/// @details
/// 该场景绕过资源状态机，直接覆盖自维护加密容器的生成、物化和密钥清零契约。
/// 先用超过一个常见分块大小的明文完成往返并比较完整字节；随后翻转密文末位，
/// GCM 认证必须失败，且目标明文和 `.part-materialize` 中间文件都不能留下。
/// 最后显式清理密钥并逐字节验证为零，防止测试只检查逻辑返回值而遗漏敏感数据。
/// @par 认证容器不变量
/// - 加密结果记录的明文大小必须与源文件一致。
/// - 合法容器物化后必须与源字节完全相等。
/// - 任意密文或认证标签变化都必须使物化失败。
/// - 认证完成前不得把部分明文发布到最终目标路径。
/// - 成功和失败路径最终都不得保留调用方持有的密钥内容。
/// @return 正常解密成功、篡改拒绝且密钥和临时文件清理完成时返回 true。
bool testEncryptedContainerAuthentication()
{
    ScopedResourceDirectory directory;
    if ( directory.path().empty() ) return false;
    const auto plainBytes = patternedBytes(80U * 1024U, 0x29U);
    const auto source     = directory.path() / "source.bin";
    const auto encrypted  = directory.path() / "source.mmrsc";
    const auto restored   = directory.path() / "restored.bin";
    const auto rejected   = directory.path() / "rejected.bin";
    if ( !writeBytes(source, plainBytes) ) return false;

    namespace Detail = MMM::Network::Collaboration::Detail;
    // 测试持有固定大小密钥缓冲，生成函数负责填入安全随机内容。
    Detail::CollaborationResourceKey key{};
    if ( !Detail::generateCollaborationResourceKey(key) ) return false;
    const auto digest =
        Detail::encryptCollaborationResourceFile(source, encrypted, key, {});
    // 摘要记录明文大小，合法容器应能物化出逐字节一致的文件。
    if ( !digest || digest->size != plainBytes.size() ||
         !Detail::materializeCollaborationResourceFile(
             encrypted, restored, key, digest->size) ||
         readBytes(restored) != std::optional<ByteBuffer>(plainBytes) ) {
        Detail::clearCollaborationResourceKey(key);
        return false;
    }

    auto encryptedBytes = readBytes(encrypted);
    if ( !encryptedBytes || encryptedBytes->empty() ) {
        Detail::clearCollaborationResourceKey(key);
        return false;
    }
    encryptedBytes->back() ^= 0x80U;
    // 修改认证标签附近的末字节，任何单比特变化都必须使 GCM 验证失败。
    if ( !writeBytes(encrypted, *encryptedBytes) ) {
        Detail::clearCollaborationResourceKey(key);
        return false;
    }
    const bool rejectedTamper =
        !Detail::materializeCollaborationResourceFile(
            encrypted, rejected, key, digest->size) &&
        !std::filesystem::exists(rejected) &&
        !std::filesystem::exists(rejected.string() + ".part-materialize");
    Detail::clearCollaborationResourceKey(key);
    // 失败路径既不能发布目标文件，也不能遗留可被误用的部分物化文件。
    return rejectedTamper &&
           std::all_of(key.begin(), key.end(), [](std::uint8_t byte) {
               return byte == 0U;
           });
}
}  // namespace

/// @brief 运行协作资源清单、分块、缓存与完整性回归测试。
/// @details
/// 资源同步使用全局应用线程池执行后台文件任务，因此入口在所有场景前初始化，
/// 并在任一场景失败后统一关闭。独立退出码分别定位正常往返、篡改拒绝、
/// 项目根边界和加密容器认证，避免后台线程在提前 return 时越过清理流程。
/// @par 退出码
/// - 1 表示正常传输、代次或会话生命周期失败。
/// - 2 表示清单或分块篡改未被正确处理。
/// - 3 表示项目根目录边界失效。
/// - 4 表示加密容器认证或密钥清理失败。
/// @return 全部场景通过时返回 0，否则返回对应场景编号。
int main()
{
    // 线程池生命周期覆盖所有同步器，且 shutdown 在局部同步器析构后安全收尾。
    auto& appThreadPool = MMM::Runtime::AppThreadPool::instance();
    appThreadPool.init();

    int result = 0;
    // else-if 保留首个失败，便于 CTest 退出码直接定位根因。
    if ( !testRoundTripCacheAndIncrementalChanges() ) {
        result = 1;
    } else if ( !testTamperRejection() ) {
        result = 2;
    } else if ( !testHostProjectBoundary() ) {
        result = 3;
    } else if ( !testEncryptedContainerAuthentication() ) {
        result = 4;
    }

    appThreadPool.shutdown();
    return result;
}
