#include "StemModelCatalog.h"

#ifndef __ANDROID__
#    include "StemSeparationWorker.h"
#    include "config/AppPaths.h"
#    include "config/StagedFile.h"
#    include "config/Utf8Path.h"

#    include <SDL3/SDL_process.h>
#    include <SDL3/SDL_timer.h>
#    include <nlohmann/json.hpp>

#    include <algorithm>
#    include <atomic>
#    include <filesystem>
#    include <fstream>
#    include <iterator>
#    include <memory>
#    include <mutex>
#    include <shared_mutex>
#    include <string_view>
#    include <system_error>
#    include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 串行化绑定配置的短读写，模型递归扫描和卸载不会持有这把锁。
std::mutex bindingsMutex;
/// @brief 成功写完绑定才推进序号，界面读取后用配置锁取得最新路径。
/// @warning 控制器逐帧观察。序号仅表示刷新请求，不直接发布内存对象。
std::atomic<std::uint64_t> bindingsRevision{};
/// @brief 文件线程用共享锁保护推理和部署，卸载须独占。
/// @warning 所有调用均使用尝试锁，界面与后台操作不会等待长时间推理。
std::shared_mutex modelFilesMutex;
/// @brief 新部署的归属凭证，兼容未来更新推理脚本的情形。
constexpr auto MODEL_RECEIPT = ".mmm-stem-model";

/// @brief 用户配置中的模型绑定文件，与外部 MSST 旧设置独立。
/// @return 当前配置根下的专用 JSON 路径。
/// @details 跟随配置根隔离，各桌面配置不会共享模型选择。
std::filesystem::path bindingsPath()
{
    return Config::AppPaths::configRootPath() / "stem-model-bindings.json";
}

/// @brief 验证独立普通文件的内容，避免把链接目标误认成安装凭证。
bool matchesModelFile(const std::filesystem::path& path,
                      std::string_view             expected)
{
    std::error_code error;
    if ( !std::filesystem::is_regular_file(
             std::filesystem::symlink_status(path, error)) ||
         error )
        return false;
    // Windows 部署器可能用文本模式写出 CRLF；比较时按文本流读回。
    const auto size = std::filesystem::file_size(path, error);
    if ( error || size < expected.size() || size > expected.size() * 2 )
        return false;
    std::ifstream stream(path);
    return stream && std::equal(std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>(),
                                expected.begin(),
                                expected.end());
}

/// @brief 只卸载独立目录；现有旧部署可通过安装器生成的原版推理文件识别。
bool isManagedStemModel(const StemCatalogEntry&      entry,
                        const std::filesystem::path& root)
{
    if ( root.filename() != entry.id ) return false;
    return matchesModelFile(root / MODEL_RECEIPT, entry.id) ||
           matchesModelFile(root / "inference.py",
                            std::string_view(STEM_DEPLOY_WORKER));
}

/// @brief 按稳定模型 ID 更新绑定。空路径移除键，其他配置保持原样。
bool writeStemModelBinding(const StemCatalogEntry& entry,
                           const std::string&      directory)
{
    const std::lock_guard lock(bindingsMutex);
    if ( !Config::AppPaths::ensureConfigRootPath() ) return false;
    nlohmann::json data;
    {
        std::ifstream stream(bindingsPath());
        data = nlohmann::json::parse(stream, nullptr, false);
    }
    if ( !data.is_object() ) data = nlohmann::json::object();
    if ( directory.empty() )
        data.erase(entry.id);
    else
        data[entry.id] = directory;
    // 先关闭读取句柄再原子替换配置文件，避免 Windows 共享冲突。
    Config::StagedOutputFile file(bindingsPath());
    file.stream() << data.dump(2);
    if ( !file.commit() ) return false;
    bindingsRevision.fetch_add(1, std::memory_order_relaxed);
    return true;
}
}  // namespace

/// @brief 以稳定模型 ID 解析目录路径，未知键不影响当前版本。
/// @return 目录顺序对应的路径数组，缺失或不合法的项为空。
/// @details 文件只保存用户确认过的位置，加载不读取权重或启动 Python。
/// @warning 首次打开工具调用。不得在模型表渲染时反复读取。
std::array<std::string, STEM_MODEL_CATALOG.size()> readStemModelBindings()
{
    const std::lock_guard                              lock(bindingsMutex);
    std::array<std::string, STEM_MODEL_CATALOG.size()> paths;
    // 配置不包含权重和运行入口，只记录用户主动绑定的本地目录。
    std::ifstream stream(bindingsPath());
    // 旧配置损坏时保留空目录表，仍可继续部署及手工绑定。
    const auto data = nlohmann::json::parse(stream, nullptr, false);
    if ( !data.is_object() ) return paths;
    // 模型 ID 用作配置键。目录调换显示顺序后仍能找到对应位置。
    for ( std::size_t i = 0; i < paths.size(); ++i ) {
        // 以 ID 查找，不以当前列表下标解释旧文件。
        const auto value = data.find(STEM_MODEL_CATALOG[i].id);
        if ( value != data.end() && value->is_string() ) {
            paths[i] = value->get<std::string>();
        }
    }
    return paths;
}

