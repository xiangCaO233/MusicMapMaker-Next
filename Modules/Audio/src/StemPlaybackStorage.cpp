#include "audio/StemPlaybackStorage.h"

#include "config/AppPaths.h"
#include "config/StagedFile.h"
#include "config/Utf8Path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::Audio
{
namespace
{
using Json = nlohmann::json;
/// @brief 串行化缓存目录的统计、发布和删除，覆盖同路径源文件的不同版本。
/// @warning 仅后台文件操作使用；不得持有混音锁时等待本锁。
std::mutex cacheMutationMutex;
// 本机目录同时保存可复用 WAV 和轻量调音台设置，两者的提交频率不同。
// current.json 指向最后成功分离的代次，WAV 文件在代次内保持不变。
// 推子修改只写自己的 mix.json，不重写音频，也不替换当前指针。
// 旧代次可能仍被另一个已捕获清单的进程读取，新发布不清理这些文件。

/// @brief 固定路径哈希，保持旧版源目录兼容并生成安全的模型目录名。
std::string cacheKey(std::string_view identity)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for ( const unsigned char byte : identity )
        hash = (hash ^ byte) * 1099511628211ULL;
    return std::to_string(hash);
}

/// @brief 按源文件规范路径查找缓存，清单另行校验具体文件版本。
std::filesystem::path sourceDirectory(const StemSourceIdentity& identity)
{
    return Config::AppPaths::configRootPath() / "stem-playback" /
           cacheKey(identity.path);
}

/// @brief 旧版清单和 WAV 位于源目录；新结果按模型键独立存放。
std::filesystem::path modelDirectory(const std::filesystem::path& source,
                                     std::string_view             modelKey)
{
    if ( modelKey.empty() ) return source;
    return source / "models" / Config::utf8ToPath(std::string(modelKey));
}

/// @brief 读取有限大小的本机 JSON；损坏或缺失时按空文件处理。
/// @param path 缓存内的清单或调音台设置文件。
/// @return 可解析的 JSON 值，或由恢复逻辑识别的无效值。
/// @warning 低频文件操作，只在准备或明确保存路径调用。
Json readMetadata(const std::filesystem::path& path)
{
    std::error_code error;
    // 清单和推子配置都很小，限制意外文件触发的大分配。
    if ( std::filesystem::file_size(path, error) > 128 * 1024 || error )
        return {};
    // 禁用解析异常，损坏的配置交给恢复侧的默认值或回退流程处理。
    std::ifstream stream(path, std::ios::binary);
    return Json::parse(stream, nullptr, false);
}

/// @brief 保存完整 JSON 再原子替换，保留上次成功写入的版本。
/// @param path 目标文件，由调用方提前创建父目录。
/// @param json 要一次性发布的完整文档。
/// @return 输出流关闭及重命名均成功时返回 true。
/// @note 同目录暂存遵循配置层统一的 Windows 和 POSIX 替换语义。
/// @note 保证正常退出和并发读取的完整性，便携存储被拔出时仍可能失败。
bool writeMetadata(const std::filesystem::path& path, const Json& json)
{
    // 旧清单仍可能被其他读取者打开，直接覆写会让读取者看到半份状态。
    // 缓存路径经过 UTF-8 转换；损坏的原曲身份回读时仍会严格失配。
    Config::StagedOutputFile output(path);
    output.stream() << json.dump(2, ' ', false, Json::error_handler_t::replace);
    return output.commit();
}

/// @brief 将源身份编码为清单字段，恢复时按完整值比较。
/// @param identity 当前源路径及文件版本。
/// @return 用于精确比较的文档值，三个字段都必须匹配。
/// @note 目录哈希只用于查找，不能单独决定原曲匹配。
Json identityJson(const StemSourceIdentity& identity)
{
    return { { "source", identity.path },
             { "size", identity.size },
             { "modified", identity.modified } };
}

/// @brief 允许读取生成的单个路径名，拒绝目录穿越及非模型命名字符。
/// @param name 清单代次名或不包含扩展名的 WAV 发布名。
/// @return 可作为单个路径组件时返回 true。
/// @note 程序负责追加扩展名，输入不接收绝对路径和点目录。
bool safeName(const std::string& name)
{
    return !name.empty() && name.size() <= 128 &&
           std::ranges::all_of(name, [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
           });
}

/// @brief 有显式选择时严格使用该模型；旧目录没有选择文件时读取旧清单。
std::optional<std::string> activeModel(const std::filesystem::path& source)
{
    const auto      selection = source / "active-model.json";
    const auto      value     = readMetadata(selection);
    std::error_code error;
    if ( !std::filesystem::exists(selection, error) && !error )
        return std::string{};
    if ( !value.is_string() ) return std::nullopt;
    auto model = value.get<std::string>();
    if ( !model.empty() && !safeName(model) ) return std::nullopt;
    return model;
}

/// @brief 只认设备原有的分轨目录名或旧版清单，排除模型子树和选择文件。
bool isLegacyCacheEntry(const std::filesystem::path& filename)
{
    const auto name = Config::pathToUtf8(filename);
    return name == "current.json" ||
           (name.starts_with("stems") && name.ends_with("-ready") &&
            safeName(name));
}

/// @brief 统计常规文件或目录树中的逻辑大小，不跟随符号链接。
std::uintmax_t cacheBytes(const std::filesystem::path& path,
                          std::error_code&             error)
{
    const auto status = std::filesystem::symlink_status(path, error);
    if ( error ) return 0;
    if ( std::filesystem::is_regular_file(status) )
        return std::filesystem::file_size(path, error);
    if ( !std::filesystem::is_directory(status) ) return 0;
    std::uintmax_t                                      bytes{};
    std::filesystem::recursive_directory_iterator       entry(path, error);
    const std::filesystem::recursive_directory_iterator end;
    for ( ; !error && entry != end; entry.increment(error) ) {
        if ( std::filesystem::is_regular_file(entry->symlink_status(error)) )
            bytes += entry->file_size(error);
    }
    return bytes;
}

/// @brief 将已通过校验的调音台状态写入本机格式。
/// @param mix 推子和启用位保持模型的声部次序。
/// @return 与 WAV 共用轨道顺序的轻量 JSON。
/// @note 用 null 区分未独听和独听编号为零的首轨。
Json settingsJson(const StemPlaybackMix& mix)
{
    return { { "useStems", mix.useStems },
             { "gains", mix.gains },
             { "enabled", mix.enabled },
             { "solo", mix.solo ? Json(*mix.solo) : Json(nullptr) } };
}

/// @brief 恢复同一代次的调音台；配置损坏时保留可用分轨和默认原版模式。
/// @param mix 已建立目录并带有默认推子、启用位和播放模式。
/// @note 设置可独立丢失，校验失败不影响使用已恢复的声部。
void restoreSettings(StemPlaybackMix& mix)
{
    // 即使恢复了 WAV，也只有同轨数的设置才能参与首次播放。
    // JSON 类型不匹配时不做隐式值转换，避免误把字符串解释为启用状态。
    auto value = readMetadata(mix.settingsPath);
    if ( !value.is_object() || !value.contains("useStems") ||
         !value["useStems"].is_boolean() || !value.contains("gains") ||
         !value["gains"].is_array() || !value.contains("enabled") ||
         !value["enabled"].is_array() ||
         value["gains"].size() != mix.gains.size() ||
         value["enabled"].size() != mix.enabled.size() )
        return;
    // 先校验整组数据，再启用分轨，避免半份损坏的推子配置影响播放。
    for ( std::size_t index = 0; index < mix.gains.size(); ++index ) {
        if ( !value["gains"][index].is_number() ||
             !value["enabled"][index].is_boolean() ||
             !std::isfinite(value["gains"][index].get<float>()) )
            return;
    }
    for ( std::size_t index = 0; index < mix.gains.size(); ++index ) {
        mix.gains[index] =
            std::clamp(value["gains"][index].get<float>(), 0.0F, 1.0F);
        mix.enabled[index] = value["enabled"][index].get<bool>();
    }
    if ( value.contains("solo") && value["solo"].is_number_unsigned() ) {
        const auto solo = value["solo"].get<std::uint64_t>();
        if ( solo < mix.gains.size() )
            mix.solo = static_cast<std::size_t>(solo);
    }
    // 模式最后恢复，不能用一条正常的模式字段激活部分无效的推子。
    mix.useStems = value["useStems"].get<bool>();
}
}  // namespace

