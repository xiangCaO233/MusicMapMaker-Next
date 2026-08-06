#include "network/collaboration/CollaborationResourceSync.h"

#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

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
constexpr auto TEST_TIMEOUT = std::chrono::seconds(10);
/// @brief 资源请求测试使用的访客标识。
constexpr MMM::Network::Collaboration::PeerId GUEST_ID = 2;

/// @brief 为资源测试创建并清理隔离目录。
class ScopedResourceDirectory
{
public:
    /// @brief 创建唯一资源目录。
    ScopedResourceDirectory()
    {
        std::error_code error;
        auto            root = std::filesystem::temp_directory_path(error);
        if ( error ) return;
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path =
            root / ("mmm-collaboration-resource-" + std::to_string(suffix));
        std::filesystem::create_directories(m_path, error);
        if ( error ) m_path.clear();
    }

    /// @brief 删除测试拥有的目录。
    ~ScopedResourceDirectory()
    {
        if ( m_path.empty() ) return;
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    ScopedResourceDirectory(const ScopedResourceDirectory&)            = delete;
    ScopedResourceDirectory& operator=(const ScopedResourceDirectory&) = delete;

    /// @brief 返回测试目录。
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    /// @brief 测试目录路径。
    std::filesystem::path m_path;
};

/// @brief 将字节完整写入测试文件。
bool writeBytes(const std::filesystem::path& path, const ByteBuffer& bytes)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if ( error ) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if ( !output ) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    return static_cast<bool>(output);
}

/// @brief 读取测试文件全部字节。
std::optional<ByteBuffer> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if ( !input ) return std::nullopt;
    const auto size = input.tellg();
    if ( size < 0 ) return std::nullopt;
    ByteBuffer bytes(
        static_cast<std::size_t>(static_cast<std::streamoff>(size)));
    input.seekg(0, std::ios::beg);
    if ( !bytes.empty() ) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if ( !input && !input.eof() ) return std::nullopt;
    return bytes;
}

/// @brief 生成可跨多个 64 KiB 分块验证的确定性内容。
ByteBuffer patternedBytes(std::size_t size, std::uint8_t seed)
{
    ByteBuffer bytes(size);
    for ( std::size_t index = 0; index < size; ++index ) {
        bytes[index] = static_cast<std::uint8_t>(
            seed + static_cast<std::uint8_t>((index * 131U) % 251U));
    }
    return bytes;
}

/// @brief 构造覆盖全部音轨配置字段的测试配置。
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
bool sameAudioConfig(const AudioTrackConfig& lhs, const AudioTrackConfig& rhs)
{
    return lhs.volume == rhs.volume && lhs.playbackSpeed == rhs.playbackSpeed &&
           lhs.playbackPitch == rhs.playbackPitch && lhs.muted == rhs.muted &&
           lhs.eqEnabled == rhs.eqEnabled && lhs.eqPreset == rhs.eqPreset &&
           lhs.eqBandGains == rhs.eqBandGains && lhs.eqBandQs == rhs.eqBandQs;
}

/// @brief 构造资源项目和谱面引用。
void makeProjectAndBeatmap(const std::filesystem::path& root, Project& project,
                           BeatMap& beatmap)
{
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
    auto& child           = beatmap.m_noteData.flicks.emplace_back();
    child.m_isSubNote     = true;
    child.m_sampleBinding = MMM::AudioSampleBinding{
        .m_audioResourceId = "effect-id",
        .m_volume          = 0.75F,
    };
    auto& polyline = beatmap.m_noteData.polylines.emplace_back();
    polyline.m_subFlicks.emplace_back(child);
    polyline.m_subNotes.emplace_back(child);
    auto& sample             = beatmap.m_audioSamples.emplace_back();
    sample.m_audioResourceId = "main-id";
    beatmap.sync();
}

/// @brief 等待并取出指定同步器的下一事件。
bool waitEvent(CollaborationResourceSync&      sync,
               CollaborationResourceSyncEvent& event)
{
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        if ( sync.pollEvent(event) ) return true;
        std::this_thread::yield();
    }
    return false;
}

/// @brief 等待房主清单准备完成。
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
struct TransferResult {
    bool                              success = false;
    bool                              error   = false;
    std::string                       errorDetail;
    CollaborationResourceBundle       bundle;
    std::size_t                       requestCount = 0;
    std::unordered_set<std::uint32_t> requestedFiles;
};

