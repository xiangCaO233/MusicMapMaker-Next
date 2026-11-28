#include "StemSeparationJob.h"

#ifndef __ANDROID__
#    include "StemSeparationWorker.h"
#    include "audio/StemPlaybackStorage.h"
#    include "config/AppPaths.h"
#    include "config/StagedFile.h"
#    include "config/Utf8Path.h"

#    include <SDL3/SDL_error.h>
#    include <SDL3/SDL_process.h>
#    include <SDL3/SDL_timer.h>
#    include <nlohmann/json.hpp>

#    include <filesystem>
#    include <fstream>
#    include <memory>
#    include <string_view>
#    include <system_error>
#    include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 检查与分离进程一致的输出名，禁止目录分隔和空输出。
/// @param name 包内模型标识、后端输出名或发布声部 ID。
/// @return 限长的 ASCII 标识才返回 true。
/// @details Python 与 C++ 用同一约束，保证声部在两边可无歧义定位。
bool validStemName(std::string_view name)
{
    if ( name.empty() || name.size() > 48 || name[0] < 'a' || name[0] > 'z' ) {
        return false;
    }
    // 产物名称独立于模型权重文件名，以稳定的 ASCII 标识转交给试听。
    for ( char ch : name ) {
        if ( (ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '_' ) {
            return false;
        }
    }
    return true;
}
}  // namespace

/// @brief 从配置目录恢复用户选定的解释器、后端与设备。
/// @details 配置缺失、旧格式或字段类型错误时保留可用默认值。
/// @return 解释器、后端地址及设备偏好组成的快照。
/// @warning 首次打开工具时读取一次，普通渲染帧不得访问该文件。
StemSeparationEnvironment loadStemSeparationEnvironment()
{
    StemSeparationEnvironment environment;
    // 只从本机配置读取可执行路径，外部模型包仅提供模型和权重。
    std::ifstream stream(Config::AppPaths::configRootPath() /
                         "stem-separation.json");
    auto          value = nlohmann::json::parse(stream, nullptr, false);
    // 启动或旧版配置迁移期间文件可能尚未建立，UI 仍可打开并导入已有声部。
    if ( !value.is_object() ) return environment;
    // 解释器路径不接受空值，否则选择器取消会清掉可用的默认环境。
    if ( value.contains("python") && value["python"].is_string() &&
         !value["python"].get_ref<const std::string&>().empty() ) {
        environment.python = value["python"].get<std::string>();
    }
    // 后端允许为空，空值对应等待用户选择实际的推理入口。
    if ( value.contains("backend") && value["backend"].is_string() ) {
        environment.backend = value["backend"].get<std::string>();
    }
    // 不把数字或字符串设备标识隐式解释为布尔值。
    if ( value.contains("cpu") && value["cpu"].is_boolean() ) {
        environment.cpu = value["cpu"].get<bool>();
    }
    return environment;
}

/// @brief 使用项目暂存文件发布完整配置，避免关闭中途留下半份 JSON。
/// @param environment 已由本机用户确认的运行入口及偏好。
/// @return 提交到最终配置文件成功时返回 true。
/// @warning 仅在有实际交互修改时调用，会创建文件并原子重命名。
bool saveStemSeparationEnvironment(const StemSeparationEnvironment& environment)
{
    // 首次使用时也支持尚未存在的用户配置目录。
    if ( !Config::AppPaths::ensureConfigRootPath() ) return false;
    Config::StagedOutputFile file(Config::AppPaths::configRootPath() /
                                  "stem-separation.json");
    // 简单字符串可包含空格或非 ASCII 路径，由 JSON 序列化统一处理。
    file.stream() << nlohmann::json{
        { "python", environment.python }, { "backend", environment.backend },
        { "cpu", environment.cpu },
    }.dump(2);
    // 即使保存失败，旧配置文件仍可用于下次启动。
    return file.commit();
}