/// @brief 查询实际原音频的元数据，路径相同但文件更新时旧分轨失效。
/// @param filePath 谱面解析或分离任务传入的 UTF-8 文件路径。
/// @return 路径、大小和修改时间共同组成的本机版本。
/// @note 只接受已存在的规则文件，失效链接不会产生可用的缓存身份。
/// @note 修改时间使用本机文件系统精度，不尝试在机器之间迁移缓存。
std::optional<StemSourceIdentity> readStemSourceIdentity(
    const std::string& filePath)
{
    if ( filePath.empty() ) return std::nullopt;
    std::error_code error;
    const auto      canonical =
        std::filesystem::canonical(Config::utf8ToPath(filePath), error);
    if ( error || !std::filesystem::is_regular_file(canonical, error) || error )
        return std::nullopt;
    const auto size = std::filesystem::file_size(canonical, error);
    if ( error ) return std::nullopt;
    // 大小和修改时间是轻量版本戳，准备路径不额外顺序读取整首歌。
    const auto time = std::filesystem::last_write_time(canonical, error);
    if ( error ) return std::nullopt;
    return StemSourceIdentity{ Config::pathToUtf8(canonical),
                               size,
                               std::to_string(
                                   time.time_since_epoch().count()) };
}

/// @brief 生成与缓存路径格式兼容的模型标识。
std::string makeStemCacheModelKey(std::string_view modelIdentity)
{
    return cacheKey(modelIdentity);
}