/// @brief 手工路由房主和访客资源事件，便于逐分块校验与篡改注入。
TransferResult transferResources(CollaborationResourceSync& host,
                                 CollaborationResourceSync& guest,
                                 const ResourceManifest&    manifest,
                                 bool corruptFirstChunk = false)
{
    guest.receiveManifest(manifest);
    TransferResult result;
    bool           corrupted = false;
    const auto     deadline  = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        CollaborationResourceSyncEvent event;
        bool                           progressed = false;
        while ( guest.pollEvent(event) ) {
            progressed = true;
            if ( event.type ==
                 CollaborationResourceSyncEvent::Type::SendRequest ) {
                const auto* request =
                    std::get_if<ResourceRequest>(&event.message);
                if ( !request ) return result;
                ++result.requestCount;
                result.requestedFiles.insert(request->resourceIndex);
                host.receiveRequest(GUEST_ID, *request);
            } else if ( event.type ==
                        CollaborationResourceSyncEvent::Type::BundleReady ) {
                result.success = true;
                result.bundle  = std::move(event.bundle);
                return result;
            } else if ( event.type ==
                        CollaborationResourceSyncEvent::Type::Error ) {
                result.error       = true;
                result.errorDetail = event.detail;
                return result;
            }
        }
        while ( host.pollEvent(event) ) {
            progressed = true;
            if ( event.type ==
                 CollaborationResourceSyncEvent::Type::SendChunk ) {
                auto* chunk = std::get_if<ResourceChunk>(&event.message);
                if ( !chunk ) return result;
                if ( corruptFirstChunk && !corrupted &&
                     !chunk->payload.empty() ) {
                    chunk->payload.front() ^= 0xFFU;
                    corrupted = true;
                }
                guest.receiveChunk(std::move(*chunk));
            } else if ( event.type ==
                        CollaborationResourceSyncEvent::Type::Error ) {
                result.error       = true;
                result.errorDetail = event.detail;
                return result;
            }
        }
        if ( !progressed ) std::this_thread::yield();
    }
    return result;
}

/// @brief 校验资源包中的路径、内容和音频配置均无损。
bool verifyInitialBundle(const CollaborationResourceBundle& bundle,
                         const Project&                     sourceProject,
                         const ByteBuffer&                  mainBytes,
                         const ByteBuffer&                  effectBytes,
                         const ByteBuffer&                  coverBytes)
{
    if ( !bundle.project || bundle.project->m_audioResources.size() != 2U ||
         bundle.pathRemap.size() != 3U ||
         bundle.pathRemap.contains("audio/unused.ogg") ) {
        return false;
    }
    for ( std::size_t index = 0;
          index < bundle.project->m_audioResources.size();
          ++index ) {
        const auto& received = bundle.project->m_audioResources[index];
        const auto& source   = sourceProject.m_audioResources[index];
        if ( received.m_id != source.m_id || received.m_type != source.m_type ||
             !sameAudioConfig(received.m_config, source.m_config) ) {
            return false;
        }
        const auto expected = index == 0 ? mainBytes : effectBytes;
        const auto actual =
            readBytes(bundle.project->m_projectRoot / received.m_path);
        if ( !actual || *actual != expected ) return false;
    }
    const auto cover = bundle.pathRemap.find("images/cover.png");
    if ( cover == bundle.pathRemap.end() ) return false;
    const auto actualCover =
        readBytes(bundle.project->m_projectRoot / cover->second);
    return actualCover && *actualCover == coverBytes;
}