/// @brief 读取版本化模型包并冻结配置，避免选择后重新解析外部路径。
/// @param path 用户在文件选择器中选定的 UTF-8 本地路径。
/// @return 可供 GUI 预览与作业共用的文件快照，或可展示的错误原因。
/// @details 模型包只提供模型及权重参数，外部 Python 路径由本机设置提供。
/// 所有声部在投递之前逐项校验，部分可用的结果不会混入列表。
/// @warning 低频文件读取；要求包较小，绝不读取权重内容或启动模型。
std::expected<StemSeparationModel, std::string> readStemSeparationModel(
    const std::string& path)
{
    std::error_code error;
    const auto      configPath =
        std::filesystem::absolute(Config::utf8ToPath(path), error);
    // 限制只读描述文件的大小，防止误选推理权重时阻塞解析大文件。
    if ( error || std::filesystem::file_size(configPath, error) > 65536 ||
         error ) {
        return std::unexpected("无法读取模型包，或文件超过 64 KiB。");
    }
    // 文本流只借用描述文件。模型启动时使用独立快照，原文件可从目录移除。
    std::ifstream stream(configPath);
    // 模型包来自外部文件，语法错误作为普通校验结果返回。
    auto plan = nlohmann::ordered_json::parse(stream, nullptr, false);
    if ( !plan.is_object() || !plan.contains("version") ||
         plan["version"] != 1 || !plan.contains("tracks") ||
         !plan["tracks"].is_object() || plan["tracks"].empty() ) {
        return std::unexpected("模型包须声明 version 1 和非空 tracks。");
    }

    // 保持有序 JSON 中的通道顺序，结果文件读取时直接复用。
    StemSeparationModel model;
    model.name = Config::pathToUtf8(configPath.stem());
    // 每个 route 都能指向独立模型。重复引用相同权重的去重由工作进程完成。
    for ( auto& [name, route] : plan["tracks"].items() ) {
        // 与工作进程共用版本和字段。先校验信息，再展示可用声部。
        if ( !validStemName(name) || !route.is_object() ||
             !route.contains("model") || !route["model"].is_string() ||
             !validStemName(route["model"].get_ref<const std::string&>()) ||
             !route.contains("target") || !route["target"].is_string() ||
             !validStemName(route["target"].get_ref<const std::string&>()) ) {
            return std::unexpected("模型包含无效声部或模型标识。");
        }
        // 只接受用于推理的两类文件，描述文件不声明 Python 或后端命令。
        for ( const auto* field : { "config", "weights" } ) {
            if ( !route.contains(field) || !route[field].is_string() ) {
                return std::unexpected("声部缺少模型配置或权重。");
            }
            // 原路径相对模型包解析。工作进程收到绝对路径，可在独立目录运行。
            const auto local =
                configPath.parent_path() /
                Config::utf8ToPath(route[field].get_ref<const std::string&>());
            // 不接受不存在的路径和目录。完整权重加载由推理后端执行。
            if ( !std::filesystem::is_regular_file(local, error) || error ) {
                return std::unexpected("找不到模型配置或权重。");
            }
            // 保留绝对路径供 JSON 跨进程传输，不将路径解释为命令或工具参数。
            route[field] = Config::pathToUtf8(local);
        }
        model.stems.push_back(name);
    }
    // 保留冻结快照，避免点击分离时重新读取发生变化的描述文件。
    model.plan = plan.dump();
    return model;
}