/// @brief 在共享配置中替换一个模型的目录绑定。
/// @param entry 此次实际完成绑定的模型。
/// @param directory 已解析并校验的模型根路径。
/// @return 完整替换配置文件时返回 true。
/// @warning UI 线程低频文件操作，只在用户确认或后台完成时提交。
bool saveStemModelBinding(const StemCatalogEntry& entry,
                          const std::string&      directory)
{
    const auto modelLock = tryLockStemModelFiles();
    return modelLock && writeStemModelBinding(entry, directory);
}

/// @brief 观察是否需要重读磁盘，不通过序号读取任何共享对象。
std::uint64_t stemModelBindingsRevision() noexcept
{
    return bindingsRevision.load(std::memory_order_relaxed);
}

/// @brief 后台推理和部署共用文件使用权，卸载中不开始读取。
std::shared_lock<std::shared_mutex> tryLockStemModelFiles()
{
    return std::shared_lock(modelFilesMutex, std::try_to_lock);
}

/// @brief 统计已绑定模型的目录占用，不访问链接指向的共享运行时。
std::optional<StemModelStorage> inspectStemModelStorage(
    const StemCatalogEntry& entry, const std::string& directory)
{
    const auto lock = tryLockStemModelFiles();
    if ( !lock ) return std::nullopt;
    const auto      path = Config::utf8ToPath(directory);
    std::error_code error;
    if ( !std::filesystem::is_directory(
             std::filesystem::symlink_status(path, error)) ||
         error )
        return std::nullopt;
    StemModelStorage result{ .managed = isManagedStemModel(entry, path) };
    std::filesystem::recursive_directory_iterator       file(path, error);
    const std::filesystem::recursive_directory_iterator end;
    for ( ; !error && file != end; file.increment(error) ) {
        if ( std::filesystem::is_regular_file(file->symlink_status(error)) )
            result.bytes += file->file_size(error);
    }
    if ( error ) return std::nullopt;
    return result;
}

/// @brief 重新校验绑定，按所确认的方式卸载或解除关联。
std::expected<void, std::string> removeStemModelBinding(
    const StemCatalogEntry& entry, const std::string& directory,
    bool removeFiles)
{
    const std::unique_lock lock(modelFilesMutex, std::try_to_lock);
    if ( !lock ) return std::unexpected("有模型正在分离或部署，请稍后重试。");
    const auto position =
        std::ranges::find_if(STEM_MODEL_CATALOG, [&](const auto& item) {
            return std::string_view(item.id) == entry.id;
        });
    // 对照磁盘，陈旧的控制器不能卸载用户刚在其他窗口绑定的目录。
    if ( directory.empty() || position == STEM_MODEL_CATALOG.end() ||
         readStemModelBindings()[position - STEM_MODEL_CATALOG.begin()] !=
             directory )
        return std::unexpected("模型绑定已变更，请重新打开模型目录。");
    if ( removeFiles ) {
        std::error_code error;
        const auto      path = Config::utf8ToPath(directory);
        const auto      root = std::filesystem::canonical(path, error);
        if ( error || root != path || !isManagedStemModel(entry, root) )
            return std::unexpected("无法确认这是 MMM 部署的模型目录。");

        // 只清理安装器拥有的条目，目录内额外放入的用户文件不应被带走。
        // 权重和环境先删，失败时保留归属信息，允许排除权限问题后重试。
        for ( const auto* name : { "env",
                                   "weights",
                                   "model.json",
                                   "inference.py",
                                   MODEL_RECEIPT } ) {
            std::filesystem::remove_all(root / name, error);
            if ( error )
                return std::unexpected(
                    "无法完整删除模型文件。请检查目录权限后重试。");
        }
        if ( std::filesystem::is_empty(root, error) && !error )
            std::filesystem::remove(root, error);
    }
    if ( !writeStemModelBinding(entry, {}) )
        return std::unexpected("无法更新模型绑定。请检查配置目录权限。");
    return {};
}

