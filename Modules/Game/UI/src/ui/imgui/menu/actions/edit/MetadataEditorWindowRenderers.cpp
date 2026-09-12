#define IMGUI_DEFINE_MATH_OPERATORS
#include "MetadataEditorWindowRenderers.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/SamplePropertyEdit.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Note.h"
#include "mmm/project/Project.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <fmt/core.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace MMM::UI
{

namespace
{
/// @brief 元数据字段 JSON 编辑器可写回的结果。
/// @details 结果携带作用域和字段键，防止一个全局弹窗的结果被错误页面消费。
struct MetadataJsonEditResult {
    /// @brief 调用者传入的作用域标识，用于区分谱面/音符/格式页。
    /// @note 消费端必须精确匹配该值后才能取走结果。
    std::string scopeId;
    /// @brief 被编辑的字段名。
    /// @note 保留打开弹窗时的键名，编辑期间不允许更换目标字段。
    std::string key;
    /// @brief 校验通过后写回属性表的 JSON 文本。
    /// @note 文本已经按四空格缩进规整，可直接持久化到字符串属性表。
    std::string value;
};

/// @brief 元数据 JSON 编辑器的跨帧 UI 状态。
/// @details 全局只允许一个 JSON 字段弹窗；状态对象覆盖打开请求、可写缓冲、
/// 校验反馈和待消费结果，调用方通过 scopeId 隔离不同页面。
struct MetadataJsonEditorState {
    /// @warning 全部字段只允许 UI 线程读写，后台线程不得持有缓冲区指针。
    /// @note 固定缓冲容量属于界面限制，不代表底层元数据格式上限。
    /// @brief 当前弹窗是否处于打开状态。
    /// @note 用户确认、取消或关闭窗口时复位。
    bool open = false;
    /// @brief 下一次渲染是否需要打开 ImGui 弹窗。
    /// @note 调用 FeedbackOpenPopup 后立即复位，避免每帧重复打开。
    bool requestOpen = false;
    /// @brief 当前编辑目标的作用域标识。
    /// @note 例如谱面 Malody 页或某一组选中音符的 Malody 页。
    std::string scopeId;
    /// @brief 当前编辑目标字段。
    /// @note 同时用于 mode_ext、beat 等已知结构的警告规则。
    std::string key;
    /// @brief 展示给用户看的完整路径。
    /// @note 由 scopeId 和 key 组合，仅用于弹窗标题区域。
    std::string displayPath;
    /// @brief 原生 JSON 编辑缓冲区。
    /// @note 固定 32KiB 上限避免每帧动态缓冲重建。
    std::array<char, 32768> jsonBuffer{};
    /// @brief 子字段键名输入缓冲区。
    /// @note 成功添加字段后清空，便于连续录入。
    std::array<char, 128> childKeyBuffer{};
    /// @brief 子字段值输入缓冲区。
    /// @note 值可为任意 JSON；解析失败时按普通字符串保存。
    std::array<char, 4096> childValueBuffer{};
    /// @brief 最近一次校验错误。
    /// @note 错误会阻止完成操作，但格式警告不会。
    std::string errorText;
    /// @brief 最近一次结构警告。
    /// @note 针对常见字段给出非阻塞式结构完整性提示。
    std::string warningText;
    /// @brief 等待调用方消费的写回结果。
    /// @note 确认时生成，只有作用域匹配的调用方可以移走。
    std::optional<MetadataJsonEditResult> result;
};

/// @brief 元数据属性表类型。
/// @details 使用透明字符串哈希和比较器，允许 string_view 风格查找而不分配。
using MetadataPropertyMap =
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>;

/// @brief 元数据字段说明列表类型。
/// @details 每项依次保存持久化键与面向用户的中文字段说明。
using MetadataFieldList = std::vector<std::pair<std::string, std::string>>;

/// @brief OSU 纯文本元数据编辑器状态。
/// @details 纯文本编辑与表格编辑共用同一属性表，通过一次性 result 交接。
struct OsuMetadataTextEditorState {
    /// @warning 全部字段只允许 UI 线程访问，结果也在 UI 帧内消费。
    /// @note 关闭弹窗不会自动产生或应用解析结果。
    /// @brief 当前弹窗是否处于打开状态。
    /// @note 关闭后保留缓冲内容，但不再绘制编辑器。
    bool open = false;
    /// @brief 下一次渲染是否需要打开 ImGui 弹窗。
    /// @note 用于把业务请求转换为下一帧 ImGui OpenPopup 调用。
    bool requestOpen = false;
    /// @brief 纯文本编辑缓冲区。
    /// @note 固定 64KiB 容量覆盖常见 OSU 头部和 Events 元数据。
    std::array<char, 65536> textBuffer{};
    /// @brief 最近一次解析错误。
    /// @note 包含行号和具体语法原因，成功解析后清空。
    std::string errorText;
    /// @brief 等待调用方消费的写回结果。
    /// @note 只有点击完成且全文解析成功时产生。
    std::optional<MetadataPropertyMap> result;
};

/// @brief 单个选中自动采样的精确属性编辑状态。
/// @details 表单按选中实体缓存资源、轨道、偏移和音量，避免每帧从组件覆盖
/// 用户尚未提交的输入；切换实体或点击重置时重新初始化。
struct SamplePropertyEditorState {
    /// @warning 表单只属于 UI 线程，组件更新必须通过编辑器命令队列。
    /// @note entity 只检测选择切换，不延长 ECS 实体生命周期。
    /// @brief 当前编辑的自动采样实体。
    /// @note entt::null 表示尚未绑定任何选择。
    entt::entity entity{ entt::null };

    /// @brief 当前表单是否已从组件初始化。
    /// @note 窗口重新打开时主动复位，确保读取最新组件状态。
    bool initialized{ false };

    /// @brief 待写回的项目音频资源引用。
    /// @note 下拉框只提供 Main 和 Effect 类型的非空资源 ID。
    std::string audioResourceId;

    /// @brief 面向用户的一基 BGM 相对轨道编号。
    /// @note 提交前转换为零基编号，再由逻辑层加上玩家轨道数。
    std::int32_t bgmLaneOneBased{ 1 };

    /// @brief 待写回的有符号毫秒偏移。
    /// @note 使用 int64 保留项目模型允许的完整时间范围。
    std::int64_t offsetMs{ 0 };

    /// @brief 待写回的物件音量。
    /// @note 合法范围由 resolveSamplePropertyEdit 统一验证。
    float volume{ 1.0F };
};

/// @brief 获取元数据 JSON 编辑器的持久状态。
/// @return 进程内唯一 JSON 编辑器状态。
/// @warning UI 每帧绘制路径：仅保存少量弹窗状态，不进行文件系统操作。
/// @note 函数静态对象避免跨翻译单元初始化顺序问题。
MetadataJsonEditorState& metadataJsonEditorState()
{
    // 状态生命周期覆盖所有元数据窗口，但结果仍按 scopeId 隔离。
    static MetadataJsonEditorState state;
    return state;
}

/// @brief 获取 OSU 纯文本元数据编辑器的持久状态。
/// @return 进程内唯一 OSU 文本编辑器状态。
/// @warning UI 每帧绘制路径：仅保存少量弹窗状态，不进行文件系统操作。
/// @note 同一时间只允许编辑一张活动谱面的 OSU 属性表。
OsuMetadataTextEditorState& osuMetadataTextEditorState()
{
    // 静态状态使模态弹窗跨帧保留输入缓存。
    static OsuMetadataTextEditorState state;
    return state;
}

/// @brief 获取自动采样精确属性编辑器的跨帧状态。
/// @return 当前唯一自动采样表单状态。
/// @warning UI 每帧绘制路径：仅保存一个选中物件的表单数据。
/// @note 实体切换由 renderSelectedSampleProperties 检测并重置。
SamplePropertyEditorState& samplePropertyEditorState()
{
    // 窗口关闭再打开时由外部状态边沿主动清除 initialized。
    static SamplePropertyEditorState state;
    return state;
}

/// @brief 标记扩展元数据已修改，并请求逻辑线程执行尾随自动保存。
/// @warning UI 交互路径：只入队标脏指令，不在 UI 线程执行文件写入。
/// @note 连续输入可产生多个标脏请求，逻辑层负责合并尾随保存。
/// @post 当前谱面会在逻辑线程进入待保存状态；本帧不保证已经落盘。
void requestBeatmapMetadataAutoSave()
{
    // 通过统一命令队列跨越 UI/逻辑线程边界。
    Logic::EditorEngine::instance().pushCommand(
        Logic::CmdMarkBeatmapMetadataDirty{});
}

/// @brief 渲染离线协作谱面提示，并把窗口内的编辑尝试交给统一只读门闩。
/// @param session 当前离线协作谱面会话。
/// @warning UI 每帧路径：只绘制固定文本；仅在鼠标点击时入队一次拦截命令。
/// @note 点击仍提交标脏命令，让统一只读门闩显示既有不可编辑反馈。
/// @pre session 已由调用方在会话锁保护下确认处于离线只读状态。
void renderCollaborationOfflineReadOnlyNotice(Logic::BeatmapSession& session)
{
    // 文案说明离线协作副本当前不可写，避免显示可编辑控件。
    ImGui::TextWrapped("%s",
                       TR("ui.collaboration.offline_edit.message").data());
    if ( ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        // 复用逻辑层权限检查，不在 UI 中复制协作提示策略。
        session.pushCommand(
            Logic::LogicCommand(Logic::CmdMarkBeatmapMetadataDirty{}));
    }
}

/// @brief 将字符串安全复制进固定 ImGui 输入缓冲区。
/// @tparam N 缓冲区容量，必须至少能容纳终止符。
/// @param buffer 目标 NUL 终止字符数组。
/// @param text 待复制 UTF-8 文本。
/// @warning UI 每帧/交互路径：只做固定上限内存复制。
/// @note 超长文本按字节截断，调用方应为预期字段配置足够容量。
/// @details 固定数组由状态对象或栈帧拥有，函数不会保存 text 视图；复制完成
/// 后缓冲始终至少保留一个终止符，可直接交给 ImGui InputText 系列接口。
template<size_t N>
void copyToInputBuffer(std::array<char, N>& buffer, std::string_view text)
{
    // 预清零移除旧长字符串尾部并保证最终 NUL 终止。
    buffer.fill('\0');
    // 最后一字节保留为终止符，不读取 string_view 边界之外内容。
    const size_t copyLen = std::min(text.size(), N - 1);
    std::copy_n(text.data(), copyLen, buffer.data());
}

/// @brief 解析 JSON 文本，不使用 C++ 异常。
/// @param text NUL 终止的 JSON 文本。
/// @return 合法 JSON 值；失败时返回 discarded 值。
/// @note allow_exceptions=false 保持项目禁异常约束。
nlohmann::json parseJsonNoThrow(const char* text)
{
    // 回调为空表示使用默认解析器，错误通过返回值表达。
    return nlohmann::json::parse(text, nullptr, false);
}

/// @brief 将 JSON 规整为缩进文本。
/// @param value 已解析 JSON 值。
/// @return 使用四空格缩进的稳定文本表示。
/// @note 只在用户交互或弹窗打开路径调用，不位于渲染主循环常态分支。
/// @warning 输出会分配与 JSON 文本大小成比例的字符串。
std::string dumpPrettyJson(const nlohmann::json& value)
{
    return value.dump(4);
}

/// @brief 将子字段输入解析为 JSON 值，非法 JSON 时按字符串保存。
/// @param text 子字段值输入。
/// @return JSON 标量、数组或对象；语法无效时返回字符串值。
/// @note 允许用户直接输入普通文本而无需手动添加 JSON 引号。
nlohmann::json parseChildValueOrString(const char* text)
{
    // 首先尝试保留数字、布尔值、null 和复合结构的原生类型。
    nlohmann::json value = parseJsonNoThrow(text);
    if ( value.is_discarded() ) {
        // 非 JSON 文本按字符串构造，后续 dump 会自动正确转义。
        return std::string(text);
    }
    return value;
}

/// @brief 将文本解析为 int32 元数据值，不使用 C++ 异常。
/// @param text 待解析的完整十进制文本。
/// @return 完整且范围合法时返回数值，否则返回空。
/// @note 不接受尾随字符，避免把部分有效输入静默写入格式元数据。
/// @details 正负号和十进制范围由 std::from_chars 处理；函数不主动裁剪空白，
/// 调用方应先按对应文本格式决定是否允许并清理空白。
std::optional<int32_t> parseInt32Metadata(std::string_view text)
{
    // 空字段与数值零语义不同，直接报告无值。
    if ( text.empty() ) return std::nullopt;

    // from_chars 不分配、不受区域设置影响且不抛异常。
    int32_t     value    = 0;
    const char* begin    = text.data();
    const char* end      = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if ( ec != std::errc{} || ptr != end ) {
        // 同时拒绝范围溢出、非法首字符和未完整消费输入。
        return std::nullopt;
    }
    return value;
}

/// @brief 将浮点数夹到 int32 范围后转换。
/// @param value 待转换浮点值。
/// @return 四舍五入并限制在 int32 可表示范围内的结果。
/// @note 用于外部格式数值同步，防止直接转换产生越界行为。
/// @pre value 应为有限值；调用处的数据来自已解析数值或谱面元数据。
int32_t clampDoubleToInt32(double value)
{
    // 将整数边界提升为 double，与输入在同一类型内比较。
    const double minValue =
        static_cast<double>(std::numeric_limits<int32_t>::min());
    const double maxValue =
        static_cast<double>(std::numeric_limits<int32_t>::max());
    // 先钳制再舍入，保证最终 static_cast 位于可表示区间。
    return static_cast<int32_t>(
        std::round(std::clamp(value, minValue, maxValue)));
}

/// @brief 判断 Malody 元数据键是否只用于内部兼容，不应在格式元数据中显示。
/// @param key Malody 属性键。
/// @return 属于内部时间兼容字段时返回 true。
/// @warning UI 每帧绘制路径：只做固定字符串比较。
/// @note 这些值由统一时间模型维护，直接编辑会与公开 offset 语义冲突。
bool isHiddenMalodyMetadataKey(std::string_view key)
{
    return key == "initialDelay" || key == "audioOffset";
}

/// @brief 根据 Malody mode 自动同步 free 字段。
/// @param props 待同步的 Malody 属性表。
/// @return free 字段发生变化时返回 true。
/// @warning UI 每帧绘制路径：只解析一个 int32 并更新一个小字符串。
/// @note Key 模式映射 free=0，Slide 模式映射 free=1，其他模式保持原值。
/// @details free 是 mode 的兼容派生字段。同步只在 mode 可严格解析且属于
/// 项目支持的 0 或 7 时发生，避免覆盖未来 Malody 模式的未知语义。
bool syncMalodyFreeFromMode(MetadataPropertyMap& props)
{
    // 缺少 mode 时无法推导 free，也不主动创建未知默认值。
    auto modeIt = props.find("mode");
    if ( modeIt == props.end() ) {
        return false;
    }

    // 严格整数解析避免对非法文本进行猜测同步。
    auto mode = parseInt32Metadata(modeIt->second);
    if ( !mode ) {
        return false;
    }

    // 只覆盖项目明确支持的两个 Malody 模式。
    const char* freeValue = nullptr;
    if ( *mode == 7 ) {
        freeValue = "1";
    } else if ( *mode == 0 ) {
        freeValue = "0";
    } else {
        // 未知 mode 可能属于未来格式，保留其现有 free 字段。
        return false;
    }

    // 已经一致时不触发自动保存或无意义字符串写入。
    auto freeIt = props.find("free");
    if ( freeIt != props.end() && freeIt->second == freeValue ) {
        return false;
    }

    // 插入或覆盖派生字段，使导出的 mode/free 保持一致。
    props["free"] = freeValue;
    return true;
}

/// @brief 读取状态栏同源的当前判定线时间，并转换为预览元数据毫秒文本。
/// @return 非负毫秒整数字符串。
/// @warning UI 交互路径：只读取同步快照或当前会话，不等待逻辑线程。
/// @note 优先活动画布快照，缺失时回退会话时间，最后默认零。
/// @details 读取过程不请求同步或等待生产者；快照与会话时间均不可用时使用
/// 已初始化的零值。负时间钳制为零，过大时间钳制到 int32 上限。
std::string readCurrentJudgelinePreviewMsText()
{
    // 活动画布 ID 为空时使用主时间轴画布的稳定默认键。
    auto&       engine         = Logic::EditorEngine::instance();
    std::string activeCameraId = engine.getActiveCameraId();
    auto        syncBuffer     = engine.getSyncBuffer(
        activeCameraId.empty() ? "Basic2DCanvas" : activeCameraId);

    // hasTime 区分有效零秒快照与缺少快照两种情况。
    double timeSeconds = 0.0;
    bool   hasTime     = false;
    if ( syncBuffer ) {
        // 读取端快照为非拥有观察指针，仅在当前访问期间使用。
        auto* snapshot = syncBuffer->getReadingSnapshot();
        if ( snapshot ) {
            timeSeconds = snapshot->currentTime;
            // 标记后不再从可能略有差异的会话时间覆盖。
            hasTime = true;
        }
    }

    if ( !hasTime ) {
        // 无画布同步数据时读取活动会话的逻辑时间。
        auto session = engine.getActiveSession();
        if ( session ) {
            timeSeconds = session->getContext().currentTime;
        }
    }

    // 预览时间不允许负值，并限制为目标格式的 int32 毫秒范围。
    int32_t timeMs = clampDoubleToInt32(std::max(0.0, timeSeconds) * 1000.0);
    return std::to_string(timeMs);
}

/// @brief 去掉纯文本编辑行首尾空白。
/// @param value 原始单行文本。
/// @return 去除首尾空白但保留中间内容的文本。
/// @note 使用 unsigned char 传给 cctype，避免负 char 未定义行为。
/// @details 函数按字节识别 C locale 空白，不修改 UTF-8 多字节字符或行内空格。
std::string trimMetadataLine(std::string value)
{
    // 谓词同时覆盖空格、制表符和其他 ASCII 空白。
    auto isNotSpace = [](unsigned char ch) { return !std::isspace(ch); };
    // 首次查找定位第一个非空白字符并删除前缀。
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), isNotSpace));
    // 反向查找定位最后一个非空白字符之后的位置并删除后缀。
    value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(),
                value.end());
    return value;
}

/// @brief 判断字符串是否以指定前缀开头。
/// @param text 完整文本。
/// @param prefix 待匹配前缀。
/// @return 长度足够且前段完全相同时返回 true。
/// @warning UI 辅助路径：只构造 string_view 子视图，不分配。
/// @note 空 prefix 按标准前缀语义匹配任意文本。
bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