/// @brief 在锁内统计各模型的独立目录；旧版数据单独列出。
std::optional<std::vector<StemPlaybackCacheInfo>> readStemPlaybackCache(
    const std::string& filePath)
{
    const auto identity = readStemSourceIdentity(filePath);
    if ( !identity ) return std::nullopt;
    const std::lock_guard              lock(cacheMutationMutex);
    const auto                         source = sourceDirectory(*identity);
    std::vector<StemPlaybackCacheInfo> result;
    std::error_code                    error;
    if ( !std::filesystem::exists(source, error) && !error ) return result;

    // 旧版从未记录模型名，只能如实放到独立行，不能归到当前部署的模型。
    StemPlaybackCacheInfo legacy{ .modelName = "旧版缓存（模型未知）",
                                  .directory = Config::pathToUtf8(source) };
    bool                  hasLegacy = false;
    std::filesystem::directory_iterator       entry(source, error);
    const std::filesystem::directory_iterator end;
    for ( ; !error && entry != end; entry.increment(error) ) {
        if ( !isLegacyCacheEntry(entry->path().filename()) ) continue;
        hasLegacy = true;
        legacy.bytes += cacheBytes(entry->path(), error);
    }
    if ( error ) return std::nullopt;
    if ( hasLegacy ) {
        const auto manifest = readMetadata(source / "current.json");
        legacy.reusable     = manifest.is_object() &&
                              manifest.contains("identity") &&
                              manifest["identity"] == identityJson(*identity);
        result.push_back(std::move(legacy));
    }

    const auto models = source / "models";
    if ( !std::filesystem::exists(models, error) && !error ) return result;
    for ( entry = std::filesystem::directory_iterator(models, error);
          !error && entry != end;
          entry.increment(error) ) {
        auto key = Config::pathToUtf8(entry->path().filename());
        if ( !safeName(key) ||
             !std::filesystem::is_directory(entry->symlink_status(error)) )
            continue;
        const auto manifest    = readMetadata(entry->path() / "current.json");
        const bool hasManifest = manifest.is_object();
        auto       name     = hasManifest && manifest.contains("modelName") &&
                                      manifest["modelName"].is_string()
                                  ? manifest["modelName"].get<std::string>()
                                  : key;
        const bool reusable = hasManifest && manifest.contains("identity") &&
                              manifest["identity"] == identityJson(*identity);
        const auto bytes    = cacheBytes(entry->path(), error);
        result.push_back({ std::move(key),
                           std::move(name),
                           Config::pathToUtf8(entry->path()),
                           bytes,
                           reusable });
    }
    if ( error ) return std::nullopt;
    std::ranges::sort(result, {}, &StemPlaybackCacheInfo::modelName);
    return result;
}

