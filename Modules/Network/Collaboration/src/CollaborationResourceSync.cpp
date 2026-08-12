#include "network/collaboration/CollaborationResourceSync.h"

#include "CollaborationResourceCipher.h"

#include "config/Utf8Path.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <iomanip>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <sstream>
#include <stop_token>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MMM::Network::Collaboration
{
namespace
{
using Json = nlohmann::json;

/// @brief 资源清单格式版本。
constexpr std::uint32_t RESOURCE_MANIFEST_VERSION = 1U;
/// @brief 单次文件请求大小，避免占满数据通道消息上限。
constexpr std::uint32_t RESOURCE_CHUNK_BYTES =
    Detail::COLLABORATION_RESOURCE_BLOCK_BYTES;
/// @brief 单个清单允许的最大文件数。
constexpr std::size_t MAX_RESOURCE_FILES = 4096U;
/// @brief 单个清单允许声明的最大总字节数。
constexpr std::uint64_t MAX_RESOURCE_TOTAL_BYTES =
    32ULL * 1024ULL * 1024ULL * 1024ULL;
/// @brief 单个文件允许的最大字节数。
constexpr std::uint64_t MAX_RESOURCE_FILE_BYTES =
    8ULL * 1024ULL * 1024ULL * 1024ULL;

/// @brief 为同机多客户端生成互不覆盖的临时分块后缀。
std::string makeTransferSuffix()
{
    static std::atomic<std::uint64_t> sequence{ 1U };
    const auto                        timestamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::to_string(timestamp) + "-" +
           std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed));
}