/// @brief 获取 OSU 元数据字段的导出默认值。
/// @param beatmap 当前谱面，用于推导基础标题、音频、封面和轨道数。
/// @param key 完整属性键，节字段使用 Section::Key 形式。
/// @return 对应 OSU 字段默认文本；未知键返回空字符串。
/// @note 默认值用于补齐导出表和单字段复位，不直接修改 BeatMap。
/// @details General 和 Difficulty 使用格式兼容的保守常量；可共享标题、作者、
/// 键数、音频和封面信息从 BaseMapMeta 推导；编辑器专属字段采用中性值。
std::string getOsuMetadataDefaultValue(const BeatMap&     beatmap,
                                       const std::string& key)
{
    // 基础元数据是可跨格式共享的默认来源。
    const auto& base = beatmap.m_baseMapMetadata;

    // 文件版本保留 v 前缀，序列化入口会防御性补齐。
    if ( key == "file_format_version" ) return "v14";
    if ( key == "General::AudioFilename" ) {
        // 新字段 song_file_hint 优先，旧 main_audio_path 仅作兼容回退。
        const auto& audioHint = base.song_file_hint.empty()
                                    ? base.main_audio_path
                                    : base.song_file_hint;
        return Config::pathToUtf8(audioHint);
    }
    // General 默认值采用 osu! 可接受的保守设置。
    if ( key == "General::AudioLeadIn" ) return "0";
    // 音频哈希留空，由实际 OSU 导出或发布工具根据文件计算。
    if ( key == "General::AudioHash" ) return "";
    // -1 表示尚未指定预览点，区别于明确选择零毫秒。
    if ( key == "General::PreviewTime" ) return "-1";
    // 普通倒计时是最兼容的非禁用默认模式。
    if ( key == "General::Countdown" ) return "1";
    // Normal 样本组不依赖额外皮肤约定。
    if ( key == "General::SampleSet" ) return "Normal";
    // 堆叠容差采用 osu! 常见默认值，保持外部编辑器预览稳定。
    if ( key == "General::StackLeniency" ) return "0.7";
    // 当前编辑器以轨道谱为主，缺失模式默认 Mania。
    if ( key == "General::Mode" ) return "3";
    // 休息段黑边默认关闭，避免导出后产生意外视觉遮挡。
    if ( key == "General::LetterboxInBreaks" ) return "0";
    // 故事板火花层级沿用 OSU 默认前景行为。
    if ( key == "General::StoryFireInFront" ) return "1";
    // 未明确配置时不要求外部皮肤精灵资源。
    if ( key == "General::UseSkinSprites" ) return "0";
    // 活动区域显示由外部客户端自行控制。
    if ( key == "General::AlwaysShowPlayfield" ) return "0";
    // NoChange 保留外部客户端的默认覆盖层布局。
    if ( key == "General::OverlayPosition" ) return "NoChange";
    // 推荐皮肤未知时留空，不伪造项目资源依赖。
    if ( key == "General::SkinPreference" ) return "";
    // 未分析媒体风险时不自动声明癫痫警告。
    if ( key == "General::EpilepsyWarning" ) return "0";
    // 倒计时偏移默认与首个节拍对齐。
    if ( key == "General::CountdownOffset" ) return "0";
    // 特殊 Mania N+1 布局需要用户明确启用。
    if ( key == "General::SpecialStyle" ) return "0";
    // 故事板宽屏标志默认关闭以保持旧客户端兼容。
    if ( key == "General::WidescreenStoryboard" ) return "0";
    // 音效默认不随播放倍率变调。
    if ( key == "General::SamplesMatchPlaybackRate" ) return "0";

    // Editor 字段只影响外部编辑器体验，不改变 MMM 时间模型。
    // 空书签表示不从 MMM 当前视图伪造外部编辑器标记。
    if ( key == "Editor::Bookmarks" ) return "";
    // 距离间距零值让外部编辑器使用自身默认行为。
    if ( key == "Editor::DistanceSpacing" ) return "0.0";
    // 四分拍细分是常见且可继续修改的初始值。
    if ( key == "Editor::BeatDivisor" ) return "4";
    // 网格大小采用 OSU 编辑器常用默认值。
    if ( key == "Editor::GridSize" ) return "16";
    // 时间轴缩放一表示不施加额外倍率。
    if ( key == "Editor::TimelineZoom" ) return "1";

    // 可共享的文本字段直接映射基础元数据。
    if ( key == "Metadata::Title" ) return base.title;
    if ( key == "Metadata::TitleUnicode" ) return base.title_unicode;
    if ( key == "Metadata::Artist" ) return base.artist;
    if ( key == "Metadata::ArtistUnicode" ) return base.artist_unicode;
    if ( key == "Metadata::Creator" ) {
        // 空作者使用稳定应用标识，避免输出缺失必填字段。
        return base.author.empty() ? "mmm" : base.author;
    }
    if ( key == "Metadata::Version" ) {
        // 空难度名使用带方括号的可辨识默认值。
        return base.version.empty() ? "[mmm]" : base.version;
    }
    if ( key == "Metadata::Source" ) return "";
    // 未提供检索标签时保持空列表而非插入应用名称。
    if ( key == "Metadata::Tags" ) return "";
    // 零谱面 ID 表示尚未由在线服务分配。
    if ( key == "Metadata::BeatmapID" ) return "0";
    // -1 谱面集 ID 表示不属于已发布集合。
    if ( key == "Metadata::BeatmapSetID" ) return "-1";

    // Difficulty 默认值保持在 osu! 常用有效范围。
    if ( key == "Difficulty::HPDrainRate" ) return "5";
    if ( key == "Difficulty::CircleSize" ) {
        // Mania 键数来自基础轨道数，无效值回退四轨。
        return std::to_string(base.track_count > 0 ? base.track_count : 4);
    }
    // 综合难度采用偏高但有效的轨道谱初始值。
    if ( key == "Difficulty::OverallDifficulty" ) return "8";
    // 轨道谱不直接使用接近圈，保留中性合法值。
    if ( key == "Difficulty::ApproachRate" ) return "5";
    // 滑条参数仍需完整输出以满足通用 OSU 文件结构。
    if ( key == "Difficulty::SliderMultiplier" ) return "1.4";
    // 单位 Tick 率作为没有 MMM 对应字段时的保守默认。
    if ( key == "Difficulty::SliderTickRate" ) return "1";

    if ( key == "Events::background" ) {
        // 视频和图片事件使用不同首字段，但共享主封面路径来源。
        if ( base.cover_type == CoverType::VIDEO ) {
            // 视频事件包含起播毫秒和引号包围的资源路径。
            return fmt::format("Video,{},\"{}\"",
                               base.video_starttime,
                               Config::pathToUtf8(base.main_cover_path));
        }
        // 图片事件同时携带背景横纵偏移。
        return fmt::format("0,0,\"{}\",{},{}",
                           Config::pathToUtf8(base.main_cover_path),
                           base.bgxoffset,
                           base.bgyoffset);
    }
    // 没有 MMM 基础休息段映射时不生成虚构事件。
    if ( key == "Events::breaks" ) return "";

    // 自定义键没有通用默认语义，由属性表原值负责提供。
    return "";
}

/// @brief 补齐 OSU 元数据默认字段。
/// @param props 待补齐属性表。
/// @param beatmap 默认值来源谱面。
/// @param fields 预定义字段及说明列表。
/// @note try_emplace 只填缺失项，不覆盖用户已经编辑的值。
/// @post props 至少包含文件版本和 fields 列表中的每一个键。
/// @warning 用户交互路径：可能为全部缺失字段分配字符串节点。
void ensureCompleteOsuMetadata(MetadataPropertyMap&     props,
                               const BeatMap&           beatmap,
                               const MetadataFieldList& fields)
{
    // 文件格式版本不属于任一节，需要单独补齐。
    props.try_emplace(
        "file_format_version",
        getOsuMetadataDefaultValue(beatmap, "file_format_version"));
    for ( const auto& [key, desc] : fields ) {
        // 描述只服务 UI，补齐过程只消费属性键。
        props.try_emplace(key, getOsuMetadataDefaultValue(beatmap, key));
    }
}

/// @brief 将 OSU 元数据同步到基础元数据副本，保证导出使用编辑后的关键字段。
/// @param props OSU 原始元数据属性。
/// @param base 接收同步结果的基础元数据副本。
/// @note 只同步 MMM 基础模型能够表达的字段，其余 OSU 属性保留在格式表。
/// @warning 用户输入提交路径：不访问文件系统，解析失败字段保持原基础值。
/// @details 同步范围包括音频提示、曲名、艺术家、作者、难度名、轨道数及背景
/// 事件。OSU 独有设置仍只存在于格式属性表，不扩张 BaseMapMeta 职责。
void syncOsuMetadataToBase(const MetadataPropertyMap& props, BaseMapMeta& base)
{
    /// @brief 查找属性并返回稳定只读指针。
    /// @param key 完整 OSU 属性键。
    /// @return 键存在时返回其字符串地址，否则返回空。
    /// @note props 在函数期间不修改，返回指针不会因哈希表重排失效。
    auto get = [&props](const std::string& key) -> const std::string* {
        // 单次查找避免 contains 后再次索引。
        auto it = props.find(key);
        if ( it == props.end() ) return nullptr;
        return &it->second;
    };

    if ( const auto* value = get("General::AudioFilename") ) {
        // 外部音频文件名写入新 hint 字段，并清除旧路径避免双重来源。
        base.song_file_hint = Config::utf8ToPath(*value);
        base.main_audio_path.clear();
    }
    // 六个共享文本字段逐项同步，缺失键保留原基础元数据。
    if ( const auto* value = get("Metadata::Title") ) base.title = *value;
    if ( const auto* value = get("Metadata::TitleUnicode") ) {
        base.title_unicode = *value;
    }
    if ( const auto* value = get("Metadata::Artist") ) base.artist = *value;
    if ( const auto* value = get("Metadata::ArtistUnicode") ) {
        base.artist_unicode = *value;
    }
    if ( const auto* value = get("Metadata::Creator") ) base.author = *value;
    if ( const auto* value = get("Metadata::Version") ) base.version = *value;

    if ( const auto* value = get("Difficulty::CircleSize") ) {
        // CircleSize 可能由文本编辑器输入整数或 JSON 数值表示。
        nlohmann::json parsed = parseJsonNoThrow(value->c_str());
        if ( parsed.is_number() ) {
            // 浮点 JSON 数值按目标 int32 约束舍入。
            base.track_count = clampDoubleToInt32(parsed.get<double>());
        } else if ( auto parsedInt = parseInt32Metadata(*value) ) {
            // 非 JSON 数值时使用严格十进制整数解析回退。
            base.track_count = *parsedInt;
        }
    }

    if ( const auto* value = get("Events::background") ) {
        // Events 背景行以逗号分隔；这里解析 MMM 可表达的首个背景事件。
        std::vector<std::string> parts;
        std::istringstream       stream(*value);
        std::string              token;
        while ( std::getline(stream, token, ',') ) {
            // 去除字段周围空白，但保留引号供路径边界识别。
            parts.push_back(trimMetadataLine(token));
        }

        if ( parts.size() >= 3 ) {
            // Video 文本或数字 1 表示视频，其余首字段按图片处理。
            base.cover_type = parts[0] == "Video" || parts[0] == "1"
                                  ? CoverType::VIDEO
                                  : CoverType::IMAGE;
            if ( auto start = parseInt32Metadata(parts[1]) ) {
                // 第二字段在视频事件中是起播时间，非法值保持原设置。
                base.video_starttime = *start;
            }

            // 第三字段是资源路径，允许标准双引号包围。
            std::string path = parts[2];
            if ( path.size() >= 2 && path.front() == '"' &&
                 path.back() == '"' ) {
                // 仅在首尾引号成对时剥离，内部引号不做猜测处理。
                path = path.substr(1, path.size() - 2);
            }
            base.main_cover_path = Config::utf8ToPath(path);

            if ( parts.size() >= 5 ) {
                // 图片事件的第四、第五字段映射背景偏移。
                if ( auto x = parseInt32Metadata(parts[3]) ) {
                    base.bgxoffset = *x;
                }
                if ( auto y = parseInt32Metadata(parts[4]) ) {
                    base.bgyoffset = *y;
                }
            }
        }
    }
}

/// @brief 将 OSU 元数据表序列化为 .osu 风格纯文本。
/// @param props 当前 OSU 属性表。
/// @param beatmap 缺失预定义字段的默认值来源。
/// @return 包含 General、Editor、Metadata、Difficulty 和 Events 的文本。
/// @warning 用户打开文本编辑器时执行，会分配输出流和格式化字符串。
/// @note 预定义字段按稳定规范顺序输出，自定义字段附加在对应节末尾。
/// @details 内部扁平键以 Section::Key 编码，写出时恢复各节本地键。Events
/// 背景和休息段使用行记录语法，不经过普通冒号键值序列化。
/// @details 文件头和每个标准节均无条件输出，因此文本编辑器打开后展示的是
/// 可独立阅读的完整元数据视图。缺失标准键从 BeatMap 推导，已有键保持原值。
/// @details 自定义键只会回到其 Section:: 前缀对应的受支持节；没有受支持
/// 节前缀的内部键不会被误写成普通 OSU 行。
std::string buildOsuMetadataText(const MetadataPropertyMap& props,
                                 const BeatMap&             beatmap)
{
    /// @brief 读取属性值，缺失时回退谱面推导默认值。
    auto get = [&props, &beatmap](const std::string& key) {
        // 显式属性始终优先，空字符串也是有效用户值。
        if ( auto it = props.find(key); it != props.end() ) {
            return it->second;
        }
        return getOsuMetadataDefaultValue(beatmap, key);
    };

    // ostringstream 按目标格式顺序逐段构建完整文本。
    std::ostringstream out;
    std::string        formatVersion = get("file_format_version");
    if ( !startsWith(formatVersion, "v") ) formatVersion = "v" + formatVersion;
    // OSU 文件头固定包含文字前缀和 v 版本标识。
    out << "osu file format " << formatVersion << "\n\n";

    /// @brief 输出指定节中不属于预定义顺序的自定义键。
    /// @param section OSU 节名称。
    /// @param knownKeys 已按规范顺序输出的本地键列表。
    /// @param spaceAfterColon 是否在冒号后插入空格。
    /// @note 自定义键保留属性表遍历顺序，不重复输出预定义字段。
    auto writeExtraKeys = [&props, &out](std::string_view section,
                                         const auto&      knownKeys,
                                         bool             spaceAfterColon) {
        // 属性表内部使用 Section::Key 扁平编码。
        const std::string prefix = fmt::format("{}::", section);
        for ( const auto& [fullKey, value] : props ) {
            // 其他节或文件版本键不属于当前输出段。
            if ( !startsWith(fullKey, prefix) ) continue;

            // 去除节前缀，恢复 OSU 文本中的本地键名。
            std::string_view localKey(fullKey.data() + prefix.size(),
                                      fullKey.size() - prefix.size());
            // 预定义字段已由稳定数组写出，不能在额外键中重复。
            const bool known = std::find_if(knownKeys.begin(),
                                            knownKeys.end(),
                                            [localKey](const char* knownKey) {
                                                return localKey == knownKey;
                                            }) != knownKeys.end();
            if ( known ) continue;

            // 各节沿用 osu! 常见冒号空格风格。
            out << localKey << (spaceAfterColon ? ": " : ":") << value << "\n";
        }
    };

    // General 节包含音频、模式和显示行为字段。
    out << "[General]\n";
    const std::array generalKeys{ "AudioFilename",
                                  "AudioLeadIn",
                                  "AudioHash",
                                  "PreviewTime",
                                  "Countdown",
                                  "SampleSet",
                                  "StackLeniency",
                                  "Mode",
                                  "LetterboxInBreaks",
                                  "StoryFireInFront",
                                  "UseSkinSprites",
                                  "AlwaysShowPlayfield",
                                  "OverlayPosition",
                                  "SkinPreference",
                                  "EpilepsyWarning",
                                  "CountdownOffset",
                                  "SpecialStyle",
                                  "WidescreenStoryboard",
                                  "SamplesMatchPlaybackRate" };
    // 规范字段始终完整输出，缺失值由 get 提供默认值。
    for ( const auto* key : generalKeys ) {
        out << key << ": " << get(fmt::format("General::{}", key)) << "\n";
    }
    // 自定义 General 字段跟随在已知字段之后。
    writeExtraKeys("General", generalKeys, true);

    // Editor 节保存外部编辑器视图和细分偏好。
    out << "\n[Editor]\n";
    const std::array editorKeys{ "Bookmarks",
                                 "DistanceSpacing",
                                 "BeatDivisor",
                                 "GridSize",
                                 "TimelineZoom" };
    for ( const auto* key : editorKeys ) {
        out << key << ": " << get(fmt::format("Editor::{}", key)) << "\n";
    }
    // General 与 Editor 常用冒号后空格格式。
    writeExtraKeys("Editor", editorKeys, true);

    // Metadata 节保存曲目和谱面身份文本。
    out << "\n[Metadata]\n";
    const std::array metadataKeys{ "Title",         "TitleUnicode", "Artist",
                                   "ArtistUnicode", "Creator",      "Version",
                                   "Source",        "Tags",         "BeatmapID",
                                   "BeatmapSetID" };
    for ( const auto* key : metadataKeys ) {
        out << key << ":" << get(fmt::format("Metadata::{}", key)) << "\n";
    }
    // Metadata 规范通常不在冒号后插入额外空格。
    writeExtraKeys("Metadata", metadataKeys, false);

    // Difficulty 节包含键数和难度参数。
    out << "\n[Difficulty]\n";
    const std::array difficultyKeys{ "HPDrainRate",       "CircleSize",
                                     "OverallDifficulty", "ApproachRate",
                                     "SliderMultiplier",  "SliderTickRate" };
    for ( const auto* key : difficultyKeys ) {
        out << key << ":" << get(fmt::format("Difficulty::{}", key)) << "\n";
    }
    // 自定义难度字段同样保留在该节末尾。
    writeExtraKeys("Difficulty", difficultyKeys, false);

    // Events 这里只编辑背景/视频行和休息段，其余事件由谱面模型处理。
    out << "\n[Events]\n";
    out << "//Background and Video events\n";
    out << get("Events::background") << "\n";
    out << "//Break Periods\n";
    // breaks 可含多行，整体按原文本写出。
    const std::string breaks = get("Events::breaks");
    out << breaks;
    if ( !breaks.empty() && breaks.back() != '\n' ) {
        // 保证返回文本以完整行结束，便于再次解析。
        out << "\n";
    }
    // 返回独立字符串，输出流生命周期不泄漏到弹窗状态。
    return out.str();
}

