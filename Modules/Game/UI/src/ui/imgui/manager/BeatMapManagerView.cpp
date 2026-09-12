#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "ui/imgui/manager/BeatMapManagerView.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/project/ProjectEvents.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>
#include <imgui_internal.h>
#include <numeric>
#include <system_error>

namespace MMM::UI
{
namespace
{
/// @brief 谱面表格列编号。
///
/// 枚举值必须与 `TableSetupColumn` 的登记顺序一致，ImGui 排序和上下文菜单都使用
/// 这些物理列索引。新增或重排列时需要同步更新所有映射。
enum BeatmapTableColumn : int {
    /// @brief 名称列。
    BeatmapTableColumnName = 0,

    /// @brief 类型列。
    BeatmapTableColumnType = 1,

    /// @brief 谱面版本列。
    BeatmapTableColumnVersion = 2,

    /// @brief 文件路径列。
    BeatmapTableColumnPath = 3,

    /// @brief 文件大小列。
    BeatmapTableColumnSize = 4,

    /// @brief 修改时间列。
    BeatmapTableColumnModifiedTime = 5
};

/// @brief 将 ASCII 字符串转换为小写，用于类型归一化和排序。
/// @param value 输入字符串。
/// @return 小写后的字符串。
///
/// 谱面文件扩展名和内部排序键只要求 ASCII 大小写折叠；逐字节保留 UTF-8 内容，
/// 避免依赖全局 locale 或产生异常。
std::string toLowerAscii(std::string value)
{
    // 原地转换复用传值参数的存储，不额外分配第二个字符串。
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            if ( ch >= 'A' && ch <= 'Z' ) {
                // ASCII 字母区间可用固定偏移安全转换。
                return static_cast<char>(ch - 'A' + 'a');
            }
            // 非大写 ASCII 以及 UTF-8 字节保持原样。
            return static_cast<char>(ch);
        });
    return value;
}

/// @brief 根据谱面路径推导显示类型。
/// @param filePath 项目内谱面相对路径。
/// @return 面向谱面管理器的短类型文本。
///
/// 已知格式使用用户熟悉的产品名；未知扩展名去掉前导点直接展示，缺少扩展名时使用
/// 本地化未知占位。路径只用于词法取扩展名，不访问文件系统。
std::string beatmapTypeFromPath(const std::string& filePath)
{
    // UTF-8 路径先转换为平台路径，再提取并规范化扩展名。
    auto extension = toLowerAscii(
        Config::pathToUtf8(Config::utf8ToPath(filePath).extension()));
    if ( extension == ".osu" ) {
        // osu! 文件使用社区通用短名称。
        return "osu";
    }
    if ( extension == ".mc" ) {
        // Malody 的 .mc 扩展映射为产品名称，避免与代码文件混淆。
        return "malody";
    }
    if ( extension == ".imd" ) {
        // .imd 在项目中代表 RM 谱面。
        return "rm";
    }
    if ( extension == ".mmm" ) {
        // 原生格式直接显示 mmm。
        return "mmm";
    }
    if ( extension.size() > 1 && extension.front() == '.' ) {
        // 未知格式展示不带点的扩展名。
        extension.erase(extension.begin());
    }
    return extension.empty() ? TR("ui.file_manager.value_unknown").data()
                             : extension;
}

/// @brief 读取文件大小和修改时间。
/// @param filePath 需要查询的绝对文件路径。
/// @return 文件系统元数据；读取失败的字段保持无效。
///
/// 大小与修改时间分别查询并分别记录有效位，一个字段失败不影响另一个。调用方在
/// 排序缓存重建时执行此低频 IO，普通表格渲染只读取返回缓存。
/// @details 路径可能指向刚被外部删除的文件，也可能因权限暂时不可访问。
/// 这些情况属于可恢复列表状态，不记录错误日志，避免目录刷新期间产生噪声。
///
/// 返回结构保留字段值与有效位，而不是把失败编码成零或 epoch；排序和显示可区分
/// 真实空文件与未知大小，也能区分合法时间点与查询失败。
BeatMapManagerView::FileMetadata queryBeatmapFileMetadata(
    const std::filesystem::path& filePath)
{
    // 默认构造的有效位均为 false，失败分支无需额外清理值。
    BeatMapManagerView::FileMetadata metadata;

    // 所有文件系统接口使用 error_code，避免不可访问文件通过异常中断 UI。
    std::error_code filesystemError;
    const auto size = std::filesystem::file_size(filePath, filesystemError);
    if ( !filesystemError ) {
        // 只有成功读取时才允许大小参与数值排序和格式化。
        metadata.size    = size;
        metadata.hasSize = true;
    }

    // 两次查询相互独立，清除前一次可能留下的错误码。
    filesystemError.clear();
    const auto modifiedTime =
        std::filesystem::last_write_time(filePath, filesystemError);
    if ( !filesystemError ) {
        // 保留 filesystem clock 原值，显示时再换算 system clock。
        metadata.lastWriteTime    = modifiedTime;
        metadata.hasLastWriteTime = true;
    }
    return metadata;
}

/// @brief 生成文件大小显示文本。
/// @param size 文件字节数。
/// @return 适合列表展示的大小文本。
///
/// 单位按 1024 进位，阈值比较使用原始字节对应的 double 值。具体小数格式由翻译
/// 模板控制，保证单位文本可本地化。
/// @details 单位边界按二进制容量定义，但展示文本沿用项目现有 KiB、MiB、GiB
/// 翻译键。 先转 double 再除法可复用格式化模板的小数精度；原始 uintmax_t
/// 仍用于字节分支， 不会因大整数转换提前误判单位。
std::string formatFileSize(std::uintmax_t size)
{
    // 常量逐级推导，避免三个阈值出现不一致。
    constexpr double kibi = 1024.0;
    constexpr double mebi = kibi * 1024.0;
    constexpr double gibi = mebi * 1024.0;

    if ( size < 1024 ) {
        // 小于 1 KiB 时保留精确字节整数。
        return TR_FMT("ui.file_manager.size_bytes", size);
    }
    const double value = static_cast<double>(size);
    if ( value < mebi ) {
        // KiB 到 MiB 区间以 KiB 展示。
        return TR_FMT("ui.file_manager.size_kib", value / kibi);
    }
    if ( value < gibi ) {
        // MiB 到 GiB 区间以 MiB 展示。
        return TR_FMT("ui.file_manager.size_mib", value / mebi);
    }
    // 更大文件统一以 GiB 展示，当前表格无需更高单位。
    return TR_FMT("ui.file_manager.size_gib", value / gibi);
}

/// @brief 将文件系统时间转换为本地时间文本。
/// @param time 文件系统时间。
/// @return 本地时间文本，格式为 yyyy/mm/dd HH:MM。
///
/// filesystem clock 与 system clock 的基准可能不同，先利用两种 clock 的当前时刻
/// 估算对应系统时间，再调用平台线程安全的本地时间转换函数。
/// @details 转换结果只面向即时 UI 展示，不写回项目或参与排序。
/// 排序仍使用原始 file_time_type，不受时区、本地夏令时或格式化精度影响。
///
/// 采用分钟精度以保持列宽稳定；精确诊断应查看文件属性，而不是扩大这个显示字段。
std::string formatModifiedTime(std::filesystem::file_time_type time)
{
    // 通过“文件时刻到文件当前时刻的偏移”映射到 system_clock。
    const auto systemTime =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            time - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    // C 时间格式化接口接收 time_t。
    const std::time_t timeValue =
        std::chrono::system_clock::to_time_t(systemTime);

    std::tm localTime{};
#ifdef _WIN32
    // Windows 使用目标在前的安全 localtime_s 签名。
    localtime_s(&localTime, &timeValue);
#else
    // POSIX 使用可重入 localtime_r，避免共享静态 tm。
    localtime_r(&timeValue, &localTime);
#endif

    // 固定缓冲足以容纳日期、空格、时间和结尾空字符。
    char buffer[32]{};
    if ( std::strftime(buffer, sizeof(buffer), "%Y/%m/%d %H:%M", &localTime) ==
         0 ) {
        // 格式化失败时不展示未初始化或截断文本。
        return TR("ui.file_manager.value_unknown").data();
    }
    return buffer;
}

