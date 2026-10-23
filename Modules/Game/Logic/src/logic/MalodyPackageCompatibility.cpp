#include "logic/MalodyPackageCompatibility.h"

#include "audio/AudioOriginAlignmentService.h"
#include "config/Utf8Path.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>

namespace
{

/// @brief 判断 MC 节点是否为 Malody 自动音频对象。
/// @param node 待检查的 note 数组节点。
/// @return 字符串 SOUND 或数值类型 1 时返回 true。
/// @note 类型字段缺失或具有其他 JSON 类型时返回 false，不尝试字符串转换。
bool isMalodyAutomaticAudioNode(const nlohmann::json& node)
{
    // 只检查标准自动采样类型，不把带 sound 字段的任意玩家音符当成主音轨。
    if ( !node.is_object() ) return false;
    const auto typeIt = node.find("type");
    if ( typeIt == node.end() ) return false;
    if ( typeIt->is_string() ) {
        // 字符串按协议原样匹配，不接受大小写变体或数字字符串。
        return typeIt->get_ref<const std::string&>() == "SOUND";
    }
    if ( typeIt->is_number_integer() ) {
        return typeIt->get<std::int64_t>() == 1;
    }
    if ( typeIt->is_number_unsigned() ) {
        return typeIt->get<std::uint64_t>() == 1;
    }
    return false;
}

}  // namespace