/// @brief 删除单个模型的所有结果，避免影响同源其他模型和选择信息。
std::expected<bool, std::error_code> removeStemPlaybackCache(
    const std::string& filePath, std::string_view modelKey)
{
    using namespace std::filesystem;
    if ( !modelKey.empty() && !safeName(std::string(modelKey)) )
        return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));
    const auto identity = readStemSourceIdentity(filePath);
    if ( !identity )
        return std::unexpected(
            std::make_error_code(std::errc::no_such_file_or_directory));
    const std::lock_guard lock(cacheMutationMutex);
    const auto            source    = sourceDirectory(*identity);
    const bool            wasActive = activeModel(source) == modelKey;
    std::error_code       error;
    if ( !modelKey.empty() ) {
        remove_all(modelDirectory(source, modelKey), error);
    } else if ( exists(source, error) && !error ) {
        // 旧文件仍位于源目录，只删除受旧版格式管理的条目。
        std::vector<path>        legacy;
        directory_iterator       entry(source, error);
        const directory_iterator end;
        for ( ; !error && entry != end; entry.increment(error) ) {
            if ( isLegacyCacheEntry(entry->path().filename()) )
                legacy.push_back(entry->path());
        }
        for ( const auto& path : legacy ) {
            if ( error ) break;
            remove_all(path, error);
        }
    }
    if ( error ) return std::unexpected(error);
    return wasActive;
}