/// @brief 验证被选择的目录来自正确模型。
/// @param entry 用户在模型目录中选择的条目。
/// @param directory 文件选择器返回的 UTF-8 路径。
/// @return 与当前模型对应的冻结推理入口及声部清单，或可读错误。
/// @details 只检查文件级完整性，权重加载和 Python 包导入由分离进程负责。
/// @details 既可选模型根目录，也可选择其中的 env 目录。
std::expected<BoundStemModel, std::string> bindStemModel(
    const StemCatalogEntry& entry, const std::string& directory)
{
    std::error_code error;
    auto            root =
        std::filesystem::canonical(Config::utf8ToPath(directory), error);
    // canonical 在目录不存在或链接失效时报告原因，选择错误不会覆盖原绑定。
    if ( error ) return std::unexpected("模型目录不可用。");
    // 从环境路径回到模型根目录，以统一找到同一份清单和权重。
    if ( root.filename() == "env" ) root = root.parent_path();
#    ifdef _WIN32
    // Windows venv 入口位于 Scripts，直接交给 SDL 处理原生命令行。
    const auto python = root / "env" / "Scripts" / "python.exe";
#    else
    // POSIX venv 的 python 可以是指向基础解释器的符号链接。
    const auto python = root / "env" / "bin" / "python";
#    endif
    // 虚拟环境的目录结构是安装器与绑定器的共同约定。
    // 用户直接选择 env 子目录时也遵从相同的检查。
    // 入口固定在所选目录内，避免沿用上次不相关模型的全局路径。
    const auto backend = root / "inference.py";
    // 文件级检测支持 venv 的正常符号链接。无文件时不会启动解释器。
    if ( !std::filesystem::is_regular_file(python, error) ||
         !std::filesystem::is_regular_file(backend, error) ) {
        return std::unexpected("目录缺少独立环境或推理入口。");
    }
    // 兼容最初的 HTDemucs 本地测试包与通过相同结构整理好的现有环境。
    auto manifest = root / "model.json";
    if ( !std::filesystem::is_regular_file(manifest, error) &&
         std::string_view(entry.id) == "htdemucs" ) {
        manifest = root / "HTDemucs.json";
    }
    // 复用既有模型包解析器。手工绑定与部署使用相同的权重校验前置条件。
    auto model = readStemSeparationModel(Config::pathToUtf8(manifest));
    // 模型解析错误沿用原有中文提示，告知作者缺失的是配置还是权重。
    if ( !model ) return std::unexpected(model.error());
    // 冻结后的计划保证 tracks 为对象且包含各声部的模型标识。
    const auto plan = nlohmann::json::parse(model->plan, nullptr, false);
    // 逐条核对模型身份。不同条目的绑定不会错误共用另一套环境。
    for ( const auto& route : plan["tracks"] ) {
        if ( route["model"] != entry.id ) {
            return std::unexpected("此目录的模型与所选条目不匹配。");
        }
    }
    // 展示名与目录行保持一致，不依赖用户部署时的父目录名称。
    // 后端计划按 ID 路由，替换展示名不会改变实际推理模型。
    model->name = entry.name;
    // 绑定结果带完整独立环境，后续选轨不会读取其他模型的运行时偏好。
    return BoundStemModel{
        std::move(*model),
        { Config::pathToUtf8(python), Config::pathToUtf8(backend), true },
        Config::pathToUtf8(root),
    };
}