/// @brief 从 .osu 风格纯文本解析 OSU 元数据表。
/// @param text 用户编辑的完整 OSU 元数据文本。
/// @param props 成功时接收新属性表，失败时保持原值。
/// @param errorText 成功时清空，失败时接收带行号的原因。
/// @return 全文满足支持的节与键值语法时返回 true。
/// @warning 用户确认路径：完整扫描文本并分配临时属性表。
/// @note 使用临时 nextProps 保证解析失败不会部分覆盖当前谱面数据。
/// @details 支持五个编辑器管理的节；未知节和注释行忽略。Events 中视频优先
/// 成为背景记录，其余非图片事件合并为休息段原始文本。
/// @details 普通节以首个冒号切分键值，允许值中继续包含冒号；键为空或缺少
/// 分隔符会携带一基行号失败。全文成功前不会替换调用方属性表。
/// @details 解析器有意不接管 TimingPoints、HitObjects 等谱面主体节，避免纯
/// 元数据窗口意外覆盖由 ECS 和专用模型维护的内容。
bool parseOsuMetadataText(std::string_view text, MetadataPropertyMap& props,
                          std::string& errorText)
{
    // 所有解析结果先进入临时容器，成功后一次性交换。
    MetadataPropertyMap      nextProps;
    std::istringstream       stream{ std::string(text) };
    std::string              line;
    std::string              section;
    std::vector<std::string> breakLines;
    // 一基行号用于生成可直接定位的错误提示。
    size_t lineNo = 0;

    // 按行解析并兼容 CRLF 尾部的独立回车字符。
    while ( std::getline(stream, line) ) {
        ++lineNo;
        // getline 只移除换行，Windows 回车需要显式剥离。
        if ( !line.empty() && line.back() == '\r' ) line.pop_back();
        // 节头和键值允许周围空白，实际值也按当前 UI 规则规整。
        std::string trimmed = trimMetadataLine(line);
        // 空行只承担可读排版，不进入属性表。
        if ( trimmed.empty() ) continue;

        if ( startsWith(trimmed, "osu file format") ) {
            // 文件版本行位于所有节之外，保存为特殊扁平键。
            std::string version = trimMetadataLine(
                trimmed.substr(std::string("osu file format").size()));
            if ( version.empty() ) {
                // 缺少版本号会导致无法重建合法文件头。
                errorText =
                    fmt::format("第 {} 行：缺少 osu 文件版本。", lineNo);
                return false;
            }
            // 保留用户是否书写 v 前缀，序列化时会统一补齐。
            nextProps["file_format_version"] = version;
            continue;
        }

        if ( trimmed.front() == '[' && trimmed.back() == ']' ) {
            // 方括号完整包围的非空行切换当前节。
            section = trimmed.substr(1, trimmed.size() - 2);
            continue;
        }

        if ( startsWith(trimmed, "//") || startsWith(trimmed, ";") ) {
            // 两类 OSU 注释均不保存进格式属性表。
            continue;
        }

        if ( section == "Events" ) {
            // Events 不使用 Key:Value，而是逗号分隔的事件记录。
            const bool isVideoEvent =
                startsWith(trimmed, "Video,") || startsWith(trimmed, "1,");
            const bool isImageEvent = startsWith(trimmed, "0,");
            // 同时存在图片与视频事件时，与谱面 Loader 保持视频优先。
            if ( isVideoEvent ||
                 (isImageEvent && !nextProps.contains("Events::background")) ) {
                // 只保留一个背景事件作为基础元数据映射来源。
                nextProps["Events::background"] = trimmed;
            } else if ( !isImageEvent ) {
                // 非背景事件按原行收集为 breaks 复合文本。
                breakLines.push_back(trimmed);
            }
            continue;
        }

        if ( section != "General" && section != "Editor" &&
             section != "Metadata" && section != "Difficulty" ) {
            // 不由该编辑器管理的节忽略，避免误映射到支持字段。
            continue;
        }

        // 只用首个冒号分隔，值中后续冒号保持原样。
        const size_t sep = trimmed.find(':');
        if ( sep == std::string::npos ) {
            errorText = fmt::format("第 {} 行：键值对缺少 ':'。", lineNo);
            // 首个语法错误立即终止，props 仍保持旧值。
            return false;
        }

        // 键和值分别规整边缘空白，内部文本不改变。
        std::string key   = trimMetadataLine(trimmed.substr(0, sep));
        std::string value = trimMetadataLine(trimmed.substr(sep + 1));
        if ( key.empty() ) {
            // 空键无法形成稳定 Section::Key 属性标识。
            errorText = fmt::format("第 {} 行：键名不能为空。", lineNo);
            return false;
        }
        // 扁平键编码与表格编辑器及序列化入口保持一致。
        nextProps[fmt::format("{}::{}", section, key)] = value;
    }

    if ( !breakLines.empty() ) {
        // 休息事件合并为换行分隔文本，保留再次序列化所需行边界。
        std::string breaks;
        for ( const auto& breakLine : breakLines ) {
            if ( !breaks.empty() ) breaks += "\n";
            // 每条事件保持其规整后的原始内容。
            breaks += breakLine;
        }
        // 尾随换行确保序列化追加后续内容时不粘连。
        breaks += "\n";
        nextProps["Events::breaks"] = breaks;
    }

    // 只有走到全文末尾才一次性提交解析结果。
    props     = std::move(nextProps);
    errorText = "";
    return true;
}

/// @brief 打开 OSU 元数据纯文本编辑器。
/// @param props 当前 OSU 属性表。
/// @param beatmap 缺失字段默认值来源。
/// @warning UI 交互路径：只更新弹窗状态，不直接修改谱面数据。
/// @note 打开时重建完整文本，丢弃上一次未提交的缓冲和错误。
/// @post 下一次 renderOsuMetadataTextEditorPopup 会请求打开模态窗口。
void openOsuMetadataTextEditor(const MetadataPropertyMap& props,
                               const BeatMap&             beatmap)
{
    // 标记跨帧状态，并请求下一次弹窗渲染调用 OpenPopup。
    auto& state       = osuMetadataTextEditorState();
    state.open        = true;
    state.requestOpen = true;
    // 新编辑会话不继承旧语法错误。
    state.errorText.clear();
    // 使用稳定序列化顺序预填固定输入缓冲区。
    copyToInputBuffer(state.textBuffer, buildOsuMetadataText(props, beatmap));
}

/// @brief 消费 OSU 纯文本编辑器的写回结果。
/// @return 尚无结果时为空，否则返回一次性属性表。
/// @warning UI 每帧绘制路径：只搬移一次结果对象。
/// @note 取走后立即清空状态，防止同一结果跨帧重复应用。
/// @post 返回非空时，状态中的 result 已复位为空。
std::optional<MetadataPropertyMap> takeOsuMetadataTextResult()
{
    // 全局状态只由当前活动谱面的 OSU 页消费。
    auto& state = osuMetadataTextEditorState();
    if ( !state.result ) return std::nullopt;
    // 移动 optional 避免复制整个属性哈希表。
    auto result = std::move(state.result);
    state.result.reset();
    return result;
}