/// @brief SHA-256 初始状态。
constexpr std::array<std::uint32_t, 8> SHA256_INITIAL_STATE{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};
/// @brief SHA-256 轮常量。
constexpr std::array<std::uint32_t, 64> SHA256_ROUND_CONSTANTS{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

/// @brief 循环右移 32 位整数。
std::uint32_t rotateRight(std::uint32_t value, std::uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

/// @brief 从大端字节读取 32 位整数。
std::uint32_t readBigEndian32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

/// @brief 向字节数组写入大端 32 位整数。
void writeBigEndian32(std::uint32_t value, std::uint8_t* bytes)
{
    bytes[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    bytes[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

/// @brief 对一个完整 64 字节块执行 SHA-256 压缩。
void transformSha256(std::array<std::uint32_t, 8>& state,
                     const std::uint8_t*           block)
{
    std::array<std::uint32_t, 64> words{};
    for ( std::size_t index = 0; index < 16U; ++index ) {
        words[index] = readBigEndian32(block + index * 4U);
    }
    for ( std::size_t index = 16U; index < 64U; ++index ) {
        const std::uint32_t s0 = rotateRight(words[index - 15U], 7U) ^
                                 rotateRight(words[index - 15U], 18U) ^
                                 (words[index - 15U] >> 3U);
        const std::uint32_t s1 = rotateRight(words[index - 2U], 17U) ^
                                 rotateRight(words[index - 2U], 19U) ^
                                 (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    for ( std::size_t index = 0; index < 64U; ++index ) {
        const std::uint32_t s1 =
            rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
        const std::uint32_t choice = (e & f) ^ ((~e) & g);
        const std::uint32_t temporary1 =
            h + s1 + choice + SHA256_ROUND_CONSTANTS[index] + words[index];
        const std::uint32_t s0 =
            rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
        const std::uint32_t majority   = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary2 = s0 + majority;
        h                              = g;
        g                              = f;
        f                              = e;
        e                              = d + temporary1;
        d                              = c;
        c                              = b;
        b                              = a;
        a                              = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/// @brief 支持分块输入、固定内存占用的 SHA-256 累加器。
class Sha256Accumulator
{
public:
    /// @brief 追加任意长度字节。
    void update(std::span<const std::uint8_t> bytes)
    {
        m_totalBytes += bytes.size();
        while ( !bytes.empty() ) {
            const auto copied =
                std::min(bytes.size(), m_buffer.size() - m_used);
            std::copy_n(bytes.begin(), copied, m_buffer.begin() + m_used);
            m_used += copied;
            bytes = bytes.subspan(copied);
            if ( m_used == m_buffer.size() ) {
                transformSha256(m_state, m_buffer.data());
                m_used = 0;
            }
        }
    }

    /// @brief 写入末尾长度并返回小写十六进制摘要。
    [[nodiscard]] std::string finish()
    {
        const std::uint64_t bitLength = m_totalBytes * 8ULL;
        m_buffer[m_used++]            = 0x80U;
        if ( m_used > 56U ) {
            std::fill(m_buffer.begin() + m_used, m_buffer.end(), 0U);
            transformSha256(m_state, m_buffer.data());
            m_used = 0;
        }
        std::fill(m_buffer.begin() + m_used, m_buffer.begin() + 56U, 0U);
        for ( std::size_t index = 0; index < 8U; ++index ) {
            m_buffer[56U + index] = static_cast<std::uint8_t>(
                (bitLength >> ((7U - index) * 8U)) & 0xFFU);
        }
        transformSha256(m_state, m_buffer.data());

        std::array<std::uint8_t, 32> digest{};
        for ( std::size_t index = 0; index < m_state.size(); ++index ) {
            writeBigEndian32(m_state[index], digest.data() + index * 4U);
        }
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for ( const auto byte : digest ) {
            output << std::setw(2) << static_cast<unsigned int>(byte);
        }
        return output.str();
    }

private:
    /// @brief 当前压缩状态。
    std::array<std::uint32_t, 8> m_state = SHA256_INITIAL_STATE;
    /// @brief 尚不足一个压缩块的尾部。
    std::array<std::uint8_t, 64> m_buffer{};
    /// @brief 尾部已占用字节数。
    std::size_t m_used = 0;
    /// @brief 已输入的完整字节数。
    std::uint64_t m_totalBytes = 0;
};

/// @brief 计算内存字节的 SHA-256 小写十六进制摘要。
std::string sha256Bytes(const ByteBuffer& input)
{
    Sha256Accumulator accumulator;
    accumulator.update(input);
    return accumulator.finish();
}

/// @brief 判断字符串是否为固定长度的小写 SHA-256。
bool isSha256(std::string_view value);

/// @brief 从清单摘要稳定派生资源协议代次。
std::uint64_t manifestGeneration(std::string_view manifestId)
{
    if ( !isSha256(manifestId) ) return 0;
    std::uint64_t generation = 0;
    for ( std::size_t index = 0; index < 16U; ++index ) {
        const char character = manifestId[index];
        generation =
            (generation << 4U) |
            static_cast<std::uint64_t>(character <= '9' ? character - '0'
                                                        : character - 'a' + 10);
    }
    return generation == 0 ? 1 : generation;
}

/// @brief 以固定缓冲区读取文件并计算 SHA-256；资源线程外禁止调用。
std::string sha256File(const std::filesystem::path& path,
                       std::stop_token              stopToken = {})
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if ( !input ) return {};
    const auto end = input.tellg();
    if ( end < 0 || static_cast<std::uint64_t>(static_cast<std::streamoff>(
                        end)) > MAX_RESOURCE_FILE_BYTES ) {
        return {};
    }
    input.seekg(0, std::ios::beg);
    Sha256Accumulator                     accumulator;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while ( input ) {
        if ( stopToken.stop_requested() ) return {};
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if ( count > 0 ) {
            accumulator.update(std::span<const std::uint8_t>(
                buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    if ( input.bad() ) return {};
    return accumulator.finish();
}

/// @brief 判断字符串是否为固定长度的小写 SHA-256。
bool isSha256(std::string_view value)
{
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

/// @brief 生成只包含安全字符的短扩展名。
std::string safeExtension(const std::filesystem::path& path)
{
    std::string extension = Config::pathToUtf8(path.extension());
    if ( extension.size() > 12U ||
         !std::all_of(extension.begin(), extension.end(), [](char character) {
             return character == '.' ||
                    (character >= '0' && character <= '9') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= 'a' && character <= 'z');
         }) ) {
        return {};
    }
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension;
}

/// @brief 资源清单中的单个文件。
struct ManifestFile {
    std::string   sha256;
    std::string   cachePath;
    std::uint64_t size = 0;
};

/// @brief 解码后的完整资源清单。
struct DecodedManifest {
    std::string                                  id;
    std::vector<ManifestFile>                    files;
    std::vector<::MMM::AudioResource>            audioResources;
    std::unordered_map<std::string, std::string> pathRemap;
};

/// @brief 安全读取 JSON 字符串字段。
bool readString(const Json& object, std::string_view key, std::string& output)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) return false;
    output = iterator->get<std::string>();
    return true;
}

/// @brief 安全读取 JSON 无符号整数字段。
template<typename Value>
bool readUnsigned(const Json& object, std::string_view key, Value& output)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_number_unsigned() ) {
        return false;
    }
    const auto value = iterator->get<std::uint64_t>();
    if ( value > std::numeric_limits<Value>::max() ) return false;
    output = static_cast<Value>(value);
    return true;
}

/// @brief 从已验证对象读取音轨配置。
bool decodeAudioConfig(const Json& source, ::MMM::AudioTrackConfig& config)
{
    if ( !source.is_object() ) return false;
    const auto readFloat = [&](std::string_view key, float& output) {
        const auto iterator = source.find(key);
        if ( iterator == source.end() || !iterator->is_number() ) return false;
        output = iterator->get<float>();
        return true;
    };
    const auto readBool = [&](std::string_view key, bool& output) {
        const auto iterator = source.find(key);
        if ( iterator == source.end() || !iterator->is_boolean() ) return false;
        output = iterator->get<bool>();
        return true;
    };
    const auto readFloatArray = [&](std::string_view    key,
                                    std::vector<float>& output) {
        const auto iterator = source.find(key);
        if ( iterator == source.end() || !iterator->is_array() ) return false;
        output.clear();
        output.reserve(iterator->size());
        for ( const auto& value : *iterator ) {
            if ( !value.is_number() ) return false;
            output.push_back(value.get<float>());
        }
        return true;
    };
    const auto preset = source.find("eqPreset");
    return readFloat("volume", config.volume) &&
           readFloat("playbackSpeed", config.playbackSpeed) &&
           readFloat("playbackPitch", config.playbackPitch) &&
           readBool("muted", config.muted) &&
           readBool("eqEnabled", config.eqEnabled) && preset != source.end() &&
           preset->is_number_integer() &&
           ((config.eqPreset = preset->get<int>()), true) &&
           readFloatArray("eqBandGains", config.eqBandGains) &&
           readFloatArray("eqBandQs", config.eqBandQs);
}

/// @brief 从清单对象读取音频资源。
bool decodeAudioResource(const Json& source, ::MMM::AudioResource& resource)
{
    if ( !source.is_object() || !readString(source, "m_id", resource.m_id) ||
         !readString(source, "m_path", resource.m_path) ||
         resource.m_id.empty() || resource.m_path.empty() ) {
        return false;
    }
    std::string type;
    if ( !readString(source, "m_type", type) ||
         (type != "Main" && type != "Effect") ) {
        return false;
    }
    resource.m_type   = type == "Effect" ? ::MMM::AudioTrackType::Effect
                                         : ::MMM::AudioTrackType::Main;
    const auto config = source.find("m_config");
    return config != source.end() &&
           decodeAudioConfig(*config, resource.m_config);
}

/// @brief 解码并严格校验资源清单。
std::optional<DecodedManifest> decodeManifest(const ByteBuffer& payload)
{
    Json root = Json::from_cbor(payload, true, false);
    if ( root.is_discarded() || !root.is_object() ) return std::nullopt;
    std::uint32_t   version = 0;
    DecodedManifest manifest;
    if ( !readUnsigned(root, "version", version) ||
         version != RESOURCE_MANIFEST_VERSION ||
         !readString(root, "id", manifest.id) || !isSha256(manifest.id) ) {
        return std::nullopt;
    }
    Json canonical = root;
    canonical.erase("id");
    if ( sha256Bytes(Json::to_cbor(canonical)) != manifest.id ) {
        return std::nullopt;
    }

    const auto files = root.find("files");
    const auto audio = root.find("audio_resources");
    const auto remap = root.find("path_remap");
    if ( files == root.end() || !files->is_array() ||
         files->size() > MAX_RESOURCE_FILES || audio == root.end() ||
         !audio->is_array() || remap == root.end() || !remap->is_array() ) {
        return std::nullopt;
    }

    std::uint64_t                   totalBytes = 0;
    std::unordered_set<std::string> cachePaths;
    for ( const auto& item : *files ) {
        ManifestFile file;
        if ( !item.is_object() || !readString(item, "sha256", file.sha256) ||
             !readString(item, "path", file.cachePath) ||
             !readUnsigned(item, "size", file.size) || !isSha256(file.sha256) ||
             file.size > MAX_RESOURCE_FILE_BYTES ) {
            return std::nullopt;
        }
        const std::filesystem::path cachePath =
            Config::utf8ToPath(file.cachePath);
        if ( cachePath.is_absolute() || cachePath.has_root_path() ||
             cachePath.parent_path() != "files" ||
             !file.cachePath.starts_with("files/") ||
             !cachePath.filename().string().starts_with(file.sha256) ) {
            return std::nullopt;
        }
        if ( !cachePaths.insert(file.cachePath).second ) return std::nullopt;
        totalBytes += file.size;
        if ( totalBytes > MAX_RESOURCE_TOTAL_BYTES ) return std::nullopt;
        manifest.files.push_back(std::move(file));
    }
    std::unordered_set<std::string> resourceIds;
    for ( const auto& item : *audio ) {
        ::MMM::AudioResource resource;
        if ( !decodeAudioResource(item, resource) ||
             !resourceIds.insert(resource.m_id).second ||
             !cachePaths.contains(resource.m_path) ) {
            return std::nullopt;
        }
        manifest.audioResources.push_back(std::move(resource));
    }
    for ( const auto& item : *remap ) {
        std::string source;
        std::string target;
        if ( !item.is_object() || !readString(item, "source", source) ||
             !readString(item, "target", target) || source.empty() ||
             target.empty() || !cachePaths.contains(target) ||
             manifest.pathRemap.contains(source) ) {
            return std::nullopt;
        }
        manifest.pathRemap.insert_or_assign(std::move(source),
                                            std::move(target));
    }
    return manifest;
}

/// @brief 判断音频资源是否被当前谱面引用。
bool isReferencedAudioResource(
    const ::MMM::AudioResource&            resource,
    const std::unordered_set<std::string>& references,
    const ::MMM::BaseMapMeta&              metadata)
{
    if ( references.contains(resource.m_id) ||
         references.contains(resource.m_path) ) {
        return true;
    }
    const auto matchesHint = [&](const std::filesystem::path& hint) {
        if ( hint.empty() ) return false;
        const auto hintText = Config::pathToUtf8(hint);
        return hintText == resource.m_id || hintText == resource.m_path ||
               hint.filename() ==
                   Config::utf8ToPath(resource.m_path).filename();
    };
    return matchesHint(metadata.song_file_hint) ||
           matchesHint(metadata.main_audio_path);
}

/// @brief 房主准备任务的纯值快照。
struct HostPreparation {
    std::filesystem::path              projectRoot;
    std::vector<::MMM::AudioResource>  audioResources;
    std::vector<std::filesystem::path> extraPaths;
};

/// @brief 后台线程输入任务。
struct ResourceTask {
    enum class Type {
        PrepareHost,
        Manifest,
        Request,
        Chunk,
    };
    Type             type{ Type::PrepareHost };
    HostPreparation  preparation;
    PeerId           peerId = 0;
    ResourceManifest manifest;
    ResourceRequest  request;
    ResourceChunk    chunk;
};

/// @brief 房主清单文件及其本机来源路径。
struct HostFile {
    ManifestFile          manifest;
    std::filesystem::path sourcePath;
};

/// @brief 判断待清理目录是否为指定缓存根目录下由本模块生成的直接子目录。
bool isOwnedGuestSessionRoot(const std::filesystem::path& cacheRoot,
                             const std::filesystem::path& sessionRoot)
{
    if ( cacheRoot.empty() || sessionRoot.empty() ) return false;
    std::error_code baseError;
    std::error_code sessionError;
    const auto      absoluteBase =
        std::filesystem::absolute(cacheRoot, baseError).lexically_normal();
    const auto absoluteSession =
        std::filesystem::absolute(sessionRoot, sessionError).lexically_normal();
    const auto name = Config::pathToUtf8(absoluteSession.filename());
    return !baseError && !sessionError && !absoluteBase.empty() &&
           absoluteSession.parent_path() == absoluteBase &&
           name.starts_with("session-") && name.size() > 8U;
}

/// @brief 删除一个已经验证归属关系的访客会话目录。
void removeOwnedGuestSessionRoot(const std::filesystem::path& cacheRoot,
                                 const std::filesystem::path& sessionRoot)
{
    if ( !isOwnedGuestSessionRoot(cacheRoot, sessionRoot) ) return;
    std::error_code removeError;
    std::filesystem::remove_all(sessionRoot, removeError);
}

/// @brief 让临时素材目录仅对当前用户开放；不支持的文件系统保持原权限。
void restrictGuestSessionPermissions(const std::filesystem::path& path)
{
    std::error_code permissionError;
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 permissionError);
}

/// @brief 将访客临时项目与其加密缓存、明文素材目录绑定到同一生命周期。
struct GuestResourceBundleOwner {
    /// @brief 访客临时项目值对象。
    ::MMM::Project project;
    /// @brief 用户配置中的协作缓存根目录，仅用于删除边界验证。
    std::filesystem::path cacheRoot;
    /// @brief 本次联机会话独占的待清理目录。
    std::filesystem::path sessionRoot;

    /// @brief 最后一个项目引用释放时自动清理会话目录。
    ~GuestResourceBundleOwner()
    {
        removeOwnedGuestSessionRoot(cacheRoot, sessionRoot);
    }
};
}  // namespace

class CollaborationResourceSync::Impl
{
public:
    Impl() : m_transferSuffix(makeTransferSuffix()) { startWorker(); }

    ~Impl()
    {
        stopWorker();
        clearHostSnapshot();
        {
            std::lock_guard eventLock(m_eventMutex);
            m_events.clear();
        }
        clearGuestSession();
        Detail::clearCollaborationResourceKey(m_hostResourceKey);
        Detail::clearCollaborationResourceKey(m_guestResourceKey);
    }

    /// @brief 重启后台线程并清空全部状态。
    void restart()
    {
        stopWorker();
        clearHostSnapshot();
        {
            std::lock_guard taskLock(m_taskMutex);
            m_tasks.clear();
        }
        {
            std::lock_guard eventLock(m_eventMutex);
            m_events.clear();
        }
        clearGuestSession();
        Detail::clearCollaborationResourceKey(m_hostResourceKey);
        Detail::clearCollaborationResourceKey(m_guestResourceKey);
        m_hostResourceKeyReady  = false;
        m_guestResourceKeyReady = false;
        {
            std::lock_guard progressLock(m_progressMutex);
            m_progress = {};
        }
        m_hostFiles.clear();
        m_hostGeneration = 0;
        m_guestManifest.reset();
        m_guestRoot.clear();
        m_guestMissingFiles.clear();
        m_guestCurrentFile = 0;
        m_guestOffset      = 0;
        m_cacheRoot.clear();
        m_transferSuffix = makeTransferSuffix();
        startWorker();
    }

    /// @brief 排队后台任务。
    void enqueue(ResourceTask task)
    {
        {
            std::lock_guard lock(m_taskMutex);
            m_tasks.push_back(std::move(task));
        }
        m_taskCondition.notify_one();
    }

    /// @brief 非阻塞取出完成事件。
    bool poll(CollaborationResourceSyncEvent& event)
    {
        std::lock_guard lock(m_eventMutex);
        if ( m_events.empty() ) return false;
        event = std::move(m_events.front());
        m_events.pop_front();
        return true;
    }

    /// @brief 获取进度副本。
    CollaborationResourceSyncProgress progress() const
    {
        std::lock_guard lock(m_progressMutex);
        return m_progress;
    }

    /// @brief 设置访客缓存根路径。
    void setCacheRoot(std::filesystem::path cacheRoot)
    {
        m_cacheRoot = std::move(cacheRoot);
        setProgress([](auto& progress) {
            progress       = {};
            progress.phase = CollaborationResourceSyncPhase::WaitingManifest;
        });
    }

    /// @brief 设置房主准备阶段。
    void setPreparing()
    {
        setProgress([](auto& progress) {
            progress       = {};
            progress.phase = CollaborationResourceSyncPhase::Preparing;
        });
    }

private:
    /// @brief 启动后台循环。
    void startWorker()
    {
        auto* appThreadPool = Runtime::AppThreadPool::instance().get();
        if ( !appThreadPool ) {
            fail("runtime_thread_pool_not_initialized");
            return;
        }
        m_stopSource     = std::stop_source{};
        const auto token = m_stopSource.get_token();
        m_workerFuture =
            appThreadPool->enqueue([this, token]() { workerLoop(token); });
    }

    /// @brief 请求后台线程停止。
    void stopWorker()
    {
        if ( !m_workerFuture.valid() ) return;
        m_stopSource.request_stop();
        m_taskCondition.notify_all();
        m_workerFuture.wait();
        m_workerFuture = std::future<void>{};
    }

    /// @brief 后台串行处理文件任务。
    void workerLoop(std::stop_token stopToken)
    {
        while ( !stopToken.stop_requested() ) {
            ResourceTask task;
            {
                std::unique_lock lock(m_taskMutex);
                m_taskCondition.wait(
                    lock, stopToken, [this]() { return !m_tasks.empty(); });
                if ( stopToken.stop_requested() ) return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            switch ( task.type ) {
            case ResourceTask::Type::PrepareHost:
                prepareHost(std::move(task.preparation), stopToken);
                break;
            case ResourceTask::Type::Manifest:
                processManifest(std::move(task.manifest), stopToken);
                break;
            case ResourceTask::Type::Request:
                processRequest(task.peerId, task.request);
                break;
            case ResourceTask::Type::Chunk:
                processChunk(std::move(task.chunk), stopToken);
                break;
            }
        }
    }

    /// @brief 原子更新进度。
    template<typename Callback> void setProgress(Callback&& callback)
    {
        std::lock_guard lock(m_progressMutex);
        callback(m_progress);
    }

    /// @brief 发布完成事件。
    void pushEvent(CollaborationResourceSyncEvent event)
    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(std::move(event));
    }

    /// @brief 将同步器切换为错误状态并发布日志事件。
    void fail(std::string detail)
    {
        setProgress([&](auto& progress) {
            progress.phase  = CollaborationResourceSyncPhase::Error;
            progress.detail = detail;
        });
        CollaborationResourceSyncEvent event;
        event.type   = CollaborationResourceSyncEvent::Type::Error;
        event.detail = std::move(detail);
        pushEvent(std::move(event));
    }

    /// @brief 清理当前同步器独占的房主资源快照目录。
    void clearHostSnapshot()
    {
        if ( m_hostSnapshotRoot.empty() ) return;
        std::error_code error;
        std::filesystem::remove_all(m_hostSnapshotRoot, error);
        m_hostSnapshotRoot.clear();
    }

    /// @brief 清理尚未发布的访客目录；已发布目录由项目共享生命周期接管。
    void clearGuestSession()
    {
        if ( !m_guestBundlePublished ) {
            removeOwnedGuestSessionRoot(m_cacheRoot, m_guestSessionRoot);
        }
        m_guestBundlePublished = false;
        m_guestSessionRoot.clear();
        m_guestEncryptedRoot.clear();
        m_guestRoot.clear();
    }

    /// @brief 准备房主清单并计算全部文件摘要。
    void prepareHost(HostPreparation preparation, std::stop_token stopToken)
    {
        if ( !Detail::generateCollaborationResourceKey(m_hostResourceKey) ) {
            fail("host_resource_key_generation_failed");
            return;
        }
        m_hostResourceKeyReady = true;
        std::vector<HostFile>                        hostFiles;
        std::unordered_map<std::string, std::size_t> sourceIndexes;
        std::unordered_map<std::string, std::string> pathRemap;

        std::error_code temporaryError;
        const auto      temporaryRoot =
            std::filesystem::temp_directory_path(temporaryError);
        if ( temporaryError ) {
            fail("host_resource_snapshot_root_failed");
            return;
        }
        m_hostSnapshotRoot =
            temporaryRoot / "mmm-collaboration-resources" / m_transferSuffix;
        std::filesystem::create_directories(m_hostSnapshotRoot, temporaryError);
        if ( temporaryError ) {
            fail("host_resource_snapshot_create_failed");
            return;
        }
        std::error_code projectRootError;
        const auto canonicalProjectRoot = std::filesystem::weakly_canonical(
            preparation.projectRoot, projectRootError);
        if ( projectRootError || canonicalProjectRoot.empty() ) {
            fail("host_project_root_invalid");
            return;
        }

        const auto addFile = [&](const std::filesystem::path& logicalPath,
                                 std::filesystem::path        sourcePath)
            -> std::optional<std::string> {
            if ( logicalPath.empty() || sourcePath.empty() )
                return std::nullopt;
            if ( sourcePath.is_relative() ) {
                sourcePath = canonicalProjectRoot / sourcePath;
            }
            std::error_code canonicalError;
            sourcePath =
                std::filesystem::weakly_canonical(sourcePath, canonicalError);
            if ( canonicalError ) return std::nullopt;
            const auto relativeSource =
                sourcePath.lexically_relative(canonicalProjectRoot);
            if ( relativeSource.empty() || relativeSource.is_absolute() ||
                 *relativeSource.begin() == ".." ) {
                return std::nullopt;
            }
            std::error_code regularError;
            if ( !std::filesystem::is_regular_file(sourcePath, regularError) ||
                 regularError ) {
                return std::nullopt;
            }
            const std::string sourceKey = Config::pathToUtf8(sourcePath);
            if ( const auto found = sourceIndexes.find(sourceKey);
                 found != sourceIndexes.end() ) {
                const auto& cachePath =
                    hostFiles[found->second].manifest.cachePath;
                pathRemap.insert_or_assign(Config::pathToUtf8(logicalPath),
                                           cachePath);
                return cachePath;
            }

            setProgress([&](auto& progress) {
                progress.currentFile = Config::pathToUtf8(logicalPath);
            });
            const auto snapshotPath =
                m_hostSnapshotRoot /
                (std::to_string(hostFiles.size()) + ".mmrsc");
            const auto digest = Detail::encryptCollaborationResourceFile(
                sourcePath, snapshotPath, m_hostResourceKey, stopToken);
            if ( !digest || digest->size > MAX_RESOURCE_FILE_BYTES ||
                 !isSha256(digest->sha256) ) {
                return std::nullopt;
            }
            const std::string cachePath =
                "files/" + digest->sha256 + safeExtension(sourcePath);
            sourceIndexes.emplace(sourceKey, hostFiles.size());
            hostFiles.push_back(
                { ManifestFile{ digest->sha256, cachePath, digest->size },
                  snapshotPath });
            pathRemap.insert_or_assign(Config::pathToUtf8(logicalPath),
                                       cachePath);
            return cachePath;
        };

        setProgress([&](auto& progress) {
            progress.totalFiles =
                static_cast<std::uint32_t>(preparation.audioResources.size() +
                                           preparation.extraPaths.size());
        });
        for ( auto& resource : preparation.audioResources ) {
            const auto cachePath = addFile(Config::utf8ToPath(resource.m_path),
                                           Config::utf8ToPath(resource.m_path));
            if ( !cachePath ) {
                if ( stopToken.stop_requested() ) return;
                fail("host_resource_missing:" + resource.m_id);
                return;
            }
            resource.m_path = *cachePath;
            setProgress([](auto& progress) { ++progress.completedFiles; });
        }
        for ( const auto& path : preparation.extraPaths ) {
            if ( !addFile(path, path) ) {
                if ( stopToken.stop_requested() ) return;
                fail("host_resource_missing:" + Config::pathToUtf8(path));
                return;
            }
            setProgress([](auto& progress) { ++progress.completedFiles; });
        }
        if ( hostFiles.size() > MAX_RESOURCE_FILES ) {
            fail("host_resource_file_limit");
            return;
        }

        std::uint64_t totalBytes = 0;
        Json          files      = Json::array();
        for ( const auto& file : hostFiles ) {
            totalBytes += file.manifest.size;
            if ( totalBytes > MAX_RESOURCE_TOTAL_BYTES ) {
                fail("host_resource_size_limit");
                return;
            }
            files.push_back(Json{
                { "sha256", file.manifest.sha256 },
                { "path", file.manifest.cachePath },
                { "size", file.manifest.size },
            });
        }
        Json audio = Json::array();
        for ( const auto& resource : preparation.audioResources ) {
            audio.push_back(resource);
        }
        std::vector<std::pair<std::string, std::string>> sortedRemap(
            pathRemap.begin(), pathRemap.end());
        std::ranges::sort(sortedRemap);
        Json remap = Json::array();
        for ( const auto& [source, target] : sortedRemap ) {
            remap.push_back(Json{ { "source", source }, { "target", target } });
        }
        Json manifest{
            { "version", RESOURCE_MANIFEST_VERSION },
            { "files", std::move(files) },
            { "audio_resources", std::move(audio) },
            { "path_remap", std::move(remap) },
        };
        const auto canonical  = Json::to_cbor(manifest);
        const auto manifestId = sha256Bytes(canonical);
        manifest["id"]        = manifestId;
        ByteBuffer payload    = Json::to_cbor(manifest);
        if ( payload.empty() || payload.size() > 1024U * 1024U ) {
            fail("host_resource_manifest_too_large");
            return;
        }

        const std::uint64_t generation = manifestGeneration(manifestId);
        m_hostFiles                    = std::move(hostFiles);
        m_hostGeneration               = generation;
        setProgress([&](auto& progress) {
            progress.phase = CollaborationResourceSyncPhase::Ready;
            progress.totalFiles =
                static_cast<std::uint32_t>(m_hostFiles.size());
            progress.completedFiles = progress.totalFiles;
            progress.totalBytes     = totalBytes;
            progress.currentFile.clear();
            progress.detail = "host_manifest_ready";
        });
        CollaborationResourceSyncEvent event;
        event.type    = CollaborationResourceSyncEvent::Type::ManifestReady;
        event.message = ResourceManifest{ generation, std::move(payload) };
        event.detail  = manifestId;
        pushEvent(std::move(event));
    }

    /// @brief 接收并比对访客缓存。
    void processManifest(ResourceManifest manifestMessage,
                         std::stop_token  stopToken)
    {
        auto decoded = decodeManifest(manifestMessage.payload);
        if ( !decoded ||
             manifestMessage.generation != manifestGeneration(decoded->id) ||
             m_cacheRoot.empty() ) {
            fail("invalid_resource_manifest");
            return;
        }
        if ( !m_guestSessionRoot.empty() ) {
            clearGuestSession();
            Detail::clearCollaborationResourceKey(m_guestResourceKey);
            m_guestResourceKeyReady = false;
            m_transferSuffix        = makeTransferSuffix();
        }
        m_guestManifest    = std::move(decoded.value());
        m_hostGeneration   = manifestMessage.generation;
        m_guestCurrentFile = 0;
        m_guestOffset      = 0;
        if ( !m_guestResourceKeyReady &&
             !Detail::generateCollaborationResourceKey(m_guestResourceKey) ) {
            fail("guest_resource_key_generation_failed");
            return;
        }
        m_guestResourceKeyReady = true;
        if ( m_guestSessionRoot.empty() ) {
            m_guestSessionRoot = m_cacheRoot / ("session-" + m_transferSuffix);
            m_guestEncryptedRoot = m_guestSessionRoot / "encrypted";
            m_guestRoot          = m_guestSessionRoot / "materialized";
        }
        std::error_code createError;
        std::filesystem::create_directories(m_guestEncryptedRoot / "files",
                                            createError);
        if ( !createError ) {
            std::filesystem::create_directories(m_guestRoot / "files",
                                                createError);
        }
        if ( createError ) {
            fail("resource_cache_create_failed");
            return;
        }
        restrictGuestSessionPermissions(m_guestSessionRoot);
        restrictGuestSessionPermissions(m_guestEncryptedRoot);
        restrictGuestSessionPermissions(m_guestRoot);

        setProgress([&](auto& progress) {
            progress       = {};
            progress.phase = CollaborationResourceSyncPhase::ComparingCache;
            progress.totalFiles =
                static_cast<std::uint32_t>(m_guestManifest->files.size());
        });
        std::vector<std::size_t> missing;
        std::uint64_t            missingBytes = 0;
        std::uint32_t            cachedFiles  = 0;
        for ( std::size_t index = 0; index < m_guestManifest->files.size();
              ++index ) {
            const auto& file = m_guestManifest->files[index];
            setProgress(
                [&](auto& progress) { progress.currentFile = file.cachePath; });
            const auto finalPath =
                m_guestRoot / Config::utf8ToPath(file.cachePath);
            std::error_code sizeError;
            const auto size = std::filesystem::file_size(finalPath, sizeError);
            const bool cached = !sizeError && size == file.size &&
                                sha256File(finalPath, stopToken) == file.sha256;
            if ( stopToken.stop_requested() ) return;
            if ( cached ) {
                ++cachedFiles;
                setProgress([](auto& progress) { ++progress.comparedFiles; });
                continue;
            }
            if ( file.size == 0 ) {
                const auto encryptedPath =
                    m_guestEncryptedRoot /
                    Config::utf8ToPath(file.cachePath + ".mmrsc");
                if ( !Detail::initializeCollaborationResourceFile(encryptedPath,
                                                                  0U) ||
                     !Detail::materializeCollaborationResourceFile(
                         encryptedPath, finalPath, m_guestResourceKey, 0U) ||
                     sha256File(finalPath, stopToken) != file.sha256 ) {
                    if ( stopToken.stop_requested() ) return;
                    fail("empty_resource_verify_failed");
                    return;
                }
                ++cachedFiles;
                setProgress([](auto& progress) { ++progress.comparedFiles; });
                continue;
            }
            missing.push_back(index);
            missingBytes += file.size;
            setProgress([](auto& progress) { ++progress.comparedFiles; });
        }
        m_guestMissingFiles = std::move(missing);
        setProgress([&](auto& progress) {
            progress.cachedFiles    = cachedFiles;
            progress.completedFiles = cachedFiles;
            progress.totalBytes     = missingBytes;
        });
        if ( m_guestMissingFiles.empty() ) {
            finishGuestBundle("resource_cache_hit");
            return;
        }
        requestCurrentGuestChunk();
    }

    /// @brief 从房主文件读取并发布一个分块。
    void processRequest(PeerId peerId, const ResourceRequest& request)
    {
        if ( !m_hostResourceKeyReady ||
             request.generation != m_hostGeneration ||
             request.resourceIndex >= m_hostFiles.size() ||
             request.requestedBytes == 0 ||
             request.requestedBytes > RESOURCE_CHUNK_BYTES ) {
            fail("invalid_resource_request");
            return;
        }
        const auto& file = m_hostFiles[request.resourceIndex];
        if ( request.offset >= file.manifest.size ) {
            fail("invalid_resource_offset");
            return;
        }
        const auto remaining   = file.manifest.size - request.offset;
        const auto bytesToRead = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, request.requestedBytes));
        if ( request.offset % RESOURCE_CHUNK_BYTES != 0 ||
             request.requestedBytes != RESOURCE_CHUNK_BYTES ) {
            fail("invalid_resource_request_alignment");
            return;
        }
        auto bytes = Detail::readCollaborationResourceBlock(file.sourcePath,
                                                            m_hostResourceKey,
                                                            file.manifest.size,
                                                            request.offset);
        if ( !bytes || bytes->size() != bytesToRead ) {
            fail("host_resource_decrypt_failed");
            return;
        }
        CollaborationResourceSyncEvent event;
        event.type    = CollaborationResourceSyncEvent::Type::SendChunk;
        event.peerId  = peerId;
        event.message = ResourceChunk{ request.generation,
                                       request.resourceIndex,
                                       request.offset,
                                       std::move(*bytes) };
        pushEvent(std::move(event));
    }

    /// @brief 顺序写入访客分块并在文件边界验证摘要。
    void processChunk(ResourceChunk chunk, std::stop_token stopToken)
    {
        if ( !m_guestManifest ||
             m_guestCurrentFile >= m_guestMissingFiles.size() ) {
            fail("unexpected_resource_chunk");
            return;
        }
        const std::size_t expectedIndex =
            m_guestMissingFiles[m_guestCurrentFile];
        if ( chunk.generation != m_hostGeneration ||
             chunk.resourceIndex != expectedIndex ||
             chunk.offset != m_guestOffset || chunk.payload.empty() ) {
            fail("resource_chunk_sequence_mismatch");
            return;
        }
        const auto& file = m_guestManifest->files[expectedIndex];
        const auto  expectedBytes =
            static_cast<std::size_t>(std::min<std::uint64_t>(
                RESOURCE_CHUNK_BYTES, file.size - m_guestOffset));
        if ( chunk.payload.size() != expectedBytes ||
             m_guestOffset % RESOURCE_CHUNK_BYTES != 0 ) {
            fail("resource_chunk_overflow");
            return;
        }
        const auto finalPath = m_guestRoot / Config::utf8ToPath(file.cachePath);
        const auto encryptedPath =
            m_guestEncryptedRoot /
            Config::utf8ToPath(file.cachePath + ".mmrsc");
        auto partPath = encryptedPath;
        partPath += ".part-" + m_transferSuffix;
        if ( (m_guestOffset == 0 &&
              !Detail::initializeCollaborationResourceFile(partPath,
                                                           file.size)) ||
             !Detail::appendCollaborationResourceBlock(partPath,
                                                       m_guestResourceKey,
                                                       file.size,
                                                       m_guestOffset,
                                                       chunk.payload) ) {
            fail("resource_cache_write_failed");
            return;
        }
        m_guestOffset += chunk.payload.size();
        setProgress([&](auto& progress) {
            progress.phase = CollaborationResourceSyncPhase::Downloading;
            progress.transferredBytes += chunk.payload.size();
            progress.currentFile = file.cachePath;
        });
        if ( m_guestOffset < file.size ) {
            requestCurrentGuestChunk();
            return;
        }

        setProgress([](auto& progress) {
            progress.phase = CollaborationResourceSyncPhase::Verifying;
        });
        if ( !Detail::materializeCollaborationResourceFile(
                 partPath, finalPath, m_guestResourceKey, file.size) ||
             sha256File(finalPath, stopToken) != file.sha256 ) {
            if ( stopToken.stop_requested() ) return;
            std::error_code removeError;
            std::filesystem::remove(partPath, removeError);
            std::filesystem::remove(finalPath, removeError);
            fail("resource_sha256_mismatch");
            return;
        }
        std::error_code removeError;
        std::filesystem::remove(encryptedPath, removeError);
        std::error_code renameError;
        std::filesystem::rename(partPath, encryptedPath, renameError);
        if ( renameError ) {
            std::filesystem::remove(finalPath, removeError);
            fail("resource_cache_commit_failed");
            return;
        }
        ++m_guestCurrentFile;
        m_guestOffset = 0;
        setProgress([](auto& progress) { ++progress.completedFiles; });
        if ( m_guestCurrentFile >= m_guestMissingFiles.size() ) {
            finishGuestBundle("resource_download_complete");
            return;
        }
        requestCurrentGuestChunk();
    }

    /// @brief 请求当前缺失文件的下一个连续分块。
    void requestCurrentGuestChunk()
    {
        const std::size_t index = m_guestMissingFiles[m_guestCurrentFile];
        const auto&       file  = m_guestManifest->files[index];
        setProgress([&](auto& progress) {
            progress.phase       = CollaborationResourceSyncPhase::Downloading;
            progress.currentFile = file.cachePath;
        });
        CollaborationResourceSyncEvent event;
        event.type    = CollaborationResourceSyncEvent::Type::SendRequest;
        event.message = ResourceRequest{
            m_hostGeneration,
            static_cast<std::uint32_t>(index),
            m_guestOffset,
            RESOURCE_CHUNK_BYTES,
        };
        pushEvent(std::move(event));
    }

    /// @brief 发布已经完整校验的访客项目资源。
    void finishGuestBundle(std::string detail)
    {
        auto owner         = std::make_shared<GuestResourceBundleOwner>();
        owner->cacheRoot   = m_cacheRoot;
        owner->sessionRoot = m_guestSessionRoot;
        owner->project.m_projectRoot        = m_guestRoot;
        owner->project.m_audioResources     = m_guestManifest->audioResources;
        owner->project.m_isTemporaryProject = true;
        std::shared_ptr<::MMM::Project> project(owner, &owner->project);
        m_guestBundlePublished = true;
        setProgress([&](auto& progress) {
            progress.phase          = CollaborationResourceSyncPhase::Ready;
            progress.completedFiles = progress.totalFiles;
            progress.currentFile.clear();
            progress.detail = detail;
        });
        CollaborationResourceSyncEvent event;
        event.type = CollaborationResourceSyncEvent::Type::BundleReady;
        event.bundle.project   = std::move(project);
        event.bundle.pathRemap = m_guestManifest->pathRemap;
        event.detail           = std::move(detail);
        pushEvent(std::move(event));
    }

    mutable std::mutex                         m_progressMutex;
    CollaborationResourceSyncProgress          m_progress;
    std::mutex                                 m_taskMutex;
    std::condition_variable_any                m_taskCondition;
    std::deque<ResourceTask>                   m_tasks;
    std::mutex                                 m_eventMutex;
    std::deque<CollaborationResourceSyncEvent> m_events;
    std::future<void>                          m_workerFuture;
    std::stop_source                           m_stopSource;

    std::vector<HostFile> m_hostFiles;
    std::uint64_t         m_hostGeneration = 0;
    /// @brief 房主清单对应的认证加密不可变快照根目录。
    std::filesystem::path m_hostSnapshotRoot;
    /// @brief 房主加密快照的进程内会话密钥。
    Detail::CollaborationResourceKey m_hostResourceKey{};
    /// @brief 房主会话密钥是否已成功生成。
    bool m_hostResourceKeyReady{ false };
    /// @brief 用户配置中的协作缓存根目录。
    std::filesystem::path          m_cacheRoot;
    std::optional<DecodedManifest> m_guestManifest;
    /// @brief 本次访客联机独占、断开后自动删除的目录。
    std::filesystem::path m_guestSessionRoot;
    /// @brief 访客认证加密容器根目录。
    std::filesystem::path m_guestEncryptedRoot;
    /// @brief 仅在会话活跃期间供解码器读取的临时明文素材根目录。
    std::filesystem::path m_guestRoot;
    /// @brief 访客加密缓存的进程内会话密钥。
    Detail::CollaborationResourceKey m_guestResourceKey{};
    /// @brief 访客会话密钥是否已成功生成。
    bool m_guestResourceKeyReady{ false };
    /// @brief 访客项目是否已接管目录清理生命周期。
    bool                     m_guestBundlePublished{ false };
    std::vector<std::size_t> m_guestMissingFiles;
    std::size_t              m_guestCurrentFile = 0;
    std::uint64_t            m_guestOffset      = 0;
    /// @brief 本同步器独占的临时文件后缀，防止同机客户端互相截断。
    std::string m_transferSuffix;
};

CollaborationResourceSync::CollaborationResourceSync()
    : m_impl(std::make_unique<Impl>())
{
}

CollaborationResourceSync::~CollaborationResourceSync() = default;

void CollaborationResourceSync::startHost(const ::MMM::Project& project,
                                          const ::MMM::BeatMap& beatmap)
{
    m_impl->restart();
    m_impl->setPreparing();
    std::unordered_set<std::string> references;
    const auto collectBinding = [&](const ::MMM::Note& note) {
        if ( note.m_sampleBinding &&
             !note.m_sampleBinding->m_audioResourceId.empty() ) {
            references.insert(note.m_sampleBinding->m_audioResourceId);
        }
    };
    for ( const auto& note : beatmap.m_noteData.notes ) collectBinding(note);
    for ( const auto& hold : beatmap.m_noteData.holds ) collectBinding(hold);
    for ( const auto& flick : beatmap.m_noteData.flicks ) collectBinding(flick);
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        collectBinding(polyline);
        for ( const auto& subNote : polyline.m_subNotes ) {
            collectBinding(subNote.get());
        }
    }
    for ( const auto& sample : beatmap.m_audioSamples ) {
        if ( !sample.m_audioResourceId.empty() ) {
            references.insert(sample.m_audioResourceId);
        }
    }

    HostPreparation preparation;
    preparation.projectRoot = project.m_projectRoot;
    for ( const auto& resource : project.m_audioResources ) {
        if ( isReferencedAudioResource(
                 resource, references, beatmap.m_baseMapMetadata) ) {
            preparation.audioResources.push_back(resource);
        }
    }
    const auto appendExtra = [&](const std::filesystem::path& path) {
        if ( !path.empty() ) preparation.extraPaths.push_back(path);
    };
    appendExtra(beatmap.m_baseMapMetadata.main_cover_path);
    appendExtra(beatmap.m_baseMapMetadata.cover_path);
    ResourceTask task;
    task.type        = ResourceTask::Type::PrepareHost;
    task.preparation = std::move(preparation);
    m_impl->enqueue(std::move(task));
}

void CollaborationResourceSync::startGuest(std::filesystem::path cacheRoot)
{
    m_impl->restart();
    m_impl->setCacheRoot(std::move(cacheRoot));
}

void CollaborationResourceSync::reset()
{
    m_impl->restart();
}

void CollaborationResourceSync::receiveManifest(ResourceManifest manifest)
{
    ResourceTask task;
    task.type     = ResourceTask::Type::Manifest;
    task.manifest = std::move(manifest);
    m_impl->enqueue(std::move(task));
}

void CollaborationResourceSync::receiveRequest(PeerId          peerId,
                                               ResourceRequest request)
{
    ResourceTask task;
    task.type    = ResourceTask::Type::Request;
    task.peerId  = peerId;
    task.request = request;
    m_impl->enqueue(std::move(task));
}

void CollaborationResourceSync::receiveChunk(ResourceChunk chunk)
{
    ResourceTask task;
    task.type  = ResourceTask::Type::Chunk;
    task.chunk = std::move(chunk);
    m_impl->enqueue(std::move(task));
}

bool CollaborationResourceSync::pollEvent(CollaborationResourceSyncEvent& event)
{
    return m_impl->poll(event);
}

CollaborationResourceSyncProgress CollaborationResourceSync::progress() const
{
    return m_impl->progress();
}
}  // namespace MMM::Network::Collaboration