/// @brief 生成文件大小列文本。
/// @param metadata 文件元数据缓存。
/// @return 大小或未知占位。
///
/// 有效位而不是数值零决定字段是否存在，因为空文件的零字节是合法值。
std::string formatSizeColumn(const BeatMapManagerView::FileMetadata& metadata)
{
    if ( !metadata.hasSize ) {
        return TR("ui.file_manager.value_unknown").data();
    }
    return formatFileSize(metadata.size);
}

/// @brief 生成修改时间列文本。
/// @param metadata 文件元数据缓存。
/// @return 修改时间或未知占位。
///
/// 转换只在有效元数据上执行，避免默认构造时间被误显示为历史日期。
std::string formatModifiedColumn(
    const BeatMapManagerView::FileMetadata& metadata)
{
    if ( !metadata.hasLastWriteTime ) {
        return TR("ui.file_manager.value_unknown").data();
    }
    return formatModifiedTime(metadata.lastWriteTime);
}

/// @brief 生成谱面 Version 列文本。
/// @param metadata 谱面元数据缓存。
/// @return Version 字段文本或未知占位。
///
/// 空版本字符串仍可能是格式中明确存在的值，因此依赖独立 `hasVersion` 标志。
///
/// 返回字符串按值交给单元格绘制 helper，调用期间不保留对缓存内部字符串的引用。
/// 这样即使后续目录刷新替换元数据数组，也不会影响当前绘制命令的文本生命周期。
/// 未知占位与大小、时间列共用相同翻译键，使缺失元数据在整张表中保持一致。
/// Version 的具体语义由谱面格式解析器决定，本视图不做二次规范化。
std::string formatVersionColumn(
    const BeatMapManagerView::FileMetadata& metadata)
{
    if ( !metadata.hasVersion ) {
        return TR("ui.file_manager.value_unknown").data();
    }
    return metadata.version;
}

/// @brief 比较两个可选数值。
/// @tparam T 可比较数值类型。
/// @param lhs 左值。
/// @param lhsValid 左值是否有效。
/// @param rhs 右值。
/// @param rhsValid 右值是否有效。
/// @return 小于返回 -1，大于返回 1，相等返回 0。
///
/// 有效值总是排在缺失值之前，与升降序方向无关；调用方在完整比较结束后统一反转
/// 结果，因此降序时缺失值会相应移动到前面，保持既有表格语义。
/// @details 无效值的具体存储内容永远不会参与 `<`
/// 比较，调用方可以保留默认构造值。
/// 两个有效值都不小于对方时视为相等，随后由谱面路径建立确定性兜底顺序。
template<typename T>
int compareOptionalValue(const T& lhs, bool lhsValid, const T& rhs,
                         bool rhsValid)
{
    if ( lhsValid != rhsValid ) {
        // 仅一侧有效时不读取无效侧的占位值。
        return lhsValid ? -1 : 1;
    }
    if ( !lhsValid ) {
        // 两侧都缺失视为相等，交给路径兜底键建立稳定顺序。
        return 0;
    }
    // 只要求类型提供严格小于运算，兼容时间点和整数。
    if ( lhs < rhs ) return -1;
    if ( rhs < lhs ) return 1;
    return 0;
}

/// @brief 组合排序菜单项显示文本。
/// @param columnLabel 排序字段显示名。
/// @param ascending 是否升序。
/// @return 带方向后缀的菜单文本。
///
/// 排序方向文字来自翻译模板，列名作为格式参数插入。
std::string makeSortMenuLabel(const char* columnLabel, bool ascending)
{
    return ascending
               ? TR_FMT("ui.resource_table.sort_ascending_fmt", columnLabel)
               : TR_FMT("ui.resource_table.sort_descending_fmt", columnLabel);
}

/// @brief 查询表格列当前是否有效显示。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @return 当前帧列有效显示时返回 true。
///
/// `IsEnabled` 反映布局阶段实际参与渲染的状态，可能与用户请求的下一帧状态不同。
/// @details helper 同时检查空表格和索引上下界，让上下文菜单在空白区域打开时无需
/// 重复防御内部列数组访问。
bool isTableColumnEnabled(const ImGuiTable* table, int column)
{
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsEnabled;
}

/// @brief 查询表格列的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @return 用户设置为显示时返回 true。
///
/// `IsUserEnabled` 用于上下文菜单勾选状态，不受当前宽度裁剪等内部状态影响。
/// @details 该状态用于计算至少保留一列的约束；它代表用户选择，而不是列本帧是否
/// 因内部布局原因暂时跳过渲染。
bool isTableColumnUserEnabled(const ImGuiTable* table, int column)
{
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsUserEnabled;
}

/// @brief 排队设置表格列下一帧的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @param enabled 是否显示。
///
/// ImGui 要求通过 `IsUserEnabledNextFrame`
/// 延迟应用列显隐；直接修改当前帧字段会破坏
/// 表格内部布局。无效指针或索引静默忽略。
/// @details 修改写入 NextFrame 字段后，当前菜单仍以打开时状态完成遍历；这避免在
/// for 循环中改变 ColumnsCount 或当前上下文列。
void queueTableColumnEnabled(ImGuiTable* table, int column, bool enabled)
{
    // 检查物理列范围，避免上下文列为 -1 时越界。
    if ( !table || column < 0 || column >= table->ColumnsCount ) {
        return;
    }
    table->Columns[column].IsUserEnabledNextFrame = enabled;
}

/// @brief 绘制可裁剪、超宽自动滚动的表格单元格文本。
/// @param text 需要绘制的文本。
/// @param cursorPos 单元格起始屏幕坐标。
/// @param width 单元格可用宽度。
/// @param height 行高。
///
/// 文本宽于单元格时使用缓慢往返偏移展示完整内容，窄文本保持静止。绘制直接进入
/// 当前窗口 DrawList，同时严格裁剪在本单元格矩形内。
/// @warning UI 热路径：每个可见行的每个文本列调用；不得分配额外大型缓存或做
/// IO。
/// @details 该 helper 不提交 ImGui Item，整行选择和悬浮仍由第一列的跨列
/// Selectable 负责，各文本列不会争夺鼠标输入或产生重复导航焦点。
///
/// 所有超宽单元格共享全局 ImGui 时间相位，表格滚动时不为每行维护动画状态。
/// 正弦插值两端经钳制后形成短暂停顿，便于读取字符串首尾。
///
/// 裁剪矩形使用传入行高而非字体高度，文字滚动不会覆盖表头或相邻行背景。
void renderScrollingTableText(const std::string& text, ImVec2 cursorPos,
                              float width, float height)
{
    // 负可用宽度按零处理，仍保持合法裁剪矩形。
    const float  textWidth = std::max(0.0f, width);
    const ImVec2 textSize  = ImGui::CalcTextSize(text.c_str());
    const float  textH     = ImGui::GetFontSize();
    // 文本按行高垂直居中，不依赖单元格默认游标偏移。
    const float offsetY = (height - textH) * 0.5f;

    float offset = 0.0f;
    if ( textSize.x > textWidth ) {
        // 额外滚动余量让尾部完全离开后留出短暂停顿感。
        const float scrollRange = textSize.x - textWidth + 40.0f;
        // 正弦曲线产生平滑往返，clamp 中段形成两端停留区。
        const float time = static_cast<float>(ImGui::GetTime());
        float       t    = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
        t                = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
        offset           = t * scrollRange;
    }

    // 以调用方保存的单元格起点建立独立裁剪区域。
    const ImVec2 textStartPos = cursorPos;
    ImGui::PushClipRect(
        textStartPos,
        ImVec2(textStartPos.x + textWidth, textStartPos.y + height),
        true);
    // X 减 offset 实现滚动，Y 加 offsetY 实现垂直居中。
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(textStartPos.x - offset, textStartPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    ImGui::PopClipRect();
}

}  // namespace