/// @brief 渲染 OSU 元数据纯文本编辑弹窗。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 每帧绘制路径：仅弹窗打开时绘制固定缓冲区。
/// @note 只有完整解析成功的文本会生成待写回结果。
/// @warning 多行缓冲每帧绘制，但全文解析只在点击完成时执行。
void renderOsuMetadataTextEditorPopup(float dpiScale)
{
    // 静态状态保存多行缓冲和跨帧错误文本。
    auto& state = osuMetadataTextEditorState();
    if ( state.requestOpen ) {
        // 一次性打开请求转换为 ImGui 弹窗栈状态。
        ::MMM::UI::FeedbackOpenPopup(
            "OSU 元数据文本编辑###OsuMetadataTextEditorPopup");
        state.requestOpen = false;
    }

    // 关闭状态不创建模态样式或解析缓冲。
    if ( !state.open ) return;

    // popupOpen 接收标题栏关闭动作，并在弹窗结束后同步回状态。
    bool                           popupOpen = state.open;
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin("OSU 元数据文本编辑###OsuMetadataTextEditorPopup",
                          &popupOpen,
                          ImGuiWindowFlags_None,
                          ImVec2(680.0f * dpiScale, 560.0f * dpiScale),
                          false) ) {
        ImGui::TextUnformatted(
            // 明确该文本编辑器只覆盖五个受支持的元数据节。
            "按 .osu 文件的 General / Editor / Metadata / Difficulty / Events "
            "格式编辑。");
        if ( !state.errorText.empty() ) {
            // 解析错误保留到用户再次成功提交或重新打开。
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "%s",
                               state.errorText.c_str());
        }

        ImGui::SetNextItemWidth(-1.0f);
        // 允许 Tab 输入以便用户整理较长事件或自定义字段。
        ImGui::InputTextMultiline("##OsuMetadataTextBuffer",
                                  state.textBuffer.data(),
                                  state.textBuffer.size(),
                                  ImVec2(0.0f, 430.0f * dpiScale),
                                  ImGuiInputTextFlags_AllowTabInput);

        if ( ::MMM::UI::FeedbackButton("完成##ApplyOsuMetadataText") ) {
            // 先解析到局部表，失败时弹窗和原属性都保持不变。
            MetadataPropertyMap parsedProps;
            if ( parseOsuMetadataText(
                     state.textBuffer.data(), parsedProps, state.errorText) ) {
                // 成功结果交由外层谱面窗口在同一 UI 线程消费。
                state.result = std::move(parsedProps);
                state.open   = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("取消##CancelOsuMetadataText") ) {
            // 取消不生成 result，也不修改调用方属性表。
            state.open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if ( !popupOpen ) {
        // 标题栏关闭与取消按钮共享同一 open 状态语义。
        state.open = false;
    }
}

/// @brief 根据字段名检查常见 JSON 结构缺失。
/// @param key 当前元数据字段名。
/// @param value 已成功解析的 JSON 值。
/// @return 多条警告以换行拼接的文本，无问题时为空。
/// @warning UI 弹窗绘制路径：仅在 JSON 编辑器打开时解析当前缓冲区。
/// @note 警告不阻止写回，只提示已知导出器可能无法完整解释的结构。
/// @details 当前覆盖 Malody mode_ext 的 column、拍位置三元数组以及 Polyline
/// seg 对象数组。未知字段仍允许作为任意合法 JSON 保存。
std::string buildMetadataJsonWarnings(const std::string&    key,
                                      const nlohmann::json& value)
{
    // 先收集独立诊断，再统一拼接，便于一个字段报告多个问题。
    std::vector<std::string> warnings;

    if ( key == "mode_ext" ) {
        // Malody mode_ext 应是带 column 等模式信息的对象。
        if ( !value.is_object() ) {
            warnings.push_back("mode_ext 通常应为对象。");
        } else if ( !value.contains("column") ) {
            // 缺少 column 不一定非法，但可能无法保留原键数。
            warnings.push_back(
                "mode_ext 缺少 column，导出后可能无法保留键数。");
        }
    }

    if ( key == "beat" || key == "endbeat" ) {
        // Malody 拍位置至少需要小节、分子和分母三个分量。
        if ( !value.is_array() || value.size() < 3 ) {
            warnings.push_back("beat/endbeat 应为 [小节, 分子, 分母] 数组。");
        }
    }

    if ( key == "seg" ) {
        // 折线段由对象数组构成，每段至少需要 beat 定位。
        if ( !value.is_array() ) {
            warnings.push_back("seg 应为数组。");
        } else {
            // 逐段报告索引，使用户可直接定位损坏元素。
            for ( size_t i = 0; i < value.size(); ++i ) {
                if ( !value[i].is_object() ) {
                    // 非对象段无法承载 beat 和其他子字段。
                    warnings.push_back(fmt::format("seg[{}] 应为对象。", i));
                    continue;
                }
                if ( !value[i].contains("beat") ) {
                    // 对象存在但缺少定位字段时单独提示。
                    warnings.push_back(fmt::format("seg[{}] 缺少 beat。", i));
                }
            }
        }
    }

    // 使用换行而非标点连接，ImGui 彩色文本可逐条清晰展示。
    std::string result;
    for ( const auto& warning : warnings ) {
        // 首条之前不添加空行。
        if ( !result.empty() ) result += "\n";
        result += warning;
    }
    return result;
}

/// @brief 打开某个元数据字段的 JSON 编辑器。
/// @param scopeId 调用页面稳定作用域标识。
/// @param key 待编辑字段名。
/// @param value 当前字段字符串值。
/// @warning UI 交互路径：只更新弹窗状态，不直接修改谱面数据。
/// @note 合法 JSON 在打开时规整缩进，非法文本原样保留供用户修复。
/// @details 新会话会替换目标定位、清空错误和子字段输入，但不会清除已经等待
/// 其他作用域消费的结果；调用者应在打开前完成本页结果消费。
/// @post 下一次 renderMetadataJsonEditorPopup 会请求打开模态窗口。
void openMetadataJsonEditor(const std::string& scopeId, const std::string& key,
                            const std::string& value)
{
    // 新目标覆盖上一个已关闭编辑会话的全部定位状态。
    auto& state       = metadataJsonEditorState();
    state.open        = true;
    state.requestOpen = true;
    state.scopeId     = scopeId;
    state.key         = key;
    // 空作用域只展示键名，非空作用域显示层级来源。
    state.displayPath =
        scopeId.empty() ? key : fmt::format("{} / {}", scopeId, key);
    // 新会话清除旧校验反馈和子字段临时输入。
    state.errorText.clear();
    state.warningText.clear();
    state.childKeyBuffer.fill('\0');
    state.childValueBuffer.fill('\0');

    // 空字段默认以对象开始，便于直接添加子字段。
    std::string    initialValue = value.empty() ? "{}" : value;
    nlohmann::json parsed       = parseJsonNoThrow(initialValue.c_str());
    if ( parsed.is_discarded() ) {
        // 保留非法原文，用户可以在弹窗中看到并修正。
        copyToInputBuffer(state.jsonBuffer, initialValue);
    } else {
        // 合法值统一四空格缩进，提高嵌套结构可读性。
        copyToInputBuffer(state.jsonBuffer, dumpPrettyJson(parsed));
    }
}

/// @brief 绘制用于打开 JSON 编辑器的字段操作按钮。
/// @param scopeId 当前格式或音符组作用域。
/// @param key 当前字段名。
/// @param value 当前字段文本值。
/// @param idPrefix 防止同页多个按钮 ID 冲突的前缀。
/// @warning UI 每帧绘制路径：只绘制按钮和复制当前字段值。
/// @note 按钮标签使用隐藏 ID 拼接前缀和键名，显示文本固定为 JSON。
/// @pre idPrefix 与 key 的组合在当前 ImGui ID 栈内必须唯一。
void renderMetadataJsonButton(const std::string& scopeId,
                              const std::string& key, const std::string& value,
                              const std::string& idPrefix)
{
    // 只有点击时才复制字段值并解析初始 JSON。
    if ( ::MMM::UI::FeedbackButton(
             fmt::format("JSON##{}_{}", idPrefix, key).c_str()) ) {
        openMetadataJsonEditor(scopeId, key, value);
    }
    if ( ImGui::IsItemHovered() ) {
        // 悬停说明该入口同时支持原生文本和对象子字段操作。
        ImGui::SetTooltip("%s", "打开原生 JSON / 子字段编辑器");
    }
}

/// @brief 绘制新增字段区域的 JSON 编辑按钮。
/// @param scopeId 当前格式或音符组作用域。
/// @param key 新字段键名缓冲。
/// @param value 新字段初始值缓冲。
/// @param idPrefix 当前新增区域的稳定 ImGui ID 前缀。
/// @warning UI 每帧绘制路径：只读取输入缓冲并打开弹窗，不直接修改谱面数据。
/// @note 键名为空时禁用按钮，避免结果无法写回属性表。
/// @pre key 和 value 指向当前帧内有效的 NUL 终止缓冲。
void renderNewMetadataJsonButton(const std::string& scopeId, const char* key,
                                 const char* value, const std::string& idPrefix)
{
    // 同时防御空指针和空 C 字符串。
    const bool hasKey = key != nullptr && key[0] != '\0';
    if ( !hasKey ) {
        ImGui::BeginDisabled();
    }

    if ( ::MMM::UI::FeedbackButton(fmt::format("JSON##{}", idPrefix).c_str()) &&
         hasKey ) {
        // 空初始值按对象处理，便于用户直接建立嵌套结构。
        openMetadataJsonEditor(
            scopeId,
            key,
            value == nullptr || value[0] == '\0' ? "{}" : std::string(value));
    }

    if ( !hasKey ) {
        // 恢复禁用栈，后续同排控件保持正常交互。
        ImGui::EndDisabled();
    }

    if ( ImGui::IsItemHovered() ) {
        // 禁用控件也由调用方布局允许悬停时显示对应原因。
        ImGui::SetTooltip("%s",
                          hasKey ? "用原生 JSON / 子字段编辑器创建该字段"
                                 : "先填写键名，再打开 JSON 编辑器");
    }
}

/// @brief 消费 JSON 编辑器的写回结果。
/// @param scopeId 当前调用页面期望的作用域标识。
/// @return 作用域匹配时返回一次性编辑结果，否则为空。
/// @warning UI 每帧绘制路径：只搬移一次小型结果对象。
/// @note 不匹配结果继续保留，等待真正的调用页面消费。
/// @post 返回非空时全局 result 已清除；返回空时状态保持不变。
std::optional<MetadataJsonEditResult> takeMetadataJsonEditResult(
    const std::string& scopeId)
{
    // 单一全局结果通过 scopeId 路由到谱面或某个音符组。
    auto& state = metadataJsonEditorState();
    if ( !state.result || state.result->scopeId != scopeId ) {
        return std::nullopt;
    }

    // 结果规模较小但复制 optional 保持返回代码直观。
    auto result = state.result;
    state.result.reset();
    return result;
}

/// @brief 渲染元数据字段 JSON 编辑弹窗。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 每帧绘制路径：仅弹窗打开时解析当前缓冲区并绘制 JSON 辅助工具。
/// @note 语法错误阻止完成；结构警告允许用户确认并保留自定义内容。
/// @details 原始文本区支持任意 JSON 类型；对象值额外显示子字段表，并支持
/// 删除、添加和覆盖成员。所有结构操作都会重新生成格式化主缓冲。
/// @details 弹窗每帧无异常解析当前缓冲以提供即时错误。完成时再次解析并把
/// 格式化文本连同作用域路由信息保存为一次性结果。
/// @details 重建为空对象只修改尚未提交的缓冲；取消或标题栏关闭不会向属性表
/// 产生任何结果，因此调用方数据保持不变。
void renderMetadataJsonEditorPopup(float dpiScale)
{
    // 全局状态保存目标字段、固定缓冲和待消费结果。
    auto& state = metadataJsonEditorState();
    if ( state.requestOpen ) {
        // 一次性请求转换为 ImGui 弹窗栈状态。
        ::MMM::UI::FeedbackOpenPopup(
            "字段 JSON 编辑###MetadataJsonEditorPopup");
        state.requestOpen = false;
    }

    // 关闭时不反复解析 32KiB 缓冲。
    if ( !state.open ) return;

    // 标题栏关闭通过 popupOpen 在 EndPopup 后同步。
    bool                           popupOpen = state.open;
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin("字段 JSON 编辑###MetadataJsonEditorPopup",
                          &popupOpen,
                          ImGuiWindowFlags_None,
                          ImVec2(620.0f * dpiScale, 520.0f * dpiScale),
                          false) ) {
        // 完整路径帮助用户确认当前编辑的是哪一层格式字段。
        ImGui::Text("字段: %s", state.displayPath.c_str());
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f),
                           "%s",
                           "可直接编辑合法 JSON，也可以向对象字段添加子字段。");
        ImGui::Separator();

        // 每帧解析当前文本，以即时更新错误和结构警告。
        nlohmann::json parsed = parseJsonNoThrow(state.jsonBuffer.data());
        if ( parsed.is_discarded() ) {
            state.warningText.clear();
            // 非法 JSON 没有可分析结构，先清除旧警告。
            if ( state.errorText.empty() ) {
                // 保留更具体的按钮操作错误，否则使用通用语法提示。
                state.errorText = "当前内容不是合法 JSON。";
            }
        } else {
            // 合法文本清除语法错误，并按字段名刷新结构提示。
            state.errorText.clear();
            state.warningText = buildMetadataJsonWarnings(state.key, parsed);
        }

        if ( !state.errorText.empty() ) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "%s",
                               state.errorText.c_str());
        }
        if ( !state.warningText.empty() ) {
            // 警告用黄色显示但不会禁用完成按钮。
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                               "%s",
                               state.warningText.c_str());
        }

        ImGui::SetNextItemWidth(-1.0f);
        // 允许 Tab 输入方便手工维护嵌套 JSON 缩进。
        ImGui::InputTextMultiline("##MetadataJsonRawEditor",
                                  state.jsonBuffer.data(),
                                  state.jsonBuffer.size(),
                                  ImVec2(0.0f, 220.0f * dpiScale),
                                  ImGuiInputTextFlags_AllowTabInput);

        if ( ::MMM::UI::FeedbackButton("格式化##FormatMetadataJson") ) {
            // 点击时重新解析，避免依赖本帧早先可能已过时的 parsed。
            parsed = parseJsonNoThrow(state.jsonBuffer.data());
            if ( parsed.is_discarded() ) {
                state.errorText = "无法格式化：当前内容不是合法 JSON。";
            } else {
                // 格式化只改变表示，不改变 JSON 值语义。
                copyToInputBuffer(state.jsonBuffer, dumpPrettyJson(parsed));
                state.errorText.clear();
            }
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(
                 "重建为空对象##ResetMetadataJsonObject") ) {
            // 显式重建为对象会丢弃当前文本，但仍需用户点击完成才写回。
            copyToInputBuffer(state.jsonBuffer, "{}");
            state.errorText.clear();
        }

        ImGui::Separator();
        // 子字段区域只对 JSON 对象提供枚举、删除和添加操作。
        ImGui::TextUnformatted("子字段");
        // 原始编辑器可能刚修改缓冲，因此在进入子字段区域前重新解析。
        parsed = parseJsonNoThrow(state.jsonBuffer.data());
        if ( parsed.is_object() ) {
            // 三列表格分别展示键、序列化值和删除操作。
            if ( ImGui::BeginTable("MetadataJsonChildrenTable",
                                   3,
                                   ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_BordersOuter |
                                       ImGuiTableFlags_Resizable,
                                   ImVec2(0.0f, 110.0f * dpiScale)) ) {
                ImGui::TableSetupColumn(
                    "Key", ImGuiTableColumnFlags_WidthFixed, 170.0f * dpiScale);
                ImGui::TableSetupColumn("Value",
                                        ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Action",
                                        ImGuiTableColumnFlags_WidthFixed,
                                        70.0f * dpiScale);
                ImGui::TableHeadersRow();

                // 先复制键列表，删除 parsed 字段时不会使当前遍历器失效。
                std::vector<std::string> childKeys;
                for ( auto it = parsed.begin(); it != parsed.end(); ++it ) {
                    // nlohmann 对象迭代器提供当前成员键名。
                    childKeys.push_back(it.key());
                }

                // 按 JSON 对象自身迭代顺序展示子字段。
                for ( const auto& childKey : childKeys ) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(childKey.c_str());
                    ImGui::TableNextColumn();
                    // 单行 dump 适合表格预览，不改变主缓冲格式。
                    std::string childValue = parsed[childKey].dump();
                    ImGui::TextWrapped("%s", childValue.c_str());
                    ImGui::TableNextColumn();
                    if ( ::MMM::UI::FeedbackButton(
                             fmt::format("删除##JsonChildDel_{}", childKey)
                                 .c_str()) ) {
                        // 删除后立即把新对象规整写回原始编辑缓冲。
                        parsed.erase(childKey);
                        copyToInputBuffer(state.jsonBuffer,
                                          dumpPrettyJson(parsed));
                        // 一帧只处理一次结构变更，避免继续使用旧键列表。
                        break;
                    }
                }
                ImGui::EndTable();
            }
        } else {
            // 数组和标量仍可在原始编辑器中修改，只禁用对象辅助工具。
            ImGui::TextDisabled("%s", "当前 JSON 不是对象，无法列出子字段。");
        }

        // 新子字段输入紧凑排列为键、值和确认按钮。
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("键:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f * dpiScale);
        ImGui::InputText("##MetadataJsonChildKey",
                         state.childKeyBuffer.data(),
                         state.childKeyBuffer.size());
        ImGui::SameLine();
        ImGui::TextUnformatted("值:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(210.0f * dpiScale);
        ImGui::InputText("##MetadataJsonChildValue",
                         state.childValueBuffer.data(),
                         state.childValueBuffer.size());
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("添加/覆盖##MetadataJsonChildAdd") ) {
            // 键名按原文使用，允许格式协议定义含空格或符号的键。
            std::string childKey = state.childKeyBuffer.data();
            if ( childKey.empty() ) {
                // 空键不能提供可辨识字段，保留当前输入等待修正。
                state.errorText = "子字段键名不能为空。";
            } else {
                // 当前值不是对象时从空对象开始，操作语义与按钮名称一致。
                parsed = parseJsonNoThrow(state.jsonBuffer.data());
                if ( parsed.is_discarded() || !parsed.is_object() ) {
                    parsed = nlohmann::json::object();
                }
                // 值优先保留 JSON 类型，普通文本自动转换为字符串。
                parsed[childKey] =
                    parseChildValueOrString(state.childValueBuffer.data());
                copyToInputBuffer(state.jsonBuffer, dumpPrettyJson(parsed));
                // 成功后清空两项子字段输入，支持连续添加。
                state.childKeyBuffer.fill('\0');
                state.childValueBuffer.fill('\0');
                state.errorText.clear();
            }
        }

        ImGui::Separator();
        // 完成与取消是唯一关闭并结束当前编辑会话的按钮。
        if ( ::MMM::UI::FeedbackButton("完成##ApplyMetadataJson") ) {
            // 最终提交前再次解析，确保最后一次文本输入已经纳入校验。
            parsed = parseJsonNoThrow(state.jsonBuffer.data());
            if ( parsed.is_discarded() ) {
                state.errorText = "无法完成：当前内容不是合法 JSON。";
            } else {
                // 结构警告保留用于最终展示，但不阻止合法 JSON 写回。
                state.warningText =
                    buildMetadataJsonWarnings(state.key, parsed);
                // 结果携带打开时的作用域和键，调用方下一帧按范围消费。
                state.result = MetadataJsonEditResult{ state.scopeId,
                                                       state.key,
                                                       dumpPrettyJson(parsed) };
                // 关闭弹窗但保留 result，直到正确页面取走。
                state.open = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("取消##CancelMetadataJson") ) {
            // 取消不生成结果，调用方属性表保持原值。
            state.open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if ( !popupOpen ) {
        // 标题栏关闭和取消按钮统一落到 open=false。
        state.open = false;
    }
}
}  // namespace

/// @brief 渲染谱面元数据编辑窗口。
/// @param showWindow 窗口可见状态，标题栏关闭时写回 false。
/// @warning UI 热路径：窗口可见时每帧执行，并持有会话递归互斥锁。
/// @details 窗口按 OSU、Malody 和 RM 三种格式展示扩展元数据。文本或表格
/// 修改通过逻辑命令标脏；共享基础字段另以 CmdUpdateBeatmapMetadata 更新。
/// @note 文件系统保存由逻辑线程尾随处理，本函数不直接写谱面文件。
/// @details 会话锁覆盖当前谱面共享指针和所有属性表引用，防止切换会话时
/// 访问失效对象。离线协作会话只展示统一只读提示。
/// @details OSU 页维持完整预定义表并提供全文编辑；Malody 页允许 JSON 子
/// 字段操作；RM 页把二进制整数属性限制为可解析文本。
/// @details 两个辅助编辑弹窗使用静态跨帧状态，只在父窗口 Begin 成功时绘制，
/// 并通过一次性结果回到当前活动谱面的对应格式页。
/// @pre showWindow 为 true 时调用；函数仍防御 false 并立即返回。
/// @post 六项 ImGui 样式变量与可选标题字体均恢复到进入函数前状态。
/// @warning 字段栈缓冲不得在对应 ImGui 调用返回后被任何对象保存。
void renderMetadataEditorWindow(bool& showWindow)
{
    // 隐藏时立即返回，不推入样式或访问会话。
    if ( !showWindow ) return;

    // 外观参数按当前内容缩放转换为像素值。
    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    // 取整圆角避免 DPI 缩放后的亚像素边缘模糊。
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    // 横纵间距沿用同一用户设置并同步缩放。
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    // 六项样式在函数末尾统一弹出，覆盖窗口及其子控件。
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    // 仅首次出现时设置建议尺寸，之后尊重用户调整。
    ImGui::SetNextWindowSize(ImVec2(800.0f * dpiScale, 550.0f * dpiScale),
                             ImGuiCond_FirstUseEver);

    // 标题栏优先使用皮肤 title 字体，缺失时回退当前 ImGui 字体。
    auto&   skinMgr   = Config::SkinManager::instance();
    ImFont* titleFont = skinMgr.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

    // 保存 Begin 前可见性，用统一反馈入口检测标题栏关闭点击。
    const bool wasOpenBeforeBegin = showWindow;
    bool opened = ImGui::Begin("谱面额外元数据编辑###MetadataEditorWindow",
                               &showWindow,
                               ImGuiWindowFlags_None);
    FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, &showWindow);

    // 字体只包围 Begin 标题创建，不影响窗口正文控件。
    if ( titleFont ) ImGui::PopFont();

    if ( opened ) {
        // 读取会话和直接访问谱面扩展属性期间持有统一递归锁。
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        // shared_ptr 局部副本保证本帧窗口绘制期间会话存活。
        auto session = engine.getActiveSession();
        if ( !session ) {
            // 无会话只显示状态，不创建可编辑属性引用。
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "当前无活动的编辑器会话。");
        } else if ( session->isCollaborationOfflineReadOnly() ) {
            // 离线协作副本统一进入只读门闩，不渲染编辑控件。
            renderCollaborationOfflineReadOnlyNotice(*session);
        } else {
            // 当前谱面由会话持有；局部共享所有权覆盖本帧引用。
            auto beatmap = session->getContext().currentBeatmap;
            if ( !beatmap ) {
                // 会话存在但尚未载入谱面时给出明确前置条件。
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "当前未加载任何谱面，请先打开或新建谱面。");
            } else {
                // 标题行标识当前实际编辑的谱面名称和难度版本。
                ImGui::Text("正在编辑谱面: %s (%s)",
                            beatmap->m_baseMapMetadata.name.c_str(),
                            beatmap->m_baseMapMetadata.version.c_str());
                // 灰色提示说明扩展字段随正常保存流程持久化。
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                   "提示：在此处修改的额外字段会在保存谱面时一"
                                   "同导出。键入即可开始编辑。");
                ImGui::Separator();
                ImGui::Spacing();

                // 三种格式共享同一 TabBar，避免同时绘制大量字段表。
                if ( ImGui::BeginTabBar("MetadataTypeTabBar") ) {
                    // OSU 页签编辑扁平 Section::Key 属性并同步共享基础字段。
                    if ( ImGui::BeginTabItem("osu! (OSU) 格式元数据") ) {
                        // 预定义列表同时控制表格顺序、说明和全文默认补齐范围。
                        // file_format_version 使用独立键，因为它位于 OSU
                        // 节头之外。 General::AudioFilename
                        // 映射基础音频提示路径，不直接导入资源。
                        // General::PreviewTime
                        // 使用毫秒，可通过当前判定线快捷填入。 General::Mode
                        // 默认 Mania，但仍允许用户保存其他模式声明。 General
                        // 其余开关按原始字符串保存，交给 OSU 导出器解释。
                        // Editor 字段描述外部编辑器状态，不反向改变 MMM
                        // 当前视图。 Metadata 标题、艺术家、作者和版本会同步到
                        // BaseMapMeta 副本。 Metadata 来源、标签和官网 ID
                        // 只保留在 OSU 格式属性表。 Difficulty::CircleSize
                        // 会同步基础轨道数并执行 int32 有界转换。 Difficulty
                        // 其他数值保持文本，避免丢失外部格式精度表示。
                        // Events::background
                        // 映射图片或视频路径、起播时间和偏移。 Events::breaks
                        // 保留多行事件文本，不拆分进 MMM 基础模型。
                        // 表格始终展示完整列表，即使属性表尚未显式包含某个键。
                        // 缺失字段使用推导默认值补齐，已有空字符串不会被覆盖。
                        // 自定义 Section::Key 仍会由纯文本编辑器写回对应节。
                        // 表格单字段缓冲上限为 1023
                        // 字节，长复合内容应使用全文入口。
                        // 文本编辑器解析成功后整表替换，语法失败则保持当前表不变。
                        // 第一个自定义键会激活完整默认表，保证随后文本可独立导出。
                        // 预定义键删除后会在同帧以默认值补回，不承担禁用字段语义。
                        // 用户若需要空字段，应保留键并把值编辑为空字符串。
                        // PreviewTime
                        // 的“当前”按钮允许缺失键直接创建当前毫秒值。
                        // 其他缺失预定义键只显示占位，默认补齐后才出现复位按钮。
                        // 自定义键没有字段级 JSON 假设，按普通 OSU 文本保存。
                        // 共享基础字段更新通过命令队列，避免直接修改会话核心元数据。
                        // 格式属性表本身位于当前谱面对象内，并由标脏流程一同保存。
                        static const MetadataFieldList OSU_FIELDS = {
                            { "file_format_version",
                              "osu! 文件格式版本 - 例如 v14" },
                            { "General::AudioFilename",
                              "音频文件名 - 谱面所使用的音频文件路径" },
                            { "General::AudioLeadIn",
                              "音频前导时间 (ms) - "
                              "谱面开始播放前的缓冲时间" },
                            { "General::AudioHash",
                              "音频哈希值 - 音频文件的 MD5" },
                            { "General::PreviewTime",
                              "预览开始时间 (ms) - "
                              "选歌界面预览音频的起点" },
                            { "General::Countdown",
                              "倒计时样式 - 0=无, 1=普通, 2=快速, 3=极速" },
                            { "General::SampleSet",
                              "默认音效样本组 - Normal, Soft, Drum" },
                            { "General::StackLeniency",
                              "堆叠容差 - 影响连打/重叠物件的错开位移" },
                            { "General::Mode",
                              "游戏模式 - 0=osu!, 1=Taiko, 2=Catch, "
                              "3=Mania" },
                            { "General::LetterboxInBreaks",
                              "休息段显示黑边 - 0/1" },
                            { "General::StoryFireInFront",
                              "故事板火花在前 - 0/1" },
                            { "General::UseSkinSprites", "使用皮肤精灵 - 0/1" },
                            { "General::AlwaysShowPlayfield",
                              "总是显示活动区域 - 0/1" },
                            { "General::OverlayPosition",
                              "界面覆盖层位置 - NoChange, Below, Above" },
                            { "General::SkinPreference", "推荐皮肤名称" },
                            { "General::EpilepsyWarning", "癫痫警告 - 0/1" },
                            { "General::CountdownOffset",
                              "倒计时偏移时间 (ms)" },
                            { "General::SpecialStyle",
                              "特殊样式 - 0/1, mania 中用于 N+1 键位布局" },
                            { "General::WidescreenStoryboard",
                              "宽屏故事板 - 0/1" },
                            { "General::SamplesMatchPlaybackRate",
                              "音效速率跟随播放速度 - 0/1" },

                            { "Editor::Bookmarks",
                              "书签 - 逗号分隔的毫秒整型数组" },
                            { "Editor::DistanceSpacing", "距离间距系数" },
                            { "Editor::BeatDivisor",
                              "节拍细分数 - 例如 4, 8, 12, 16" },
                            { "Editor::GridSize", "网格大小" },
                            { "Editor::TimelineZoom", "时间轴缩放倍率" },

                            { "Metadata::Title", "歌曲标题 (对应 base 标题)" },
                            { "Metadata::TitleUnicode",
                              "歌曲标题 (原语/Unicode)" },
                            { "Metadata::Artist", "艺术家 (对应 base 艺术家)" },
                            { "Metadata::ArtistUnicode",
                              "艺术家 (原语/Unicode)" },
                            { "Metadata::Creator", "谱面创作者" },
                            { "Metadata::Version", "难度版本名" },
                            { "Metadata::Source", "歌曲来源 - 如动漫/游戏名" },
                            { "Metadata::Tags", "检索标签 - 空格分隔" },
                            { "Metadata::BeatmapID", "谱面唯一 ID (官网分配)" },
                            { "Metadata::BeatmapSetID",
                              "谱面集唯一 ID (官网分配)" },

                            { "Difficulty::HPDrainRate", "HP 减少速率 (0-10)" },
                            { "Difficulty::CircleSize", "键数 / 轨道数" },
                            { "Difficulty::OverallDifficulty",
                              "综合难度 / 判定严准度 (0-10)" },
                            { "Difficulty::ApproachRate",
                              "缩圈速度 / 下落速度 (0-10)" },
                            { "Difficulty::SliderMultiplier", "滑条速度倍率" },
                            { "Difficulty::SliderTickRate",
                              "滑条 Tick 生成率" },

                            { "Events::background",
                              "背景图片设置串 - 格式: 0,0,\"文件名\",x,y" },
                            { "Events::breaks", "休息时间段定义串" }
                        };

                        // operator[] 为当前谱面建立 OSU 属性表入口。
                        auto& props = beatmap->m_metadata
                                          .map_properties[MapMetadataType::OSU];
                        // 本帧变更标志集中触发基础元数据同步和逻辑命令。
                        bool osuPropsChanged = false;
                        if ( auto result = takeOsuMetadataTextResult() ) {
                            // 全文结果一次性替换表格属性，再补齐标准字段。
                            props = std::move(*result);
                            ensureCompleteOsuMetadata(
                                props, *beatmap, OSU_FIELDS);
                            // 标记后在页签尾部同步可共享基础字段。
                            osuPropsChanged = true;
                        }
                        if ( !props.empty() ) {
                            // 一旦用户开始使用 OSU
                            // 元数据，维持完整标准字段集合。
                            ensureCompleteOsuMetadata(
                                props, *beatmap, OSU_FIELDS);
                        }

                        if ( props.empty() ) {
                            // 空属性表没有可序列化全文，文本编辑入口暂时禁用。
                            ImGui::BeginDisabled();
                        }
                        if ( ::MMM::UI::FeedbackButton(
                                 "文本编辑##open_osu_text_editor") &&
                             !props.empty() ) {
                            // 点击时从当前表格状态生成完整 OSU 文本。
                            openOsuMetadataTextEditor(props, *beatmap);
                        }
                        if ( props.empty() ) {
                            // 恢复禁用栈，后续表格和新增字段保持可交互。
                            ImGui::EndDisabled();
                        }
                        if ( ImGui::IsItemHovered() ) {
                            // 根据空表状态解释启用文本编辑器的前置条件。
                            ImGui::SetTooltip(
                                "%s",
                                props.empty()
                                    ? "先添加至少一个 OSU "
                                      "元数据字段，随后会补齐"
                                      "完整默认表。"
                                    : "打开 .osu 风格纯文本元数据编辑器");
                        }

                        // 表格允许纵向滚动、交替行底色、外边框和列宽调整。
                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;

                        // 为底部新增字段表单预留固定高度。
                        float footerHeight = 45.0f * dpiScale;
                        if ( Utils::VerticalScrollbarStyleScope scrollbarStyle(
                                 dpiScale);
                             ImGui::BeginTable("OSUMetadataTable",
                                               4,
                                               tableFlags,
                                               ImVec2(0.0f, -footerHeight)) ) {
                            // 键列固定宽度，确保长 Section::Key 不挤压值输入。
                            ImGui::TableSetupColumn(
                                "键名 (Key)",
                                ImGuiTableColumnFlags_WidthFixed,
                                220.0f * dpiScale);
                            // 说明列使用伸展策略吸收窗口宽度变化。
                            ImGui::TableSetupColumn(
                                "描述 (Description)",
                                ImGuiTableColumnFlags_WidthStretch);
                            // 值列固定逻辑宽度，提供一致的文本编辑空间。
                            ImGui::TableSetupColumn(
                                "数值 (Value)",
                                ImGuiTableColumnFlags_WidthFixed,
                                250.0f * dpiScale);
                            // 操作列容纳当前时间或默认按钮。
                            ImGui::TableSetupColumn(
                                "操作 (Action)",
                                ImGuiTableColumnFlags_WidthFixed,
                                130.0f * dpiScale);
                            // 表头由列定义自动生成并固定在滚动区域顶部。
                            ImGui::TableHeadersRow();

                            // 第一阶段按规范列表稳定顺序渲染所有预定义字段。
                            for ( const auto& [key, desc] : OSU_FIELDS ) {
                                // 每个字段占一行：键、说明、值和快捷操作。
                                ImGui::TableNextRow();
                                // 进入说明列并对齐到控件基线。
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();

                                // 缺失预定义键使用灰色文字提示尚未显式存在。
                                bool hasKey = props.contains(key);
                                if ( !hasKey ) {
                                    ImGui::PushStyleColor(
                                        ImGuiCol_Text,
                                        ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                }
                                ImGui::TextUnformatted(key.c_str());
                                if ( !hasKey ) {
                                    ImGui::PopStyleColor();
                                }
                                if ( ImGui::IsItemHovered() ) {
                                    // 双击键名复制，方便用户在外部资料中查找。
                                    ImGui::SetTooltip(
                                        "双击可以复制该内置键名到剪贴板。");
                                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                                        // 剪贴板写入只发生在明确双击交互。
                                        ImGui::SetClipboardText(key.c_str());
                                    }
                                }

                                // 值列使用整行可用宽度的文本输入框。
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                // 字段说明以较弱颜色显示，突出可编辑值。
                                ImGui::PushStyleColor(
                                    ImGuiCol_Text,
                                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                ImGui::TextWrapped("%s", desc.c_str());
                                ImGui::PopStyleColor();

                                // 操作列按字段能力绘制快捷按钮或占位符。
                                ImGui::TableNextColumn();
                                // 固定栈缓冲承接当前字符串并供 InputText 修改。
                                char valBuf[1024] = { 0 };
                                if ( hasKey ) {
                                    // 超长属性按控件容量截断显示，保持 NUL
                                    // 终止。
                                    std::string currentVal = props.at(key);
                                    size_t      copyLen    = std::min(
                                        currentVal.size(), sizeof(valBuf) - 1);
                                    // 目标已零初始化，无需显式写入结尾终止符。
                                    std::copy(currentVal.begin(),
                                              currentVal.begin() + copyLen,
                                              valBuf);
                                }

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_osu_") + key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    // 文本变化立即写回属性表，保存由页签尾部统一请求。
                                    props[key]      = valBuf;
                                    osuPropsChanged = true;
                                }

                                ImGui::TableNextColumn();
                                // PreviewTime
                                // 额外提供从当前判定线读取的快捷操作。
                                const bool isPreviewTimeKey =
                                    key == "General::PreviewTime";
                                if ( isPreviewTimeKey ) {
                                    if ( ::MMM::UI::FeedbackButton(
                                             (std::string(
                                                  "当前##current_osu_") +
                                              key)
                                                 .c_str()) ) {
                                        // 快照时间转换为非负 int32 毫秒文本。
                                        props[key] =
                                            readCurrentJudgelinePreviewMsText();
                                        osuPropsChanged = true;
                                    }
                                    if ( ImGui::IsItemHovered() ) {
                                        // 提示说明读取来源和目标单位。
                                        ImGui::SetTooltip(
                                            "%s",
                                            "读取当前判定线时间并写入毫秒值。");
                                    }
                                    if ( hasKey ) {
                                        // 当前时间和默认按钮在已有字段时同行排列。
                                        ImGui::SameLine();
                                    }
                                }
                                if ( hasKey ) {
                                    // 默认按钮按当前基础谱面重新推导单字段值。
                                    if ( ::MMM::UI::FeedbackButton(
                                             (std::string("默认##reset_osu_") +
                                              key)
                                                 .c_str()) ) {
                                        props[key] = getOsuMetadataDefaultValue(
                                            *beatmap, key);
                                        // 复位同样属于可持久化属性变化。
                                        osuPropsChanged = true;
                                    }
                                } else if ( !isPreviewTimeKey ) {
                                    // 缺失且无快捷动作的字段用占位符保持表格结构。
                                    ImGui::TextDisabled("-");
                                }
                            }

                            // 第二阶段收集不在预定义列表中的自定义字段。
                            std::vector<std::string> customKeys;
                            for ( const auto& [k, v] : props ) {
                                // 与静态字段列表逐项比较，属性表规模通常很小。
                                bool isPredefined = false;
                                for ( const auto& [pk, pd] : OSU_FIELDS ) {
                                    if ( pk == k ) {
                                        // 首次命中即可停止内层扫描。
                                        isPredefined = true;
                                        break;
                                    }
                                }
                                if ( !isPredefined ) {
                                    // 先复制键，删除属性时不会使 map
                                    // 遍历器失效。
                                    customKeys.push_back(k);
                                }
                            }

                            // 自定义字段按属性表当前遍历顺序展示。
                            for ( const auto& key : customKeys ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(key.c_str());
                                if ( ImGui::IsItemHovered() ) {
                                    // 提醒用户该键没有内置默认值或字段说明。
                                    ImGui::SetTooltip("这是一个自定义键。");
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextDisabled("自定义键值");

                                ImGui::TableNextColumn();
                                // 栈缓冲将现有字符串适配为 InputText 可写存储。
                                char        valBuf[1024] = { 0 };
                                std::string currentVal   = props.at(key);
                                size_t copyLen = std::min(currentVal.size(),
                                                          sizeof(valBuf) - 1);
                                // 缓冲已清零，截断复制后仍保证 NUL 终止。
                                std::copy(currentVal.begin(),
                                          currentVal.begin() + copyLen,
                                          valBuf);

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_osu_custom_") +
                                          key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    // 输入变化写回当前键并合并到本帧变更标志。
                                    props[key]      = valBuf;
                                    osuPropsChanged = true;
                                }

                                ImGui::TableNextColumn();
                                if ( ::MMM::UI::FeedbackButton(
                                         (std::string("删除##del_osu_") + key)
                                             .c_str()) ) {
                                    // customKeys 是副本，删除不会破坏当前循环。
                                    props.erase(key);
                                    osuPropsChanged = true;
                                }
                            }

                            // 与 BeginTable 成对结束 OSU 滚动表格。
                            ImGui::EndTable();
                        }

                        // 表格下方提供自定义键和值的单行新增表单。
                        ImGui::Separator();
                        // 标签与同排输入框按框体基线对齐。
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("新增自定义键名:");
                        ImGui::SameLine();
                        // 静态缓冲跨帧保留尚未提交的键名。
                        static char newOsuKey[128] = "";
                        // 新键限制为 128 字节缓存，显示宽度按 DPI 缩放。
                        ImGui::SetNextItemWidth(200.0f * dpiScale);
                        ImGui::InputText(
                            "##new_osu_key", newOsuKey, sizeof(newOsuKey));

                        ImGui::SameLine();
                        ImGui::Text("数值:");
                        ImGui::SameLine();
                        // 值缓冲独立保存，避免键输入变化覆盖其内容。
                        static char newOsuVal[256] = "";
                        // 新值提供稍宽输入区域，容量仍由固定数组约束。
                        ImGui::SetNextItemWidth(250.0f * dpiScale);
                        ImGui::InputText(
                            "##new_osu_val", newOsuVal, sizeof(newOsuVal));

                        ImGui::SameLine();
                        if ( ::MMM::UI::FeedbackButton(
                                 "添加##add_osu_field") ) {
                            // 空键不创建属性，输入继续保留等待修正。
                            std::string nk = newOsuKey;
                            if ( !nk.empty() ) {
                                // 同名键按新增/覆盖语义更新，随后维持完整默认表。
                                props[nk] = newOsuVal;
                                ensureCompleteOsuMetadata(
                                    props, *beatmap, OSU_FIELDS);
                                osuPropsChanged = true;
                                // 成功添加后清空两项缓冲，方便连续录入。
                                newOsuKey[0] = '\0';
                                newOsuVal[0] = '\0';
                            }
                        }

                        if ( osuPropsChanged ) {
                            // 所有 OSU 编辑路径在这里统一同步共享基础字段。
                            if ( !props.empty() ) {
                                // 删除预定义字段后也立即恢复完整格式表。
                                ensureCompleteOsuMetadata(
                                    props, *beatmap, OSU_FIELDS);
                                // 在副本上同步，避免 UI
                                // 线程直接替换基础元数据。
                                BaseMapMeta updatedBase =
                                    beatmap->m_baseMapMetadata;
                                syncOsuMetadataToBase(props, updatedBase);
                                // 逻辑命令负责应用、标脏和后续自动保存。
                                engine.pushCommand(
                                    Logic::CmdUpdateBeatmapMetadata{
                                        std::move(updatedBase) });
                            }
                        }

                        ImGui::EndTabItem();
                    }

                    // Malody 页签支持普通字符串和 JSON 复合字段共同编辑。
                    if ( ImGui::BeginTabItem("Malody (MALODY) 格式元数据") ) {
                        // 固定列表提供常见键的稳定顺序和格式语义说明。
                        // id 与 $ver 按 Malody
                        // 原始文本语义保存，不做数值强制转换。 preview
                        // 使用毫秒文本并提供当前判定线快捷写入。 mode
                        // 只在可解析为 0 或 7 时驱动 free 派生字段。 free
                        // 保留兼容导出语义，用户编辑 mode 后会自动纠正。 aimode
                        // 允许简单文本或 JSON 结构，由导出端最终解释。 mode_ext
                        // 常为对象，JSON 编辑器会提示缺少 column。 initialDelay
                        // 和 audioOffset 属于内部兼容状态，不在表中展示。
                        // 未知自定义键仍完整展示并允许 JSON 辅助编辑。
                        // 所有值继续存储为字符串，避免改变既有序列化协议。
                        // 普通文本输入不要求合法 JSON，兼容未知自定义格式字段。
                        // JSON 按钮只在弹窗完成后覆盖目标键，取消不会改变属性。
                        // 新字段 JSON 入口要求先填写键名，空值默认建立对象。
                        // 清除 mode 后不会推导 free；现有 free
                        // 按未知模式语义保留。 编辑 mode 为 0 或 7 会强制对应
                        // free 值并合并一次保存请求。 mode_ext、aimode
                        // 等复合值可用对象子字段工具减少语法错误。 preview
                        // 快捷值来自活动画布读快照，不等待逻辑线程同步。
                        // 内部延迟字段隐藏但仍留在属性表，不会因页面渲染被删除。
                        // 自定义键删除使用键副本迭代，哈希表失效不会影响本帧循环。
                        // 页面所有变化仅标记谱面脏，实际文件写入由逻辑层尾随执行。
                        static const std::vector<
                            std::pair<std::string, std::string>>
                            MALODY_FIELDS = {
                                { "id", "谱面 ID" },
                                { "preview",
                                  "音频预览时间戳 (ms) - 选歌界面试听起点" },
                                { "mode",
                                  "游戏模式 - 0=Key 模式，7=Slide 模式" },
                                { "free",
                                  "自由模式标记 - mode=7 时为 1，mode=0 时为 "
                                  "0" },
                                { "$ver", "文件格式版本" },
                                { "aimode", "AI 辅助模式配置" },
                                { "mode_ext", "模式额外扩展配置 (JSON 串)" },
                                { "extra", "额外顶层扩展配置 (JSON 串)" }
                            };

                        // operator[] 建立当前谱面的 Malody 属性表入口。
                        auto& props =
                            beatmap->m_metadata
                                .map_properties[MapMetadataType::MALODY];
                        // 作用域标识将全局 JSON 弹窗结果路由回谱面页面。
                        const std::string metadataScopeId = "map_malody";
                        // 本帧所有编辑路径共用一次尾随自动保存请求。
                        bool malodyPropsChanged = false;
                        if ( auto result =
                                 takeMetadataJsonEditResult(metadataScopeId) ) {
                            // JSON 结果以格式化文本写回既有字符串属性协议。
                            props[result->key] = result->value;
                            malodyPropsChanged = true;
                        }
                        if ( syncMalodyFreeFromMode(props) ) {
                            // 打开页签时也修复 mode 与派生 free 的不一致。
                            malodyPropsChanged = true;
                        }

                        // 表格支持滚动、交替底色、外边框和调整列宽。
                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;

                        // 负高度为底部新增字段表单预留空间。
                        float footerHeight = 45.0f * dpiScale;
                        if ( Utils::VerticalScrollbarStyleScope scrollbarStyle(
                                 dpiScale);
                             ImGui::BeginTable("MalodyMetadataTable",
                                               4,
                                               tableFlags,
                                               ImVec2(0.0f, -footerHeight)) ) {
                            // 键列固定宽度以容纳 mode_ext 等格式键。
                            ImGui::TableSetupColumn(
                                "键名 (Key)",
                                ImGuiTableColumnFlags_WidthFixed,
                                220.0f * dpiScale);
                            // 说明列伸展并允许长中文说明自动换行。
                            ImGui::TableSetupColumn(
                                "描述 (Description)",
                                ImGuiTableColumnFlags_WidthStretch);
                            // 值列固定宽度，普通字符串和 JSON 原文共享入口。
                            ImGui::TableSetupColumn(
                                "数值 (Value)",
                                ImGuiTableColumnFlags_WidthFixed,
                                250.0f * dpiScale);
                            // 操作列需同时容纳 JSON、当前时间与清除按钮。
                            ImGui::TableSetupColumn(
                                "操作 (Action)",
                                ImGuiTableColumnFlags_WidthFixed,
                                190.0f * dpiScale);
                            // 生成表头后开始逐字段行绘制。
                            ImGui::TableHeadersRow();

                            // 第一阶段按固定列表渲染预定义字段。
                            for ( const auto& [key, desc] : MALODY_FIELDS ) {
                                // 每行依次绘制键名、说明、文本值和操作区。
                                ImGui::TableNextRow();
                                // 说明列按控件基线对齐并使用弱文本颜色。
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();

                                // 缺失键使用灰色提示，但仍允许通过 JSON 创建。
                                bool hasKey = props.contains(key);
                                if ( !hasKey ) {
                                    ImGui::PushStyleColor(
                                        ImGuiCol_Text,
                                        ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                }
                                ImGui::TextUnformatted(key.c_str());
                                if ( !hasKey ) {
                                    ImGui::PopStyleColor();
                                }
                                if ( ImGui::IsItemHovered() ) {
                                    // 双击复制内置键名，方便查阅格式资料。
                                    ImGui::SetTooltip(
                                        "双击可以复制该内置键名到剪贴板。");
                                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                                        // 剪贴板写入只发生于明确双击。
                                        ImGui::SetClipboardText(key.c_str());
                                    }
                                }

                                // 值列把属性字符串复制到固定编辑缓冲。
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                // 说明列使用弱文本色，突出编辑值和操作。
                                ImGui::PushStyleColor(
                                    ImGuiCol_Text,
                                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                ImGui::TextWrapped("%s", desc.c_str());
                                ImGui::PopStyleColor();

                                // 操作列始终提供 JSON
                                // 入口，再按字段追加快捷操作。
                                ImGui::TableNextColumn();
                                // 栈缓冲把字符串属性适配到 ImGui InputText。
                                char valBuf[1024] = { 0 };
                                if ( hasKey ) {
                                    // 超长值按控件容量截断，缓冲仍保持 NUL
                                    // 终止。
                                    std::string currentVal = props.at(key);
                                    size_t      copyLen    = std::min(
                                        currentVal.size(), sizeof(valBuf) - 1);
                                    std::copy(currentVal.begin(),
                                              currentVal.begin() + copyLen,
                                              valBuf);
                                }

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_mld_") + key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    // 文本变化立即写回当前属性表。
                                    props[key]         = valBuf;
                                    malodyPropsChanged = true;
                                    if ( syncMalodyFreeFromMode(props) ) {
                                        // mode 编辑可能同时更新 free 派生字段。
                                        malodyPropsChanged = true;
                                    }
                                }

                                ImGui::TableNextColumn();
                                // 缺失字段传空值，JSON 编辑器会以空对象开始。
                                const std::string fieldValue =
                                    hasKey ? props.at(key) : std::string();
                                renderMetadataJsonButton(metadataScopeId,
                                                         key,
                                                         fieldValue,
                                                         "map_mld_builtin");
                                if ( key == "preview" ) {
                                    // preview
                                    // 提供读取当前判定线毫秒位置的快捷操作。
                                    ImGui::SameLine();
                                    if ( ::MMM::UI::FeedbackButton(
                                             "当前##current_mld_preview") ) {
                                        props[key] =
                                            readCurrentJudgelinePreviewMsText();
                                        // 快捷写入合并到本帧保存请求。
                                        malodyPropsChanged = true;
                                    }
                                    if ( ImGui::IsItemHovered() ) {
                                        // 提示说明读取来源和目标单位。
                                        ImGui::SetTooltip(
                                            "%s",
                                            "读取当前判定线时间并写入毫秒值。");
                                    }
                                }
                                if ( hasKey ) {
                                    // 已有键提供清除按钮，缺失键仅保留创建入口。
                                    ImGui::SameLine();
                                    if ( ::MMM::UI::FeedbackButton(
                                             (std::string("清除##clear_mld_") +
                                              key)
                                                 .c_str()) ) {
                                        // 删除后同步派生 free，避免 mode
                                        // 清除留下冲突。
                                        props.erase(key);
                                        malodyPropsChanged = true;
                                        if ( syncMalodyFreeFromMode(props) ) {
                                            malodyPropsChanged = true;
                                        }
                                    }
                                }
                            }

                            // 第二阶段收集非预定义且非内部兼容的自定义字段。
                            std::vector<std::string> customKeys;
                            for ( const auto& [k, v] : props ) {
                                // 先检查固定列表，避免重复渲染内置字段。
                                bool isPredefined = false;
                                for ( const auto& [pk, pd] : MALODY_FIELDS ) {
                                    if ( pk == k ) {
                                        // 命中后立即停止小列表扫描。
                                        isPredefined = true;
                                        break;
                                    }
                                }
                                if ( !isPredefined &&
                                     !isHiddenMalodyMetadataKey(k) ) {
                                    // 键副本允许后续循环安全删除属性。
                                    customKeys.push_back(k);
                                }
                            }

                            // 自定义字段沿用属性表当前遍历顺序。
                            for ( const auto& key : customKeys ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(key.c_str());
                                if ( ImGui::IsItemHovered() ) {
                                    // 明确该键没有内置说明或默认语义。
                                    ImGui::SetTooltip("这是一个自定义键。");
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextDisabled("自定义键值");

                                ImGui::TableNextColumn();
                                // 与预定义字段使用相同容量的编辑缓冲。
                                char        valBuf[1024] = { 0 };
                                std::string currentVal   = props.at(key);
                                size_t copyLen = std::min(currentVal.size(),
                                                          sizeof(valBuf) - 1);
                                // 缓冲预清零，截断复制后仍为合法 C 字符串。
                                std::copy(currentVal.begin(),
                                          currentVal.begin() + copyLen,
                                          valBuf);

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_mld_custom_") +
                                          key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    // 输入变化应用到当前格式属性表。
                                    props[key]         = valBuf;
                                    malodyPropsChanged = true;
                                    if ( syncMalodyFreeFromMode(props) ) {
                                        // 自定义键通常快速返回，仍复用统一不变量入口。
                                        malodyPropsChanged = true;
                                    }
                                }

                                ImGui::TableNextColumn();
                                renderMetadataJsonButton(metadataScopeId,
                                                         key,
                                                         props.at(key),
                                                         "map_mld_custom");
                                // JSON 与删除按钮同行，操作区保持紧凑。
                                ImGui::SameLine();
                                if ( ::MMM::UI::FeedbackButton(
                                         (std::string("删除##del_mld_") + key)
                                             .c_str()) ) {
                                    // customKeys
                                    // 是副本，删除不会使当前循环失效。
                                    props.erase(key);
                                    malodyPropsChanged = true;
                                    if ( syncMalodyFreeFromMode(props) ) {
                                        malodyPropsChanged = true;
                                    }
                                }
                            }

                            // 结束 Malody 表格并恢复表格布局状态。
                            ImGui::EndTable();
                        }

                        // 表格下方提供新键、初始值和普通添加入口。
                        ImGui::Separator();
                        // 新增字段标签与输入框按同一基线排列。
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("新增自定义键名:");
                        ImGui::SameLine();
                        // 静态缓冲跨帧保存尚未提交的键名。
                        static char newMldKey[128] = "";
                        // 键名输入使用固定显示宽度和 128 字节容量。
                        ImGui::SetNextItemWidth(200.0f * dpiScale);
                        ImGui::InputText(
                            "##new_mld_key", newMldKey, sizeof(newMldKey));

                        ImGui::SameLine();
                        ImGui::Text("数值:");
                        ImGui::SameLine();
                        // 值缓冲独立于键名保留用户输入。
                        static char newMldVal[256] = "";
                        // 初始值输入宽于键名，并保持在同一行。
                        ImGui::SetNextItemWidth(250.0f * dpiScale);
                        ImGui::InputText(
                            "##new_mld_val", newMldVal, sizeof(newMldVal));

                        ImGui::SameLine();
                        if ( ::MMM::UI::FeedbackButton(
                                 "添加##add_mld_field") ) {
                            // 空键不创建属性，等待用户继续输入。
                            std::string nk = newMldKey;
                            if ( !nk.empty() ) {
                                // 同名键按新增/覆盖语义写入。
                                props[nk]          = newMldVal;
                                malodyPropsChanged = true;
                                if ( syncMalodyFreeFromMode(props) ) {
                                    // 新增 mode 时同步派生 free。
                                    malodyPropsChanged = true;
                                }
                                // 成功后清空两个输入缓存。
                                newMldKey[0] = '\0';
                                newMldVal[0] = '\0';
                            }
                        }

                        if ( malodyPropsChanged ) {
                            // 本帧所有变更合并为一次逻辑线程尾随保存请求。
                            requestBeatmapMetadataAutoSave();
                        }

                        ImGui::EndTabItem();
                    }

                    // RM 页签只编辑文件头和物件表使用的两个 int32 字段。
                    if ( ImGui::BeginTabItem("RM (RM) 格式元数据") ) {
                        // 固定字段列表同时提供顺序和格式说明。
                        // mapLength 缺失时从基础谱面长度有界转换得到默认值。
                        // tabRows 缺失时使用零，等待用户按目标表结构填写。
                        // 已有非法文本不会静默覆盖，窗口显示默认值并给出警告。
                        // 用户修改后始终写回合法十进制 int32 字符串。
                        // RM
                        // 页面不展示任意自定义键，避免误写未知二进制表头字段。
                        // mapLength
                        // 的默认值只用于显示，用户未编辑时不创建属性键。
                        // tabRows 同理保持缺失状态，直到 InputInt
                        // 实际发生变化。
                        // 输入步长一和快速步长一百只影响交互，不改变存储单位。
                        // 非法旧值保留在属性表，直到用户编辑后才被合法文本替换。
                        // 此页不修改 BaseMapMeta::map_length，格式覆盖仅服务 RM
                        // 导出。
                        static const std::vector<
                            std::pair<std::string, std::string>>
                            RM_FIELDS = { { "mapLength",
                                            "谱面长度，RM/IMD 文件头 int32" },
                                          { "tabRows",
                                            "表格行数，RM/IMD 物件表 int32" } };

                        // 当前谱面的 RM 扁平属性表作为直接编辑目标。
                        auto& props = beatmap->m_metadata
                                          .map_properties[MapMetadataType::RM];
                        // 本帧变化在页签尾部合并为一次标脏请求。
                        bool rmPropsChanged = false;

                        // RM 字段少但仍保持与其他格式一致的表格样式。
                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;

                        if ( Utils::VerticalScrollbarStyleScope scrollbarStyle(
                                 dpiScale);
                             ImGui::BeginTable("RMMetadataTable",
                                               3,
                                               tableFlags,
                                               ImVec2(0.0f, 0.0f)) ) {
                            // RM 键名固定且较短，仍沿用统一键列宽度。
                            ImGui::TableSetupColumn(
                                "键名 (Key)",
                                ImGuiTableColumnFlags_WidthFixed,
                                220.0f * dpiScale);
                            // 说明列伸展填充剩余窗口空间。
                            ImGui::TableSetupColumn(
                                "描述 (Description)",
                                ImGuiTableColumnFlags_WidthStretch);
                            // 值列固定宽度并使用整数输入控件。
                            ImGui::TableSetupColumn(
                                "数值 (Value)",
                                ImGuiTableColumnFlags_WidthFixed,
                                250.0f * dpiScale);
                            // 输出三列表头后绘制两个固定字段。
                            ImGui::TableHeadersRow();

                            // 两个预定义字段均始终展示，缺失值采用推导默认。
                            for ( const auto& [key, desc] : RM_FIELDS ) {
                                ImGui::TableNextRow();
                                // 说明列展示二进制字段来源和类型。
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();

                                // 缺失键用灰色文字提示尚未显式持久化。
                                bool hasKey = props.contains(key);
                                if ( !hasKey ) {
                                    ImGui::PushStyleColor(
                                        ImGuiCol_Text,
                                        ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                }
                                ImGui::TextUnformatted(key.c_str());
                                if ( !hasKey ) {
                                    ImGui::PopStyleColor();
                                }
                                if ( ImGui::IsItemHovered() ) {
                                    // 双击复制格式字段名，方便外部核对。
                                    ImGui::SetTooltip(
                                        "双击可以复制该内置键名到剪贴板。");
                                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                                        ImGui::SetClipboardText(key.c_str());
                                    }
                                }

                                // 值列将文本解析结果呈现为 InputInt。
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                // 描述列使用弱文本色，突出可编辑整数。
                                ImGui::PushStyleColor(
                                    ImGuiCol_Text,
                                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                ImGui::TextWrapped("%s", desc.c_str());
                                ImGui::PopStyleColor();

                                ImGui::TableNextColumn();
                                // tabRows 默认零，mapLength 从基础元数据推导。
                                int32_t defaultValue = 0;
                                if ( key == "mapLength" ) {
                                    // 浮点毫秒长度安全转换到 RM int32
                                    // 文件头范围。
                                    defaultValue = clampDoubleToInt32(
                                        beatmap->m_baseMapMetadata.map_length);
                                }

                                // 非法已有文本回退显示默认值，但保留警告。
                                int32_t value = defaultValue;
                                if ( hasKey ) {
                                    value = parseInt32Metadata(props.at(key))
                                                .value_or(defaultValue);
                                }

                                // ImGui InputInt 使用 int，项目目标平台与 int32
                                // 对齐。
                                int valueInput = value;
                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputInt(
                                         (std::string("##val_rm_") + key)
                                             .c_str(),
                                         &valueInput,
                                         1,
                                         100) ) {
                                    // 任何编辑都将合法 int32 文本写回属性表。
                                    props[key] = std::to_string(
                                        static_cast<int32_t>(valueInput));
                                    rmPropsChanged = true;
                                }
                                if ( hasKey &&
                                     !parseInt32Metadata(props.at(key))
                                          .has_value() ) {
                                    // 旧非法文本在用户编辑前仍明确显示修正提示。
                                    ImGui::TextColored(
                                        ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                        "%s",
                                        "当前值不是合法 int32，编辑后会修正。");
                                }
                            }

                            // 结束 RM 表格并恢复 ImGui 表格栈。
                            ImGui::EndTable();
                        }

                        if ( rmPropsChanged ) {
                            // 整数属性变化只需标记扩展元数据脏，不改基础字段。
                            requestBeatmapMetadataAutoSave();
                        }

                        ImGui::EndTabItem();
                    }

                    // 所有格式页签结束后恢复 TabBar 栈。
                    ImGui::EndTabBar();
                }
            }
        }
    }
    if ( opened ) {
        // 辅助模态弹窗只在父窗口已 Begin 成功时绘制。
        renderOsuMetadataTextEditorPopup(dpiScale);
        renderMetadataJsonEditorPopup(dpiScale);
    }
    // Begin 返回 false 时仍必须调用 End 保持 ImGui 栈平衡。
    ImGui::End();

    // 与函数开头六次 PushStyleVar 精确配对。
    ImGui::PopStyleVar(6);
}