/// @brief 尝试受支持的本机 Python，以后台进程创建目录并安装隔离依赖。
/// @param entry 当前部署条目，ID 同时用作固定子目录名。
/// @param parent 作者选择的已存在父目录。
/// @param stop 后台部署的协作取消信号。
/// @return 已可绑定的模型目录，或缺环境、下载及保存失败的信息。
/// @warning 后台文件任务。运行 Python、安装依赖和下载权重均可阻塞。
std::expected<BoundStemModel, std::string> deployStemModel(
    const StemCatalogEntry& entry, const std::string& parent,
    std::stop_token stop)
{
    const auto modelLock = tryLockStemModelFiles();
    if ( !modelLock ) return std::unexpected("正在卸载模型，请稍后部署。");
    std::error_code error;
    const auto      selected =
        std::filesystem::canonical(Config::utf8ToPath(parent), error);
    // 父目录由文件选择器给出，拒绝已经失效的挂载路径。
    if ( error ) return std::unexpected("部署目录不可用。");
    // 每个模型有固定子目录，已有文件必须通过绑定流程使用。
    const auto destination = selected / entry.id;
    // 非空目标可能是自己维护的部署，避免覆盖。Python 也会排他创建以防竞态。
    if ( std::filesystem::exists(destination, error) || error ) {
        return std::unexpected("目标模型目录已存在，请选择绑定已有环境。");
    }
    // 部署程序和取消文件独占工作目录，可在任务返回时统一清理。
    const auto temp = std::filesystem::temp_directory_path(error);
    if ( error ) return std::unexpected("无法获取部署暂存目录。");
    Config::ScopedTemporaryDirectory scratch(temp, "mmm-stem-setup");
    if ( scratch.path().empty() )
        return std::unexpected("无法创建部署暂存目录。");
    // 写出内嵌入口，Python 原生 venv 能在固化后复制运行脚本。
    const auto script = scratch.path() / "deploy.py";
    {
        std::ofstream file(script);
        file << STEM_DEPLOY_WORKER;
        file.close();
        // 脚本关闭成功后才启动解释器，避免子进程读取到不完整源码。
        if ( !file ) return std::unexpected("无法写入部署程序。");
    }

    // 复用分离工作进程中的子进程取消与回收逻辑。
    {
        std::ofstream helper(scratch.path() / "separate_stems.py");
        helper << STEM_SEPARATION_WORKER;
        helper.close();
        if ( !helper ) return std::unexpected("无法写入部署依赖。");
    }
    const auto cancel    = scratch.path() / "cancel";
    const auto cancelArg = Config::pathToUtf8(cancel);
    // 绝对文件名支持空格与 Unicode；SDL 将 argv 转成目标平台进程参数。
    const auto scriptArg = Config::pathToUtf8(script);
    // 只传目标子目录。脚本无需知道用户配置目录或当前工程。
    const auto targetArg = Config::pathToUtf8(destination);
    // PATH 由用户环境提供。仅缺失或版本不支持时尝试下一个解释器。
#    ifdef _WIN32
    constexpr std::array candidates{ "python.exe", "python3.exe" };
#    else
    constexpr std::array candidates{
        "python3.12", "python3.11", "python3", "python"
    };
#    endif
    // 每次尝试都是直接执行，不经 shell 重解释用户路径或参数。
    for ( const auto* python : candidates ) {
        // 任务可能仍在文件池排队，取消后不得再启动下载。
        if ( stop.stop_requested() ) return std::unexpected("已取消部署。");
        // 模型 ID 也是部署脚本的白名单键，父目录只作为路径值传递。
        const char* args[]{ python,   scriptArg.c_str(), "deploy",
                            entry.id, targetArg.c_str(), cancelArg.c_str(),
                            nullptr };
        std::unique_ptr<SDL_Process, decltype(&SDL_DestroyProcess)> process(
            SDL_CreateProcess(args, false), SDL_DestroyProcess);
        // SDL 继承输出流，以便从桌面程序的启动日志追查安装器错误。
        // PATH 中缺少候选程序时可以安全尝试下一项，因为此时没有写入。
        if ( !process ) continue;
        int code = 0;
        // 子进程持有磁盘目录的清理责任，不能在请求取消后立即删除暂存文件。
        // UI 只发停止信号。控制文件由后台创建，避免渲染线程磁盘写入。
        bool signalled = false;
        while ( !SDL_WaitProcess(process.get(), false, &code) ) {
            if ( stop.stop_requested() && !signalled ) {
                // 让 Python 先回收 pip/下载任务，再清除未完成的模型目录。
                std::ofstream marker(cancel);
                marker.close();
                signalled = static_cast<bool>(marker);
            }
            // 文件工作线程观察退出，不占用每帧或音频调度线程。
            SDL_Delay(100);
        }
        if ( stop.stop_requested() ) return std::unexpected("已取消部署。");
        // 42 是版本探针不兼容的专用退出码，此时还没有创建环境。
        if ( code == 42 ) continue;
        // 真正安装或下载失败时停止，不用别的解释器覆盖失败现场。
        if ( code != 0 ) {
            return std::unexpected(
                "部署失败。请检查网络、磁盘空间及 Python venv 支持。");
        }
        // 后端成功后仍走绑定校验。半成品与错误型号都不能发布到界面。
        auto bound = bindStemModel(entry, targetArg);
        if ( bound ) {
            // 归属凭证让未来更换安装器版本后仍能识别本次部署。
            Config::StagedOutputFile receipt(destination / MODEL_RECEIPT);
            receipt.stream() << entry.id;
            static_cast<void>(receipt.commit());
        }
        return bound;
    }
    // 所有候选都不存在或版本不兼容。给出可执行的安装信息，
    // 与网络及磁盘错误区分开。
    return std::unexpected(
        "请安装 Python 3.11 或 3.12（含 venv），并加入 PATH。");
}
}  // namespace MMM::UI
#endif