/// @brief 原子提交新代次，先生成音频和设置，再将当前清单指向它们。
/// @param identity 推理开始前捕获的源版本。
/// @param files 与内存分轨同序的 WAV，调用者保活到函数返回。
/// @param mix 该代次首次打开时使用的调音台。
/// @param stop 在复制轨道之间和最终发布前检查的停止令牌。
/// @return 新代次的设置路径。失败返回空，保留上一代的清单。
/// @warning 批量复制只允许在后台文件池执行。
/// @note 原曲只用于标识；应用不在源文件夹创建分轨工程资产。
/// @note 调用方可在保存失败后继续使用已经解码的内存结果。
std::filesystem::path storeStemPlaybackResult(
    const StemSourceIdentity& identity, std::span<const std::string> files,
    const StemPlaybackMix& mix, std::stop_token stop)
{
    // 模型运行期间原曲可能已被替换，存盘前再次核对源版本。
    if ( stop.stop_requested() || files.empty() || !mix.catalog ||
         files.size() != mix.catalog->tracks().size() ||
         readStemSourceIdentity(identity.path) != identity )
        return {};
    // 暂存目录也属于本源缓存，清理不能与复制同一目录并发执行。
    const std::lock_guard lock(cacheMutationMutex);
    if ( !mix.cacheModelKey.empty() && !safeName(mix.cacheModelKey) ) return {};
    const auto      source    = sourceDirectory(identity);
    const auto      directory = modelDirectory(source, mix.cacheModelKey);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if ( error ) return {};
    // 同卷暂存允许一次重命名发布整组文件，避免跨卷逐个替换。
    // 每个任务独占暂存路径，不会解码或删除另一并发作业的中间文件。
    Config::ScopedTemporaryDirectory stage(directory, "stems");
    if ( stage.path().empty() ) return {};
    // 只提升包含完整产物的子目录，外层仍由临时目录对象统一回收。
    const auto stagedFiles = stage.path() / "audio";
    if ( !std::filesystem::create_directory(stagedFiles, error) || error )
        return {};

    // 从完整产物复制文件，模型的临时工作区可以在提交结束后立即回收。
    // 设置与 WAV 放在同一代次，后台推子写入不会重新打开模型结果目录。
    // 名称列表对应已解码目录；清单顺序定义推子及静音字段的索引。
    std::vector<std::string> names;
    for ( const auto& file : files ) {
        if ( stop.stop_requested() ) return {};
        const auto path = Config::utf8ToPath(file);
        auto       name = Config::pathToUtf8(path.stem());
        if ( !safeName(name) ) return {};
        // 不覆盖同名声部，任何重复或磁盘写入失败均使整批暂存无效。
        if ( !std::filesystem::copy_file(
                 path, stagedFiles / (name + ".wav"), error) ||
             error )
            return {};
        names.push_back(std::move(name));
    }
    if ( stop.stop_requested() ||
         !writeMetadata(stagedFiles / "mix.json", settingsJson(mix)) ||
         readStemSourceIdentity(identity.path) != identity )
        return {};

    // 已发布的代次文件保持不可变，切谱或其他进程仍可读取之前捕获的清单。
    const auto generation =
        Config::pathToUtf8(stage.path().filename()) + "-ready";
    const auto published = directory / Config::utf8ToPath(generation);
    std::filesystem::rename(stagedFiles, published, error);
    if ( error ) return {};
    if ( stop.stop_requested() ||
         !writeMetadata(directory / "current.json",
                        { { "version", 1 },
                          { "identity", identityJson(identity) },
                          { "generation", generation },
                          { "tracks", names },
                          { "modelName", mix.cacheModelName } }) ) {
        // 目录仅由当前任务创建，清单未指向它时可以安全回收。
        std::filesystem::remove_all(published, error);
        return {};
    }
    // 模型清单独立提交；如果选择文件写入失败，已完成的模型结果仍可手动恢复。
    if ( !writeMetadata(source / "active-model.json", Json(mix.cacheModelKey)) )
        return {};
    return published / "mix.json";
}