/// @brief 从自动采样组件重置精确属性表单。
/// @param state 待重置表单。
/// @param entity 当前自动采样实体。
/// @param sample 当前自动采样组件。
/// @param playerTrackCount 当前玩家轨道数。
/// @warning UI 交互路径：只复制组件值并进行有界轨道换算。
/// @note 组件保存绝对轨道，表单展示玩家轨道之后的一基 BGM 相对编号。
/// @post state 绑定 entity 且 initialized 为 true，其余值反映 sample 当前状态。
void resetSamplePropertyEditorState(SamplePropertyEditorState&    state,
                                    entt::entity                  entity,
                                    const Logic::SampleComponent& sample,
                                    std::int32_t playerTrackCount)
{
    // 绑定实体并标记已初始化，后续帧保留用户未提交的表单修改。
    state.entity      = entity;
    state.initialized = true;
    // 资源引用、偏移和音量按组件当前值建立表单基线。
    state.audioResourceId = sample.m_audioResourceId;
    state.offsetMs        = sample.m_offsetMs;
    state.volume          = sample.m_volume;

    // 非法或落在玩家轨道内的绝对轨道防御性回退首条 BGM 轨。
    std::uint32_t relativeLane = 0;
    if ( playerTrackCount > 0 &&
         sample.m_track >= static_cast<std::uint32_t>(playerTrackCount) ) {
        // 从绝对轨道减去玩家轨道数，得到零基 BGM 相对编号。
        relativeLane =
            sample.m_track - static_cast<std::uint32_t>(playerTrackCount);
    }
    // 提升到 uint64 后加一，避免极端 uint32 值溢出。
    const auto oneBasedLane = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(relativeLane) + 1U,
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()));
    // 钳制到表单 int32 可表示上限后再转换。
    state.bgmLaneOneBased = static_cast<std::int32_t>(oneBasedLane);
}

