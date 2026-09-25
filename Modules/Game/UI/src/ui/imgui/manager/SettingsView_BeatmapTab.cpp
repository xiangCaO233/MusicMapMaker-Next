#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>

namespace MMM::UI
{
namespace
{

/// @brief 绘制带减号、当前值和加号的紧凑轨道数步进器。
/// @param bounds 设置项为控件预留的 Clay 边界。
/// @param trackCount 当前持久化轨道数量。
/// @param minimumTrackCount 允许减少到的最小数量。
/// @param id 当前设置项的稳定 ImGui ID。
/// @param removeTooltip 减号按钮提示。
/// @param addTooltip 加号按钮提示。
/// @param updateCount 合法点击后接收相邻目标数量的回调。
///
/// 控件采用“减号—当前值—加号”顺序，不允许直接文本输入。这样每次交互只产生
/// 相邻数量，命令层可以逐次维护新增或删除轨道所需的不变量。
///
/// `minimumTrackCount` 由调用场景决定：草稿轨至少保留一轨，BGM 轨允许为零。
/// 最大值受 `int32_t` 上限保护，避免加一发生有符号溢出。
///
/// 按钮即使禁用也保留 Tooltip 悬浮命中，让用户能够理解当前边界。回调只在有效
/// 点击后执行，不会因为禁用状态或普通悬浮产生命令。
/// @warning 设置窗口渲染期间每帧调用；回调只允许入队，不得同步修改逻辑状态。
template<typename UpdateCount>
void drawTrackCountStepper(Clay_BoundingBox bounds, std::int32_t trackCount,
                           std::int32_t minimumTrackCount, const char* id,
                           const char* removeTooltip, const char* addTooltip,
                           UpdateCount&& updateCount)
{
    // 以当前 frame 高度构造方形按钮，使控件随主题和 DPI 一同缩放。
    const float buttonSize = ImGui::GetFrameHeight();
    // 主题样式只用于计算安全内边距，不在此 helper 中永久修改。
    const auto& style = ImGui::GetStyle();
    // 同时测量加减号，确保二者使用一致且能够完整显示的横向空间。
    const float buttonGlyphWidth =
        std::max(ImGui::CalcTextSize("-").x, ImGui::CalcTextSize("+").x);
    // 预留一像素安全量，并防止极小按钮推导出负内边距。
    const float maxHorizontalPadding =
        std::max(0.0F, (buttonSize - buttonGlyphWidth) * 0.5F - 1.0F);
    // 纵向内边距沿用主题，横向值只在主题过宽时收窄。
    const ImVec2 compactButtonPadding{
        std::min(style.FramePadding.x, maxHorizontalPadding),
        style.FramePadding.y,
    };
    /// 绘制一个采用紧凑内边距的方形反馈按钮。
    /// @param label 包含可见符号和唯一隐藏 ID 的 ImGui 标签。
    /// @return 用户在当前帧有效点击时返回 true。
    ///
    /// 方形按钮只收窄横向内边距，避免大 FramePadding 主题裁掉加减号。
    /// 两项样式在绘制后立即出栈，不影响中间数值文本或页面其他按钮。
    const auto drawButton = [&](const char* label) {
        // 同时压入 padding 和文字居中规则，保证符号位于方形中央。
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, compactButtonPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5F, 0.5F));
        const bool clicked =
            ::MMM::UI::FeedbackButton(label, ImVec2(buttonSize, buttonSize));
        // 两项样式严格配对恢复，点击结果以值返回。
        ImGui::PopStyleVar(2);
        return clicked;
    };

    // Clay 提供绝对屏幕矩形，ImGui 游标从该行的值列起点开始。
    ImGui::SetCursorScreenPos({ bounds.x, bounds.y });
    // 外层稳定 ID 隔离草稿轨与 BGM 轨内部复用的按钮标签。
    ImGui::PushID(id);
    // 达到场景最小值时只禁用减号，数值与加号仍保持可用。
    ImGui::BeginDisabled(trackCount <= minimumTrackCount);
    if ( drawButton("-##RemovePersistentTrack") ) {
        // 目标值严格为当前值减一，由命令回调异步应用。
        updateCount(trackCount - 1);
    }
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) ) {
        // AllowWhenDisabled 保证边界原因仍可通过提示传达。
        ImGui::SetTooltip("%s", removeTooltip);
    }

    // 当前数量与两个按钮保持同一行，并按 frame 内边距垂直对齐。
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d", trackCount);
    ImGui::SameLine();

    // 仅在加一仍可由 int32_t 表示时启用加号。
    const bool canAdd = trackCount < std::numeric_limits<std::int32_t>::max();
    ImGui::BeginDisabled(!canAdd);
    if ( drawButton("+##AddPersistentTrack") ) {
        // 目标值严格为当前值加一，不在 UI 内直接重排轨道内容。
        updateCount(trackCount + 1);
    }
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) ) {
        // 加号同样在上限禁用时继续显示解释性提示。
        ImGui::SetTooltip("%s", addTooltip);
    }
    // 恢复外层 ID 栈，避免影响后续设置项。
    ImGui::PopID();
}

}  // namespace