namespace MMM::Logic
{

/// @brief 写出 MC 首拍修正与对应主音频，原始资源只读。
/// @param beatMap 导出来源模型。
/// @param outputPath MC 目标路径。
/// @param mainAudioSource 已解析的主音轨路径，为空时使用谱面歌曲提示。
/// @param deferAudioEncoding 包事务已承担音频处理时不在单谱面层重复编码。
/// @param errorMessage 具体失败原因。
/// @param exportedMainAudio 接收最终引用路径，供倍速导出等入口反馈实际产物。
/// @return MC 和所有本次生成资源均成功时为真。
/// @details 分两阶段提交：先构造完整文档并完成独立音频编码，再打开 MC。
/// 构造失败不会截断旧 MC；编码失败只回收本次创建的资源，绝不覆盖输入音频。
/// @note mainAudioSource 由应用入口解析项目稳定 ID，模型层只处理协议资源引用。
/// @note deferAudioEncoding 仅供包事务使用；包级预检已确认对应资源同步编码。
/// @note 音乐位移来自文件内部静音，不能再次移动 Main SOUND 的触发位置。
/// @warning 用户导出路径：访问文件并执行离线编码，禁止从播放循环调用。
bool writeMalodyExportWithAudio(const BeatMap&               beatMap,
                                const std::filesystem::path& outputPath,
                                const std::filesystem::path& mainAudioSource,
                                bool                         deferAudioEncoding,
                                std::string&                 errorMessage,
                                std::filesystem::path*       exportedMainAudio)
{
    errorMessage.clear();
    // 从同一份模型构造文档与前导计划，避免两次独立计算选用不同参考 BPM。
    // 曲线采样和格式能力检查都由模型层完成，音频层不重新解释红绿线规则。
    nlohmann::json document;
    double         leadInMilliseconds = 0;
    if ( !beatMap.buildMalodyExportDocument(document, leadInMilliseconds) ) {
        // 失败可能来自模式或物件能力，不创建空文档来掩盖转换错误。
        errorMessage = "MC 首拍或物件无法导出";
        return false;
    }
    std::filesystem::path generatedAudio;
    // 空路径表示尚未创建资源，失败清理据此区分本次文件和既有用户文件。
    const auto        songIt = document["meta"]["song"].find("file");
    const std::string songFile =
        songIt != document["meta"]["song"].end() && songIt->is_string()
            ? songIt->get<std::string>()
            : std::string{};
    // 纯谱面没有歌曲文件时只需平移物件。存在文件提示却没有对应 Main 时，
    // 需要补头的导出必须明确失败，不能生成看似成功但音乐错位的产物。
    if ( !deferAudioEncoding && leadInMilliseconds > 0 && !songFile.empty() ) {
        // 只有确实腾出一拍的情况需要解码和编码，普通新增零点不改音乐文件。
        // 只对明确的主音轨补静音；普通效果音的触发点已随谱面一起平移。
        // 多次触发的同一主资源不能共用一份补头音频，必须在写出前明确拒绝。
        nlohmann::json* pairedMain = nullptr;
        // 完整引用优先，避免不同目录的同名效果音也被算作 Main。
        // 文件名回退仅兼容没有精确引用的旧谱面，不能扩大已确认资源的身份。
        const bool hasExactMainReference = std::any_of(
            document["note"].begin(),
            document["note"].end(),
            [&](const auto& node) {
                return isMalodyAutomaticAudioNode(node) &&
                       node.value("sound", std::string{}) == songFile;
            });
        // 身份优先级先独立确定，不能因 note[] 的时间排序不同而改变 Main 选择。
        // 精确命中后仍检查真正 Main 是否重复，不能用优先级掩盖多次触发歧义。
        // 节点指针只在当前数组内借用，之后不扩容 note[]，保证重写资源名安全。
        // 字符串 SOUND 与数值类型 1 共用同一识别器，兼容 Key 和 Slide。
        for ( auto& node : document["note"] ) {
            if ( !isMalodyAutomaticAudioNode(node) || !node.contains("sound") ||
                 !node["sound"].is_string() )
                continue;
            const auto& sound = node["sound"].get_ref<const std::string&>();
            if ( sound != songFile &&
                 (hasExactMainReference ||
                  Config::utf8ToPath(sound).filename() !=
                      Config::utf8ToPath(songFile).filename()) )
                continue;
            if ( pairedMain ) {
                errorMessage = "MC 前导补偿需要唯一的主音轨自动采样";
                return false;
            }
            pairedMain = &node;
        }
        if ( !pairedMain ) {
            // 歌曲提示不具备播放调度权限，不能在这里凭空新增一个 SOUND。
            // 缺少自动采样需要由调用方修正资源关联，而不是隐式改变谱面。
            errorMessage = "MC 前导补偿没有找到可同步的主音轨自动采样";
            return false;
        }
        auto inputPath = mainAudioSource;
        if ( inputPath.empty() ) {
            // 独立文件按来源谱面解析相对路径，不能改用新导出目录猜测源音频。
            inputPath = Config::utf8ToPath(songFile);
            // 绝对歌曲路径直接使用，相对路径才以来源谱面文件目录为根。
            if ( inputPath.is_relative() )
                inputPath = beatMap.m_baseMapMetadata.map_path.parent_path() /
                            inputPath;
        }
        std::error_code error;
        // 使用无异常文件系统接口；资源缺失在创建任何编码文件之前返回错误。
        // 输入来源必须是原谱面目录或项目解析结果，不能相对输出目录猜测。
        if ( !std::filesystem::is_regular_file(inputPath, error) || error ) {
            errorMessage = "MC 前导补偿无法读取主音轨源文件";
            return false;
        }
        // 新资源使用源格式重编码，以免改变扩展名与实际容器的对应关系。
        // 使用未占用文件名，既不覆盖原音频，也不覆盖用户已有的导出资源。
        const auto stem = outputPath.stem();
        for ( unsigned index = 0; index < 10000; ++index ) {
            // 有界候选搜索避免异常目录状态导致无限循环；每个候选都检查占用。
            // 扩展名沿用源文件，使编码器选择的格式与最终 MC 引用一致。
            auto name = stem;
            name += "_lead-in";
            if ( index ) name += "_" + std::to_string(index);
            name += inputPath.extension();
            auto       candidate = outputPath.parent_path() / name;
            const bool exists    = std::filesystem::exists(candidate, error);
            if ( error ) break;
            // 存在但属于其他导出的文件不能复用，其静音长度未必相同。
            if ( !exists ) {
                generatedAudio = std::move(candidate);
                break;
            }
        }
        if ( generatedAudio.empty() ) {
            // 候选已耗尽或目录访问失败时尚未生成文件，直接报告失败。
            errorMessage = "MC 前导补偿无法创建独立音频副本";
            return false;
        }
        const auto encoded = Audio::AudioOriginAlignmentService::alignToOrigin({
            .inputPath         = inputPath,
            .outputPath        = generatedAudio,
            .phaseMilliseconds = -leadInMilliseconds,
        });
        // 离线对齐服务用负相位增加静音，内部同时负责解码、长度和编码失败处理。
        // 不通过修改文件名、容器时长或 MC offset 冒充真正的音频后移。
        if ( !encoded.success ) {
            // 编码失败只清理本次新文件，不能留下可被误引用的半成品。
            std::filesystem::remove(generatedAudio, error);
            errorMessage = encoded.errorMessage;
            return false;
        }
        const auto filename = Config::pathToUtf8(generatedAudio.filename());
        // 元数据和自动采样一起改引用，避免编辑器提示与游戏实际播放资源分离。
        document["meta"]["song"]["file"] = filename;
        (*pairedMain)["sound"]           = filename;
        // 主音轨保留原触发锚点和独立 offset，新增静音承担音乐内容的后移。
        // 此处只改输出文档，源 BeatMap 和项目资源清单始终只读。
    }
    std::ofstream stream(outputPath);
    // 统一在音频成功之后写 MC；关闭流时也检查错误，覆盖延迟写盘失败。
    // 失败结果不能保留只写了一半却引用新音频的“成功”状态。
    if ( stream ) stream << document.dump(4);
    const bool written = stream.good();
    stream.close();
    if ( !written || stream.fail() ) {
        // MC 失败时回收本次生成的音频，不让失败导出积累孤立资源。
        std::error_code error;
        if ( !generatedAudio.empty() )
            std::filesystem::remove(generatedAudio, error);
        errorMessage = "MC 文档写出失败";
        return false;
    }
    if ( exportedMainAudio ) {
        // 只在全部写入完成后交付结果路径，失败时调用方原来的路径保持不变。
        // 结果跟随最终文档，不能向界面报告已被前导音频替代的旧产物。
        *exportedMainAudio =
            outputPath.parent_path() /
            Config::utf8ToPath(
                document["meta"]["song"]["file"].get<std::string>());
    }
    return true;
}

/// @brief 删除 Malody 自动主音轨对象中的 vol 字段。
/// @param document 待修改的 MC JSON 根对象。
/// @param mainAudioReferences 解析为项目 Main 音轨的资源引用集合。
/// @return 实际删除的 vol 字段数量。
/// @note 只处理 type 为 SOUND 或数值 1 的自动采样，不修改玩家物件绑定音效。
/// @details 本函数修改导出 JSON，不改变项目中的主音轨音量设置。
/// @note 原本没有 vol 的节点保持不变，重复调用不会继续产生改动。
/// @warning 打包兼容处理，遍历导出 note 数组，不用于会话逐帧更新。
std::size_t stripMalodyMainAudioVolumeFields(
    nlohmann::json&                        document,
    const std::unordered_set<std::string>& mainAudioReferences)
{
    if ( !document.is_object() || mainAudioReferences.empty() ) return 0;
    // 缺少可识别的目标引用时保持原文档，不猜测哪条 SOUND 是主音轨。
    const auto noteIt = document.find("note");
    if ( noteIt == document.end() || !noteIt->is_array() ) return 0;
    // 畸形或缺失 note 节点不自动修补，兼容处理不承担重建谱面的职责。

    std::size_t removedCount = 0;
    for ( auto& node : *noteIt ) {
        if ( !isMalodyAutomaticAudioNode(node) ) continue;
        const auto soundIt = node.find("sound");
        // 资源归属由调用方解析出的引用集合决定，不按路径后缀或轨道位置判断。
        if ( soundIt == node.end() || !soundIt->is_string() ) continue;
        if ( !mainAudioReferences.contains(
                 soundIt->get_ref<const std::string&>()) ) {
            continue;
        }
        // 仅移除 vol，保留时间、资源引用与所有其他扩展字段。
        removedCount += node.erase("vol");
        // erase 返回实际移除数量，原本没有 vol 的目标不计作改动。
    }
    // 返回字段数而非 SOUND 数，调用方可据此判断是否需要报告兼容调整。
    return removedCount;
}

}  // namespace MMM::Logic