/// @brief 获取自动采样属性校验失败对应的本地化键。
/// @param issue 校验失败原因。
/// @return 本地化键。
/// @note 多个底层原因合并为资源、轨道或音量三类用户提示。
/// @warning UI 热路径：固定 switch，不分配字符串。
/// @note 返回静态字符串字面量，调用方不得释放或修改。
/// @post 空字符串只表示无需显示错误，不代表校验入口一定成功。
const char* samplePropertyIssueTranslationKey(
    Logic::SamplePropertyEditIssue issue)
{
    // 资源缺失与类型不支持对用户都表现为无效资源选择。
    switch ( issue ) {
    case Logic::SamplePropertyEditIssue::MissingResource:
    case Logic::SamplePropertyEditIssue::UnsupportedResourceType:
        return "ui.edit.sample_properties.invalid_resource";
    case Logic::SamplePropertyEditIssue::InvalidPlayerTrackCount:
    case Logic::SamplePropertyEditIssue::InvalidBgmLane:
    case Logic::SamplePropertyEditIssue::AbsoluteTrackOverflow:
        // 三类绝对/相对轨道问题共享无效轨道提示。
        return "ui.edit.sample_properties.invalid_lane";
    case Logic::SamplePropertyEditIssue::InvalidVolume:
        return "ui.edit.sample_properties.invalid_volume";
    case Logic::SamplePropertyEditIssue::None: break;
    }
    // None 或未来未映射枚举返回空键，不绘制错误文本。
    return "";
}