/// @brief 创建谱面管理器并订阅外部目录刷新通知。
/// @param subViewName 子视图名称。
///
/// 文件监控或项目资源变更可能从其他线程发布刷新事件，回调只设置原子脏标志；真正
/// 的文件元数据读取和排序缓存重建延迟到 UI 线程。
BeatMapManagerView::BeatMapManagerView(const std::string& subViewName)
    : ISubView(subViewName)
{
    // 保存订阅 ID，析构时精确移除当前实例回调。
    m_projectDirectoryRefreshedSubId =
        Event::EventBus::instance()
            .subscribe<Event::ProjectDirectoryRefreshedEvent>(
                [this](const Event::ProjectDirectoryRefreshedEvent&) {
                    // release 发布目录变化，UI 帧用 acquire 消费后重建缓存。
                    m_projectDirectoryRefreshPending.store(
                        true, std::memory_order_release);
                });
}

/// @brief 取消谱面管理器的目录刷新订阅。
///
/// 订阅回调捕获 this，必须在成员销毁前注销，避免 EventBus 后续访问失效实例。
BeatMapManagerView::~BeatMapManagerView()
{
    // 使用构造时记录的 ID，不影响其他视图对同类事件的订阅。
    Event::EventBus::instance()
        .unsubscribe<Event::ProjectDirectoryRefreshedEvent>(
            m_projectDirectoryRefreshedSubId);
}

/// @brief 获取谱面管理器中不可再换行控件所需的最小内容尺寸。
/// @param dpiScale 当前窗口 DPI 缩放。
/// @return 可容纳标题、至少一行和固定页脚的最小内容尺寸。
///
/// 返回值用于父级管理器布局协商，不代表当前实际尺寸。谱面列表折叠时不为数据行
/// 预留高度；没有项目时只保证居中提示可完整显示。
/// @warning UI 热路径：子视图可见时每帧查询；仅保留轻量文本测量。
/// @details
/// 宽度只考虑不可换行标题或提示，不扫描所有谱面名称。数据单元格具备裁剪
/// 和滚动显示能力，因此最长文件名不应反向扩大父级侧栏最小宽度。
ImVec2 BeatMapManagerView::getMinContentSize(float dpiScale) const
{
    // 当前项目决定是提示态还是列表态。
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    // DPI 至少按 1 处理，避免低于一倍时控件变得不可操作。
    const float scale     = std::max(1.0f, dpiScale);
    const float panelPad  = 4.0f * scale;
    const float rowHeight = 28.0f * scale;
    const float footerH   = 44.0f * scale;
    // 标题宽度包含折叠按钮、控件间距和本地化文本。
    const float headerWidth =
        ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x +
        ImGui::CalcTextSize(TR("ui.beatmap_manager.beatmaps").data()).x;

    if ( !project ) {
        // 空项目状态只需要提示文本和四周 padding。
        const char* hint = TR("ui.beatmap_manager.initial_hint").data();
        return ImVec2(
            std::ceil(ImGui::CalcTextSize(hint).x + panelPad * 2.0f),
            std::ceil(ImGui::GetTextLineHeightWithSpacing() + panelPad * 2.0f));
    }

    // 最小列表只为一行保留高度，不随大型项目无限扩大父布局。
    const float visibleRows =
        m_showBeatmapList ? std::min<size_t>(project->m_beatmaps.size(), 1) : 0;
    // 标题、可选数据行与页脚共同决定最小高度。
    const float minHeight = panelPad * 2.0f + ImGui::GetFrameHeight() +
                            visibleRows * rowHeight + footerH;
    return ImVec2(std::ceil(headerWidth + panelPad * 2.0f),
                  std::ceil(minHeight));
}