/// @brief 渲染谱面设置页。
///
/// 本页编辑当前活动谱面的元数据，包括标题信息、封面类型、推荐 BPM、轨道数量和
/// 工程内资源引用。普通字段先写入 `m_editingMeta` 草稿，本帧发生变化时再通过
/// `CmdUpdateBeatmapMetadata` 提交给逻辑线程。
///
/// 草稿轨和 BGM 轨数量使用各自的专用命令，不写入元数据草稿。这些命令可能需要
/// 调整持久化轨道结构，因此由 EditorEngine 队列串行处理。
///
/// 页面持有 session
/// 递归互斥锁直到布局回调执行完成，确保活动谱面、SessionContext
/// 和元数据引用在本帧内稳定。不得把这些引用保存到函数之外。
///
/// `.imd` 格式只允许编辑项目能够安全表达的字段。受格式约束的标题、封面类型、
/// BPM 和资源控件会禁用，但版本、轨道信息和只读时长仍按现有能力展示。
///
/// 工程资源路径统一优先保存为工程根目录相对路径。兼容旧数据时，可识别误带工程
/// 根目录名称的相对路径；绝对路径只用于无法相对化或没有 Project 上下文的显示。
///
/// 资源下拉框只枚举工程内允许扩展名的普通文件。文件不存在时以警告色显示当前
/// 引用，但不会在无用户选择时清空数据，便于工程迁移后恢复资源。
///
/// 页面不直接保存 Project 或 BeatMap 文件，也不加载音频、图片与视频内容。
/// 资源扫描只生成候选相对路径，实际消费由音频和渲染模块负责。
///
/// 元数据草稿以谱面路径变化作为切换信号。同一路径下，外部工具可能同步修改轨道
/// 数、封面和背景字段，因此这些结构字段每帧从当前谱面刷新；正在输入的标题文本
/// 则保留在草稿中，避免其他 UI 刷新导致编辑内容丢失。
///
/// 路径兼容 helper 不使用异常接口。权限错误、文件被并发删除和跨卷相对化失败都
/// 通过 `std::error_code` 转为候选缺失或回退显示，不能让设置页中断渲染。
///
/// 文件扩展名比较统一转换为小写。此处的白名单只控制下拉候选可见性，不承担
/// 媒体内容验证；解码器仍需独立检查实际文件格式和内容。
///
/// 资源组合框关闭时不得递归扫描工程目录。打开时生成的候选只在当前布局回调内
/// 使用，不缓存文件迭代器，也不把绝对路径写入工程资源字段。
///
/// 所有 ImGui 样式栈、禁用栈、ID 栈和裁剪栈必须在同一回调内成对恢复。特别是
/// 失效资源的警告色会在弹窗打开前提前出栈，避免候选列表全部继承警告颜色。
/// @warning UI 热路径：设置窗口打开且谱面页可见时每帧执行，并持有 session 锁；
/// 资源目录递归扫描只应发生在对应组合框打开时，不得移动到无条件路径。
void SettingsView::drawBeatmapSettings()
{
    // Engine 管理活动会话、当前工程和逻辑命令队列。
    // 本页不创建会话，也不改变当前工程选择。
    auto& engine = Logic::EditorEngine::instance();
    // 锁覆盖本函数与 Clay 回调执行，防止活动谱面在绘制中途被替换。
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    // shared_ptr 副本保证会话在当前设置页绘制期间存活。
    auto session = engine.getActiveSession();
    // Project 是 Engine 持有的非拥有观察指针，可为空以兼容独立谱面上下文。
    auto* project = engine.getCurrentProject();

    if ( !session || !session->getContext().currentBeatmap ) {
        // 没有活动谱面时只显示明确提示，不登记任何捕获空上下文的回调。
        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::TextColored(
            dangerCol, "%s", TR("ui.settings.beatmap.no_beatmap").data());
        return;
    }

    // 锁保护下借用当前谱面，路径作为切换草稿的稳定身份线索。
    auto&       beatmap = *session->getContext().currentBeatmap;
    std::string currentPath =
        Config::pathToUtf8(beatmap.m_baseMapMetadata.map_path);
    if ( m_lastBeatmapPath != currentPath ) {
        // 切换谱面时完整刷新编辑草稿，避免上一谱面的未提交文本串入新谱面。
        m_editingMeta     = beatmap.m_baseMapMetadata;
        m_lastBeatmapPath = currentPath;
    } else {
        const auto& currentMeta = beatmap.m_baseMapMetadata;
        // 右侧工具栏和原始元数据编辑器可直接修改这些字段，设置页需要同步外部变更。
        // 这里只同步会由其他 UI
        // 即时修改的结构字段，文本输入草稿继续由本页保留。
        m_editingMeta.track_count     = currentMeta.track_count;
        m_editingMeta.main_cover_path = currentMeta.main_cover_path;
        m_editingMeta.cover_path      = currentMeta.cover_path;
        m_editingMeta.cover_type      = currentMeta.cover_type;
        m_editingMeta.video_starttime = currentMeta.video_starttime;
        m_editingMeta.bgxoffset       = currentMeta.bgxoffset;
        m_editingMeta.bgyoffset       = currentMeta.bgyoffset;
    }
    // 文本草稿与外部结构字段同步完成后，所有控件读取同一份 m_editingMeta。
    // 后续控件统一编辑草稿引用，changed 决定是否提交完整元数据命令。
    // changed 初始为 false，纯展示、折叠和资源悬浮不会触发命令。
    auto& meta    = m_editingMeta;
    bool  changed = false;

    /// 从旧式相对路径开头剥离重复的工程目录名。
    /// @param path 待检查的资源路径。
    /// @return 成功识别时返回剥离后的规范路径，否则返回空路径。
    ///
    /// 仅处理相对路径，且第一段必须与当前工程根目录文件名完全一致。返回空路径
    /// 同时表示“不适用”，调用方还需结合原路径选择回退行为。
    auto stripProjectFolderPrefix = [&](const std::filesystem::path& path) {
        // 缺少工程、根目录、输入或遇到绝对路径时不执行兼容剥离。
        if ( !project || project->m_projectRoot.empty() || path.empty() ||
             path.is_absolute() ) {
            return std::filesystem::path{};
        }

        // 第一段必须正好是工程目录名，避免误删普通资源子目录。
        auto iterator = path.begin();
        if ( iterator == path.end() ||
             *iterator != project->m_projectRoot.filename() ) {
            return std::filesystem::path{};
        }

        // 按路径组件重新拼接剩余部分，保持平台分隔符语义。
        std::filesystem::path stripped;
        ++iterator;
        for ( ; iterator != path.end(); ++iterator ) {
            stripped /= *iterator;
        }
        // 词法规范化不访问文件系统，仅清理点号和冗余分隔组件。
        return stripped.lexically_normal();
    };

    /// 规范化准备写入元数据的工程资源路径。
    /// @param path 用户选择或旧元数据提供的路径。
    /// @return 优先返回可验证存在的工程相对路径，否则返回原路径的词法规范形式。
    auto normalizeProjectResourcePath = [&](const std::filesystem::path& path) {
        // 绝对路径和无工程上下文的路径无法安全相对化，仅做词法规范化。
        if ( path.empty() || path.is_absolute() || !project ) {
            return path.lexically_normal();
        }

        // 对误带工程目录名的旧相对路径，只有目标确实存在时才采用剥离结果。
        const auto stripped = stripProjectFolderPrefix(path);
        if ( !stripped.empty() ) {
            // 使用 error_code 避免权限或损坏路径通过异常打断 UI 帧。
            std::error_code filesystemError;
            if ( std::filesystem::exists(project->m_projectRoot / stripped,
                                         filesystemError) &&
                 !filesystemError ) {
                return stripped.lexically_normal();
            }
        }
        // 无法验证兼容路径时保留原始相对语义，避免静默指向其他文件。
        return path.lexically_normal();
    };

    /// 把元数据中的资源引用解析成用于存在性检查的实际路径。
    /// @param path 可能为空、绝对或相对的资源路径。
    /// @return 首选存在的实际路径；均不存在时返回直接拼接后的候选路径。
    ///
    /// 解析顺序先尊重新式工程相对路径，再尝试剥离旧式工程目录前缀。返回不存在的
    /// directPath 可让调用方继续显示原引用并用警告色标记。
    auto resolveProjectPath = [&](const std::filesystem::path& path) {
        // 空值、绝对路径或无工程上下文时无需拼接工程根目录。
        if ( path.empty() || path.is_absolute() || !project ) {
            return path.lexically_normal();
        }

        // 新式路径直接相对工程根目录解析，优先级高于兼容分支。
        auto directPath = (project->m_projectRoot / path).lexically_normal();
        // error_code 路径避免文件系统异常进入每帧 UI 调用链。
        std::error_code filesystemError;
        if ( std::filesystem::exists(directPath, filesystemError) &&
             !filesystemError ) {
            return directPath;
        }

        // 直接路径不存在时，尝试旧式重复工程目录前缀。
        const auto stripped = stripProjectFolderPrefix(path);
        if ( !stripped.empty() ) {
            // 兼容候选仍限制在当前工程根目录之下。
            auto strippedPath =
                (project->m_projectRoot / stripped).lexically_normal();
            // 清除第一次 exists 的错误，再独立检查兼容候选。
            filesystemError.clear();
            if ( std::filesystem::exists(strippedPath, filesystemError) &&
                 !filesystemError ) {
                return strippedPath;
            }
        }

        // 均不存在时返回首选候选，调用方据此生成一致的警告状态。
        return directPath;
    };

    /// 生成适合在设置页中展示的工程资源路径。
    /// @param path 元数据中保存的资源引用。
    /// @return 优先为工程相对 UTF-8 路径，无法相对化时为绝对 UTF-8 路径。
    auto displayProjectPath = [&](const std::filesystem::path& path) {
        // 空引用显示为空字符串，不制造当前目录等误导性文本。
        if ( path.empty() ) return std::string{};
        if ( !project || path.is_relative() ) {
            // 相对路径先应用旧格式兼容规范化，再转换为 ImGui 使用的 UTF-8。
            return Config::pathToUtf8(normalizeProjectResourcePath(path));
        }

        // 绝对路径位于工程根目录内时缩短成相对形式，便于阅读和比较。
        std::error_code ec;
        auto            relativePath =
            std::filesystem::relative(path, project->m_projectRoot, ec);
        if ( !ec && !relativePath.empty() ) {
            return Config::pathToUtf8(relativePath);
        }
        // 跨卷、权限错误或空相对结果时保留完整路径用于诊断。
        return Config::pathToUtf8(path);
    };

    /// 收集工程根目录中扩展名符合白名单的普通文件。
    /// @param allowedExtensions 小写且包含点号的扩展名集合。
    /// @return 相对于工程根目录的 UTF-8 路径列表。
    ///
    /// 遍历跳过无权限目录和单个错误项，不因一个损坏链接或竞态删除而终止整个
    /// 下拉框。调用只应位于已打开的资源组合框中，避免关闭状态每帧扫描工程。
    auto collectProjectResources =
        [&](std::initializer_list<std::string_view> allowedExtensions) {
            // 无 Project 时没有权威资源根目录，返回空候选集合。
            std::vector<std::string> resources;
            if ( !project ) return resources;

            // skip_permission_denied 配合 error_code 保持 UI 无异常机制。
            std::error_code                               filesystemError;
            std::filesystem::recursive_directory_iterator it(
                project->m_projectRoot,
                std::filesystem::directory_options::skip_permission_denied,
                filesystemError);
            std::filesystem::recursive_directory_iterator end;
            // 根迭代器本身创建失败时直接返回空列表。
            if ( filesystemError ) return resources;

            for ( ; it != end; it.increment(filesystemError) ) {
                if ( filesystemError ) {
                    // 清除当前迭代错误并继续，让其他可访问资源仍能显示。
                    filesystemError.clear();
                    continue;
                }
                if ( !it->is_regular_file(filesystemError) ||
                     filesystemError ) {
                    // 目录、特殊文件和状态查询失败项都不作为可选资源。
                    filesystemError.clear();
                    continue;
                }

                // 扩展名统一转成小写，实现大小写不敏感的格式筛选。
                auto ext = Config::pathToUtf8(it->path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
                    return static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c)));
                });
                // 白名单由调用场景提供，音频、图像与视频候选彼此隔离。
                const bool accepted = std::any_of(
                    allowedExtensions.begin(),
                    allowedExtensions.end(),
                    [&](std::string_view allowed) { return ext == allowed; });
                if ( !accepted ) continue;

                // 持久化候选转换为工程相对路径，避免工程移动后引用失效。
                auto relativePath = std::filesystem::relative(
                    it->path(), project->m_projectRoot, filesystemError);
                if ( filesystemError ) {
                    // 无法相对化的单个资源被忽略，不泄漏绝对路径到元数据候选。
                    filesystemError.clear();
                    continue;
                }
                // 下拉框只需要 UTF-8 相对路径，不在此处读取资源内容。
                resources.push_back(Config::pathToUtf8(relativePath));
            }
            // 保留目录迭代顺序；本页不额外排序或去重。
            // 调用方应把结果限制在当前打开的组合框作用域内使用。
            return resources;
        };

    // IMD 检测只依据当前谱面文件扩展名，用于控制格式不支持的编辑项。
    bool isImd = false;
    if ( !beatmap.m_baseMapMetadata.map_path.empty() ) {
        // 扩展名转换为 UTF-8 小写形式，兼容大小写不同的文件名。
        auto ext =
            Config::pathToUtf8(beatmap.m_baseMapMetadata.map_path.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if ( ext == ".imd" ) {
            // 禁用状态仅阻止 UI 写入，不改变已经加载的元数据。
            isImd = true;
        }
    }

    // 根 VBox 每帧重建布局关系，设置草稿和折叠状态独立保留。
    m_contentVBox.clear();
    // 统一间距与内边距使谱面页和其他设置页保持一致。
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    // 行索引覆盖标题与设置项，section 索引只覆盖展开分组。
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 使用布局缓存中的统一标签列宽，避免设置页每帧重复测量全部标签。
    // 当前内容缩放参与缓存选择，DPI 改变后标签和值列仍保持对齐。
    const float maxLabelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());

    /// 创建可折叠的谱面设置分组标题。
    /// @param label 本地化标题，也参与当前页面的稳定 ID 构造。
    /// @param defaultOpen StateStorage 无记录时使用的首次展开状态。
    /// @return 展开时返回当前帧内容 section，折叠时返回空指针。
    ///
    /// 折叠状态属于 ImGui 会话，不进入谱面元数据。返回指针由 SettingsView 缓存
    /// 所有，只能在当前布局构建与渲染期间使用。
    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        // 页面前缀、行、节和标题共同隔离各分组的控件状态。
        std::string baseIdStr = "MAP_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        // 提前读取展开状态，决定本帧是否登记该组内部控件。
        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        // 标题独占一行并采用当前 ImGui frame 高度以适应 DPI。
        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                // Clay 分配绝对矩形，ImGui 游标必须移动到标题起点。
                ImGui::SetCursorScreenPos({ r.x, r.y });
                // 悬浮与按下色从当前主题 Header 色轻微增亮。
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                // 三项颜色压栈与回调末尾 PopStyleColor(3) 配对。
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                // 临时收窄 WorkRect，避免标题超出 Clay 分配的值域宽度。
                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                // 标题取消窗口内边距以完整覆盖这一行。
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                // 数值 ID 借用指针重载传入，不代表可解引用地址。
                // CollapsingHeader 无嵌套内容，不需要配对 TreePop。
                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                // 回调结束前恢复样式和 WorkRect，避免污染后续控件。
                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                // 点击结果写回窗口状态，下一帧据此重建内容 section。
                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        // 标题布局始终存在，内容布局只在展开分支加入根 VBox。
        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            // 装饰 section 统一提供背景、间距和内边距。
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            // Fit 高度紧贴实际设置行，根 VBox 负责分组间距。
            return &sec;
        }
        // 折叠分组返回空指针，不创建隐藏控件或占用高度。
        return nullptr;
    };

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.beatmap.info").data(), true) ) {
        // 基本信息组编辑标题、艺术家、谱师、难度版本，并展示只读文件路径。
        // 各字符串保持原始用户文本，不在 UI 层进行裁剪、转义或大小写归一化。
        // 采用统一标签宽度，使所有文本输入和值展示共享对齐基线。

        /// 登记一个编辑元数据字符串的单行输入控件。
        /// @param labelPtr 设置行可见标签，同时作为当前控件 ID 作用域。
        /// @param valRef 直接连接 `m_editingMeta` 中的目标字符串。
        /// @param enabled false 时保留显示但禁止用户编辑。
        ///
        /// ImGui 使用固定 256 字节临时缓冲，输入发生变化后再回写 std::string。
        /// 禁用状态用于 IMD 不支持的字段，不会清空已有文本。
        auto DrawInput = [&](const char*  labelPtr,
                             std::string& valRef,
                             bool         enabled = true) {
            addSettingItem(
                *sec,
                rowIndex,
                labelPtr,
                maxLabelW,
                [labelPtr, &valRef = valRef, &changed, enabled](
                    Clay_BoundingBox r, bool) {
                    // 可见标签建立独立 ID 作用域，内部统一复用 `##Input`。
                    ImGui::PushID(labelPtr);
                    if ( !enabled ) {
                        // 禁用只包围当前输入框，后续设置行恢复正常状态。
                        ImGui::BeginDisabled();
                    }
                    // 每帧从草稿复制到有界缓冲，并显式保证末尾 NUL。
                    char buf[256];
                    strncpy(buf, valRef.c_str(), sizeof(buf));
                    buf[sizeof(buf) - 1] = '\0';
                    // 输入框占满 Clay 分配的值列宽度。
                    ImGui::SetNextItemWidth(r.width);
                    if ( ImGui::InputText("##Input", buf, sizeof(buf)) ) {
                        valRef = buf;
                        // 文本实际改变时才触发元数据命令。
                        changed = true;
                    }
                    if ( !enabled ) {
                        // 与当前控件的 BeginDisabled 严格配对。
                        ImGui::EndDisabled();
                    }
                    // 恢复标签 ID 作用域，避免影响下一设置项。
                    ImGui::PopID();
                });
        };

        // IMD 仅保留版本可编辑；其他文本字段按格式能力禁用。
        DrawInput(
            TR_CACHE("ui.settings.beatmap.name").data(), meta.name, !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.title").data(), meta.title, !isImd);
        DrawInput(TR_CACHE("ui.settings.beatmap.title_unicode").data(),
                  meta.title_unicode,
                  !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.artist").data(), meta.artist, !isImd);
        DrawInput(TR_CACHE("ui.settings.beatmap.artist_unicode").data(),
                  meta.artist_unicode,
                  !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.mapper").data(), meta.author, !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.version").data(), meta.version, true);
        // 难度版本在 IMD 中仍允许编辑，因此显式传入 true。

        // 路径行同时准备简短相对文本与悬浮提示使用的实际路径。
        std::string relativePathStr = "";
        std::string absolutePathStr = "";
        if ( !beatmap.m_baseMapMetadata.map_path.empty() ) {
            // resolveProjectPath 兼容新式与旧式工程相对路径。
            auto absolutePath =
                resolveProjectPath(beatmap.m_baseMapMetadata.map_path);
            absolutePathStr = Config::pathToUtf8(absolutePath);
            if ( project ) {
                // 有 Project 时优先展示可迁移的相对形式。
                relativePathStr =
                    displayProjectPath(beatmap.m_baseMapMetadata.map_path);
            } else {
                // 独立谱面上下文没有相对基准，显示实际路径。
                relativePathStr = absolutePathStr;
            }
        }

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.path").data(),
            maxLabelW,
            [relativePathStr, absolutePathStr](Clay_BoundingBox r, bool) {
                // 路径字符串按值捕获，保证布局回调执行时缓冲仍有效。
                ImVec2 cursorPos = ImGui::GetCursorScreenPos();
                ImVec2 textSize  = ImGui::CalcTextSize(relativePathStr.c_str());

                // 超宽相对路径才计算滚动位移，完整可见时保持静止。
                float offset       = 0.0f;
                float visibleWidth = r.width;

                if ( textSize.x > visibleWidth ) {
                    // 额外留出尾部间隔，使往复运动的终点容易辨认。
                    float scrollRange = textSize.x - visibleWidth + 40.0f;
                    // ImGui 单调时间只驱动视觉动画，不修改元数据。
                    float time = (float)ImGui::GetTime();
                    // 正弦曲线产生平滑往复滚动，再钳制形成两端停顿。
                    float t = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
                    t       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
                    offset  = t * scrollRange;
                }

                // 依据 frame 高度计算文本垂直居中位置。
                float textH   = ImGui::GetFontSize();
                float widgetH = ImGui::GetFrameHeight();
                float offsetY = (widgetH - textH) * 0.5f;

                // 剪切到 Clay 值列，路径不会覆盖左侧标签或相邻行。
                ImGui::PushClipRect(
                    cursorPos,
                    ImVec2(cursorPos.x + r.width, cursorPos.y + widgetH),
                    true);

                // 透明 Dummy 提供完整值列的 hover 区域，用于显示绝对路径提示。
                ImGui::SetCursorScreenPos(cursorPos);
                ImGui::Dummy(ImVec2(r.width, widgetH));
                if ( ImGui::IsItemHovered() ) {
                    // Tooltip 保留完整实际路径，滚动文本只负责紧凑展示。
                    ImGui::SetTooltip("%s", absolutePathStr.c_str());
                }

                // 直接提交到窗口 DrawList，使用当前主题文字颜色。
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(cursorPos.x - offset, cursorPos.y + offsetY),
                    ImGui::GetColorU32(ImGuiCol_Text),
                    relativePathStr.c_str());

                // 恢复父窗口裁剪范围，避免截断下一行。
                ImGui::PopClipRect();
            });
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.beatmap.cover_type").data(), true) ) {
        // 封面类型组编辑背景媒介、视频起始时间以及二维背景偏移。
        // 切换类型只改变解释方式，不自动清除另一类型下已有资源路径。
        // 采用统一标签宽度，使动态出现的视频起始行保持对齐。

        if ( isImd ) {
            // IMD 格式不支持这些字段的安全回写，整组保持只读展示。
            ImGui::BeginDisabled();
        }

        // cover_type 的整数值与图片、视频两个稳定枚举值对应。
        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.beatmap.cover_type").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.beatmap.cover_type.image").data(), 0 },
              { TR_CACHE("ui.settings.beatmap.cover_type.video").data(), 1 } },
            (int&)meta.cover_type,
            changed);
        // 单选 helper 只改草稿枚举与 changed，不立即刷新背景资源。

        if ( meta.cover_type == MMM::CoverType::VIDEO ) {
            // 视频起始时间只在视频背景语义下显示，切回图片时保留原值。
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.beatmap.video_start").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               // InputInt
                               // 直接编辑元数据整数单位，消费者负责时间解释。
                               ImGui::SetNextItemWidth(r.width);
                               if ( ImGui::InputInt("##VideoStart",
                                                    &meta.video_starttime) ) {
                                   // 实际输入变化才提交完整元数据命令。
                                   changed = true;
                               }
                           });
        }

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.bg_offset").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // ImGui 双整数控件使用局部连续数组，再分别回写 X/Y 字段。
                int offsets[2] = { meta.bgxoffset, meta.bgyoffset };
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragInt2("##BgOffset", offsets) ) {
                    // 两个方向作为一次交互共同提交，避免中间不一致。
                    meta.bgxoffset = offsets[0];
                    meta.bgyoffset = offsets[1];
                    changed        = true;
                }
            });

        if ( isImd ) {
            // 与组首 BeginDisabled 配对，恢复后续偏好设置的交互状态。
            ImGui::EndDisabled();
        }
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.beatmap.preference").data(), true) ) {
        // 谱面偏好组包含推荐 BPM、主轨数、持久化草稿/BGM 轨数和只读时长。
        // 时长来自当前元数据，仅用于展示，不接受在设置页直接覆盖。
        // 主轨数属于元数据；草稿轨和 BGM 轨属于 SessionContext 持久化结构。

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.bpm").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                if ( isImd ) {
                    // IMD 推荐 BPM 在此页不可安全写回，仅保留可见值。
                    ImGui::BeginDisabled();
                }
                // 使用 float 适配 ImGui API，提交时恢复元数据 double 类型。
                float bpm = (float)meta.preference_bpm;
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat(
                         "##BPM", &bpm, 0.1f, -1.0f, 1000.0f, "%.2f") ) {
                    meta.preference_bpm = (double)bpm;
                    // -1 保留格式约定中的未指定语义，上限限制异常输入。
                    changed = true;
                }
                if ( isImd ) {
                    // 禁用作用域只包围 BPM 控件，不影响可编辑轨道字段。
                    ImGui::EndDisabled();
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.tracks").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 主轨数仍是元数据字段，由统一 CmdUpdateBeatmapMetadata 提交。
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::InputInt("##Tracks", &meta.track_count) ) {
                    // 具体有效性和结构同步由逻辑命令处理，本页只记录输入变化。
                    changed = true;
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.draft_tracks").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // SessionContext 是当前显示数量的权威来源，至少保留一条草稿轨。
                const auto draftTrackCount =
                    std::max(1, session->getContext().draftTrackCount);
                drawTrackCountStepper(
                    r,
                    draftTrackCount,
                    1,
                    "DraftTrackCount",
                    TR("ui.settings.beatmap.draft_tracks_remove").data(),
                    TR("ui.settings.beatmap.draft_tracks_add").data(),
                    [&](std::int32_t count) {
                        // 回调只向 Engine 入队，避免持锁 UI 路径直接重建轨道。
                        engine.pushCommand(Logic::CmdUpdateDraftTrackCount{
                            count,
                        });
                        // 当前帧显示值仍来自锁定的
                        // SessionContext，下一帧读取命令结果。
                    });
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.bgm_tracks").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // BGM 轨允许为零，负值上下文按零显示以保护步进边界。
                const auto bgmTrackCount =
                    std::max(0, session->getContext().bgmTrackCount);
                drawTrackCountStepper(
                    r,
                    bgmTrackCount,
                    0,
                    "BgmTrackCount",
                    TR("ui.settings.beatmap.bgm_tracks_remove").data(),
                    TR("ui.settings.beatmap.bgm_tracks_add").data(),
                    [&](std::int32_t count) {
                        // 专用命令维护 BGM 轨资源与 SessionContext 的一致性。
                        engine.pushCommand(Logic::CmdUpdateBgmTrackCount{
                            count,
                        });
                        // 不提前修改上下文，避免 UI
                        // 与逻辑线程各自维护一份计数。
                    });
            });

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.beatmap.length").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // map_length 以毫秒存储，格式化工具接收秒值。
                           const auto lengthText =
                               MMM::UI::Utils::formatCanvasDuration(
                                   meta.map_length / 1000.0);
                           // 时长为只读派生信息，不设置 changed。
                           ImGui::SetCursorScreenPos({ r.x, r.y });
                           ImGui::TextUnformatted(lengthText.c_str());
                       });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.beatmap.resource").data(),
                               true) ) {
        // 资源组只保存工程内引用，不读取或预览实际媒体内容。
        // 当前引用即使不在候选列表中也继续显示，以保留可修复信息。
        // 采用统一标签宽度，使音频、封面与背景组合框对齐。
        // 每行右侧导入按钮把外部资源复制到当前工程并绑定此谱面。
        // 下拉框仍只列工程内候选，失效引用可以通过导入按钮修复。
        // 背景行仅导入图片，导入成功后同步切换背景媒体类型。

        if ( isImd ) {
            // IMD 资源字段在此页不可安全回写，整组保留只读展示。
            // 新增导入按钮同处禁用范围，不能绕过格式写入限制。
            ImGui::BeginDisabled();
        }

        // 导入按钮与组合框并排；宽度取两种文案的较大值，三行值列保持对齐。
        // 这里只记录请求和当时的项目/谱面身份；文件选择器在会话锁外打开。
        // 宽度随当前语言与字体变化，SettingsView 的最小宽度测量同步覆盖。
        // 长路径只裁剪组合框预览，不能挤掉固定宽的可点击导入按钮。
        const char* audioImportLabel =
            TR_CACHE("ui.settings.beatmap.import_audio").data();
        const char* imageImportLabel =
            TR_CACHE("ui.settings.beatmap.import_image").data();
        const float importButtonWidth =
            std::max(ImGui::CalcTextSize(audioImportLabel).x,
                     ImGui::CalcTextSize(imageImportLabel).x) +
            ImGui::GetStyle().FramePadding.x * 2.0F;
        /// @brief 为资源行的下拉框预留右侧导入按钮宽度。
        /// @note 极窄窗口仍保留正宽度，避免负值传给 ImGui 布局。
        const auto resourceComboWidth = [&](Clay_BoundingBox bounds) {
            return std::max(1.0F,
                            bounds.width - importButtonWidth -
                                ImGui::GetStyle().ItemSpacing.x);
        };
        /// @brief 记录资源导入请求，实际选择与复制交给锁外的低频路径。
        /// @note 目标枚举区分同名的封面与背景“导入图片”按钮。
        const auto drawResourceImportButton =
            [&](const char* label, BeatmapResourceTarget target) {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(target));
                ImGui::BeginDisabled(!project);
                // 无工程时资源缺少持久归属，按钮保留占位但不能触发。
                if ( ::MMM::UI::FeedbackButton(
                         label, ImVec2(importButtonWidth, 0.0F)) ) {
                    m_beatmapResourceTarget     = target;
                    m_openBeatmapResourcePicker = true;
                    m_resourceImportProjectRoot = project->m_projectRoot;
                    m_resourceImportBeatmapPath =
                        beatmap.m_baseMapMetadata.map_path;
                    // 对话框可能跨帧返回，结果必须与这两项身份重新比较。
                    m_beatmapResourceImportError.clear();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            };

        // 音频选择优先使用外部格式提示字段，并限定为 Project 登记的主音频资源。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.audio").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                /// @brief 歌曲文件提示只服务于外部格式，不决定实际播放时间线。
                /// 空提示时回退到主音频路径，兼容本项目原生元数据。
                const auto& audioHint        = meta.song_file_hint.empty()
                                                   ? meta.main_audio_path
                                                   : meta.song_file_hint;
                std::string currentAudioPath = displayProjectPath(audioHint);
                // 预览与比较都使用规范化后的显示路径。
                std::string audioPreview = currentAudioPath;

                // 当前引用不存在时以警告色提示，但不自动清空草稿字段。
                bool audioExists =
                    project &&
                    std::filesystem::exists(resolveProjectPath(audioHint));
                bool audioPushed = false;
                if ( !audioExists && !currentAudioPath.empty() ) {
                    // 仅非空失效引用需要警告，未选择资源保持普通颜色。
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    audioPushed = true;
                }

                // 组合框占满值列；打开后才遍历 Project 已登记音频。
                ImGui::SetNextItemWidth(resourceComboWidth(r));
                if ( ::MMM::UI::FeedbackBeginCombo("##AudioCombo",
                                                   audioPreview.c_str()) ) {
                    if ( audioPushed ) {
                        // 弹窗内容不继承失效预览的警告色，立即恢复文本样式。
                        ImGui::PopStyleColor();
                        audioPushed = false;
                    }
                    if ( project ) {
                        // 只列出 Main 类型，BGM 和音效资源不能成为谱面主音频。
                        for ( const auto& res : project->m_audioResources ) {
                            if ( res.m_type != MMM::AudioTrackType::Main )
                                // 非主音频保留在 Project
                                // 清单中，但不出现在此下拉框。
                                continue;
                            bool isSelected = (currentAudioPath == res.m_path);
                            // 可见资源 ID
                            // 后附路径作为隐藏标识，避免同名资源冲突。
                            if ( ::MMM::UI::FeedbackSelectable(
                                     (res.m_id + "##" + res.m_path).c_str(),
                                     isSelected) ) {
                                meta.song_file_hint =
                                    normalizeProjectResourcePath(
                                        Config::utf8ToPath(res.m_path));
                                // 选择提示字段后清空旧主音频路径，消除双重来源歧义。
                                meta.main_audio_path.clear();
                                changed = true;
                            }
                            // 打开弹窗时将键盘焦点定位到当前资源。
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                // 未打开组合框时，补回预览阶段压入的警告颜色。
                if ( audioPushed ) ImGui::PopStyleColor();
                drawResourceImportButton(audioImportLabel,
                                         BeatmapResourceTarget::Audio);
                // 主音频的工程登记和谱面绑定由统一导入结果完成。
            });

        // 封面选择扫描工程图片文件，并把选择保存为工程相对路径。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.cover").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 当前显示路径经过旧前缀兼容和工程相对化。
                std::string currentCoverPath =
                    displayProjectPath(meta.cover_path);
                std::string coverPreview = currentCoverPath;

                // error_code 避免文件状态异常中断当前 UI 帧。
                std::error_code coverExistsError;
                bool            coverExists =
                    project &&
                    std::filesystem::exists(resolveProjectPath(meta.cover_path),
                                            coverExistsError) &&
                    !coverExistsError;
                bool coverPushed = false;
                if ( !coverExists && !currentCoverPath.empty() ) {
                    // 非空但无法解析的引用使用主题警告色。
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    coverPushed = true;
                }

                // 目录递归扫描仅在组合框打开后的分支发生。
                ImGui::SetNextItemWidth(resourceComboWidth(r));
                if ( ::MMM::UI::FeedbackBeginCombo("##CoverCombo",
                                                   coverPreview.c_str()) ) {
                    if ( coverPushed ) {
                        // 弹窗候选恢复普通文本颜色，不继承预览警告样式。
                        ImGui::PopStyleColor();
                        coverPushed = false;
                    }
                    if ( project ) {
                        // 封面白名单限定常见静态图片格式。
                        std::vector<std::string> images =
                            collectProjectResources(
                                { ".png", ".jpg", ".jpeg", ".bmp" });

                        for ( const auto& imgPath : images ) {
                            // 相对路径同时作为显示文本和唯一隐藏 ID
                            // 的组成部分。
                            bool isSelected = (currentCoverPath == imgPath);
                            if ( ::MMM::UI::FeedbackSelectable(
                                     (imgPath + "##" + imgPath).c_str(),
                                     isSelected) ) {
                                meta.cover_path = Config::utf8ToPath(imgPath);
                                // 只更新草稿，函数末尾统一提交元数据命令。
                                changed = true;
                            }
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                // 保持 PushStyleColor 与所有控制流路径严格配对。
                if ( coverPushed ) ImGui::PopStyleColor();
                drawResourceImportButton(imageImportLabel,
                                         BeatmapResourceTarget::Cover);
                // 封面导入不隐式覆盖背景字段。
            });

        // 背景选择依据 cover_type 在图像与视频扩展名集合之间切换。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.background").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // main_cover_path 是实际背景引用，独立于缩略封面 cover_path。
                std::string currentBgPath =
                    displayProjectPath(meta.main_cover_path);
                std::string bgPreview = currentBgPath;

                // 存在性检查失败与明确不存在都产生同样的警告预览。
                std::error_code bgExistsError;
                const bool      bgExists =
                    project &&
                    std::filesystem::exists(
                        resolveProjectPath(meta.main_cover_path),
                        bgExistsError) &&
                    !bgExistsError;
                bool bgPushed = false;
                if ( !bgExists && !currentBgPath.empty() ) {
                    // 空引用是合法未选择状态，不使用警告颜色。
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    bgPushed = true;
                }

                // 打开组合框后才扫描匹配当前背景类型的资源。
                ImGui::SetNextItemWidth(resourceComboWidth(r));
                if ( ::MMM::UI::FeedbackBeginCombo("##BgCombo",
                                                   bgPreview.c_str()) ) {
                    if ( bgPushed ) {
                        // 候选列表恢复主题正常文本色。
                        ImGui::PopStyleColor();
                        bgPushed = false;
                    }
                    if ( project ) {
                        // 背景类型是用户的显式选择，下拉项只展示同类型资源。
                        // 类型切换不会自动替换既有路径，失配引用会由警告色提示。
                        std::vector<std::string> backgroundResources;
                        if ( meta.cover_type == MMM::CoverType::VIDEO ) {
                            // 视频白名单覆盖当前支持的常见容器扩展名。
                            backgroundResources =
                                collectProjectResources({ ".mp4",
                                                          ".avi",
                                                          ".mkv",
                                                          ".webm",
                                                          ".mov",
                                                          ".flv",
                                                          ".m4v" });
                        } else {
                            // 图片背景与封面沿用同一静态图像白名单。
                            backgroundResources = collectProjectResources(
                                { ".png", ".jpg", ".jpeg", ".bmp" });
                        }

                        for ( const auto& backgroundPath :
                              backgroundResources ) {
                            // 比较统一使用工程相对 UTF-8 路径。
                            bool isSelected = (currentBgPath == backgroundPath);
                            if ( ::MMM::UI::FeedbackSelectable(
                                     (backgroundPath + "##" + backgroundPath)
                                         .c_str(),
                                     isSelected) ) {
                                auto chosenPath =
                                    Config::utf8ToPath(backgroundPath);
                                // 选择只写元数据草稿，不在 UI 回调中加载背景。
                                meta.main_cover_path = chosenPath;
                                changed              = true;

                                // 如果背景是图片且封面为空，则自动沿用同一张图片。
                                // 已有封面始终保留，避免隐式覆盖用户显式选择。
                                auto ext =
                                    Config::pathToUtf8(chosenPath.extension());
                                std::transform(ext.begin(),
                                               ext.end(),
                                               ext.begin(),
                                               ::tolower);
                                if ( ext == ".png" || ext == ".jpg" ||
                                     ext == ".jpeg" || ext == ".bmp" ) {
                                    if ( meta.cover_path.empty() ) {
                                        // 使用同一工程相对路径维持封面与背景可迁移性。
                                        meta.cover_path = chosenPath;
                                    }
                                }
                            }
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                // 未打开弹窗时恢复预览阶段可能压入的警告颜色。
                if ( bgPushed ) ImGui::PopStyleColor();
                drawResourceImportButton(imageImportLabel,
                                         BeatmapResourceTarget::Background);
                // 背景导入保留用户已有的显式封面选择。
            });

        if ( isImd ) {
            // 与资源组首 BeginDisabled 配对，恢复后续页面状态。
            ImGui::EndDisabled();
        }
    }

    // 完成所有分组登记后统一执行 Clay 布局和 ImGui 回调。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    // 根布局使用当前剩余宽度，高度由展开的标题和设置行自动计算。
    ImVec2 sz = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    // 推进游标，使父窗口正确计算滚动范围。
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    // 失败信息在资源控件下方显示，避免同步文件错误被静默吞掉。
    if ( !m_beatmapResourceImportError.empty() ) {
        ImGui::TextColored(Utils::UIThemeUtils::getWarningColor(),
                           "%s",
                           m_beatmapResourceImportError.c_str());
    }

    if ( changed ) {
        // 一帧内所有元数据字段变化合并成一个逻辑命令，保持原子更新语义。
        // 专用轨道数量命令不设置 changed，避免与完整元数据命令重复处理。
        engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ meta });
    }
}

}  // namespace MMM::UI
