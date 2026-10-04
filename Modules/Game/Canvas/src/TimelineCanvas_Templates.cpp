#include "canvas/TimelineCanvas.h"

#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/timing/TimingTemplate.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <mutex>
#include <nfd.h>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#ifdef _WIN32
#    include <windows.h>
#endif

namespace MMM::Canvas
{
namespace
{
/// @brief 模板库是个人配置，随 MMM_CONFIG_ROOT 隔离第二客户端。
/// @note 仅在用户打开或保存窗口时查询路径，不放入 UI 常态热路径。
// 模板库跟随用户配置根，而不是跟随当前谱面目录。
// 保存一个组不会向来源音频、谱面或项目包追加文件。
// 独立配置目录的协作客户端因此拥有独立的个人库。
// 路径只在低频操作获取，AppPaths 可能创建配置目录。
// 禁止把这个查询移进 render 的每帧入口。
// 模板库文件名不包含用户名称，改名不会丢失已存模板。
std::filesystem::path templateLibraryPath()
{
    return Config::AppPaths::configRootPath() / "timing_templates.json";
}

/// @brief 载入经过领域校验的个人模板，损坏库保留在原路径供修复。
/// @details 大小和数量均有上限，解析禁用异常；不执行外部脚本。
// state 是当前编辑窗口拥有的值状态，不引用外部可变配置。
// 读取只发生在打开工具时，不对文件建立逐帧轮询。
// 新配置允许空库，损坏既有库则禁止保存覆盖。
// 文件大小先检查，避免将超大 JSON 整体解析进内存。
// 结构完整后才逐个读取领域模板，失败不保留半个库。
// 打开工具失败仍可编辑工作副本，原文件保留供修复。
// 库与谱面不同，没有可执行 Lua 或表达式外部资源权限。
// 模板公式的读取使用受限数学解析器，不执行任意脚本。
// 成功读取后才恢复写入标志，不能把错误文字当作状态机。
// 关闭并重新打开可重新读取用户修复后的文件。
void loadTemplateLibrary(TimingTemplateEditorState& state)
{
    state.m_library.clear();
    state.m_storageMessage.clear();
    const auto      path = templateLibraryPath();
    std::error_code error;
    // 新配置目录没有模板库是正常情况，不用错误提示打断创建流程。
    if ( !std::filesystem::exists(path, error) && !error ) return;
    // 在读取任何既有内容前关闭写入能力。
    // 文件存在但无法访问时不能误认为它是空库。
    // 状态标志与错误说明分离，说明文字改动不影响保护逻辑。
    state.m_libraryWritable = false;
    const auto size         = std::filesystem::file_size(path, error);
    if ( error || size > 16 * 1024 * 1024 ) {
        state.m_storageMessage = "模板库无法读取或超过 16 MiB；原文件已保留。";
        return;
    }
    std::ifstream input(path, std::ios::binary);
    if ( !input ) {
        state.m_storageMessage = "无法打开时间点模板库。";
        return;
    }
    // 第三个参数关闭解析异常，损坏配置通过 discarded 表达。
    // 顶层版本是库版本，每个子模板还有独立定义版本。
    // 数组上限控制同时显示和保存的模板数量。
    // 不接受顶层裸数组，避免旧配置被误解释成当前模式。
    const auto document = nlohmann::json::parse(input, nullptr, false);
    if ( document.is_discarded() || !document.is_object() ||
         !document.contains("version") || document["version"] != 1 ||
         !document.contains("templates") || !document["templates"].is_array() ||
         document["templates"].size() > 256 ) {
        state.m_storageMessage = "模板库格式损坏；原文件已保留。";
        return;
    }
    // 整库载入失败时不展示半个库，避免用户保存时覆盖没读出的条目。
    // 候选库与窗口当前库分开，只有全部通过才交换。
    // 已成功读取的前几项不会因后项损坏而被用户保存成残缺库。
    // 库中条目是值对象，离开来源项目后仍可重复放置。
    // 领域读取器负责基准、偏移、参数和插值合法性。
    std::vector<TimingTemplate> candidates;
    for ( const auto& item : document["templates"] ) {
        auto parsed = readTimingTemplate(item);
        if ( !parsed ) {
            state.m_storageMessage = "模板库载入失败：" + parsed.error();
            return;
        }
        candidates.push_back(std::move(*parsed));
    }
    state.m_library         = std::move(candidates);
    state.m_libraryWritable = true;
}

/// @brief 先完整写临时文件，再原子替换个人模板库。
/// @return 错误时返回原因，调用方保持原内存库及磁盘文件。
/// @note 文件操作只由显式保存或删除触发，不跟随数值输入逐次写入。
// library 是准备写出的完整候选库，不直接改动窗口当前库。
// 序列化前有模板数量上限，序列化后再检查真实字节数。
// 临时文件与目标在同一目录，避免跨盘移动退化为复制删除。
// 写入和关闭必须全部成功，才能替换旧文件路径。
// 替换失败返回错误，调用方不交换候选库。
// 保存操作不创建谱面历史，也不发布协作写入命令。
// Windows 使用原生 Unicode 路径，支持中文配置目录。
// POSIX 使用同目录 rename，无需先删除原文件。
// 不承诺掉电持久性，只有完成写入与路径替换的成功状态。
// 文件 IO 仅在明确保存或删除时执行，输入框编辑不触发保存。
std::string saveTemplateLibrary(const std::vector<TimingTemplate>& library)
{
    if ( library.size() > 256 ) return "个人模板库最多保存 256 个模板。";
    // 使用稳定版本包裹库，字段由领域模型集中定义。
    // 不将正在显示的目标时间戳写入模板偏移。
    // 运行时拍轴缓存不会进入 JSON，换谱面后必须重新绑定。
    // 文本长度检查的是最终编码结果，包含元数据和公式。
    nlohmann::json document = { { "version", 1 }, { "templates", library } };
    const auto     text =
        document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    if ( text.size() > 16 * 1024 * 1024 ) return "模板库超过 16 MiB。";
    // 路径由配置服务提供，不从模板名称拼接用户输入路径。
    // 模板名称因此不能改变保存目的地或写入项目资源。
    // 文件访问范围固定在个人配置库中。
    const auto path = templateLibraryPath();
    // 临时后缀固定在同一用户根内，不碰谱面资源目录。
    // 显式关闭流后才能重命名，Windows 不允许移动仍被占用的输出。
    // 失败留下的临时文件不作为下次载入来源。
    auto temporary = path;
    temporary += ".tmp";
    {
        // 流关闭前检查写入结果，半写入不能替代上一份可用模板库。
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if ( !output ) return "无法写入模板库临时文件。";
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        if ( !output ) return "模板库写入失败，原文件已保留。";
    }
#ifdef _WIN32
    // Windows rename 不替换既有文件，显式使用 Unicode 原生替换入口。
    // 不先删除旧库，因此提交失败仍保留原文件。
    if ( !MoveFileExW(
             temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) )
        return "模板库替换失败，原文件已保留。错误码：" +
               std::to_string(GetLastError());
#else
    // 临时文件和目标在同一目录，POSIX rename 一次替换既有路径。
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if ( error ) return "模板库替换失败，原文件已保留：" + error.message();
#endif
    return {};
}

/// @brief 模板换名或加载时一次性同步名称缓冲区。
// state 的固定缓冲区容量与领域模型名称上限一致。
// 名称只在加载或新建时复制，普通帧不重置正在编辑的文本。
// 复制实际字节数后显式终止，避免上个较长名称残留。
// 保存时使用工作副本名称，不重新从组合框显示字符串猜测。
// 模板名称是个人库键，同名保存会更新既有定义。
// 原始库条目在成功保存前保持不变。
void syncTemplateName(TimingTemplateEditorState& state)
{
    const auto count =
        std::min(state.m_draft.m_name.size(), state.m_name.size() - 1);
    std::memcpy(state.m_name.data(), state.m_draft.m_name.data(), count);
    state.m_name[count] = '\0';
}

/// @brief 将会话组件复制为领域模型，秒转毫秒只发生在这个边界。
// component 是锁内借用，函数返回完全独立的 Timing 副本。
// 领域模型毫秒与会话组件秒不能混用。
// 只在此边界做单位转换，后面的模板函数统一使用 Timing。
// 效果参数是绝对值，不转为相对当前速度的倍率。
// 元数据保留来源属性，放置后外部格式仍能正常往返。
// 插值定义保留只读函数对象，不复制每个输出采样事件。
// 函数没有实体编号输出，模板不能绑定已删除的源对象。
// 非 BPM 的附带 bpm 字段不作为拍轴红线。
Timing copyTemplateTiming(const Logic::TimelineComponent& component)
{
    Timing timing;
    timing.m_timestamp             = component.m_timestamp * 1000.0;
    timing.m_timingEffect          = component.m_effect;
    timing.m_timingEffectParameter = component.m_value;
    timing.m_bpm =
        component.m_effect == TimingEffect::BPM ? component.m_value : 0;
    timing.m_metadata      = component.m_metadata;
    timing.m_interpolation = component.m_interpolation;
    return timing;
}

/// @brief 输入变化时重建预览，并提示目标已有同类型段落的冲突。
/// @note 只扫描打开或刷新时的值副本，提交时逻辑再次验证实际注册表。
// state.m_context 是打开或刷新时捕获的值快照。
// 预览不持有会话锁，不等待逻辑更新一整轮。
// 放置算法只在 dirty 输入事件后调用，不逐帧反解所有点。
// 领域错误时清空旧预览，避免把上次有效落点显示成新输入结果。
// 目标占用只用于本地提示，协作更新可能在窗口打开后改变它。
// 实际放置在逻辑线程重新检查当前注册表。
// 预览保存实际时间和段落，表格可以直接显示最终秒数。
// 源选区与当前目标重叠时仍允许保存定义。
// 范围冲突阻挡放置，不阻挡用户把原组选区存入个人库。
// 普通同刻点允许并存，沿用现有时间线创建语义。
void refreshTemplatePreview(TimingTemplateEditorState& state)
{
    // 先消费脏标记，失败后不在下一帧重复执行相同昂贵换算。
    // 用户修改任意相关字段或显式刷新才再次尝试。
    // 错误文字与文件读写提示独立保存。
    // 新的无效输入不会沿用旧有效 preview。
    state.m_dirty = false;
    state.m_preview.clear();
    auto placed = placeTimingTemplate(state.m_draft,
                                      state.m_anchorSeconds,
                                      state.m_context,
                                      state.m_fallbackBpm);
    if ( !placed ) {
        state.m_error = placed.error();
        return;
    }
    state.m_error.clear();
    for ( const auto& item : *placed ) {
        const double start = item.m_timestamp / 1000.0;
        const double end =
            start +
            (item.m_interpolation ? item.m_interpolation->m_duration : 0);
        for ( const auto& other : state.m_context ) {
            if ( item.m_timingEffect != other.m_timingEffect ) continue;
            const double otherStart = other.m_timestamp / 1000.0;
            const double otherEnd =
                otherStart +
                (other.m_interpolation ? other.m_interpolation->m_duration : 0);
            // 普通点可以位于段落边界；内部点和两段的正长度交集才构成冲突。
            if ( (item.m_interpolation && other.m_interpolation &&
                  std::min(end, otherEnd) >
                      std::max(start, otherStart) + 1e-9) ||
                 (item.m_interpolation && otherStart > start + 1e-9 &&
                  otherStart < end - 1e-9) ||
                 (other.m_interpolation && start > otherStart + 1e-9 &&
                  start < otherEnd - 1e-9) ) {
                state.m_error =
                    "目标范围已有同类型插值段落或时间点，请调整基准时间。";
                break;
            }
        }
    }
    // 缓存保留全部有效落点，存在占用冲突时也方便用户逐行检查。
    // 错误标志禁用实际放置，保存定义仍可继续。
    // 下一次输入变更会整体替换这个缓存，不积累旧预览行。
    // 常态帧读取缓存只做格式化输出，没有拍轴重建。
    state.m_preview = std::move(*placed);
}

/// @brief 返回只读查看页，取消的草稿和导入候选不进入个人库。
/// @note 保存成功也走同一入口，因此展示内容始终来自已提交的库。
/// @param state 同时拥有已保存个人库与当前操作页的工作副本。
/// @details 取消创建、取消放置及保存成功都经过此处统一复位。
/// 个人库不是草稿来源的可变别名，重新赋值才恢复已保存定义。
/// 若刚删除最后一个模板，索引变为 -1，查看页展示空库引导。
/// 有效的原选择优先保留；索引失效才回到第一项。
/// 导入候选在这里销毁，不会被下次进入添加页自动提交。
/// 放置预览同时丢弃，不能在查看页展示上一目标的绝对时间。
/// 状态提示由调用方保留或替换，成功信息不会被统一清空。
/// 不重新读文件，因此取消操作不受文件选择器或磁盘耗时影响。
void showTemplateLibrary(TimingTemplateEditorState& state)
{
    // 编辑副本与库分离，取消时重新复制选中条目恢复一致显示。
    if ( state.m_libraryIndex < 0 ||
         static_cast<std::size_t>(state.m_libraryIndex) >=
             state.m_library.size() )
        state.m_libraryIndex = state.m_library.empty() ? -1 : 0;
    // 这个赋值只发生于显式导航，逐帧查看不进行模板所有权复制。
    // 选中索引属于个人库，导入列表的索引不能在此混用。
    state.m_draft = state.m_libraryIndex >= 0
                        ? state.m_library[state.m_libraryIndex]
                        : TimingTemplate{};
    syncTemplateName(state);
    state.m_phase = TimingTemplateEditorPhase::View;
    state.m_preview.clear();
    state.m_imports.clear();
    state.m_importSource.clear();
    state.m_confirmDelete   = false;
    state.m_replaceExisting = false;
    state.m_error.clear();
    state.m_dirty = false;
}

/// @brief 显式确认后把全部候选提交到个人库，失败保留原库和当前页面。
/// @warning 只由保存或确认添加触发，允许一次文件写入；不在常态帧执行。
/// @param state 写入保护、个人库和操作提示的所有者。
/// @param candidates 已编辑或已导入的候选；允许借用 state 的导入列表。
/// @param allowReplace 是否已明确允许替换同名个人库条目。
/// @return 全部候选校验和持久化成功后返回 true，并进入查看页。
/// @details 合并先在本地副本进行，不能让失败导入改变当前库。
/// 来源文件中的重复名称由领域读取器拒绝，目标同名由本步骤处理。
/// 写入保护既限制新建，也限制替换，避免损坏旧库被新内容覆盖。
/// 保存后的选中项使用合并结果的索引，不能沿用文件内序号。
/// 在清空导入候选后不再读取 candidates，允许输入与状态内部容器别名。
/// 文件替换是提交边界：其前后不会发布任何谱面写入命令。
/// 编辑成功只保存个人预设，不自动放置到来源谱面。
bool commitTemplateCandidates(TimingTemplateEditorState&         state,
                              const std::vector<TimingTemplate>& candidates,
                              bool                               allowReplace)
{
    // 修复文件后需要重新打开工具取得完整库，不能用空候选绕过保护。
    // 错误留在当前操作页，用户可以返回查看而不会丢失既有文件。
    if ( !state.m_libraryWritable ) {
        state.m_storageMessage = "原模板库载入失败，请修复文件后重新打开。";
        return false;
    }
    // 在候选副本完成全部校验，不能先写入有效前项再报告后项失败。
    auto library  = state.m_library;
    int  selected = -1;
    for ( const auto& candidate : candidates ) {
        // UI 可编辑参数，候选即使来自曾经合法的文件也要重新验证。
        // 校验不检查目标占用：这是个人库操作，并非谱面放置。
        auto valid = readTimingTemplate(nlohmann::json(candidate));
        if ( !valid ) {
            state.m_storageMessage = valid.error();
            return false;
        }
        auto found = std::find_if(
            library.begin(), library.end(), [&](const auto& saved) {
                return saved.m_name == candidate.m_name;
            });
        if ( found != library.end() && !allowReplace ) {
            state.m_storageMessage = "已存在同名模板：" + candidate.m_name +
                                     "。请改名或明确允许替换。";
            return false;
        }
        // end 对应即将追加的位置；前项追加后再查找不会复用失效迭代器。
        // 第一项决定保存后的展示选择，不强迫用户从库首重新查找。
        const int index = static_cast<int>(found - library.begin());
        if ( selected < 0 ) selected = index;
        if ( found == library.end() )
            library.push_back(candidate);
        else
            *found = candidate;
    }
    // 写入成功后才切换到查看页；磁盘失败不丢弃可重试的草稿。
    const auto error = saveTemplateLibrary(library);
    if ( !error.empty() ) {
        state.m_storageMessage = error;
        return false;
    }
    // 只有持久化成功才发布新的内存库，失败仍可在当前页重试。
    // 导入与草稿使用值语义，回到查看不会继续引用来源候选。
    state.m_library      = std::move(library);
    state.m_libraryIndex = selected;
    showTemplateLibrary(state);
    state.m_storageMessage = "模板已保存到个人库。";
    return true;
}

/// @brief 有界读取模板文件，只建立添加页候选，尚不写入个人库。
/// @warning 用户选择文件后的低频路径；读取最多 16 MiB，解析关闭异常。
/// @param state 添加页拥有的待确认候选和错误提示。
/// @param path 文件选择器返回的平台原生路径，UTF-8 转换已完成。
/// @details 文件内容只用于生成值定义，不保存脚本或实体引用。
/// 大小检查后再分配缓冲区，读取途中增长的内容不会继续扩容。
/// 文件被截断或读取失败时拒绝内容，不对短数据进行猜测恢复。
/// 单模板和整库通过同一领域入口，字段与版本校验保持一致。
/// 候选替换是内存操作，必须等待添加页确认才能写入个人库。
/// 损坏文件不会破坏此前有效候选或磁盘模板库。
void loadTemplateImport(TimingTemplateEditorState&   state,
                        const std::filesystem::path& path)
{
    std::error_code error;
    const auto      size = std::filesystem::file_size(path, error);
    if ( error || size == 0 || size > 16 * 1024 * 1024 ) {
        state.m_storageMessage = "模板文件无法读取、为空或超过 16 MiB。";
        return;
    }
    // 固定缓冲上限，文件在读取期间增长也不能突破检查后的预算。
    std::ifstream input(path, std::ios::binary);
    std::string   content(static_cast<std::size_t>(size), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if ( !input || input.gcount() != static_cast<std::streamsize>(size) ) {
        state.m_storageMessage = "模板文件读取失败，个人库未改变。";
        return;
    }
    // discarded、未知版本、超预算和非法子项统一返回 expected 错误。
    // 没有有效候选时不会创建空模板或自动改为其他数据格式。
    auto parsed = readTimingTemplateBundle(
        nlohmann::json::parse(content, nullptr, false));
    if ( !parsed ) {
        state.m_storageMessage = parsed.error();
        return;
    }
    // 完整校验成功才替换候选，不能展示一份部分有效的文件。
    // 每次成功读取都重新建立审阅索引，前一文件索引不能映射到新文件。
    // 同名替换选项同时复位，授权不能从上一份文件继承。
    // 确认前展示来源路径，重新选择失败仍明确保留上一文件的有效候选。
    state.m_importSource    = Config::pathToUtf8(path);
    state.m_imports         = std::move(*parsed);
    state.m_importIndex     = 0;
    state.m_draft           = state.m_imports.front();
    state.m_replaceExisting = false;
    state.m_storageMessage.clear();
}

/// @brief 添加页请求文件选择器，遵循用户配置的原生或内置样式。
/// @warning 原生选择器仅在明确点击时阻塞 UI；日常绘制不调用该入口。
/// @param state 只用于接收用户确认选择后的候选或错误。
/// @details 原生窗口同步返回路径，内置窗口跨帧保留子模态。
/// 两种选择器均经过 loadTemplateImport，不复制格式验证逻辑。
/// 文件扩展名过滤只便于选择，不构成对内容合法性的信任。
/// 取消是正常控制流，既不清空个人库，也不生成失败日志。
/// NFD 输出指针在本次操作结束前释放，不跨 UI 帧持有。
/// 内置对话框固定键与父工具稳定 ID 一起维持弹层生命周期。
/// 本入口不修改最近目录配置，取消选择不会产生隐式配置写入。
void openTemplateImportPicker(TimingTemplateEditorState& state)
{
    const auto& settings  = Config::AppConfig::instance().getEditorSettings();
    const auto  directory = settings.lastFilePickerPath.empty()
                                ? std::string(".")
                                : settings.lastFilePickerPath;
    if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t*            path     = nullptr;
        const nfdu8filteritem_t filter[] = { { "Timing templates", "json" } };
        const auto              result =
            UI::NativeFileDialog::openFile(&path, filter, 1, directory.c_str());
        // 原生路径仅在这一低频调用期间存活，不保存在窗口状态中。
        if ( result == NFD_OKAY && path )
            loadTemplateImport(state, Config::utf8ToPath(path));
        else if ( result == NFD_ERROR || result == NFD_OKAY )
            state.m_storageMessage = "无法打开模板文件选择器。";
        // 即使后续格式验证失败也释放原生路径，避免错误路径泄漏。
        // 路径已转换为平台值对象，候选不会借用 NFD 管理的内存。
        if ( path ) NFD_FreePathU8(path);
        return;
    }
    // 内置选择器以子模态运行；确认与取消均由当前父窗口消费结果。
    IGFD::FileDialogConfig config;
    config.path              = directory;
    config.countSelectionMax = 1;
    config.flags             = ImGuiFileDialogFlags_Modal;
    ImGuiFileDialog::Instance()->OpenDialog(
        "TimingTemplateImport", "添加模板文件", ".json", config);
}

/// @brief 展示点组，查看与放置使用文本，创建才显示输入和增删操作。
/// @warning 每帧仅绘制工作副本；相对轴转换和文件访问不在行循环执行。
/// @param state 当前页的值副本，行引用不逃逸本帧表格循环。
/// @param editable 仅创建页为 true，控制输入、设基准及增删操作。
/// @param placement 仅放置页为 true，增加目标绝对时间列。
/// @details 不使用禁用输入模拟只读，查看页只输出文本以减少视觉干扰。
/// 绝对落点来自放置页缓存，库查看不依赖目标 BPM 或播放位置。
/// 相对偏移按显示分母缩放，保存定义仍以完整拍数或秒数表示。
/// 删除操作在循环结束应用，避免 vector 移动使 point 引用悬空。
/// 插值作为整体定义，不展开为输出采样行，也不拆掉原函数。
/// 普通点类型和值可修改；段落类型和值维持来源数学定义。
/// 基准索引改变仅平移坐标，不改变已选择点组的先后距离。
/// 新建空草稿仍可添加首点，删除至空则由保存入口阻止提交。
/// 数量预算与领域模型一致，防止用户界面生成无法保存的大组。
void renderTemplatePoints(TimingTemplateEditorState& state, bool editable,
                          bool placement)
{
    auto&        draft = state.m_draft;
    const double scale =
        draft.m_variable == TimingVariable::Beat ? state.m_division : 1.0;
    int remove = -1;
    // 文件审阅也要声明偏移单位，不能把拍域的数值误读为秒。
    // 分拍分母来自工具显示设置，与模板实际保存的完整拍数区分。
    if ( !editable ) {
        if ( draft.m_variable == TimingVariable::Beat )
            ImGui::Text("相对偏移单位：1/%d 拍。", state.m_division);
        else
            ImGui::TextUnformatted("相对偏移单位：秒。");
    }
    // 列数按用途选择，查看不展示目标秒数或不可用的删除按钮。
    const int columns = editable || placement ? 5 : 4;
    if ( ImGui::BeginTable(
             "TemplatePoints",
             columns,
             ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                 ImGuiTableFlags_ScrollY,
             ImVec2(0,
                    std::max(120.f,
                             std::min(340.f,
                                      ImGui::GetContentRegionAvail().y -
                                          150.f)))) ) {
        ImGui::TableSetupColumn("基准 / 序号");
        ImGui::TableSetupColumn("类型");
        ImGui::TableSetupColumn("相对偏移 / 段尾");
        ImGui::TableSetupColumn("参数");
        if ( editable )
            ImGui::TableSetupColumn("操作");
        else if ( placement )
            ImGui::TableSetupColumn("目标时间（秒）");
        ImGui::TableHeadersRow();
        // 先按草稿的稳定顺序绘制，查看时不排序，也不读取来源 registry。
        // 局部索引 ID 保证多个相同类型输入框不会共用交互状态。
        for ( std::size_t i = 0; i < draft.m_points.size(); ++i ) {
            auto& point  = draft.m_points[i];
            auto& timing = point.m_timing;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // 只读页保留基准标识便于检查提前 SV，但不提供改变基准的入口。
            // 创建页的基准按钮不改变容器长度，行内引用仍有效。
            if ( i == draft.m_anchor )
                ImGui::Text("★ %zu", i + 1);
            else if ( !editable )
                ImGui::Text("%zu", i + 1);
            else if ( UI::FeedbackSmallButton("设为基准") ) {
                // 保留原物理落点，让后续切换秒/拍轴仍以同一来源定位。
                auto placed = placeTimingTemplate(draft,
                                                  state.m_anchorSeconds,
                                                  state.m_context,
                                                  state.m_fallbackBpm);
                if ( placed )
                    state.m_anchorSeconds = (*placed)[i].m_timestamp / 1000.0;
                else if ( draft.m_variable == TimingVariable::Time )
                    state.m_anchorSeconds += point.m_offset;
                reanchorTimingTemplate(draft, i);
                state.m_dirty = true;
            }
            ImGui::TableSetColumnIndex(1);
            if ( editable ) {
                int effect = static_cast<int>(timing.m_timingEffect) -
                             (draft.m_variable == TimingVariable::Beat ? 1 : 0);
                // 含段落的定义整体保存，不能在这里把它改为不兼容的类型。
                // 禁止拆分段落效果，避免原有函数域与新类型语义不一致。
                // 拍轴组合框排除 BPM，不能创建依赖自身红线定位的模板。
                ImGui::BeginDisabled(timing.m_interpolation.has_value());
                if ( ImGui::Combo("##Type",
                                  &effect,
                                  draft.m_variable == TimingVariable::Beat
                                      ? "Scroll\0Jump\0HS\0"
                                      : "BPM\0Scroll\0Jump\0HS\0") ) {
                    timing.m_timingEffect = static_cast<TimingEffect>(
                        effect +
                        (draft.m_variable == TimingVariable::Beat ? 1 : 0));
                    state.m_dirty = true;
                }
                ImGui::EndDisabled();
            } else {
                // 只读模板均来自领域校验或已知初始化，效果枚举在四种类型内。
                // 文本显示不会制造可以操作却没有实际行为的禁用控件。
                const char* names[] = { "BPM", "Scroll", "Jump", "HS" };
                ImGui::TextUnformatted(
                    names[static_cast<int>(timing.m_timingEffect)]);
            }
            ImGui::TableSetColumnIndex(2);
            if ( editable ) {
                double offset = point.m_offset * scale;
                // 基准必须保持零偏移；负偏移始终允许输入。
                ImGui::BeginDisabled(i == draft.m_anchor);
                // 同量平移段尾保持原段长，单独编辑段尾才改变持续范围。
                // 输入不量化至网格，用户可以精确表达分拍之间的偏移。
                if ( ImGui::InputDouble("##Offset", &offset, 0, 0, "%.9f") ) {
                    const double change = offset / scale - point.m_offset;
                    point.m_offset      = offset / scale;
                    point.m_endOffset += change;
                    state.m_dirty = true;
                }
                ImGui::EndDisabled();
                if ( timing.m_interpolation ) {
                    double end = point.m_endOffset * scale;
                    if ( ImGui::InputDouble("段尾", &end, 0, 0, "%.9f") ) {
                        point.m_endOffset = end / scale;
                        state.m_dirty     = true;
                    }
                }
            } else {
                ImGui::Text("%.9g", point.m_offset * scale);
                if ( timing.m_interpolation )
                    ImGui::Text("段尾 %.9g", point.m_endOffset * scale);
            }
            ImGui::TableSetColumnIndex(3);
            if ( editable ) {
                ImGui::BeginDisabled(timing.m_interpolation.has_value());
                if ( ImGui::InputDouble("##Value",
                                        &timing.m_timingEffectParameter,
                                        0,
                                        0,
                                        "%.9g") )
                    state.m_dirty = true;
                ImGui::EndDisabled();
            } else
                ImGui::Text("%.9g", timing.m_timingEffectParameter);
            // 插值仍是一条模板定义，不把每个输出采样点展开为按钮。
            if ( timing.m_interpolation ) ImGui::TextUnformatted("插值段落");
            if ( editable ) {
                ImGui::TableSetColumnIndex(4);
                if ( UI::FeedbackSmallButton("删除") )
                    remove = static_cast<int>(i);
                // 放置时间列只读缓存；用户改目标基准时整体重建一次。
                // 换算失败用缺省符号，不能把前一预览的旧时间当作当前结果。
            } else if ( placement ) {
                ImGui::TableSetColumnIndex(4);
                if ( i < state.m_preview.size() )
                    ImGui::Text("%.9f",
                                state.m_preview[i].m_timestamp / 1000.0);
                else
                    ImGui::TextUnformatted("—");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // 行循环结束才删除，避免已引用 point 被 vector 移动而失效。
    if ( remove >= 0 ) {
        const auto index = static_cast<std::size_t>(remove);
        draft.m_points.erase(draft.m_points.begin() + remove);
        // 删除基准才建立新原点；删除其前面的行只修正索引。
        // 避免无关删除操作偷偷平移其余所有点。
        if ( !draft.m_points.empty() ) {
            if ( index == draft.m_anchor )
                reanchorTimingTemplate(draft, 0);
            else if ( index < draft.m_anchor )
                --draft.m_anchor;
        } else
            draft.m_anchor = 0;
        state.m_dirty = true;
    }
    if ( editable ) {
        ImGui::BeginDisabled(draft.m_points.size() >= 4096);
        if ( UI::FeedbackButton("添加时间点") ) {
            TimingTemplatePoint point;
            // 新点相距一个显示单位，空草稿第一条自然成为基准。
            point.m_offset = draft.m_points.empty()
                                 ? 0
                                 : draft.m_points.back().m_offset + 1.0 / scale;
            point.m_timing.m_timingEffect          = TimingEffect::SCROLL;
            point.m_timing.m_timingEffectParameter = 1;
            draft.m_points.push_back(std::move(point));
            state.m_dirty = true;
        }
        ImGui::EndDisabled();
    }
}
}  // namespace

/// @brief 用户显式刷新时捕获完整时间线，避免每帧读取可变会话状态。
/// @warning 低频按钮路径使用会话锁复制组件；常态窗口只读取值副本。
// selected 非空时收集完整实体选择集，而不是可见标记列表。
// 当前快照实例与活动会话必须匹配，避免切换间隙读取错谱面。
// 会话锁只覆盖一次值复制，不带到模态绘制或文件保存。
// 红线和占用范围来自同一次捕获，避免拼接不一致的状态。
// 未打开谱面时返回失败，不创建欢迎页上的悬空模板。
// 读取失败不发布任何命令，调用方可以关闭窗口或重开。
// 函数保持四类时间点和完整插值定义，框选不会展开段落。
// 首选 BPM 只作为没有红线时的回退，不覆盖真实 BPM 时间线。
bool TimelineCanvas::captureTimingTemplateContext(std::vector<Timing>* selected)
{
    if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) return false;
    auto& state  = m_timingTemplateEditor;
    auto& engine = Logic::EditorEngine::instance();
    // 活动会话和注册表只能在同一锁保护范围内借用。
    // 这里是显式按钮路径，允许等待一次低频捕获。
    // 禁止为了刷新 hover 或表格数字每帧调用本函数。
    // 离开后工作副本不保存 registry 或 component 指针。
    std::lock_guard lock(engine.getSessionMutex());
    const auto      session = engine.getActiveSession();
    if ( !session ||
         reinterpret_cast<std::uintptr_t>(
             session->getContext().currentBeatmap.get()) != state.m_instanceId )
        return false;
    state.m_context.clear();
    const auto view =
        session->getContext()
            .timelineRegistry.view<const Logic::TimelineComponent>();
    for ( const auto entity : view ) {
        auto copy = copyTemplateTiming(
            view.get<const Logic::TimelineComponent>(entity));
        // 直接按实体集合捕获选区，不限制在当前可见时间段。
        // 框选后的滚动不会使模板偷偷少保存几条时间点。
        if ( selected && m_selectedTimingEntities.contains(entity) )
            selected->push_back(copy);
        state.m_context.push_back(std::move(copy));
    }
    state.m_fallbackBpm = m_currentSnapshot->fallbackBpm;
    return true;
}

/// @brief 从个人库或空白工作副本打开模板工具。
/// @warning 用户点击入口的低频路径；文件读取和排序只执行一次。
// 菜单入口不隐式捕获选区，覆盖草稿由窗口内明确操作触发。
// 每次打开都初始化新工作状态，不把前一谱面的目标范围带入。
// 记录谱面实例，后续即使同路径重开也会阻止旧窗口提交。
// 目标基准默认当前播放时间，用户可在窗口内细调秒数。
// 个人模板库只读一次，使用中的条目复制到工作副本。
// 空库新建普通 Scroll 点，使工具无需现成选区也能使用。
// ImGui 弹窗请求延迟到同一 ID 栈绘制，避免入口之间 ID 不一致。
void TimelineCanvas::openTimingTemplateEditor()
{
    if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) return;
    auto& state = m_timingTemplateEditor;
    // 重置发生在明确打开动作，常态帧不分配或丢弃编辑列表。
    // 当前模板库、错误和预览均属于新工作副本。
    // 清空选择库索引后，库加载成功才设置可用索引。
    state                 = TimingTemplateEditorState{};
    state.m_instanceId    = m_currentSnapshot->beatmapInstanceId;
    state.m_anchorSeconds = m_currentSnapshot->currentTime;
    if ( !captureTimingTemplateContext(nullptr) ) return;
    loadTemplateLibrary(state);
    // 默认进入只读查看页，空库显示引导，不隐式新建草稿。
    showTemplateLibrary(state);
    state.m_requestOpen = true;
}

/// @brief 捕获当前选区为秒轴草稿，并保留已加载的个人模板库。
/// @warning 用户按钮路径：允许一次会话捕获和稳定排序，禁止逐帧调用。
/// @note 无效选区保留旧草稿；错误由工具窗口显示。
bool TimelineCanvas::captureSelectedTimingTemplate()
{
    std::vector<Timing> selected;
    // 与目标 BPM 使用同一份捕获，避免选区与红线属于不同版本。
    if ( !captureTimingTemplateContext(&selected) ) return false;
    // 同刻按效果排序，结果不依赖 ECS 存储布局或框选方向。
    std::stable_sort(
        selected.begin(), selected.end(), [](const auto& a, const auto& b) {
            return a.m_timestamp == b.m_timestamp
                       ? a.m_timingEffect < b.m_timingEffect
                       : a.m_timestamp < b.m_timestamp;
        });
    auto& state = m_timingTemplateEditor;
    // 秒轴可接收含 BPM 的混合组选区，用户随后可选择其他基准。
    auto draft = makeTimingTemplate(selected,
                                    0,
                                    TimingVariable::Time,
                                    state.m_context,
                                    state.m_fallbackBpm);
    if ( !draft ) {
        // 捕获期间被删除的选区可变成空组，错误不能覆盖仍有效的旧草稿。
        state.m_error = draft.error();
        return false;
    }
    // 领域校验成功后才整体替换，不能把库中原条目改成半份选区。
    state.m_draft         = std::move(*draft);
    state.m_anchorSeconds = selected.front().m_timestamp / 1000.0;
    // 捕获得到的是未保存草稿，不能仍显示上一项的个人库索引。
    state.m_libraryIndex = -1;
    state.m_error.clear();
    syncTemplateName(state);
    // 捕获只建立定义，进入放置页后才检查目标范围并生成预览。
    state.m_dirty = true;
    return true;
}

/// @brief 编辑可复用点组，并将完整定义交给既有时间线权限和撤销路由。
/// @warning 每帧 UI 路径；仅输入变更重算预览，文件和 ECS 操作由按钮触发。
// 窗口使用固定 ### 内部 ID，名称编辑不影响停靠或关闭状态。
// 请求和绘制位于相同父 ID 栈，隐藏 Timeline 时仍能完成操作。
// 初次尺寸只在 FirstUseEver 设置，用户可以拖动边缘调整窗口。
// 最大尺寸按工作区约束，小屏幕允许窗口内容自身滚动。
// 每帧只绘制工作副本；读取配置、捕获 ECS 和持久化均受按钮驱动。
// 轴切换改变相对坐标的表示，不能把原秒数直接解释成拍数。
// 基准切换只平移坐标，预览目标位置保持不变。
// 内部表格行 ID 使用稳定本次索引，同名按钮不会相互覆盖。
// 删除延迟到行循环结束，避免失效引用和跳过后续条目。
// 放置只发布一条批量命令，没有逐行网络发送。
// 个人库保存和谱面放置是两个独立操作，不自动替用户同时执行。
// 关闭工作副本不撤销已存个人模板，也不修改当前谱面。
/// @details 页面转换约束：
/// 查看进入添加时尚无草稿，新增内容必须明确选择来源。
/// 查看进入创建表示修改选中项，取消时恢复库中的原定义。
/// 添加进入创建初始化普通 Scroll 点，用户随后可以扩展为完整点组。
/// 添加进入选区创建先完成一次捕获；无有效选区就留在添加页显示错误。
/// 两种创建保存成功回查看，保存失败保留当前输入供修复或重试。
/// 创建取消回来源页；查看发起的编辑取消回查看，不绕回添加。
/// 查看进入放置时捕获最新目标，并使用当前播放时间作为初始基准。
/// 放置确认只发布命令，返回查看不把目标绝对时间写进模板定义。
/// 放置取消只清空预览；保存模板库和修改谱面是不同提交边界。
/// 添加候选取消回查看，候选对象销毁后不能在下次打开时隐式提交。
/// @details 文件添加约束：
/// 文件选择器是添加页的子模态，父页保持不变直到选择器完成。
/// 父工具不能消费子窗口的 Esc，否则会造成孤立的文件选择器。
/// 导入审阅索引与个人库索引分别存储，防止误编辑同序号的旧模板。
/// 导入的只读表格与库查看共享显示，输入控件只属于创建页。
/// 文件确认之前个人库保持原值，选择其他文件只替换待确认候选。
/// 整库导入不能部分成功，任何冲突或写入失败都保留当前候选。
/// 同名替换标志每次成功读文件重新初始化，不复用之前的授权。
/// 单个模板和整库共享版本读取器，未知格式不会作为空库被接受。
/// @details 编辑与放置数据约束：
/// 名称只在创建页输入，查看和放置均从当前选中定义输出文本。
/// 来源秒数在创建阶段仅用于轴换算，不作为用户输入的放置目标。
/// 原点和分拍刻度不产生采样事件，段落输出密度继续保存于定义。
/// 从选区捕获不展开插值段，重新放置仍使用原有段落命令语义。
/// 含 BPM 的混合定义锁定秒域，普通效果组可以选择拍域定位。
/// 查看页不显示目标冲突，因为目标尚未由用户选择。
/// 创建页不显示来源占用冲突，否则同组选区不能保存为预设。
/// 只有放置页使用目标占用预览，逻辑线程再次校验最终注册表。
/// 预览显示的实际秒数不是最终命令的权威输入，只用于人工检查。
/// 逻辑命令携带完整定义、目标基准及谱面实例，避免跨谱面误写。
/// @details 交互与性能约束：
/// phase 取本帧值决定布局，导航按钮的结果由下一帧显示。
/// 类型列表和名称来自已有值副本，关闭文件选择器后不持有路径指针。
/// 浏览库不捕获 ECS，也不读取磁盘，因此播放不会触发额外会话等待。
/// 目标红线和占用范围只在进入放置或明确刷新时复制。
/// 修改目标时间只重建缓存，放置确认前不发出网络命令。
/// 库文件读写提示与预览错误分别保留，错误文本不作为状态机条件。
/// 同一组放置沿用既有批量路由，协作权限和单次撤销保持一致。
/// 新状态不得在每帧新增文件操作、完整 registry 枚举或后台等待。
/// 表格按当前用途省略列，不能通过禁用大量无关按钮模拟不同页面。
/// 用户调整的工具尺寸在页面切换后保留，只有首次使用应用默认尺寸。
void TimelineCanvas::renderTimingTemplateEditor()
{
    // 菜单发布值标志，初始化在当前父 ID 栈消费，隐藏时间线同样执行。
    if ( m_requestTimingTemplateEditor ) {
        m_requestTimingTemplateEditor = false;
        openTimingTemplateEditor();
    }
    auto& state = m_timingTemplateEditor;
    // 打开请求仅消费一次，后续显隐由 ImGui 模态栈维持。
    // 不能每帧 OpenPopup，否则关闭按钮会被下一帧重新打开。
    // 状态 open 独立于 Timeline 的显示配置。
    // 因此隐藏时间线不会吞掉尚未完成的模板编辑。
    if ( state.m_requestOpen ) {
        ImGui::OpenPopup("时间点模板###TimingTemplateEditor");
        state.m_requestOpen = false;
        state.m_open        = true;
    }
    if ( !state.m_open ) return;
    // 尺寸约束只限制工作区，不覆盖用户每次 resize 的结果。
    // 表格滚动服务较大点组，底部操作继续由模态窗口布局管理。
    // 该窗口不继承工具栏只允许调整长轴的专用限制。
    const auto work = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(
        ImVec2(std::min(880.f, work.x * .95f), std::min(650.f, work.y * .85f)),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(520.f, work.x * .95f), std::min(300.f, work.y * .95f)),
        ImVec2(work.x * .95f, work.y * .95f));
    bool open = true;
    if ( ImGui::BeginPopupModal("时间点模板###TimingTemplateEditor", &open) ) {
        auto&      draft = state.m_draft;
        const bool sameMap =
            m_currentSnapshot && m_currentSnapshot->hasBeatmap &&
            m_currentSnapshot->beatmapInstanceId == state.m_instanceId;
        // 入口页与草稿页的操作权独立，个人库为空时也可以进入添加。
        // phase 使用值枚举，不通过按钮文本或错误提示推断当前流程。
        const auto phase = state.m_phase;
        // 当前页面在本帧保持固定，按钮只决定下一帧，避免混合两页布局。
        const bool editing = phase == TimingTemplateEditorPhase::Create ||
                             phase == TimingTemplateEditorPhase::FromSelection;
        // selected 是已存库选项；FromSelection 草稿不视作已存模板。
        // 即使模板名称与库中相同，也不能在保存前假装加入了个人库。
        const bool  selected = state.m_libraryIndex >= 0 &&
                               static_cast<std::size_t>(state.m_libraryIndex) <
                                   state.m_library.size();
        const char* headings[] = {
            "模板查看", "模板添加", "模板创建", "从选中创建", "模板放置"
        };
        ImGui::TextUnformatted(headings[static_cast<int>(phase)]);
        ImGui::Separator();
        // 查看页不显示名称或参数输入，库中的定义只有进入创建页才可修改。
        // 放置按钮先复制目标上下文，不能依赖工具打开时的旧占用范围。
        if ( phase == TimingTemplateEditorPhase::View ) {
            ImGui::TextWrapped(
                "选择模板查看内容，再选择编辑或放置。添加入口用于创建或导入新模"
                "板。");
            if ( ImGui::BeginCombo(
                     "个人模板库",
                     selected ? draft.m_name.c_str() : "暂无模板") ) {
                for ( std::size_t i = 0; i < state.m_library.size(); ++i ) {
                    ImGui::PushID(static_cast<int>(i));
                    if ( UI::FeedbackSelectable(
                             state.m_library[i].m_name.c_str(),
                             state.m_libraryIndex == static_cast<int>(i)) ) {
                        // 显式选择时复制值；常态帧不复制整组或读取磁盘。
                        state.m_libraryIndex = static_cast<int>(i);
                        showTemplateLibrary(state);
                        state.m_storageMessage.clear();
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            // 页面切换采用下一帧布局；本帧选中不同模板仅刷新值副本。
            // 空库不绘制一条自动生成的 Scroll，避免用户误认为已有预设。
            if ( selected ) {
                ImGui::Text("定位依据：%s；时间点：%zu",
                            draft.m_variable == TimingVariable::Beat
                                ? "节拍 / 分拍"
                                : "时间戳（秒）",
                            draft.m_points.size());
                renderTemplatePoints(state, false, false);
            } else
                ImGui::TextWrapped(
                    "个人库为空。点击“添加模板”，选择文件导入、空白创建或从选中"
                    "创建。");
            if ( state.m_confirmDelete ) {
                // 删除确认独占管理操作区，不与放置按钮并排造成误触。
                ImGui::TextWrapped(
                    "删除个人库中的“%s”？谱面中的时间点不会改变。",
                    draft.m_name.c_str());
                if ( UI::FeedbackButton("确认删除") ) {
                    auto library = state.m_library;
                    // 删除针对当前查看项，取消删除不会改变任何库条目。
                    // 候选库与磁盘都成功更新后才修复查看页索引。
                    library.erase(library.begin() + state.m_libraryIndex);
                    const auto error = saveTemplateLibrary(library);
                    if ( error.empty() ) {
                        state.m_library = std::move(library);
                        showTemplateLibrary(state);
                        state.m_storageMessage = "已删除模板。";
                    } else
                        state.m_storageMessage = error;
                }
                ImGui::SameLine();
                if ( UI::FeedbackButton("取消删除") )
                    state.m_confirmDelete = false;
            } else {
                if ( UI::FeedbackButton("添加模板") ) {
                    state.m_phase = TimingTemplateEditorPhase::Add;
                    state.m_storageMessage.clear();
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!selected);
                // 既有库条目仍保留原值，编辑页改动可以完整取消。
                // 取消修改返回查看；从添加页新建则返回来源选择。
                if ( UI::FeedbackButton("编辑模板") ) {
                    state.m_phase           = TimingTemplateEditorPhase::Create;
                    state.m_editReturnPhase = TimingTemplateEditorPhase::View;
                    state.m_storageMessage.clear();
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!sameMap);
                if ( UI::FeedbackButton("放置模板") &&
                     captureTimingTemplateContext(nullptr) ) {
                    // 每次进入放置使用最新目标，不沿用创建时的来源基准。
                    state.m_phase         = TimingTemplateEditorPhase::Place;
                    state.m_anchorSeconds = m_currentSnapshot->currentTime;
                    state.m_dirty         = true;
                    state.m_storageMessage.clear();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(!state.m_libraryWritable);
                if ( UI::FeedbackButton("删除模板") )
                    state.m_confirmDelete = true;
                ImGui::EndDisabled();
                ImGui::EndDisabled();
            }
            // 添加先选择来源，不能在来源选择时发布谱面放置命令。
            // 文件候选与创建草稿使用不同存储，取消导入不会残留为已存项。
        } else if ( phase == TimingTemplateEditorPhase::Add ) {
            if ( state.m_imports.empty() ) {
                ImGui::TextWrapped(
                    "选择模板来源。创建或导入后还需确认保存，才会加入个人模板库"
                    "。");
                if ( UI::FeedbackButton("从文件导入") )
                    openTemplateImportPicker(state);
                ImGui::TextWrapped(
                    "支持单个模板 JSON 和模板库 "
                    "JSON，先检查内容，再确认添加。");
                ImGui::Separator();
                // 空白创建从普通 Scroll 开始，避免用户面对无法编辑的空表格。
                // 取消之后恢复个人库查看项，不把默认点存进个人库。
                if ( UI::FeedbackButton("空白创建") ) {
                    draft = TimingTemplate{};
                    TimingTemplatePoint point;
                    point.m_timing.m_timingEffect = TimingEffect::SCROLL;
                    point.m_timing.m_timingEffectParameter = 1;
                    draft.m_points.push_back(std::move(point));
                    state.m_libraryIndex    = -1;
                    state.m_phase           = TimingTemplateEditorPhase::Create;
                    state.m_editReturnPhase = TimingTemplateEditorPhase::Add;
                    state.m_replaceExisting = false;
                    state.m_storageMessage.clear();
                    state.m_error.clear();
                    syncTemplateName(state);
                }
                ImGui::TextWrapped(
                    "手动设置类型、参数和相对偏移，选择组内基准。");
                ImGui::Separator();
                // 捕获必须使用打开工具时的来源实例，不能从另一张谱面取选择集。
                // 非空查询是常量操作，完整复制只发生在点击之后。
                ImGui::BeginDisabled(!sameMap ||
                                     m_selectedTimingEntities.empty());
                if ( UI::FeedbackButton("从选中创建") &&
                     captureSelectedTimingTemplate() ) {
                    state.m_phase = TimingTemplateEditorPhase::FromSelection;
                    state.m_editReturnPhase = TimingTemplateEditorPhase::Add;
                    state.m_replaceExisting = false;
                    state.m_storageMessage.clear();
                    state.m_dirty = false;
                }
                ImGui::EndDisabled();
                ImGui::TextWrapped(
                    "先在时间线上框选时间点，再进入此页。捕获后检查基准和偏移，"
                    "再保存。");
                if ( m_selectedTimingEntities.empty() )
                    ImGui::TextUnformatted("当前没有选中的时间点。");
            } else {
                ImGui::TextWrapped("文件：%s", state.m_importSource.c_str());
                ImGui::Text("待添加：%zu 个模板（尚未保存）",
                            state.m_imports.size());
                // 文件内模板逐个审阅，但确认始终添加整个经过校验的候选组。
                // 点击审阅项不会更新个人库索引，也不会偷偷替换磁盘条目。
                if ( ImGui::BeginCombo("查看文件内容", draft.m_name.c_str()) ) {
                    for ( std::size_t i = 0; i < state.m_imports.size(); ++i ) {
                        ImGui::PushID(static_cast<int>(i));
                        if ( UI::FeedbackSelectable(
                                 state.m_imports[i].m_name.c_str(),
                                 state.m_importIndex == static_cast<int>(i)) ) {
                            state.m_importIndex = static_cast<int>(i);
                            draft               = state.m_imports[i];
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                renderTemplatePoints(state, false, false);
                // 文件可携带同名预设，替换必须由本次添加页明确勾选。
                // 没勾选时合并整体失败，先前有效候选也不会部分写入。
                ImGui::Checkbox("允许替换个人库中的同名模板",
                                &state.m_replaceExisting);
                if ( UI::FeedbackButton("确认添加文件中的模板") )
                    commitTemplateCandidates(
                        state, state.m_imports, state.m_replaceExisting);
                ImGui::SameLine();
                if ( UI::FeedbackButton("重新选择文件") )
                    openTemplateImportPicker(state);
            }
            if ( !state.m_error.empty() )
                ImGui::TextWrapped("%s", state.m_error.c_str());
            if ( UI::FeedbackButton("返回模板查看") )
                showTemplateLibrary(state);
            // 手工创建与选区创建共享定义编辑，但保持独立状态标题与返回方向。
            // 这里不显示目标基准输入，避免保存预设被误认为已经放入谱面。
        } else if ( editing ) {
            ImGui::TextWrapped(
                phase == TimingTemplateEditorPhase::FromSelection
                    ? "已捕获选中的时间点。检查组内基准和偏移，保存后即可重复使"
                      "用。"
                    : "设置模板定义并保存到个人库。目标时间在放置步骤设置。");
            if ( ImGui::InputText(
                     "模板名称", state.m_name.data(), state.m_name.size()) ) {
                draft.m_name  = state.m_name.data();
                state.m_dirty = true;
            }
            // 是否允许拍轴依据真实效果枚举，不依据模板名称。
            // 含 BPM 的混合组也锁定秒轴，避免定位坐标依赖自己创建的红线。
            // 普通 Scroll、Jump 和 HS 可以共享同一个拍域基准。
            // 效果改变后立即重新判断，不允许先切轴再偷偷添加 BPM。
            const bool hasBpm = std::any_of(
                draft.m_points.begin(),
                draft.m_points.end(),
                [](const auto& point) {
                    return point.m_timing.m_timingEffect == TimingEffect::BPM;
                });
            // 当前轴是领域模型中的稳定枚举，显示索引仅用于组合框。
            // 切换失败保留旧轴和旧定义，而不产生一半秒一半拍的条目。
            // 换算使用当前完整红线，跨 BPM 边界同样保持物理落点。
            // 目标基准不变，只有各点和段尾的偏移单位变化。
            int variable = draft.m_variable == TimingVariable::Beat ? 1 : 0;
            ImGui::BeginDisabled(hasBpm || !sameMap);
            if ( ImGui::Combo(
                     "定位依据", &variable, "时间戳（秒）\0节拍 / 分拍\0") ) {
                // 轴切换先在当前目标上还原实际时间，再转成新坐标。
                // 切换不是直接把秒数当拍数，不改变当前预览的物理落点。
                if ( captureTimingTemplateContext(nullptr) ) {
                    auto placed = placeTimingTemplate(draft,
                                                      state.m_anchorSeconds,
                                                      state.m_context,
                                                      state.m_fallbackBpm);
                    if ( placed ) {
                        auto converted = makeTimingTemplate(
                            *placed,
                            draft.m_anchor,
                            variable == 1 ? TimingVariable::Beat
                                          : TimingVariable::Time,
                            state.m_context,
                            state.m_fallbackBpm);
                        if ( converted ) {
                            converted->m_name = draft.m_name;
                            draft             = std::move(*converted);
                            state.m_error.clear();
                            state.m_dirty = true;
                        } else
                            state.m_error = converted.error();
                    } else
                        state.m_error = placed.error();
                }
            }
            ImGui::EndDisabled();
            if ( hasBpm )
                ImGui::TextUnformatted("含 BPM 的模板只能依据时间戳。");
            // N 调整只改变显示缩放，不量化已存偏移。
            // 偏移数量允许非整数，仍能表达网格之间的拍位。
            // 1/192 可直接填写 -1，用户无需手算秒数。
            // 实际时间依完整红线决定，不把整组显示成恒定 Hz。
            if ( draft.m_variable == TimingVariable::Beat ) {
                if ( ImGui::InputInt("分拍分母 N", &state.m_division) ) {
                    // 显示单位只改变输入刻度，不改变已经保存的相对拍数。
                    state.m_division = std::clamp(state.m_division, 1, 65536);
                }
                ImGui::TextWrapped(
                    "偏移以 1/%d 拍为单位：填 -1 表示基准前 1/%d "
                    "拍；可选任意一行为基准。",
                    state.m_division,
                    state.m_division);
            } else
                ImGui::TextUnformatted(
                    "偏移单位为秒；基准前的时间点使用负偏移。");

            ImGui::TextWrapped(
                "基准点偏移为零；提前的时间点使用负偏移。点击“设为基准”保留点间"
                "距离。");
            // 编辑表格只保留类型、相对偏移和参数，目标绝对时间留给放置页。
            // 对来源谱面的占用冲突不检查，已有组选区同样允许保存为模板。
            renderTemplatePoints(state, true, false);
            // 创建不检查来源谱面占用，否则选区原地捕获会被误报为不能保存。
            if ( state.m_dirty ) {
                state.m_dirty = false;
                state.m_storageMessage.clear();
            }
            if ( !state.m_error.empty() )
                ImGui::TextWrapped("%s", state.m_error.c_str());
            // 修改既有选项允许保存回原名；另存到其他同名项仍需明确授权。
            // 仅当前选中库条目的原名可以直接更新，改为其他同名项需再次授权。
            // 创建新模板时库索引是 -1，不能因名字相同就自动覆盖旧项。
            const bool ownName =
                selected &&
                state.m_library[state.m_libraryIndex].m_name == draft.m_name;
            const bool duplicate =
                std::any_of(state.m_library.begin(),
                            state.m_library.end(),
                            [&](const auto& saved) {
                                return saved.m_name == draft.m_name;
                            });
            if ( duplicate && !ownName )
                ImGui::Checkbox("允许替换个人库中的同名模板",
                                &state.m_replaceExisting);
            ImGui::BeginDisabled(!state.m_libraryWritable ||
                                 draft.m_points.empty());
            if ( UI::FeedbackButton("保存模板并返回查看") )
                commitTemplateCandidates(
                    state, { draft }, ownName || state.m_replaceExisting);
            ImGui::EndDisabled();
            ImGui::SameLine();
            // 保存之前全部改变都在草稿内，取消只恢复页面和展示副本。
            // 不发送谱面命令，不创建撤销项，也不写个人库文件。
            if ( UI::FeedbackButton("取消创建 / 编辑") ) {
                const auto destination = state.m_editReturnPhase;
                showTemplateLibrary(state);
                state.m_phase = destination;
                state.m_storageMessage.clear();
            }
            // 放置只接受已保存模板，不允许未保存的创建草稿越过确认步骤。
            // 页面中的类型、偏移和参数均只读，基准秒数只作用于本次放置。
        } else if ( phase == TimingTemplateEditorPhase::Place ) {
            ImGui::TextWrapped(
                "放置模板：%s。这里只调整目标时间，模板定义保持不变。",
                draft.m_name.c_str());
            if ( ImGui::InputDouble(
                     "放置基准（秒）", &state.m_anchorSeconds, 0, 0, "%.9f") )
                state.m_dirty = true;
            ImGui::BeginDisabled(!sameMap);
            // 手动选取当前时间才更新基准，播放过程中不会每帧追着播放位置走。
            // 本次同时刷新红线与占用范围，时间和换算上下文属于同次捕获。
            if ( UI::FeedbackButton("使用当前播放时间") &&
                 captureTimingTemplateContext(nullptr) ) {
                state.m_anchorSeconds = m_currentSnapshot->currentTime;
                state.m_dirty         = true;
            }
            ImGui::SameLine();
            if ( UI::FeedbackButton("刷新目标 BPM / 范围") &&
                 captureTimingTemplateContext(nullptr) )
                state.m_dirty = true;
            ImGui::EndDisabled();
            // 只有放置页检查目标占用；输入事件合并为一次落点重建。
            if ( state.m_dirty ) refreshTemplatePreview(state);
            // 缓存先更新再绘制，让目标输入变化当帧即可显示新的实际落点。
            // 模板定义没有被改写，取消放置后可以继续复用同一模板。
            renderTemplatePoints(state, false, true);
            if ( !sameMap )
                ImGui::TextUnformatted("谱面已切换，请重新打开工具后放置。");
            if ( !state.m_error.empty() )
                ImGui::TextWrapped("%s", state.m_error.c_str());
            ImGui::BeginDisabled(!sameMap || !state.m_error.empty() ||
                                 state.m_preview.empty());
            if ( UI::FeedbackButton("确认放置整组") ) {
                // 逻辑线程复核最新红线、权限与范围，整组只有一次撤销动作。
                Logic::CmdCreateTimelineEvents command;
                command.requireAllValid           = true;
                command.templateDefinition        = draft;
                command.templateAnchorSeconds     = state.m_anchorSeconds;
                command.templateBeatmapInstanceId = state.m_instanceId;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(std::move(command)));
                // 命令已发布不代表逻辑最终接受，提示只说明已经提交。
                // 权限与冲突的最终结果仍通过既有命令反馈路由展示。
                showTemplateLibrary(state);
                state.m_storageMessage = "已提交整组放置。";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( UI::FeedbackButton("取消放置") ) showTemplateLibrary(state);
        }
        // 文件提示与目标预览错误分开，避免保存失败被落点重建清掉。
        // 查看页可以保留上一步成功提示，其他页保持当前操作上下文。
        if ( !state.m_storageMessage.empty() )
            ImGui::TextWrapped("%s", state.m_storageMessage.c_str());
        if ( phase == TimingTemplateEditorPhase::View ) {
            if ( UI::FeedbackButton("关闭工具") ) {
                state.m_open = false;
                ImGui::CloseCurrentPopup();
            }
        }
        // 内置导入子模态在父 ID 栈绘制，不能提前切换页面或消费父 Esc。
        const bool importing =
            ImGuiFileDialog::Instance()->IsOpened("TimingTemplateImport");
        // 文件选择器覆盖工具时保留添加状态，不能同时导航回查看。
        // prepare 只服务已打开的子窗口，不改变父工具用户调整后的尺寸。
        if ( importing ) {
            UI::Utils::CenteredModalPopupScope popupStyle;
            UI::Utils::prepareCenteredModalWindow({ 600, 400 });
            if ( ImGuiFileDialog::Instance()->Display(
                     "TimingTemplateImport",
                     ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 400, 300 }) ) {
                // 只有确认才读取文件；取消仍需关闭子对话框的内部状态。
                // 返回路径进行 UTF-8 转换，中文文件名在 Windows 仍保持正确。
                if ( ImGuiFileDialog::Instance()->IsOk() )
                    loadTemplateImport(
                        state,
                        Config::utf8ToPath(
                            ImGuiFileDialog::Instance()->GetFilePathName()));
                ImGuiFileDialog::Instance()->Close();
            }
        }
        if ( !importing && ImGui::IsKeyPressed(ImGuiKey_Escape) ) {
            // Esc 在操作页取消当前步骤，只有查看页才关闭整个工具。
            if ( phase == TimingTemplateEditorPhase::View ) {
                state.m_open = false;
                ImGui::CloseCurrentPopup();
            } else {
                // 文件候选取消回查看，创建取消回其来源页；取消不复用旧预览。
                // 返回状态与关闭工具分开，Esc 不会把半份草稿存到个人库。
                const auto destination = editing
                                             ? state.m_editReturnPhase
                                             : TimingTemplateEditorPhase::View;
                showTemplateLibrary(state);
                state.m_phase = destination;
                state.m_storageMessage.clear();
            }
        }
        ImGui::EndPopup();
    }
    if ( !open ) state.m_open = false;
}
}  // namespace MMM::Canvas