/// @brief 渲染单个选中自动采样的精确属性表单。
/// @param context 当前会话上下文。
/// @param engine 编辑器逻辑入口。
/// @param selectedSamples 选中的自动采样实体。
/// @param dpiScale 当前 DPI 缩放。
/// @warning UI 热路径：窗口打开时每帧执行；只遍历项目音频资源和当前选中
/// 自动采样，不执行文件系统访问或 ECS 全量扫描。
/// @details 精确表单只支持单个自动采样；资源、BGM 轨道、偏移与音量先经
/// 纯逻辑校验，再通过 CmdUpdateAudioSampleProperties 一次性提交。
/// @note 表单状态不会随组件每帧刷新，避免逻辑更新覆盖正在键入的内容；切换
/// 实体、重新打开窗口或点击重置时才重新读取组件。
/// @details 资源下拉框只接受已登记的 Main 或 Effect 资源，并兼容按 ID 或
/// 历史路径解析当前绑定。非法引用仍原样显示以便用户识别和替换。
/// @details 播放期间即使表单校验成功也禁用应用，避免时间线资源与音频线程
/// 正在消费的采样状态发生冲突。
void renderSelectedSampleProperties(
    Logic::SessionContext& context, Logic::EditorEngine& engine,
    const std::vector<entt::entity>& selectedSamples, float dpiScale)
{
    // 没有自动采样选择时不占用窗口空间。
    if ( selectedSamples.empty() ) return;

    // 独立标题将自动采样表单与玩家音符元数据分区。
    ImGui::TextUnformatted(TR("ui.edit.sample_properties.header").data());
    ImGui::Separator();
    if ( selectedSamples.size() != 1U ) {
        // 多选时不提供部分应用，避免不同原值产生含糊批量语义。
        const auto message = TR_FMT("ui.edit.sample_properties.single_only",
                                    selectedSamples.size());
        ImGui::TextColored(
            ImVec4(1.0F, 0.75F, 0.25F, 1.0F), "%s", message.c_str());
        ImGui::Spacing();
        // 保留后续玩家音符元数据区域，不继续构造采样表单。
        return;
    }

    // 选择列表来自上一段 ECS 视图，使用前仍复核实体和组件有效性。
    const entt::entity entity = selectedSamples.front();
    if ( !context.sampleRegistry.valid(entity) ||
         !context.sampleRegistry.all_of<Logic::SampleComponent>(entity) ) {
        return;
    }

    // 组件只读引用用于初始化和校验，不在 UI 线程直接修改。
    const auto& sample =
        context.sampleRegistry.get<const Logic::SampleComponent>(entity);
    // 切换实体时重建表单，同一实体则保留尚未提交输入。
    auto& state = samplePropertyEditorState();
    if ( !state.initialized || state.entity != entity ) {
        resetSamplePropertyEditorState(
            state, entity, sample, context.trackCount);
    }

    // 项目资源列表提供可选音频，项目为空时仍显示现有引用文本。
    const auto* project = engine.getCurrentProject();
    /// @brief 按资源 ID 或路径解析当前项目音频资源。
    /// @param reference 组件或表单保存的资源引用。
    /// @return 匹配资源的非拥有指针，未找到时为空。
    /// @warning UI 热路径：线性遍历项目音频资源，不访问文件系统。
    const auto resolveResource =
        [&](std::string_view reference) -> const ::MMM::AudioResource* {
        // 缺少项目或空引用时无需遍历资源表。
        if ( !project || reference.empty() ) return nullptr;
        const auto iterator =
            std::find_if(project->m_audioResources.begin(),
                         project->m_audioResources.end(),
                         [&](const ::MMM::AudioResource& resource) {
                             // 同时兼容稳定 ID 和历史路径引用。
                             return resource.m_id == reference ||
                                    resource.m_path == reference;
                         });
        // 指针仅在本帧项目资源容器不变期间使用。
        return iterator == project->m_audioResources.end()
                   ? nullptr
                   : std::addressof(*iterator);
    };

    // 预览文本附加资源类型，未解析引用则原样显示便于修复。
    const auto*       selectedResource = resolveResource(state.audioResourceId);
    const std::string resourcePreview =
        selectedResource
            ? fmt::format(
                  "{} [{}]",
                  selectedResource->m_id,
                  TR(selectedResource->m_type == ::MMM::AudioTrackType::Main
                         ? "ui.edit.sample_properties.resource_main"
                         : "ui.edit.sample_properties.resource_effect")
                      .data())
            : state.audioResourceId;

    // 资源组合框占满可用宽度，便于显示较长资源 ID。
    ImGui::SetNextItemWidth(-1.0F);
    if ( FeedbackBeginCombo(
             TR("ui.edit.sample_properties.resource").data(),
             resourcePreview.empty()
                 ? TR("ui.edit.sample_properties.select_resource").data()
                 : resourcePreview.c_str()) ) {
        if ( project ) {
            // 下拉框只遍历当前项目已经登记的音频资源。
            for ( std::size_t index = 0;
                  index < project->m_audioResources.size();
                  ++index ) {
                const auto& resource = project->m_audioResources[index];
                // 空 ID 无法形成稳定采样绑定，跳过该条目。
                if ( resource.m_id.empty() ) continue;
                // 自动采样只支持 Main 与 Effect 两类播放资源。
                const bool validType =
                    resource.m_type == ::MMM::AudioTrackType::Main ||
                    resource.m_type == ::MMM::AudioTrackType::Effect;
                if ( !validType ) continue;

                // 本地化类型标签帮助区分相同命名来源。
                const auto typeText =
                    TR(resource.m_type == ::MMM::AudioTrackType::Main
                           ? "ui.edit.sample_properties.resource_main"
                           : "ui.edit.sample_properties.resource_effect");
                // 隐藏索引保证重复显示文本仍拥有唯一 ImGui ID。
                const auto label = fmt::format("{} [{}]##sample_resource_{}",
                                               resource.m_id,
                                               typeText.data(),
                                               index);
                const bool selected =
                    selectedResource == std::addressof(resource);
                if ( FeedbackSelectable(label.c_str(), selected) ) {
                    // 选择只更新表单，点击应用前组件保持不变。
                    state.audioResourceId = resource.m_id;
                    selectedResource      = std::addressof(resource);
                }
                // 打开组合框时滚动并聚焦当前资源。
                if ( selected ) ImGui::SetItemDefaultFocus();
            }
        }
        FeedbackEndCombo();
    }

    // 三个数值字段使用一致逻辑宽度并按 DPI 缩放。
    ImGui::SetNextItemWidth(180.0F * dpiScale);
    ImGui::InputScalar(TR("ui.edit.sample_properties.bgm_lane").data(),
                       ImGuiDataType_S32,
                       &state.bgmLaneOneBased);
    ImGui::SetNextItemWidth(180.0F * dpiScale);
    ImGui::InputScalar(TR("ui.edit.sample_properties.offset_ms").data(),
                       ImGuiDataType_S64,
                       &state.offsetMs);
    ImGui::SetNextItemWidth(180.0F * dpiScale);
    ImGui::InputFloat(TR("ui.edit.sample_properties.volume").data(),
                      &state.volume,
                      0.01F,
                      0.1F,
                      "%.6f");

    // 用户可能在本帧切换资源，校验前重新解析指针。
    selectedResource = resolveResource(state.audioResourceId);
    // UI 使用一基编号，非正输入映射为显式无效值 -1。
    const std::int32_t zeroBasedLane =
        state.bgmLaneOneBased > 0 ? state.bgmLaneOneBased - 1 : -1;
    // 纯逻辑入口统一验证资源类型、轨道范围、偏移和音量。
    const auto editResult = Logic::resolveSamplePropertyEdit(sample,
                                                             context.trackCount,
                                                             selectedResource,
                                                             zeroBasedLane,
                                                             state.offsetMs,
                                                             state.volume);
    if ( !editResult.m_sample ) {
        // 将结构化校验原因映射为本地化错误文本。
        const char* key = samplePropertyIssueTranslationKey(editResult.m_issue);
        if ( key[0] != '\0' ) {
            ImGui::TextColored(
                ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s", TR(key).data());
        }
    } else if ( context.isPlaying ) {
        // 有效表单仍禁止播放中修改，避免音频时间线并发重建。
        ImGui::TextColored(
            ImVec4(1.0F, 0.75F, 0.25F, 1.0F),
            "%s",
            TR("ui.edit.sample_properties.pause_to_edit").data());
    }

    // 只有校验成功且暂停时允许提交命令。
    ImGui::BeginDisabled(!editResult.m_sample || context.isPlaying);
    if ( FeedbackButton(TR("ui.edit.sample_properties.apply").data()) ) {
        // 命令按值携带用户表单，逻辑线程负责组件更新和撤销语义。
        engine.pushCommand(Logic::CmdUpdateAudioSampleProperties{
            .entity          = entity,
            .audioResourceId = state.audioResourceId,
            .bgmLane         = zeroBasedLane,
            .offsetMs        = state.offsetMs,
            .volume          = state.volume,
        });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if ( FeedbackButton(TR("ui.edit.sample_properties.reset").data()) ) {
        // 重置重新读取组件当前值，丢弃尚未提交的表单修改。
        resetSamplePropertyEditorState(
            state, entity, sample, context.trackCount);
    }
    ImGui::Spacing();
}

/// @brief 渲染选中谱面物件的元数据与自动采样精确属性窗口。
/// @param showWindow 窗口可见状态，标题栏关闭时写回 false。
/// @warning UI 热路径：窗口可见时遍历可交互音符与自动采样 ECS 视图。
/// @details 玩家音符按完整元数据指纹分组，同组编辑批量应用；自动采样只在
/// 单选时提供资源、BGM 轨、偏移和音量精确表单。
/// @note 元数据属性直接更新 ECS 组件，自动采样属性通过逻辑命令提交。
/// @details 音符元数据指纹对格式类型与字段键排序，消除哈希表遍历顺序差异；
/// 同指纹实体共享代表值，任何字段编辑或删除都会批量保持组内一致。
/// @details 选择收集排除子音符，避免父子物件分别编辑造成格式字段分裂；自动
/// 采样来自独立注册表并在玩家音符组之前绘制。
/// @details 每组按最早和最晚时间展示摘要，单组默认展开。OSU 与 Malody 支持
/// 任意键值增删，Malody 额外复用全局 JSON 编辑器，RM 只暴露 Parameter。
/// @details 新增字段缓冲按组号和格式枚举隔离，并在窗口重新打开时清空，避免
/// 选择集合变化后把旧输入应用到新组。
/// @warning 批量元数据写入发生在会话锁内，不跨帧保存组件引用。
/// @post 六项 ImGui 样式变量和可选标题字体均恢复；关闭时更新 showWindow。
/// @note JSON 辅助结果只由创建它的组和格式作用域消费一次。
void renderNoteMetadataEditorWindow(bool& showWindow)
{
    // 窗口隐藏时不访问 ECS 或推入样式状态。
    if ( !showWindow ) return;

    /// @brief 单个格式页新增字段使用的固定输入缓存。
    /// @note key 和 val 在窗口重新打开时统一清空。
    struct InputBuffer {
        /// @brief 新元数据键名缓冲。
        char key[128] = "";
        /// @brief 新元数据值缓冲。
        char val[256] = "";
    };
    // 缓冲按组索引和格式类型组合键保存，跨帧保留未提交输入。
    static std::unordered_map<std::string, InputBuffer> inputBuffers;
    // 上一帧可见状态用于检测窗口从关闭到打开的边沿。
    static bool lastShowState = false;
    if ( showWindow && !lastShowState ) {
        // 新一轮窗口交互不继承旧分组输入或采样表单。
        inputBuffers.clear();
        samplePropertyEditorState().initialized = false;
    }
    // 保存当前状态供下一帧边沿判断。
    lastShowState = showWindow;

    // 按当前 DPI 和用户外观设置构造窗口局部样式。
    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    // 圆角取整到设备像素，避免缩放后边缘模糊。
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    // 横纵项目间距共享同一配置值。
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    // 六项样式变量在函数末尾统一弹出。
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    // 仅首次使用建议尺寸，之后保留用户调整。
    ImGui::SetNextWindowSize(ImVec2(750.0f * dpiScale, 500.0f * dpiScale),
                             ImGuiCond_FirstUseEver);

    // 标题优先使用皮肤 title 字体，正文恢复默认字体。
    auto&   skinMgr   = Config::SkinManager::instance();
    ImFont* titleFont = skinMgr.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

    // ### 后缀固定内部 ID，本地化标题变化不影响停靠状态。
    std::string windowTitle = TR("ui.edit.note_metadata.title").toString() +
                              "###NoteMetadataEditorWindow";
    // 保存 Begin 前状态以触发统一关闭按钮反馈。
    const bool wasOpenBeforeBegin = showWindow;
    bool       opened =
        ImGui::Begin(windowTitle.c_str(), &showWindow, ImGuiWindowFlags_None);
    FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, &showWindow);

    // 字体只包围窗口标题创建。
    if ( titleFont ) ImGui::PopFont();

    if ( opened ) {
        // 访问会话和 ECS 期间持有编辑器递归锁。
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        // 局部 shared_ptr 保证本帧绘制期间会话存活。
        auto session = engine.getActiveSession();
        if ( !session ) {
            // 无会话时只显示本地化提示，不构造 ECS 视图。
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "%s",
                               TR("ui.tools.no_active_session").data());
        } else if ( session->isCollaborationOfflineReadOnly() ) {
            // 离线协作会话统一进入只读门闩。
            renderCollaborationOfflineReadOnlyNotice(*session);
        } else {
            // 先收集玩家音符和自动采样两套注册表中的选中实体。
            /// @brief 玩家音符批量分组所需的轻量选择记录。
            struct SelectedNote {
                /// @brief noteRegistry 中的稳定实体标识。
                entt::entity entity;
                /// @brief 用于组摘要范围显示的音符时间戳。
                double timestamp;
            };
            // 仅保存非子音符选择，后续按元数据指纹分组。
            std::vector<SelectedNote> selectedNotes;
            // 可变上下文用于最终写回 NoteComponent 元数据。
            auto& sessionContext = session->getContextMutable();
            auto& registry       = sessionContext.noteRegistry;

            // 视图限定同时具有音符与交互组件的可选择实体。
            auto view = registry.view<const Logic::NoteComponent,
                                      const Logic::InteractionComponent>();
            // 仅遍历该筛选视图，不扫描无交互组件的所有实体。
            for ( auto e : view ) {
                const auto& interaction =
                    view.get<const Logic::InteractionComponent>(e);
                if ( interaction.isSelected ) {
                    // 子音符由其父物件元数据语义管理，不独立进入批量编辑。
                    const auto& nc = view.get<const Logic::NoteComponent>(e);
                    if ( nc.m_isSubNote ) continue;
                    // 时间戳副本避免摘要阶段再次访问组件。
                    selectedNotes.push_back({ e, nc.m_timestamp });
                }
            }

            // 自动采样来自独立注册表，只需保存实体 ID。
            std::vector<entt::entity> selectedSamples;
            const auto                sampleView =
                sessionContext.sampleRegistry
                    .view<const Logic::SampleComponent,
                          const Logic::InteractionComponent>();
            // 视图已限定采样与交互组件，循环只检查选择标志。
            for ( auto entity : sampleView ) {
                if ( sampleView.get<const Logic::InteractionComponent>(entity)
                         .isSelected ) {
                    // 多选数量由精确表单入口统一处理。
                    selectedSamples.push_back(entity);
                }
            }

            // 自动采样表单绘制在玩家音符元数据分组之前。
            renderSelectedSampleProperties(
                sessionContext, engine, selectedSamples, dpiScale);

            if ( selectedNotes.empty() ) {
                // 只有两类选择都为空时显示全局无选择提示。
                if ( selectedSamples.empty() ) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                        "%s",
                        TR("ui.edit.note_metadata.no_selection").data());
                }
            } else {
                // 将 note_properties 序列化为稳定字符串作为分组键。
                /// @brief 为音符元数据构造与容器遍历顺序无关的指纹。
                /// @param meta 当前音符元数据。
                /// @return 按格式类型和排序键拼接的稳定字符串。
                /// @warning UI 热路径：对单个选中音符的小型属性表进行排序。
                auto serializeMetadata =
                    [](const ::MMM::NoteMetadata& meta) -> std::string {
                    // 空元数据产生空指纹，所有无扩展字段音符归为一组。
                    std::string result;
                    // 按 NoteMetadataType 枚举值固定顺序遍历四类格式。
                    for ( int typeIdx = 0; typeIdx < 4; ++typeIdx ) {
                        auto metaType =
                            static_cast<::MMM::NoteMetadataType>(typeIdx);
                        auto it = meta.note_properties.find(metaType);
                        if ( it == meta.note_properties.end() ) continue;
                        // 空格式表与不存在该格式具有相同分组语义。
                        if ( it->second.empty() ) continue;
                        // 类型索引和花括号分隔不同格式域。
                        result += std::to_string(typeIdx) + "{";
                        // 哈希表键先排序，消除插入顺序对指纹的影响。
                        std::vector<std::string> keys;
                        for ( const auto& [k, v] : it->second ) {
                            keys.push_back(k);
                        }
                        std::sort(keys.begin(), keys.end());
                        for ( const auto& k : keys ) {
                            // 同时拼接键和值，任何属性差异都会形成新组。
                            result += k + "=" + it->second.at(k) + ";";
                        }
                        result += "}";
                    }
                    // 指纹仅用于本帧 map 分组，不写入谱面数据。
                    return result;
                };

                /// @brief 共享相同元数据指纹的一组选中音符。
                struct MetadataGroup {
                    /// @brief 需要批量应用编辑的实体集合。
                    std::vector<entt::entity> entities;
                    /// @brief 组内最早音符时间，用于摘要标题。
                    double minTime;
                    /// @brief 组内最晚音符时间，用于摘要标题。
                    double maxTime;
                };
                // std::map 按指纹稳定排序组，降低每帧显示顺序抖动。
                std::map<std::string, MetadataGroup> groups;

                // 单次遍历把选择记录归入对应指纹组并更新时间范围。
                for ( const auto& sn : selectedNotes ) {
                    // 实体来自当前有效视图，锁内访问组件保持稳定。
                    auto& nc = registry.get<Logic::NoteComponent>(sn.entity);
                    std::string fingerprint = serializeMetadata(nc.m_metadata);
                    auto&       group       = groups[fingerprint];
                    // operator[] 首次见到指纹时创建空组。
                    group.entities.push_back(sn.entity);
                    if ( group.entities.size() == 1 ) {
                        // 首个实体同时初始化最小和最大时间。
                        group.minTime = sn.timestamp;
                        group.maxTime = sn.timestamp;
                    } else {
                        // 后续实体只扩展已有时间包围范围。
                        group.minTime = std::min(group.minTime, sn.timestamp);
                        group.maxTime = std::max(group.maxTime, sn.timestamp);
                    }
                }

                // 摘要显示选中音符总数和不同元数据组数量。
                std::string summaryStr = TR_FMT("ui.edit.note_metadata.summary",
                                                selectedNotes.size(),
                                                groups.size());
                ImGui::TextUnformatted(summaryStr.c_str());
                ImGui::Separator();
                ImGui::Spacing();

                // 各组编辑区域使用子窗口滚动，不影响外层标题和采样表单。
                Utils::VerticalScrollbarStyleScope scrollbarStyle(dpiScale);
                ImGui::BeginChild("NoteMetaGroups",
                                  ImVec2(0, 0),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_None);

                /// @brief 将一次 JSON 弹窗结果批量写入指定音符组。
                /// @param scopeId 当前组与格式组合的作用域标识。
                /// @param metaType 目标音符元数据格式。
                /// @param entities 当前指纹组实体列表。
                /// @warning UI 热路径：仅有待消费结果时遍历当前组。
                auto applyNoteJsonEditResult =
                    [&](const std::string&               scopeId,
                        ::MMM::NoteMetadataType          metaType,
                        const std::vector<entt::entity>& entities) {
                        if ( auto result =
                                 takeMetadataJsonEditResult(scopeId) ) {
                            // 同组实体在打开弹窗时具有相同基线，统一覆盖目标键。
                            for ( auto e : entities ) {
                                auto& nc =
                                    registry.get<Logic::NoteComponent>(e);
                                // JSON 文本继续保存于字符串属性表协议。
                                nc.m_metadata
                                    .note_properties[metaType][result->key] =
                                    result->value;
                            }
                        }
                    };

                // 一基组号用于用户标题和 ImGui ID 隔离。
                int groupIdx = 0;
                for ( auto& [fingerprint, group] : groups ) {
                    // map 已稳定排序，组号在当前帧保持一致。
                    ++groupIdx;
                    // 时间范围使用画布统一格式，避免手工处理毫秒精度。
                    const auto minTimeText =
                        MMM::UI::Utils::formatCanvasTime(group.minTime);
                    const auto maxTimeText =
                        MMM::UI::Utils::formatCanvasTime(group.maxTime);
                    std::string headerStr =
                        // 标题同时显示组号、实体数量和时间范围。
                        TR_FMT("ui.edit.note_metadata.group_header",
                               groupIdx,
                               group.entities.size(),
                               minTimeText,
                               maxTimeText);

                    // 组索引隔离三种格式页签及其所有行控件 ID。
                    ImGui::PushID(groupIdx);

                    // 单组默认展开，多组默认折叠以控制窗口长度。
                    bool headerOpen = ::MMM::UI::FeedbackCollapsingHeader(
                        headerStr.c_str(),
                        groups.size() == 1 ? ImGuiTreeNodeFlags_DefaultOpen
                                           : ImGuiTreeNodeFlags_None);

                    if ( headerOpen ) {
                        // 同组元数据指纹一致，取首个实体作为只读展示代表。
                        auto  firstEntity = group.entities.front();
                        auto& firstNc =
                            registry.get<Logic::NoteComponent>(firstEntity);

                        // 每个组拥有独立 OSU、Malody 与 RM 页签状态。
                        if ( ImGui::BeginTabBar(
                                 fmt::format("MetaTabBar_{}", groupIdx)
                                     .c_str()) ) {
                            // OSU 音符属性按普通字符串键值批量编辑。
                            if ( ImGui::BeginTabItem("OSU") ) {
                                // 本页直接编辑 NoteMetadata，不同步谱面级 OSU
                                // 基础字段。
                                // 组内批量写入维持所有实体相同的指纹语义。
                                // 每帧分组会在编辑后重新计算，字段变化可能形成新组顺序。
                                // 键排序只影响显示，底层属性表继续使用原哈希容器。
                                // 清除最后字段时移除整个格式映射，空表不参与后续指纹。
                                // 新增缓冲只属于当前组和 OSU
                                // 枚举，切换页签不会覆盖。
                                // 固定当前页的格式枚举，供属性表索引和缓存键使用。
                                auto metaType = ::MMM::NoteMetadataType::OSU;
                                // 代表实体可能尚无该格式属性表。
                                auto it =
                                    firstNc.m_metadata.note_properties.find(
                                        metaType);
                                // 静态空表避免仅为展示而向代表实体插入格式项。
                                static const decltype(firstNc.m_metadata
                                                          .note_properties)::
                                    mapped_type emptyMap;
                                // 当前组指纹一致，首实体属性可代表所有组员键值。
                                const auto& refProps =
                                    (it !=
                                     firstNc.m_metadata.note_properties.end())
                                        ? it->second
                                        : emptyMap;

                                // 三列表格展示键、可编辑值和清除操作。
                                ImGuiTableFlags tableFlags =
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_BordersOuter |
                                    ImGuiTableFlags_Resizable;

                                if ( ImGui::BeginTable(
                                         fmt::format("NoteMetaTable_OSU_{}",
                                                     groupIdx)
                                             .c_str(),
                                         3,
                                         tableFlags,
                                         ImVec2(0.0f, 0.0f)) ) {
                                    // 键列固定宽度，长格式键不会压缩操作区。
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.key_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        220.0f * dpiScale);
                                    // 值列伸展以利用组窗口剩余宽度。
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.value_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthStretch);
                                    // OSU 操作列只需容纳清除按钮。
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.action_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        60.0f * dpiScale);
                                    // 本地化表头在每组表格顶部生成。
                                    ImGui::TableHeadersRow();

                                    // 复制并排序哈希表键，确保每帧行顺序稳定。
                                    std::vector<std::string> keys;
                                    for ( const auto& [k, v] : refProps ) {
                                        // 值稍后按键读取，此处仅收集排序键。
                                        keys.push_back(k);
                                    }
                                    std::sort(keys.begin(), keys.end());

                                    // 每个键占用一行并批量影响本组全部实体。
                                    for ( const auto& key : keys ) {
                                        ImGui::TableNextRow();
                                        // 值列使用当前代表属性预填编辑缓冲。
                                        ImGui::TableNextColumn();
                                        ImGui::AlignTextToFramePadding();
                                        ImGui::TextUnformatted(key.c_str());

                                        // 操作列提供当前键的批量清除。
                                        ImGui::TableNextColumn();
                                        // 栈缓冲适配 ImGui 可写文本接口。
                                        char        valBuf[1024] = { 0 };
                                        std::string currentVal =
                                            refProps.at(key);
                                        // 超长值截断到控件容量，末尾由零初始化保证。
                                        size_t copyLen =
                                            std::min(currentVal.size(),
                                                     sizeof(valBuf) - 1);
                                        std::copy(currentVal.begin(),
                                                  currentVal.begin() + copyLen,
                                                  valBuf);

                                        ImGui::SetNextItemWidth(-1.0f);
                                        if ( ImGui::InputText(
                                                 fmt::format("##nm_osu_{}_{}",
                                                             groupIdx,
                                                             key)
                                                     .c_str(),
                                                 valBuf,
                                                 sizeof(valBuf)) ) {
                                            // 输入变化同步写回所有同指纹组实体。
                                            for ( auto e : group.entities ) {
                                                // 实体来自当前锁内有效选择列表。
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                // operator[]
                                                // 按需建立格式表和目标键。
                                                nc.m_metadata
                                                    .note_properties[metaType]
                                                                    [key] =
                                                    valBuf;
                                            }
                                        }

                                        ImGui::TableNextColumn();
                                        if ( ::MMM::UI::FeedbackButton(
                                                 fmt::format(
                                                     "{}##clr_osu_{}_{}",
                                                     TR("ui.edit.note_"
                                                        "metadata.clear_"
                                                        "btn")
                                                         .data(),
                                                     groupIdx,
                                                     key)
                                                     .c_str()) ) {
                                            // 清除操作同样批量应用整个组。
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                // 取得目标格式表并删除当前字段。
                                                auto& m = nc.m_metadata
                                                              .note_properties
                                                                  [metaType];
                                                m.erase(key);
                                                if ( m.empty() ) {
                                                    // 空格式表整体移除，保持元数据结构精简。
                                                    nc.m_metadata
                                                        .note_properties.erase(
                                                            metaType);
                                                }
                                            }
                                        }
                                    }
                                    // 结束当前组 OSU 属性表。
                                    ImGui::EndTable();
                                }

                                // 表格下方提供当前组 OSU 自定义字段新增表单。
                                ImGui::Separator();
                                // 新增标签与输入控件在同一基线上排列。
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_key").data());
                                ImGui::SameLine();
                                // 缓存键组合组号和格式枚举，隔离各页未提交输入。
                                std::string bufKey =
                                    fmt::format("{}_{}",
                                                groupIdx,
                                                static_cast<int>(metaType));
                                // operator[] 首次进入页签时建立零初始化缓冲。
                                auto& buf = inputBuffers[bufKey];
                                // 键输入较窄，容量由 InputBuffer::key 决定。
                                ImGui::SetNextItemWidth(150.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmk_osu_{}", groupIdx)
                                        .c_str(),
                                    buf.key,
                                    sizeof(buf.key));
                                ImGui::SameLine();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_value")
                                        .data());
                                ImGui::SameLine();
                                // 值输入提供更宽区域并保持同排布局。
                                ImGui::SetNextItemWidth(200.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmv_osu_{}", groupIdx)
                                        .c_str(),
                                    buf.val,
                                    sizeof(buf.val));
                                ImGui::SameLine();
                                if ( ::MMM::UI::FeedbackButton(
                                         fmt::format("{}##nma_osu_{}",
                                                     TR("ui.edit.note_metadata."
                                                        "add_btn")
                                                         .data(),
                                                     groupIdx)
                                             .c_str()) ) {
                                    // 空键不执行批量写入，保留表单等待修正。
                                    std::string nk = buf.key;
                                    if ( !nk.empty() ) {
                                        // 对组内每个实体创建或覆盖相同键值。
                                        for ( auto e : group.entities ) {
                                            auto& nc =
                                                registry
                                                    .get<Logic::NoteComponent>(
                                                        e);
                                            // 保持分组内元数据继续完全一致。
                                            nc.m_metadata
                                                .note_properties[metaType][nk] =
                                                buf.val;
                                        }
                                        // 成功后清空当前组和格式的输入缓冲。
                                        buf.key[0] = '\0';
                                        buf.val[0] = '\0';
                                    }
                                }

                                ImGui::EndTabItem();
                            }

                            // Malody 音符属性额外提供 JSON 复合值编辑入口。
                            if ( ImGui::BeginTabItem("MALODY") ) {
                                // JSON
                                // 结果按组作用域消费，其他组无法误取当前字段结果。
                                // 直接文本编辑允许保存尚未完成的 JSON
                                // 或专有字符串。 JSON
                                // 辅助入口只接受合法完成结果并统一格式化文本。
                                // 代表实体在批量写入前与组员指纹一致，可安全提供初始值。
                                // 删除字段批量保持一致，并在空表时清理格式类型节点。
                                // 普通新增与 JSON
                                // 新增共享同一输入缓冲但提交路径独立。
                                // 当前格式枚举参与属性索引和作用域构造。
                                auto metaType = ::MMM::NoteMetadataType::MALODY;
                                // 组号加格式枚举形成全局 JSON 结果路由键。
                                std::string metadataScopeId =
                                    fmt::format("note_{}_{}",
                                                groupIdx,
                                                static_cast<int>(metaType));
                                // 若有匹配结果，先批量应用再读取代表属性表。
                                applyNoteJsonEditResult(
                                    metadataScopeId, metaType, group.entities);
                                // JSON 应用可能已创建目标格式表，随后重新查找。
                                auto it =
                                    firstNc.m_metadata.note_properties.find(
                                        metaType);
                                // 缺失格式时使用只读静态空表，不污染组件。
                                static const decltype(firstNc.m_metadata
                                                          .note_properties)::
                                    mapped_type emptyMap;
                                const auto&     refProps =
                                    (it !=
                                     firstNc.m_metadata.note_properties.end())
                                        ? it->second
                                        : emptyMap;

                                // 操作列更宽，以容纳 JSON 与清除两个按钮。
                                ImGuiTableFlags tableFlags =
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_BordersOuter |
                                    ImGuiTableFlags_Resizable;

                                if ( ImGui::BeginTable(
                                         fmt::format("NoteMetaTable_MLD_{}",
                                                     groupIdx)
                                             .c_str(),
                                         3,
                                         tableFlags,
                                         ImVec2(0.0f, 0.0f)) ) {
                                    // 键列固定宽度以容纳 Malody
                                    // 常见复合字段名。
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.key_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        220.0f * dpiScale);
                                    // 值列伸展用于直接编辑 JSON
                                    // 原文或普通字符串。
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.value_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthStretch);
                                    // 操作列更宽，可同时放置 JSON 与清除按钮。
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.action_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        130.0f * dpiScale);
                                    // 本地化表头与其他格式页保持一致。
                                    ImGui::TableHeadersRow();

                                    // 排序键副本提供稳定行顺序和安全删除基础。
                                    std::vector<std::string> keys;
                                    for ( const auto& [k, v] : refProps ) {
                                        keys.push_back(k);
                                    }
                                    std::sort(keys.begin(), keys.end());

                                    // 表格值来自代表实体，所有编辑批量应用组员。
                                    for ( const auto& key : keys ) {
                                        ImGui::TableNextRow();
                                        // 值列从代表实体读取组内共同值。
                                        ImGui::TableNextColumn();
                                        ImGui::AlignTextToFramePadding();
                                        ImGui::TextUnformatted(key.c_str());

                                        // 操作列先绘制 JSON
                                        // 辅助入口，再绘制清除。
                                        ImGui::TableNextColumn();
                                        // 普通文本入口仍允许快速编辑 JSON
                                        // 字符串原文。
                                        char        valBuf[1024] = { 0 };
                                        std::string currentVal =
                                            refProps.at(key);
                                        size_t copyLen =
                                            std::min(currentVal.size(),
                                                     sizeof(valBuf) - 1);
                                        std::copy(currentVal.begin(),
                                                  currentVal.begin() + copyLen,
                                                  valBuf);

                                        ImGui::SetNextItemWidth(-1.0f);
                                        if ( ImGui::InputText(
                                                 fmt::format("##nm_mld_{}_{}",
                                                             groupIdx,
                                                             key)
                                                     .c_str(),
                                                 valBuf,
                                                 sizeof(valBuf)) ) {
                                            // 直接文本编辑不做 JSON
                                            // 校验，保留格式自由度。
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                nc.m_metadata
                                                    .note_properties[metaType]
                                                                    [key] =
                                                    valBuf;
                                            }
                                        }

                                        ImGui::TableNextColumn();
                                        // JSON
                                        // 按钮打开带结构校验和子字段工具的弹窗。
                                        renderMetadataJsonButton(
                                            metadataScopeId,
                                            key,
                                            refProps.at(key),
                                            fmt::format("note_mld_{}_field",
                                                        groupIdx));
                                        // 清除按钮与 JSON 入口同行显示。
                                        ImGui::SameLine();
                                        if ( ::MMM::UI::FeedbackButton(
                                                 fmt::format(
                                                     "{}##clr_mld_{}_{}",
                                                     TR("ui.edit.note_"
                                                        "metadata.clear_"
                                                        "btn")
                                                         .data(),
                                                     groupIdx,
                                                     key)
                                                     .c_str()) ) {
                                            // 按当前组批量删除 Malody 字段。
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                auto& m = nc.m_metadata
                                                              .note_properties
                                                                  [metaType];
                                                m.erase(key);
                                                if ( m.empty() ) {
                                                    // 最后字段删除后移除空格式表。
                                                    nc.m_metadata
                                                        .note_properties.erase(
                                                            metaType);
                                                }
                                            }
                                        }
                                    }
                                    // 结束当前组 Malody 属性表。
                                    ImGui::EndTable();
                                }

                                // 表格下方新增表单支持普通文本或 JSON
                                // 辅助创建。
                                ImGui::Separator();
                                // 新增区域标签与输入控件保持基线对齐。
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_key").data());
                                ImGui::SameLine();
                                // 缓存键与 OSU 页相同规则，但格式枚举不同。
                                std::string bufKey =
                                    fmt::format("{}_{}",
                                                groupIdx,
                                                static_cast<int>(metaType));
                                auto& buf = inputBuffers[bufKey];
                                // 键名显示宽度按 DPI 缩放。
                                ImGui::SetNextItemWidth(150.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmk_mld_{}", groupIdx)
                                        .c_str(),
                                    buf.key,
                                    sizeof(buf.key));
                                ImGui::SameLine();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_value")
                                        .data());
                                ImGui::SameLine();
                                // 初始值输入使用当前组独立缓存。
                                ImGui::SetNextItemWidth(200.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmv_mld_{}", groupIdx)
                                        .c_str(),
                                    buf.val,
                                    sizeof(buf.val));
                                ImGui::SameLine();
                                if ( ::MMM::UI::FeedbackButton(
                                         fmt::format("{}##nma_mld_{}",
                                                     TR("ui.edit.note_metadata."
                                                        "add_btn")
                                                         .data(),
                                                     groupIdx)
                                             .c_str()) ) {
                                    // 普通添加允许任意字符串，空键则保持输入。
                                    std::string nk = buf.key;
                                    if ( !nk.empty() ) {
                                        // 新字段批量写入组内所有实体。
                                        for ( auto e : group.entities ) {
                                            auto& nc =
                                                registry
                                                    .get<Logic::NoteComponent>(
                                                        e);
                                            nc.m_metadata
                                                .note_properties[metaType][nk] =
                                                buf.val;
                                        }
                                        // 成功后清空当前缓存，JSON
                                        // 按钮随后看到空键。
                                        buf.key[0] = '\0';
                                        buf.val[0] = '\0';
                                    }
                                }
                                ImGui::SameLine();
                                // JSON 新增入口复用当前键值缓冲作为初始内容。
                                renderNewMetadataJsonButton(
                                    metadataScopeId,
                                    buf.key,
                                    buf.val,
                                    fmt::format("note_mld_{}_new_field",
                                                groupIdx));

                                ImGui::EndTabItem();
                            }

                            // RM 音符页只编辑二进制行 Parameter 整数字段。
                            if ( ImGui::BeginTabItem("RM") ) {
                                // RM 音符格式当前只识别
                                // Parameter，未知键不在本页修改。 缺失
                                // Parameter 显示零但不创建键，操作列保持占位。
                                // 非法文本显示零和警告，用户输入后转为合法
                                // int32 字符串。 批量清除最后字段时移除 RM
                                // 格式节点，恢复空元数据指纹。 InputInt
                                // 的步长参数不限制用户键盘输入，转换保持原行为。
                                // RM 枚举用于定位每个音符的对应属性表。
                                auto metaType = ::MMM::NoteMetadataType::RM;
                                auto it =
                                    firstNc.m_metadata.note_properties.find(
                                        metaType);
                                // 缺失 RM 属性时使用静态空表展示默认零。
                                static const decltype(firstNc.m_metadata
                                                          .note_properties)::
                                    mapped_type emptyMap;
                                const auto&     refProps =
                                    (it !=
                                     firstNc.m_metadata.note_properties.end())
                                        ? it->second
                                        : emptyMap;

                                // 单字段仍使用标准三列表格保持页面一致。
                                ImGuiTableFlags tableFlags =
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_BordersOuter |
                                    ImGuiTableFlags_Resizable;

                                if ( ImGui::BeginTable(
                                         fmt::format("NoteMetaTable_RM_{}",
                                                     groupIdx)
                                             .c_str(),
                                         3,
                                         tableFlags,
                                         ImVec2(0.0f, 0.0f)) ) {
                                    // 键列展示固定 Parameter 名称。
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.key_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        220.0f * dpiScale);
                                    // 值列伸展并容纳整数输入框。
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.value_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthStretch);
                                    // 操作列只需容纳清除按钮或占位符。
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.action_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        60.0f * dpiScale);
                                    // 输出与其他音符格式页一致的本地化表头。
                                    ImGui::TableHeadersRow();

                                    // Parameter 是 RM/IMD 物件行约定的 int32
                                    // 参数键。
                                    const std::string key = "Parameter";
                                    // 缺失时显示默认零但不立即创建属性。
                                    const bool hasKey = refProps.contains(key);
                                    int32_t    value  = 0;
                                    if ( hasKey ) {
                                        // 旧非法文本回退零，并在输入框下显示警告。
                                        value =
                                            parseInt32Metadata(refProps.at(key))
                                                .value_or(0);
                                    }

                                    ImGui::TableNextRow();
                                    // 值列显示解析后的有界整数。
                                    ImGui::TableNextColumn();
                                    ImGui::AlignTextToFramePadding();
                                    if ( !hasKey ) {
                                        ImGui::PushStyleColor(
                                            ImGuiCol_Text,
                                            ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                    }
                                    ImGui::TextUnformatted(key.c_str());
                                    if ( !hasKey ) {
                                        ImGui::PopStyleColor();
                                    }
                                    if ( ImGui::IsItemHovered() ) {
                                        ImGui::SetTooltip(
                                            "%s",
                                            "RM/IMD 行参数，二进制 int32。");
                                    }

                                    // 操作列根据字段实际存在性选择按钮或占位符。
                                    ImGui::TableNextColumn();
                                    // InputInt 使用局部 int，变化后再转为 int32
                                    // 文本。
                                    int valueInput = value;
                                    ImGui::SetNextItemWidth(-1.0f);
                                    if ( ImGui::InputInt(
                                             fmt::format(
                                                 "##nm_rm_{}_{}", groupIdx, key)
                                                 .c_str(),
                                             &valueInput,
                                             1,
                                             100) ) {
                                        // 修改批量覆盖组内全部音符的
                                        // Parameter。
                                        for ( auto e : group.entities ) {
                                            auto& nc =
                                                registry
                                                    .get<Logic::NoteComponent>(
                                                        e);
                                            // 以十进制字符串保持 NoteMetadata
                                            // 存储协议。
                                            nc.m_metadata
                                                .note_properties[metaType]
                                                                [key] =
                                                std::to_string(
                                                    static_cast<int32_t>(
                                                        valueInput));
                                        }
                                    }
                                    if ( hasKey &&
                                         !parseInt32Metadata(refProps.at(key))
                                              .has_value() ) {
                                        // 保留旧值时明确提示其不是合法 int32。
                                        ImGui::TextColored(
                                            ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                            "%s",
                                            "当前值不是合法 "
                                            "int32，编辑后会修正。");
                                    }

                                    ImGui::TableNextColumn();
                                    if ( hasKey ) {
                                        // 只有实际存在 Parameter
                                        // 时显示清除按钮。
                                        if ( ::MMM::UI::FeedbackButton(
                                                 fmt::format("{}##clr_rm_{}",
                                                             TR("ui.edit."
                                                                "note_metadata."
                                                                "clear_btn")
                                                                 .data(),
                                                             groupIdx)
                                                     .c_str()) ) {
                                            // 清除操作批量作用当前指纹组。
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                auto& m = nc.m_metadata
                                                              .note_properties
                                                                  [metaType];
                                                m.erase(key);
                                                if ( m.empty() ) {
                                                    // 删除最后字段后移除空 RM
                                                    // 格式表。
                                                    nc.m_metadata
                                                        .note_properties.erase(
                                                            metaType);
                                                }
                                            }
                                        }
                                    } else {
                                        // 缺失字段用占位符保持操作列布局。
                                        ImGui::TextDisabled("-");
                                    }
                                    // 结束当前组 RM 单字段表格。
                                    ImGui::EndTable();
                                }

                                ImGui::EndTabItem();
                            }

                            // 恢复当前组格式页签栈。
                            ImGui::EndTabBar();
                        }
                    }

                    // 与组开头 PushID 配对，避免影响下一组控件。
                    ImGui::PopID();

                    if ( groupIdx < static_cast<int>(groups.size()) ) {
                        // 组间留出垂直间距，最后一组不添加尾部空白。
                        ImGui::Spacing();
                    }
                }

                // 结束可滚动元数据组子窗口。
                ImGui::EndChild();
            }
        }
    }
    if ( opened ) {
        // JSON 辅助弹窗只在父窗口 Begin 成功时绘制。
        renderMetadataJsonEditorPopup(dpiScale);
    }
    // 即使 Begin 返回 false 也必须调用 End。
    ImGui::End();

    // 与窗口开头六项 PushStyleVar 精确配对。
    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