/// @brief 只恢复源版本及完整声部均匹配的结果，失败由谱面保留原版播放。
/// @param filePath 待播放谱面的主轨绝对路径。
/// @param decodePool 独立于调用线程的 PCM 解码池。
/// @return 可发布的完整内存调音台；无有效代次时为空。
/// @note 加载直接读取 WAV，不查找或执行分离模型。
/// @note 只承诺恢复完整分轨。丢失一个文件也不能发布部分混音。
/// @note 无持久化播放状态时返回空，由已有主轨负责继续播放原曲。
std::optional<StemPlaybackMix> loadStoredStemPlayback(
    const std::string& filePath, ice::ThreadPool& decodePool,
    std::optional<std::string_view> modelKey)
{
    const auto identity = readStemSourceIdentity(filePath);
    if ( !identity ) return std::nullopt;
    const auto source = sourceDirectory(*identity);
    const auto model =
        modelKey ? std::optional(std::string(*modelKey)) : activeModel(source);
    if ( !model || (!model->empty() && !safeName(*model)) ) return std::nullopt;
    const auto directory = modelDirectory(source, *model);
    auto       manifest  = readMetadata(directory / "current.json");
    if ( !manifest.is_object() || manifest["version"] != 1 ||
         manifest["identity"] != identityJson(*identity) ||
         !manifest["generation"].is_string() ||
         !manifest["tracks"].is_array() || manifest["tracks"].empty() )
        return std::nullopt;
    const auto& generation =
        manifest["generation"].get_ref<const std::string&>();
    if ( !safeName(generation) || !generation.ends_with("-ready") )
        return std::nullopt;
    const auto published = directory / Config::utf8ToPath(generation);
    std::vector<std::string> files;
    // 不接受在清单外发现的文件，旧代次残留或额外输出不加入调音台。
    // 目录枚举顺序因系统而异；恢复只能按发布清单的顺序读取。
    for ( const auto& name : manifest["tracks"] ) {
        if ( !name.is_string() ||
             !safeName(name.get_ref<const std::string&>()) )
            return std::nullopt;
        files.push_back(Config::pathToUtf8(
            published / (name.get_ref<const std::string&>() + ".wav")));
    }
    // 全部声部解码成功后才发布；流式片段或单轨缺失不能构成完整替代。
    // 新目录的存在只说明文件曾经写出，仍须验证每条轨现在都能解码。
    auto catalog = StemAudition::loadAlignedFiles(files, decodePool);
    if ( !catalog || readStemSourceIdentity(identity->path) != identity )
        return std::nullopt;
    StemPlaybackMix mix{
        .catalog = std::make_shared<const StemAudition>(std::move(*catalog)),
        .gains   = std::vector<float>(files.size(), 1.0F),
        .enabled = std::vector<bool>(files.size(), true),
        .settingsPath   = published / "mix.json",
        .cacheModelKey  = *model,
        .cacheModelName = manifest["modelName"].is_string()
                              ? manifest["modelName"].get<std::string>()
                              : std::string{},
    };
    // 调音台可以单独损坏，默认模式仍能播原曲并让用户手工重设。
    restoreSettings(mix);
    return mix;
}

/// @brief 选定一个已验证代次，配置先落盘，重启时才能直接进入分轨模式。
bool selectStoredStemPlayback(const std::string&     filePath,
                              const StemPlaybackMix& mix)
{
    const auto identity = readStemSourceIdentity(filePath);
    if ( !identity || mix.settingsPath.empty() ||
         (!mix.cacheModelKey.empty() && !safeName(mix.cacheModelKey)) )
        return false;
    const std::lock_guard lock(cacheMutationMutex);
    const auto            source    = sourceDirectory(*identity);
    const auto            directory = modelDirectory(source, mix.cacheModelKey);
    auto                  manifest  = readMetadata(directory / "current.json");
    if ( !manifest.is_object() ||
         manifest["identity"] != identityJson(*identity) ||
         !manifest["generation"].is_string() )
        return false;
    const auto& generation =
        manifest["generation"].get_ref<const std::string&>();
    if ( !safeName(generation) ||
         mix.settingsPath !=
             directory / Config::utf8ToPath(generation) / "mix.json" )
        return false;
    return writeMetadata(mix.settingsPath, settingsJson(mix)) &&
           writeMetadata(source / "active-model.json", Json(mix.cacheModelKey));
}

/// @brief 每代次单独保存推子，旧任务不会覆盖新分离结果的配置。
/// @param mix 控制线程发布的不可变快照，无需读取活动播放状态。
/// @return 配置确实写入磁盘时返回 true。
/// @warning 落盘可阻塞，只由对应源的序列化保存任务调用。
/// @note 重新分离时生成新的目标路径，旧保存任务只能修改旧代次。
bool storeStemPlaybackSettings(const StemPlaybackMix& mix)
{
    const std::lock_guard lock(cacheMutationMutex);
    // 只有成功落盘的结果有设置路径；会话内存结果不创建虚假清单。
    return !mix.settingsPath.empty() &&
           writeMetadata(mix.settingsPath, settingsJson(mix));
}
}  // namespace MMM::Audio