/// @brief 覆盖完整传输、缓存复用、单资源变更和损坏缓存重取。
bool testRoundTripCacheAndIncrementalChanges()
{
    ScopedResourceDirectory directory;
    if ( directory.path().empty() ) return false;
    const auto       projectRoot = directory.path() / "host";
    const auto       cacheRoot   = directory.path() / "cache";
    const auto       mainPath    = projectRoot / "audio/main.bin";
    const auto       effectPath  = projectRoot / "audio/effect.wav";
    const auto       coverPath   = projectRoot / "images/cover.png";
    const auto       unusedPath  = projectRoot / "audio/unused.ogg";
    ByteBuffer       mainBytes   = patternedBytes(150003U, 17U);
    const ByteBuffer effectBytes{ 'a', 'b', 'c' };
    const ByteBuffer coverBytes = patternedBytes(4097U, 91U);
    if ( !writeBytes(mainPath, mainBytes) ||
         !writeBytes(effectPath, effectBytes) ||
         !writeBytes(coverPath, coverBytes) ||
         !writeBytes(unusedPath, patternedBytes(2048U, 33U)) ) {
        return false;
    }

    Project project;
    BeatMap beatmap;
    makeProjectAndBeatmap(projectRoot, project, beatmap);
    CollaborationResourceSync host;
    host.startHost(project, beatmap);
    const auto manifest = waitManifest(host);
    if ( !manifest ) return false;

    const auto manifestJson =
        nlohmann::json::from_cbor(manifest->payload, true, false);
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
        if ( file.value("size", 0U) == 3U ) {
            knownHashFound = file.value("sha256", std::string{}) ==
                             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb"
                             "410ff61f20015ad";
        }
    }
    if ( !knownHashFound ) return false;

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
    firstGuest.startGuest(cacheRoot);
    const auto first =
        transferResources(host, firstGuest, *stableManifest, false);
    if ( !first.success || first.error || first.requestedFiles.size() != 3U ||
         !verifyInitialBundle(
             first.bundle, project, mainBytes, effectBytes, coverBytes) ) {
        return false;
    }
    const auto firstProgress = firstGuest.progress();
    if ( firstProgress.phase != CollaborationResourceSyncPhase::Ready ||
         firstProgress.completedFiles != 3U ||
         firstProgress.transferredBytes !=
             mainBytes.size() + effectBytes.size() + coverBytes.size() ) {
        return false;
    }

    CollaborationResourceSync cachedGuest;
    cachedGuest.startGuest(cacheRoot);
    const auto cached =
        transferResources(host, cachedGuest, *stableManifest, false);
    const auto cachedProgress = cachedGuest.progress();
    if ( !cached.success || cached.requestCount != 0U ||
         cachedProgress.cachedFiles != 3U ||
         cachedProgress.transferredBytes != 0U ) {
        return false;
    }

    mainBytes[mainBytes.size() / 2U] ^= 0x5AU;
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
    if ( !changed.success || changed.requestedFiles.size() != 1U ||
         changedProgress.cachedFiles != 2U ||
         changedProgress.transferredBytes != mainBytes.size() ||
         !verifyInitialBundle(
             changed.bundle, project, mainBytes, effectBytes, coverBytes) ) {
        return false;
    }

    const auto effectRemap = changed.bundle.pathRemap.find("audio/effect.wav");
    if ( effectRemap == changed.bundle.pathRemap.end() ||
         !writeBytes(cacheRoot / effectRemap->second,
                     patternedBytes(effectBytes.size(), 0xE1U)) ) {
        return false;
    }
    CollaborationResourceSync repairedGuest;
    repairedGuest.startGuest(cacheRoot);
    const auto repaired =
        transferResources(host, repairedGuest, *changedManifest, false);
    const auto repairedProgress = repairedGuest.progress();
    return repaired.success && repaired.requestedFiles.size() == 1U &&
           repairedProgress.cachedFiles == 2U &&
           verifyInitialBundle(
               repaired.bundle, project, mainBytes, effectBytes, coverBytes);
}

/// @brief 覆盖清单篡改、代次错配和分块篡改的拒绝路径。
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
    const auto                chunkCache = directory.path() / "chunk-cache";
    chunkGuest.startGuest(chunkCache);
    const auto corrupted = transferResources(host, chunkGuest, *manifest, true);
    if ( !corrupted.error || corrupted.success ||
         corrupted.errorDetail != "resource_sha256_mismatch" ) {
        return false;
    }
    std::error_code error;
    const auto      filesRoot = chunkCache / "files";
    if ( !std::filesystem::exists(filesRoot, error) || error ) return false;
    for ( const auto& entry :
          std::filesystem::directory_iterator(filesRoot, error) ) {
        if ( error || entry.path().filename().string().contains(".part-") ) {
            return false;
        }
    }
    return !error;
}

/// @brief 验证清单不会读取项目根目录之外的相对资源。
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
    project.m_projectRoot = projectRoot;
    project.m_audioResources.push_back(AudioResource{
        .m_id   = "outside-id",
        .m_path = "../outside.bin",
        .m_type = AudioTrackType::Effect,
    });
    BeatMap beatmap;
    beatmap.m_audioSamples.emplace_back().m_audioResourceId = "outside-id";

    CollaborationResourceSync host;
    host.startHost(project, beatmap);
    CollaborationResourceSyncEvent event;
    return waitEvent(host, event) &&
           event.type == CollaborationResourceSyncEvent::Type::Error &&
           event.detail == "host_resource_missing:outside-id";
}
}  // namespace

/// @brief 运行协作资源清单、分块、缓存与完整性回归测试。
int main()
{
    if ( !testRoundTripCacheAndIncrementalChanges() ) return 1;
    if ( !testTamperRejection() ) return 2;
    if ( !testHostProjectBoundary() ) return 3;
    return 0;
}