/// @brief 生成当前作业的参数，等待推理结束再打开完整声部。
/// @param model 已从描述文件解析出的模型配置与全部目标声部。
/// @param source 同一个作业内各权重共用的原曲文件路径。
/// @param interpreter 含有推理依赖的 Python 解释器。
/// @param backend 用户本机安装的 MSST 入口。
/// @param cpu 允许在无 GPU 的环境强制使用 CPU。
/// @param decodePool 与本文件任务分离的解码池，避免后台线程等待自身。
/// @param stop 由界面提交取消；后台将它转发为当前作业私有标记。
/// @param useStems 任务开始时的播放模式，用于首次写入新分离结果。
/// @return 完全驻留且源帧对齐的试听目录，或错误原因。
/// @warning 仅后台文件池可调用。可能等待模型推理、转换及音频解码。
std::expected<Audio::StemPlaybackMix, std::string> runStemSeparation(
    const StemSeparationModel& model, const std::string& source,
    const std::string& interpreter, const std::string& backend, bool cpu,
    ice::ThreadPool& decodePool, std::stop_token stop, bool useStems)
{
    if ( stop.stop_requested() ) return std::unexpected("已取消分离。");
    const auto identity = Audio::readStemSourceIdentity(source);
    if ( !identity ) return std::unexpected("无法读取原音频文件。");
    std::error_code error;
    // 临时区独立于工程，退出时清理也不会访问原曲所在目录。
    const auto temp = std::filesystem::temp_directory_path(error);
    if ( error ) return std::unexpected("无法获取分离临时目录。");
    // 路径由项目的独占工作目录 helper 创建，并在解码后统一回收。
    Config::ScopedTemporaryDirectory work(temp, "mmm-stems");
    if ( work.path().empty() ) {
        return std::unexpected("无法创建分离临时目录。");
    }
    const auto& path = work.path();
    // 计划通过普通 UTF-8 文件传递，量大时也不受系统命令行长度限制。
    const auto planPath = path / "model.json";
    {
        std::ofstream stream(planPath);
        stream << model.plan;
        // 进程看到的计划必须已关闭，尤其不能让 Windows 句柄影响共享读取。
        stream.close();
        if ( !stream ) return std::unexpected("无法写入分离任务配置。");
    }

    const auto plan = Config::pathToUtf8(planPath);
    // 输出目录留给工作进程创建。成功发布前该路径不会暴露有效声部。
    const auto output    = path / "output";
    const auto outputArg = Config::pathToUtf8(output);
    // 每个任务只观察自己的取消文件，并与模型产物一起清理。
    const auto cancelPath = path / "cancel";
    const auto cancelArg  = Config::pathToUtf8(cancelPath);
    // Python 入口随二进制嵌入。通过 argv 交接空格、Unicode 与文件路径。
    // SDL 负责桌面平台进程启动，模型包无法注入命令行选项。
    // Python -c 只执行内置脚本；参数数组以空指针结束。
    // 可选 CPU 标志位于尾部，关闭时该位置即参数数组终止符。
    const char* args[]{ interpreter.c_str(),
                        "-c",
                        STEM_SEPARATION_WORKER,
                        source.c_str(),
                        plan.c_str(),
                        outputArg.c_str(),
                        "--backend",
                        backend.c_str(),
                        "--convert-input",
                        "--cancel-file",
                        cancelArg.c_str(),
                        cpu ? "--cpu" : nullptr,
                        nullptr };
    // 设备回调与 UI 都不持有进程对象。它的生命周期限定在这份文件任务中。
    std::unique_ptr<SDL_Process, decltype(&SDL_DestroyProcess)> process(
        SDL_CreateProcess(args, false), SDL_DestroyProcess);
    // 创建失败没有可消费的结果，不进入后续解码。
    if ( !process ) {
        return std::unexpected("无法启动模型解释器：" +
                               std::string(SDL_GetError()));
    }
    // 后台文件任务可阻塞；UI 通过 future 就绪状态消费结果。
    // 这里仅在后台线程等待进程句柄。音频队列继续回调，旧组可继续试听。
    // 进程退出后才交回临时目录所有权，避免外部写入与清理相互竞争。
    int result = 0;
    // 从等待线程写标记，UI 线程只发布 stop_token，控制动作不做磁盘 I/O。
    bool cancellationSent = false;
    while ( !SDL_WaitProcess(process.get(), false, &result) ) {
        if ( stop.stop_requested() && !cancellationSent ) {
            // 让 Python 回收它启动的模型进程，再等待退出。直接杀 Python
            // 会绕过工作目录的析构，并可能留下仍在推理的子进程。
            std::ofstream cancelled(cancelPath);
            cancelled.close();
            // 如果写入失败，不把未送达的通知当成后端已经开始退出。
            cancellationSent = static_cast<bool>(cancelled);
        }
        // 只在文件工作线程观察外部进程，UI 渲染和播放不等待该周期。
        SDL_Delay(100);
    }
    if ( stop.stop_requested() ) return std::unexpected("已取消分离。");
    // 取消请求在退出状态解读前优先处理，避免向作者报告推理故障。
    if ( result != 0 ) {
        return std::unexpected(
            "分离进程失败。请检查所选 Python、MSST 和 FFmpeg。");
    }
    // 只读声明的产物，不扫描后端目录或猜测额外可用轨道。
    std::vector<std::string> tracks;
    // 只准备所请求的声部数，避免把后端生成的其他音频占进播放缓存。
    tracks.reserve(model.stems.size());
    // 各声部共用已校验的源零点，按预览顺序建立试听目录。
    for ( const auto& stem : model.stems ) {
        tracks.push_back(Config::pathToUtf8(output / (stem + ".wav")));
    }
    // PCM 完全驻留后才收回临时磁盘，播放和区间取样不会访问临时文件。
    // loadAlignedFiles 完成全量解码，允许随后释放任务工作区。
    // 单轨解码失败时整组仍不发布，旧试听节点由 UI 保留。
    auto audition = Audio::StemAudition::loadAlignedFiles(tracks, decodePool);
    if ( !audition ) return std::unexpected("无法解码分离产物。");
    if ( stop.stop_requested() ) return std::unexpected("已取消分离。");
    if ( Audio::readStemSourceIdentity(source) != identity )
        return std::unexpected("分离期间原音频已变更。");

    // 保存解码通过的完整一代。提交失败仍可使用内存结果，但不会替换旧清单。
    Audio::StemPlaybackMix mix{
        .catalog =
            std::make_shared<const Audio::StemAudition>(std::move(*audition)),
        .gains    = std::vector<float>(tracks.size(), 1.0F),
        .enabled  = std::vector<bool>(tracks.size(), true),
        .useStems = useStems,
    };
    mix.settingsPath =
        Audio::storeStemPlaybackResult(*identity, tracks, mix, stop);
    if ( mix.settingsPath.empty() && stop.stop_requested() )
        return std::unexpected("已取消分离。");
    return mix;
}
}  // namespace MMM::UI
#endif