/// @brief 更新谱面列表、排序菜单、新建入口和单谱面管理模态。
/// @param layoutContext 父级提供的屏幕起点、可用尺寸和 DPI。
/// @param sourceManager 非拥有 UI 管理器，用于打开新建谱面向导。
///
/// 普通帧只绘制排序索引对应的可见行。项目、谱面数量、排序规则或目录刷新发生变化
/// 时才重建索引与文件元数据缓存；该低频路径会访问文件系统并加载谱面元数据。
///
/// 行左键创建编辑 Session，右键打开移除管理模态。移除操作通过 EditorEngine 命令
/// 队列提交，不在视图中直接改写项目结构。
///
/// 表格的物理列顺序由 `BeatmapTableColumn`
/// 固定。用户可以改变列宽、显示状态和视觉 顺序，但排序规格仍通过物理索引映射到
/// `BeatmapSortKey`，不能使用显示顺序推导。
///
/// 排序缓存包含项目原数组索引、按原索引存放的元数据，以及当前项目指针和数量快照。
/// 排序只移动索引，行渲染再用索引关联元数据，项目对象不会因视图排序发生重排。
///
/// 文件大小、修改时间、Version 和默认音频资源均在缓存重建时获取。普通帧不会触碰
/// 文件系统或解析谱面；外部修改通过目录刷新事件把缓存标为脏。
///
/// 表格行使用 ImGuiListClipper
/// 限制绘制范围。即使项目包含大量谱面，普通帧也只格式
/// 化当前可见行；完整索引遍历仅发生在项目或排序状态变化之后。
///
/// 右键管理目标保存项目内相对路径，不保存数组元素地址或索引。排序缓存重建不会使
/// 模态目标失效，删除命令也由逻辑层按稳定路径解析实际谱面。
/// @warning UI 热路径：子视图可见时每帧调用；文件 IO
/// 和全量排序只能位于脏缓存分支。
/// @details 所有可见按钮通过 FeedbackButton 或 FeedbackMenuItem 绘制，保证反馈
/// 与其他业务 UI 一致；手工 DrawList 内容只承担文本和背景装饰。
///
/// 字体、样式变量、颜色和 Popup 都在各自控制流中成对恢复。新增提前返回时必须像
/// 无项目分支一样先恢复已压入的字体，避免污染同一 ImGui 帧后续视图。
///
/// 管理模态使用两级确认：外层选定谱面，内层确认破坏性移除。内层取消不清空目标，
/// 外层取消或确认成功才结束完整管理状态。
void BeatMapManagerView::onUpdate(LayoutContext& layoutContext,
                                  UIManager*     sourceManager)
{
    // 原子 exchange 同时消费跨线程刷新标志，避免同一通知重复触发多帧重建。
    if ( m_projectDirectoryRefreshPending.exchange(
             false, std::memory_order_acq_rel) ) {
        m_beatmapSortCacheDirty = true;
    }

    // 本帧统一取得 EditorEngine、当前项目和皮肤配置。
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    // 文件管理器专用字体可选；缺失时沿用当前 ImGui 字体。
    float   dpiScale        = layoutContext.m_dpiScale;
    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) {
        // 显式传 LegacySize 保持字体栈与皮肤配置的尺寸一致。
        ImGui::PushFont(fileManagerFont, fileManagerFont->LegacySize);
    }

    // 极窄侧栏中 padding 不超过可用宽度的 2%，防止内容被完全挤压。
    const float panelPadding = std::min(
        4.0f * dpiScale, std::max(0.0f, layoutContext.m_avail.x) * 0.02f);
    const float rowHeight = 28.0f * dpiScale;

    if ( !project ) {
        // 无项目提示按当前内容区域水平和垂直居中。
        const char* hint     = TR("ui.beatmap_manager.initial_hint").data();
        ImVec2      textSize = ImGui::CalcTextSize(hint);
        ImVec2      textPos  = {
            layoutContext.m_startPos.x +
                std::max(0.0f, layoutContext.m_avail.x - textSize.x) * 0.5f,
            layoutContext.m_startPos.y +
                std::max(0.0f, layoutContext.m_avail.y - textSize.y) * 0.5f
        };
        ImGui::SetCursorScreenPos(textPos);
        ImGui::TextDisabled("%s", hint);

        // 对应上方可选 PushFont，提前返回前必须恢复字体栈。
        if ( fileManagerFont ) ImGui::PopFont();
        return;
    }

    // 固定页脚从总高度中扣除，列表区域使用剩余空间。
    float  footerH  = 44.0f * dpiScale;
    float  listH    = std::max(0.0f, layoutContext.m_avail.y - footerH);
    ImVec2 listPos  = { layoutContext.m_startPos.x + panelPadding,
                        layoutContext.m_startPos.y + panelPadding };
    ImVec2 listSize = { std::max(0.0f,
                                 layoutContext.m_avail.x - panelPadding * 2.0f),
                        std::max(0.0f, listH - panelPadding) };

    // 子窗口独立管理表格滚动，外层子视图坐标由 LayoutContext 提供。
    ImGui::SetCursorScreenPos(listPos);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(
        "BeatmapListChild", listSize, false, ImGuiWindowFlags_None);

    // 折叠标题使用 Clay 边界适配父级宽度，并保留 ImGui 帧高。
    ImVec2           headerPos = ImGui::GetCursorScreenPos();
    Clay_BoundingBox headerBox{ .x      = headerPos.x,
                                .y      = headerPos.y,
                                .width  = ImGui::GetContentRegionAvail().x,
                                .height = ImGui::GetFrameHeight() };
    Utils::renderCollapsingHeader(TR("ui.beatmap_manager.beatmaps").data(),
                                  &m_showBeatmapList,
                                  headerBox);

    if ( m_showBeatmapList ) {
        // 表格滚动条使用统一 DPI 样式作用域。
        Utils::VerticalScrollbarStyleScope verticalScrollbarStyle(dpiScale);
        // 列支持排序、重排、隐藏与调整宽度；数据区独立垂直滚动。
        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp;

        if ( ImGui::BeginTable("BeatmapManagerTableV2",
                               6,
                               tableFlags,
                               ImGui::GetContentRegionAvail()) ) {
            // 表头固定在垂直滚动区域顶部。
            ImGui::TableSetupScrollFreeze(0, 1);
            // 名称列默认升序并吸收剩余宽度。
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_name").data(),
                ImGuiTableColumnFlags_DefaultSort |
                    ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            // 类型列固定宽度，随当前字体大小保留可读下限。
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_type").data(),
                ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(76.0f, 86.0f * ImGui::GetFontSize() / 17.0f));
            // Version 列固定宽度并默认显示。
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_version").data(),
                ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(96.0f, 120.0f * ImGui::GetFontSize() / 17.0f));
            // 完整路径较长，默认隐藏且显示时使用伸缩宽度。
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_path").data(),
                ImGuiTableColumnFlags_DefaultHide |
                    ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            // 大小属于辅助元数据，默认隐藏。
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_size").data(),
                ImGuiTableColumnFlags_DefaultHide |
                    ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(96.0f, 104.0f * ImGui::GetFontSize() / 17.0f));
            // 修改时间默认隐藏，宽度按固定格式文本和单元格 padding 计算。
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_modified_time").data(),
                ImGuiTableColumnFlags_DefaultHide |
                    ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortDescending,
                std::max(168.0f,
                         ImGui::CalcTextSize("0000/00/00 00:00").x +
                             ImGui::GetStyle().CellPadding.x * 4.0f +
                             ImGui::GetFrameHeight()));
            if ( ImGuiTable* table = ImGui::GetCurrentTable() ) {
                // 使用本视图自定义菜单，禁用 ImGui 内置列上下文菜单。
                table->DisableDefaultContextMenu = true;
            }
            // 所有列登记完成后提交表头行。
            ImGui::TableHeadersRow();

            // ImGui 只在排序规格变化时标记 SpecsDirty，避免每帧重置缓存。
            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if ( sortSpecs && sortSpecs->SpecsCount > 0 &&
                 sortSpecs->SpecsDirty ) {
                const ImGuiTableColumnSortSpecs& primarySpec =
                    sortSpecs->Specs[0];
                // 名称是未识别列索引的防御性默认排序键。
                BeatmapSortKey newSortKey = BeatmapSortKey::Name;
                if ( primarySpec.ColumnIndex == BeatmapTableColumnType ) {
                    newSortKey = BeatmapSortKey::Type;
                } else if ( primarySpec.ColumnIndex ==
                            BeatmapTableColumnVersion ) {
                    newSortKey = BeatmapSortKey::Version;
                } else if ( primarySpec.ColumnIndex ==
                            BeatmapTableColumnPath ) {
                    newSortKey = BeatmapSortKey::Path;
                } else if ( primarySpec.ColumnIndex ==
                            BeatmapTableColumnSize ) {
                    newSortKey = BeatmapSortKey::Size;
                } else if ( primarySpec.ColumnIndex ==
                            BeatmapTableColumnModifiedTime ) {
                    newSortKey = BeatmapSortKey::ModifiedTime;
                }

                const SortDirection newDirection =
                    primarySpec.SortDirection == ImGuiSortDirection_Descending
                        ? SortDirection::Descending
                        : SortDirection::Ascending;
                if ( newSortKey != m_beatmapSortKey ||
                     newDirection != m_beatmapSortDirection ) {
                    // 只有内部排序状态真正变化才标记缓存脏。
                    m_beatmapSortKey        = newSortKey;
                    m_beatmapSortDirection  = newDirection;
                    m_beatmapSortCacheDirty = true;
                }
                // 通知 ImGui 本次排序规格已经消费。
                sortSpecs->SpecsDirty = false;
            }

            /// 把表格排序恢复为名称升序，并在发生变化时使缓存失效。
            ///
            /// 默认状态与名称列的 `DefaultSort`
            /// 标志一致。已经处于默认值时不重复
            /// 标记缓存，避免菜单打开期间产生无意义的全量重建。
            auto resetBeatmapTableSort = [&]() {
                if ( m_beatmapSortKey != BeatmapSortKey::Name ||
                     m_beatmapSortDirection != SortDirection::Ascending ) {
                    m_beatmapSortKey        = BeatmapSortKey::Name;
                    m_beatmapSortDirection  = SortDirection::Ascending;
                    m_beatmapSortCacheDirty = true;
                }
            };

            /// 应用上下文菜单选择的排序键与方向。
            /// @param sortKey 目标字段。
            /// @param direction 目标方向。
            ///
            /// 这里只更新视图状态，实际排序延迟到菜单绘制后的统一脏缓存分支。
            auto applyBeatmapSort = [&](BeatmapSortKey sortKey,
                                        SortDirection  direction) {
                if ( m_beatmapSortKey != sortKey ||
                     m_beatmapSortDirection != direction ) {
                    m_beatmapSortKey        = sortKey;
                    m_beatmapSortDirection  = direction;
                    m_beatmapSortCacheDirty = true;
                }
            };

            /// 绘制替代 ImGui 默认实现的表头排序、尺寸与显隐菜单。
            ///
            /// 菜单使用 ImGuiTable 内部上下文列信息，因此必须在 BeginTable 与
            /// EndTable
            /// 之间同步调用。列显隐变更排队到下一帧，排序变更只更新本地缓存键。
            auto renderBeatmapTableHeaderContextMenu = [&]() {
                // 菜单只在有效表格上下文中工作。
                ImGuiTable* table = ImGui::GetCurrentTable();
                if ( !table ) return;

                // 菜单 padding 设下限，避免紧凑主题使条目难以点击。
                ImGuiStyle&  style = ImGui::GetStyle();
                const ImVec2 popupPadding(
                    std::max(style.WindowPadding.x, 8.0f),
                    std::max(style.WindowPadding.y, 6.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPadding);
                ImGui::PushStyleVar(
                    ImGuiStyleVar_ItemSpacing,
                    ImVec2(std::max(style.ItemSpacing.x, 8.0f),
                           std::max(style.ItemSpacing.y, 4.0f)));
                const bool popupOpen = ImGui::TableBeginContextMenuPopup(table);
                if ( !popupOpen ) {
                    // Begin 未打开时仍需恢复前面压入的两个样式变量。
                    ImGui::PopStyleVar(2);
                    return;
                }

                // 右键落在有效列时保存物理列索引，否则使用 -1
                // 表示表格空白区域。
                const int contextColumn =
                    table->ContextPopupColumn >= 0 &&
                            table->ContextPopupColumn < table->ColumnsCount
                        ? table->ContextPopupColumn
                        : -1;
                if ( contextColumn >= 0 &&
                     isTableColumnEnabled(table, contextColumn) &&
                     ::MMM::UI::FeedbackMenuItem(
                         TR("ui.beatmap_manager.table_menu.size_column_fit")
                             .data()) ) {
                    // 单列自动适配只对当前实际显示列开放。
                    ImGui::TableSetColumnWidthAutoSingle(table, contextColumn);
                }

                if ( ::MMM::UI::FeedbackMenuItem(
                         TR("ui.beatmap_manager.table_menu.size_all_default")
                             .data()) ) {
                    // 全列自动适配保留当前显隐状态。
                    ImGui::TableSetColumnWidthAutoAll(table);
                }

                /// 绘制一个带当前选中状态的具体排序菜单项。
                /// @param sortKey 菜单项对应的内部排序字段。
                /// @param direction 菜单项对应的升降序。
                /// @param columnLabel 本地化列名。
                auto sortMenuItem = [&](BeatmapSortKey sortKey,
                                        SortDirection  direction,
                                        const char*    columnLabel) {
                    const std::string label = makeSortMenuLabel(
                        columnLabel, direction == SortDirection::Ascending);
                    const bool selected = m_beatmapSortKey == sortKey &&
                                          m_beatmapSortDirection == direction;
                    if ( ::MMM::UI::FeedbackMenuItem(
                             label.c_str(), nullptr, selected) ) {
                        applyBeatmapSort(sortKey, direction);
                    }
                };
                if ( ::MMM::UI::FeedbackBeginMenu(
                         TR("ui.resource_table.sort").data()) ) {
                    // 每个可排序字段同时提供升序和降序入口。
                    sortMenuItem(BeatmapSortKey::Name,
                                 SortDirection::Ascending,
                                 TR("ui.beatmap_manager.column_name").data());
                    sortMenuItem(BeatmapSortKey::Name,
                                 SortDirection::Descending,
                                 TR("ui.beatmap_manager.column_name").data());
                    sortMenuItem(BeatmapSortKey::Type,
                                 SortDirection::Ascending,
                                 TR("ui.beatmap_manager.column_type").data());
                    sortMenuItem(BeatmapSortKey::Type,
                                 SortDirection::Descending,
                                 TR("ui.beatmap_manager.column_type").data());
                    sortMenuItem(
                        BeatmapSortKey::Version,
                        SortDirection::Ascending,
                        TR("ui.beatmap_manager.column_version").data());
                    sortMenuItem(
                        BeatmapSortKey::Version,
                        SortDirection::Descending,
                        TR("ui.beatmap_manager.column_version").data());
                    sortMenuItem(BeatmapSortKey::Path,
                                 SortDirection::Ascending,
                                 TR("ui.beatmap_manager.column_path").data());
                    sortMenuItem(BeatmapSortKey::Path,
                                 SortDirection::Descending,
                                 TR("ui.beatmap_manager.column_path").data());
                    sortMenuItem(BeatmapSortKey::Size,
                                 SortDirection::Ascending,
                                 TR("ui.beatmap_manager.column_size").data());
                    sortMenuItem(BeatmapSortKey::Size,
                                 SortDirection::Descending,
                                 TR("ui.beatmap_manager.column_size").data());
                    sortMenuItem(
                        BeatmapSortKey::ModifiedTime,
                        SortDirection::Ascending,
                        TR("ui.beatmap_manager.column_modified_time").data());
                    sortMenuItem(
                        BeatmapSortKey::ModifiedTime,
                        SortDirection::Descending,
                        TR("ui.beatmap_manager.column_modified_time").data());
                    ::MMM::UI::FeedbackEndMenu();
                }

                if ( ::MMM::UI::FeedbackBeginMenu(
                         TR("ui.beatmap_manager.table_menu.reset").data()) ) {
                    // 完整重置同时恢复 ImGui 列设置和本视图默认排序。
                    if ( ::MMM::UI::FeedbackMenuItem(
                             TR("ui.beatmap_manager.table_menu.reset_all")
                                 .data()) ) {
                        ImGui::TableResetSettings(table);
                        resetBeatmapTableSort();
                    }
                    if ( ::MMM::UI::FeedbackMenuItem(
                             TR("ui.beatmap_manager.table_menu.reset_columns")
                                 .data()) ) {
                        // 列宽重置不改变显隐与排序。
                        ImGui::TableSetColumnWidthAutoAll(table);
                    }
                    if ( ::MMM::UI::FeedbackMenuItem(
                             TR("ui.beatmap_manager.table_menu.show_all_"
                                "columns")
                                 .data()) ) {
                        // 显隐请求排队到下一帧，避免修改当前表格布局。
                        for ( int column = 0; column < table->ColumnsCount;
                              ++column ) {
                            queueTableColumnEnabled(table, column, true);
                        }
                    }
                    if ( ::MMM::UI::FeedbackMenuItem(
                             TR("ui.beatmap_manager.table_menu.reset_sort")
                                 .data()) ) {
                        // 仅排序重置保留用户列布局。
                        resetBeatmapTableSort();
                    }
                    ::MMM::UI::FeedbackEndMenu();
                }

                // 分隔线后的条目直接控制六列可见性。
                ImGui::Separator();

                const std::array<const char*, 6> columnLabels{
                    TR("ui.beatmap_manager.column_name").data(),
                    TR("ui.beatmap_manager.column_type").data(),
                    TR("ui.beatmap_manager.column_version").data(),
                    TR("ui.beatmap_manager.column_path").data(),
                    TR("ui.beatmap_manager.column_size").data(),
                    TR("ui.beatmap_manager.column_modified_time").data()
                };
                // 先统计启用列，确保菜单不会隐藏最后一列。
                int enabledColumnCount = 0;
                for ( int column = 0; column < table->ColumnsCount; ++column ) {
                    if ( isTableColumnUserEnabled(table, column) ) {
                        enabledColumnCount++;
                    }
                }
                for ( int column = 0; column < table->ColumnsCount; ++column ) {
                    // 已隐藏列总能重新启用；显示列仅在还有其他列时可关闭。
                    const bool enabled =
                        isTableColumnUserEnabled(table, column);
                    const bool canToggle = !enabled || enabledColumnCount > 1;
                    if ( ::MMM::UI::FeedbackMenuItem(columnLabels[column],
                                                     nullptr,
                                                     enabled,
                                                     canToggle) ) {
                        queueTableColumnEnabled(table, column, !enabled);
                    }
                }

                // 弹窗和两项样式变量在同一 lambda 内完整配对。
                ImGui::EndPopup();
                ImGui::PopStyleVar(2);
            };
            // 每帧登记菜单；仅右键打开时实际绘制内容。
            renderBeatmapTableHeaderContextMenu();

            /// 重建谱面索引、文件元数据和当前排序顺序。
            ///
            /// 项目谱面数组保持原顺序，视图仅重排独立索引。每个元数据槽与原数组
            /// 索引一一对应，排序比较和可见行渲染必须先从排序行映射回该索引。
            ///
            /// 文件不存在或谱面解析失败时，有效位保留
            /// false；未知字段仍可通过路径
            /// 兜底键参与稳定排序，而不会阻止其他谱面显示。
            ///
            /// 重建先创建连续原索引并查询元数据，再依据当前键稳定排序索引，最后一次
            /// 性提交项目身份、数量和干净标志，行渲染不会观察到半完成缓存。
            ///
            /// `stable_sort`
            /// 在主键与路径都相等时保留项目原顺序，兼容历史工程中的重复
            /// 条目，并减少同值行在重复重建后的视觉跳动。
            ///
            /// 字符串字段只做 ASCII 小写折叠，不承诺自然语言排序，避免 UI
            /// 低频重建 依赖全局 locale 或重量级 Unicode 排序状态。
            ///
            /// 可选数值依赖有效位而非哨兵值：零字节文件可能合法，只有成功查询或解析
            /// 后才允许字段参与对应比较。
            /// @warning 低频脏路径：包含文件 IO、谱面加载和全量 stable_sort。
            auto rebuildSortCache = [&]() {
                // 索引和元数据数组始终与项目谱面数组保持相同长度。
                const auto& beatmaps = project->m_beatmaps;
                m_sortedBeatmapIndices.resize(beatmaps.size());
                m_beatmapFileMetadata.resize(beatmaps.size());
                std::iota(m_sortedBeatmapIndices.begin(),
                          m_sortedBeatmapIndices.end(),
                          size_t{ 0 });
                for ( size_t index = 0; index < beatmaps.size(); ++index ) {
                    // 项目保存相对路径，文件查询前与项目根目录组合。
                    const auto filePath =
                        project->m_projectRoot /
                        Config::utf8ToPath(beatmaps[index].m_filePath);
                    m_beatmapFileMetadata[index] =
                        queryBeatmapFileMetadata(filePath);
                    // 谱面解析补充文件系统无法提供的版本与默认音频关联。
                    auto beatmap = MMM::BeatMap::loadFromFile(filePath);
                    if ( !beatmap.m_baseMapMetadata.map_path.empty() ) {
                        // map_path 非空作为谱面成功载入的现有判据。
                        m_beatmapFileMetadata[index].version =
                            beatmap.m_baseMapMetadata.version;
                        m_beatmapFileMetadata[index].hasVersion = true;
                        if ( const auto* audioResource =
                                 Logic::ProjectResourceService::
                                     findDefaultBeatmapAudioResource(
                                         *project,
                                         beatmap,
                                         beatmaps[index].m_filePath) ) {
                            // 只复制稳定资源 ID，不保留 Project 内部指针。
                            m_beatmapFileMetadata[index].audioResourceId =
                                audioResource->m_id;
                            m_beatmapFileMetadata[index].hasAudioResource =
                                true;
                        }
                    }
                }
                // 排序只重排索引，不改变项目持久化中的谱面顺序。
                std::stable_sort(
                    m_sortedBeatmapIndices.begin(),
                    m_sortedBeatmapIndices.end(),
                    [&](size_t lhsIndex, size_t rhsIndex) {
                        const auto& lhs = beatmaps[lhsIndex];
                        const auto& rhs = beatmaps[rhsIndex];
                        // 每个键归一化为三态比较结果，最后统一处理方向。
                        int compareResult = 0;
                        switch ( m_beatmapSortKey ) {
                        case BeatmapSortKey::Name:
                            // 名称采用 ASCII 不区分大小写比较，UTF-8
                            // 字节顺序保持稳定。
                            compareResult =
                                toLowerAscii(lhs.m_name)
                                    .compare(toLowerAscii(rhs.m_name));
                            break;
                        case BeatmapSortKey::Type:
                            // 类型由扩展名映射后的显示短文本决定。
                            compareResult = beatmapTypeFromPath(lhs.m_filePath)
                                                .compare(beatmapTypeFromPath(
                                                    rhs.m_filePath));
                            break;
                        case BeatmapSortKey::Version:
                            // Version
                            // 依赖谱面解析缓存，无效值通过独立有效位排序。
                            compareResult = compareOptionalValue(
                                toLowerAscii(
                                    m_beatmapFileMetadata[lhsIndex].version),
                                m_beatmapFileMetadata[lhsIndex].hasVersion,
                                toLowerAscii(
                                    m_beatmapFileMetadata[rhsIndex].version),
                                m_beatmapFileMetadata[rhsIndex].hasVersion);
                            break;
                        case BeatmapSortKey::Path:
                            // 路径比较使用项目内相对 UTF-8
                            // 文本，不依赖绝对根目录。
                            compareResult =
                                toLowerAscii(lhs.m_filePath)
                                    .compare(toLowerAscii(rhs.m_filePath));
                            break;
                        case BeatmapSortKey::Size:
                            // 文件大小按整数数值比较，不使用已格式化单位文本。
                            compareResult = compareOptionalValue(
                                m_beatmapFileMetadata[lhsIndex].size,
                                m_beatmapFileMetadata[lhsIndex].hasSize,
                                m_beatmapFileMetadata[rhsIndex].size,
                                m_beatmapFileMetadata[rhsIndex].hasSize);
                            break;
                        case BeatmapSortKey::ModifiedTime:
                            // 修改时间按 filesystem clock
                            // 时间点比较，不受显示时区影响。
                            compareResult = compareOptionalValue(
                                m_beatmapFileMetadata[lhsIndex].lastWriteTime,
                                m_beatmapFileMetadata[lhsIndex]
                                    .hasLastWriteTime,
                                m_beatmapFileMetadata[rhsIndex].lastWriteTime,
                                m_beatmapFileMetadata[rhsIndex]
                                    .hasLastWriteTime);
                            break;
                        }

                        if ( compareResult == 0 ) {
                            // 路径作为稳定兜底键，避免相同显示值在重建后乱序。
                            compareResult =
                                toLowerAscii(lhs.m_filePath)
                                    .compare(toLowerAscii(rhs.m_filePath));
                        }
                        if ( m_beatmapSortDirection ==
                             SortDirection::Descending ) {
                            // 降序反转完整比较，包括路径兜底顺序。
                            compareResult = -compareResult;
                        }
                        return compareResult < 0;
                    });
                // 快照项目身份和数量，并在全部缓存可用后清除脏标记。
                m_cachedBeatmapCount    = beatmaps.size();
                m_cachedBeatmapProject  = project;
                m_beatmapSortCacheDirty = false;
            };

            if ( m_beatmapSortCacheDirty ||
                 m_cachedBeatmapCount != project->m_beatmaps.size() ||
                 m_cachedBeatmapProject != project ) {
                // 三种失效源覆盖外部刷新、数组数量变化和项目对象切换。
                rebuildSortCache();
            }

            // Clipper 只遍历当前滚动窗口内的固定高度行。
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_sortedBeatmapIndices.size()),
                          rowHeight);
            while ( clipper.Step() ) {
                for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                      ++row ) {
                    const size_t beatmapIndex =
                        m_sortedBeatmapIndices[static_cast<size_t>(row)];
                    if ( beatmapIndex >= project->m_beatmaps.size() ) {
                        // 防御缓存与项目在同帧异步变化造成的陈旧索引。
                        continue;
                    }

                    const auto& beatmap = project->m_beatmaps[beatmapIndex];
                    const auto  typeText =
                        beatmapTypeFromPath(beatmap.m_filePath);
                    FileMetadata metadata;
                    if ( beatmapIndex < m_beatmapFileMetadata.size() ) {
                        // 元数据按原项目索引读取，而非排序后的行号。
                        metadata = m_beatmapFileMetadata[beatmapIndex];
                    }

                    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                    ImGui::TableNextColumn();

                    const ImVec2 nameCellPos = ImGui::GetCursorScreenPos();
                    const float  nameCellWidth =
                        ImGui::GetContentRegionAvail().x;
                    const std::string rowId =
                        "##BeatmapRow_" + beatmap.m_filePath;
                    // Selectable 跨越所有列，任意单元格都可激活谱面。
                    const bool clicked = ::MMM::UI::FeedbackSelectable(
                        rowId.c_str(),
                        false,
                        ImGuiSelectableFlags_SpanAllColumns,
                        { 0.0f, rowHeight });
                    const bool hovered = ImGui::IsItemHovered();
                    if ( clicked ) {
                        // 左键按相对路径载入谱面，并以项目条目名称创建
                        // Session。
                        XINFO("Request to load beatmap: {}", beatmap.m_name);
                        auto fullPath = project->m_projectRoot /
                                        Config::utf8ToPath(beatmap.m_filePath);
                        auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                            MMM::BeatMap::loadFromFile(fullPath));
                        engine.createSession(loadedBeatmap, beatmap.m_name);
                    }

                    if ( hovered &&
                         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
                        // 右键只记录稳定相对路径，模态在表格之后绘制。
                        m_manageBeatmapPath = beatmap.m_filePath;
                        m_openManageModal   = true;
                    }
                    if ( hovered ) {
                        // Tooltip 补充默认隐藏列和音频资源关联信息。
                        const std::string audioText =
                            metadata.hasAudioResource
                                ? metadata.audioResourceId
                                : TR("ui.file_manager.value_unknown").data();
                        const std::string tooltipText =
                            fmt::format("File: {}\nType: {}\nAudio: {}",
                                        beatmap.m_filePath,
                                        typeText,
                                        audioText);
                        Utils::renderTooltip(tooltipText.c_str());
                    }

                    const std::string nameText =
                        std::string(ICON_MMM_FILE) + "  " + beatmap.m_name;
                    // 第一列在 Selectable
                    // 之后手工绘制图标名称，保留整行点击区域。
                    renderScrollingTableText(
                        nameText, nameCellPos, nameCellWidth, rowHeight);

                    // 后续五列共享裁剪滚动文本渲染器。
                    ImGui::TableNextColumn();
                    renderScrollingTableText(typeText,
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(formatVersionColumn(metadata),
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(beatmap.m_filePath,
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(formatSizeColumn(metadata),
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(formatModifiedColumn(metadata),
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);
                }
            }
            // BeginTable 成功后在全部行与 Clipper 处理完成时配对结束。
            ImGui::EndTable();
        }
    }

    // 列表子窗口与零 padding 样式在离开列表区域前恢复。
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // 页脚占据列表下方固定高度，按钮在其中垂直居中并横向铺满。
    float  btnSize    = 32.0f * dpiScale;
    ImVec2 footerPos  = { layoutContext.m_startPos.x + panelPadding,
                          layoutContext.m_startPos.y + listH };
    ImVec2 footerSize = {
        std::max(0.0f, layoutContext.m_avail.x - panelPadding * 2.0f), footerH
    };
    ImVec2 buttonPos = {
        footerPos.x, footerPos.y + std::max(0.0f, footerSize.y - btnSize) * 0.5f
    };

    // 加号入口使用弱化文本和透明按钮底色，悬浮或按下时提高反馈层透明度。
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
    Utils::pushFixedButtonStyleVars();

    // 手工绘制整宽背景，FeedbackButton 只负责输入和统一声音反馈。
    ImGui::SetCursorScreenPos(buttonPos);
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    ImVec4      bgCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    bgCol.w *= 0.5f;
    // 圆角沿用当前主题的 FrameRounding。
    float rounding = ImGui::GetStyle().FrameRounding;
    if ( ImGui::IsMouseHoveringRect(
             buttonPos,
             { buttonPos.x + footerSize.x, buttonPos.y + btnSize }) ) {
        // 整个页脚条悬浮时增强背景，而不局限于加号文字区域。
        bgCol.w *= 1.5f;
    }

    // 背景矩形与按钮使用相同屏幕边界。
    dl->AddRectFilled(buttonPos,
                      { buttonPos.x + footerSize.x, buttonPos.y + btnSize },
                      ImGui::ColorConvertFloat4ToU32(bgCol),
                      rounding);

    if ( ::MMM::UI::FeedbackButton(ICON_MMM_PLUS,
                                   ImVec2(footerSize.x, btnSize)) ) {
        // UIManager 按注册名查找唯一新建谱面向导实例。
        auto* wizard =
            sourceManager->getView<NewBeatmapWizard>("NewBeatmapWizard");
        // 视图未注册时静默忽略，避免空指针访问。
        if ( wizard ) wizard->open();
    }

    // 恢复页脚局部样式，避免影响随后的管理模态。
    Utils::popFixedButtonStyleVars();
    ImGui::PopStyleColor(4);
    if ( ImGui::IsItemHovered() ) {
        // 悬浮提示说明图标按钮的新建谱面语义。
        ImGui::SetTooltip("%s", TR_CACHE("ui.file.new_map").data());
    }

    // 非空管理路径同时作为模态存活状态和当前操作目标。
    bool showBMModal = !m_manageBeatmapPath.empty();
    if ( showBMModal ) {
        // 可见标题包含相对路径，`###` 后缀确保切换目标时内部 ID 不变。
        std::string windowTitle =
            fmt::format("{} {}###BeatmapManageWindow",
                        TR("ui.beatmap_manager.manage_title").data(),
                        m_manageBeatmapPath);
        if ( m_openManageModal ) {
            // 右键只设置打开边沿，OpenPopup 必须在当前 ImGui 帧中调用。
            ::MMM::UI::FeedbackOpenPopup(windowTitle.c_str());
            m_openManageModal = false;
        }
        // 管理窗口使用 DPI 缩放宽度和自动内容高度。
        Utils::CenteredModalPopupScope manageWindowScope(dpiScale);
        const bool                     manageWindowOpened =
            manageWindowScope.begin(windowTitle.c_str(),
                                    &showBMModal,
                                    ImGuiWindowFlags_NoCollapse,
                                    { 420 * dpiScale, 0.0f });
        // 标题栏关闭状态与按钮请求统一汇总到一个本地标志。
        bool closeManageModal = !showBMModal;
        if ( manageWindowOpened ) {
            // Clay 只负责本帧模态内容的尺寸与对齐，控件仍由 ImGui 绘制。
            CLayVBox    modalLayout;
            const auto& modalStyle = ImGui::GetStyle();
            /// 把非负浮点布局尺寸向上取整到 Clay 的 uint16 像素。
            auto toLayoutPixels = [](float value) {
                return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
            };
            // padding 和间距分别取 DPI 基线与主题值的较大者。
            float padding =
                std::max(16.0f * dpiScale, modalStyle.WindowPadding.x);
            const float modalGap =
                std::max(16.0f * dpiScale, modalStyle.ItemSpacing.y);
            const float buttonGap =
                std::max(12.0f * dpiScale, modalStyle.ItemSpacing.x);
            const float buttonH =
                std::max(32.0f * dpiScale, ImGui::GetFrameHeight());
            // 删除按钮宽度同时满足最小触控尺寸和本地化文本宽度。
            const float removeButtonW =
                std::max(140.0f * dpiScale,
                         ImGui::CalcTextSize(
                             TR("ui.beatmap_manager.remove_beatmap").data())
                                 .x +
                             modalStyle.FramePadding.x * 2.0f);
            // 取消按钮采用独立较小下限，但同样保证文本完整。
            const float cancelButtonW =
                std::max(100.0f * dpiScale,
                         ImGui::CalcTextSize(TR("ui.common.cancel").data()).x +
                             modalStyle.FramePadding.x * 2.0f);
            const uint16_t modalPaddingPx = toLayoutPixels(padding);
            // 四边使用相同 padding，元素之间使用统一纵向 gap。
            modalLayout.setPadding(
                modalPaddingPx, modalPaddingPx, modalPaddingPx, modalPaddingPx);
            modalLayout.setSpacing(toLayoutPixels(modalGap));

            // 顶部分隔线占满内容宽度，不重复显示已在窗口标题中的路径。
            modalLayout.addElement(
                "ModalSep",
                Sizing::Grow(),
                Sizing::Fixed(1),
                [=, this](Clay_BoundingBox r, bool) {
                    // Clay 回调把布局矩形转换为 ImDrawList 线段。
                    ImGui::GetWindowDrawList()->AddLine(
                        { r.x, r.y },
                        { r.x + r.width, r.y },
                        ImGui::GetColorU32(ImGuiCol_Separator));
                });

            // 两个操作按钮在单行内居中并保持主题水平间距。
            CLayHBox btnRow;
            btnRow.setAlignment(Alignment::Center());
            btnRow.setSpacing(toLayoutPixels(buttonGap));

            btnRow.addElement(
                "RemoveBtn",
                Sizing::Fixed(removeButtonW),
                Sizing::Fixed(buttonH),
                [=](Clay_BoundingBox r, bool) {
                    // ImGui 游标移动到 Clay 分配的绝对矩形。
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.beatmap_manager.remove_beatmap").data(),
                             { r.width, r.height }) ) {
                        // 删除需要二次确认，不立即发布命令。
                        ::MMM::UI::FeedbackOpenPopup("RemoveBeatmapConfirm");
                    }
                });

            btnRow.addElement(
                "CancelBtn",
                Sizing::Fixed(cancelButtonW),
                Sizing::Fixed(buttonH),
                [=, this, &closeManageModal](Clay_BoundingBox r, bool) {
                    // 捕获引用只在本帧同步 renderInCurrent 回调期间有效。
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.cancel").data(),
                             { r.width, r.height }) ) {
                        // 取消只关闭当前管理模态，不修改谱面。
                        closeManageModal = true;
                    }
                });

            // 按钮行横向增长，纵向固定为测得的按钮高度。
            modalLayout.addLayout(
                "BtnRowLayout", btnRow, Sizing::Grow(), Sizing::Fixed(buttonH));

            // Clay 返回实际内容尺寸，Dummy 推进 ImGui 游标并建立窗口高度。
            ImVec2 modalSize = modalLayout.renderInCurrent(
                ImGui::GetCursorScreenPos(),
                { ImGui::GetContentRegionAvail().x, 0 });
            ImGui::Dummy(modalSize);

            // 删除确认使用嵌套居中模态，目标仍由 m_manageBeatmapPath 持有。
            {
                // 独立样式作用域让确认框在外层管理窗口之上再次居中。
                Utils::CenteredModalPopupScope removeModalScope(dpiScale);
                if ( removeModalScope.begin("RemoveBeatmapConfirm") ) {
                    // 文本明确说明删除动作，目标路径由外层标题提供上下文。
                    ImGui::Text("%s",
                                TR("ui.beatmap_manager.remove_confirm").data());
                    ImGui::Spacing();
                    // 确认按钮使用 DPI 缩放的固定宽度，避免弹窗随翻译跳动。
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.confirm").data(),
                             { 100 * dpiScale, 0 }) ) {
                        // 命令复制相对路径，执行时不依赖本视图成员生命周期。
                        engine.pushCommand(
                            Logic::CmdRemoveBeatmap{ m_manageBeatmapPath });
                        // 同时请求关闭确认层和外层管理窗口。
                        closeManageModal = true;
                        // 当前调用位于确认弹窗上下文，先关闭最内层 Popup。
                        ImGui::CloseCurrentPopup();
                    }
                    // 两个确认操作保持同一行，便于用户比较。
                    ImGui::SameLine();
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.cancel").data(),
                             { 100 * dpiScale, 0 }) ) {
                        // 取消只关闭确认层，不改变外层 close 标志。
                        ImGui::CloseCurrentPopup();
                    }
                    // 与 removeModalScope.begin 成功分支配对。
                    ImGui::EndPopup();
                }
            }

            if ( closeManageModal ) {
                // 本地可见标志在本帧立即清零，阻止后续继续保留目标。
                showBMModal = false;
                // 此时当前 Popup 已回到外层管理窗口，关闭对应模态。
                ImGui::CloseCurrentPopup();
            }
            // 与 manageWindowScope.begin 成功分支配对。
            ImGui::EndPopup();
        }
        if ( !showBMModal ) {
            // 清空稳定路径是外层模态生命周期的最终状态转换。
            m_manageBeatmapPath.clear();
        }
    }

    // 对应函数开头可选 PushFont，所有正常与模态路径最终都在此恢复。
    if ( fileManagerFont ) ImGui::PopFont();
}

}  // namespace MMM::UI
