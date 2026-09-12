#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "ui/imgui/manager/AudioManagerView.h"
#include "audio/AudioManager.h"
#include "common/AudioResourceDragPayload.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "mmm/project/AudioResource.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>
#include <imgui_internal.h>
#include <nfd.h>
#include <system_error>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 音频资源表格列编号。
///
/// 枚举值与 TableSetupColumn 顺序和 ImGui 排序规格 ColumnIndex 保持一致。
/// 这些值也用于读取列显隐状态和定位单元格，不能脱离注册顺序单独调整。
/// 列重排只改变 ImGui 的显示顺序，不会改变这里的逻辑索引。
enum AudioTableColumn : int {
    /// @brief 音频资源 ID 列。
    AudioTableColumnId = 0,

    /// @brief 音频资源类型列。
    AudioTableColumnType = 1,

    /// @brief 音频资源路径列。
    AudioTableColumnPath = 2,

    /// @brief 音频文件大小列。
    AudioTableColumnSize = 3,

    /// @brief 修改时间列。
    AudioTableColumnModifiedTime = 4
};

/// @brief 文件系统元数据读取结果。
///
/// 每个值配有独立有效位，查询失败不会把默认零误认为真实文件属性。
/// 该结构按值保存在表格缓存行中，不持有路径或文件句柄。
/// 两项查询可部分成功，因此不存在覆盖整个结构的统一有效标志。
struct FileMetadata {
    /// @brief 文件大小字节数；读取失败时保持为 0。
    std::uintmax_t size{ 0 };

    /// @brief 是否成功读取文件大小。
    bool hasSize{ false };

    /// @brief 文件最后修改时间；读取失败时不参与精确排序。
    std::filesystem::file_time_type lastWriteTime{};

    /// @brief 是否成功读取最后修改时间。
    bool hasLastWriteTime{ false };
};

/// @brief 使用独立交互音量的音效 key 前缀。
///
/// UI 交互音效在混音时使用界面反馈音量，其余皮肤音效走普通音效音量。
/// 前缀常量集中定义，避免分类与后续新增皮肤键名规则分散在表格逻辑中。
constexpr const char* INTERACTION_SFX_KEY_PREFIX = "ui.";

/// @brief 判断皮肤音效是否属于界面交互音效。
/// @param key 音效 ID。
/// @return 属于交互音效时返回 true。
///
/// 使用位置零的前缀匹配，名称中间出现 ui. 不会被误分类。
bool isInteractionSfxKey(const std::string& key)
{
    // 不做大小写折叠，音效 ID 遵循皮肤配置定义的精确命名约定。
    // rfind 返回零仅当字符串从目标前缀开始。
    return key.rfind(INTERACTION_SFX_KEY_PREFIX, 0) == 0;
}

/// @brief 按音频管理器实际字体计算不可折行文本宽度。
/// @param text 待测量 UTF-8 文本；空指针返回零。
/// @return filemanager 字体或当前 ImGui 字体下的像素宽度。
///
/// UI 主线程路径允许访问 SkinManager 和 ImGui
/// 当前字体状态，供即时布局缓存回退。
/// @warning UI 热路径：只执行字体测量，不分配长期资源。
///
/// 此重载适合无法复用 UiFrameSnapshot 的局部即时测量。
/// 批量布局推导应优先使用显式字体重载，避免重复访问全局配置。
float measureAudioManagerText(const char* text)
{
    // 空标签无需访问皮肤或字体 Atlas。
    if ( !text ) return 0.0f;

    // 文件管理器专用字体与表格实际渲染保持一致。
    // 字体只借用 SkinManager 所有的对象，测量过程不转移生命周期。
    auto&   skinCfg = Config::SkinManager::instance();
    ImFont* font    = skinCfg.getFont("filemanager");
    if ( font ) {
        // 不折行并使用当前 ImGui 字号测量完整字符串。
        // CalcTextSizeA 的返回值包含二维尺寸，此处布局只关心水平占用。
        return font
            ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, text, nullptr)
            .x;
    }
    // 皮肤未提供专用字体时回退当前 ImGui 字体。
    return ImGui::CalcTextSize(text).x;
}

/// @brief 使用 UI 快照中的文件管理器字体计算不可折行文本宽度。
/// @param text 待测量 UTF-8 文本。
/// @param font 快照捕获的字体对象。
/// @param fontSize 快照捕获的字号。
/// @return 完整字符串像素宽度；输入不完整时返回零。
///
/// 该重载用于准备缓存，所有 ImGui 状态都由调用方在主线程快照中提供。
/// 字体和字号必须来自同一快照，否则结果不能作为布局缓存键对应的尺寸。
/// 函数不访问翻译器，调用方应在捕获快照后提供最终显示文本。
float measureAudioManagerText(const char* text, ImFont* font, float fontSize)
{
    // 工作线程不能自行查询 ImGui 当前字体，缺失快照时安全返回零。
    if ( !text || !font ) return 0.0f;

    // FLT_MAX 禁止换行，wrapWidth 为零保持单行表格语义。
    // end 参数为空表示测量到字符串终止符，结果与实际完整标签一致。
    return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, nullptr).x;
}

/// @brief 向当前最小高度累加一行列表内容和列表间距。
/// @param height 当前累计高度。
/// @param rowCount 已累计行数。
/// @param rowHeight 新行内容高度。
/// @param rowSpacing 相邻行间距。
///
/// 第一行前不加间距，之后每行只加一次，供最小内容高度计算复用。
void addAudioManagerListRow(float& height, size_t& rowCount, float rowHeight,
                            float rowSpacing)
{
    // 调用方负责传入非负尺寸，本函数只维护行间距的不重复计数规则。
    if ( rowCount > 0 ) {
        // 间距只存在于两行之间。
        height += rowSpacing;
    }
    // 内容高度和行计数同步推进。
    height += rowHeight;
    // 行数在高度更新后递增，使下一次调用能够识别已有前置行。
    rowCount++;
}

/// @brief 将单个 ASCII 字符转换为小写。
/// @param value 输入字符。
/// @return ASCII 大写字母对应的小写字符，其余字符保持不变。
///
/// 显式 ASCII 转换不依赖 locale，也不会破坏 UTF-8 多字节序列。
unsigned char lowerAscii(unsigned char value)
{
    // 只进入标准 ASCII 大写区间，UTF-8 高位字节不会参与算术转换。
    if ( value >= 'A' && value <= 'Z' ) {
        // 字母区间连续，可用固定偏移转换。
        return static_cast<unsigned char>(value - 'A' + 'a');
    }
    // 数字、标点、小写字母和非 ASCII 字节原样返回。
    return value;
}

/// @brief 比较两个 ASCII 字符串的大小，忽略大小写。
/// @param lhs 左侧字符串。
/// @param rhs 右侧字符串。
/// @return 小于返回负值，等于返回 0，大于返回正值。
///
/// 逐字节折叠 ASCII 后比较公共前缀，前缀相等时较短字符串在前。非 ASCII 字节保持
/// 原值，因此结果确定但不等同于自然语言排序。
int compareAsciiText(const std::string& lhs, const std::string& rhs)
{
    // 只遍历双方共有长度，避免越界。
    const size_t commonSize = std::min(lhs.size(), rhs.size());
    for ( size_t index = 0; index < commonSize; ++index ) {
        // 转为 unsigned char 后再比较，避免平台 char 符号差异。
        const auto left  = lowerAscii(static_cast<unsigned char>(lhs[index]));
        const auto right = lowerAscii(static_cast<unsigned char>(rhs[index]));
        // 首个不同折叠字节即可确定字典序，无需检查剩余后缀。
        if ( left < right ) return -1;
        if ( left > right ) return 1;
    }
    // 公共前缀相同后由长度打破前缀关系。
    if ( lhs.size() < rhs.size() ) return -1;
    if ( lhs.size() > rhs.size() ) return 1;
    // 长度也一致说明两串在 ASCII 忽略大小写规则下等价。
    return 0;
}

/// @brief 读取文件大小和修改时间。
/// @param filePath 需要查询的绝对文件路径。
/// @return 文件系统元数据；读取失败的字段保持无效。
///
/// 大小和修改时间分别查询、分别记录有效位，一个属性失败不会阻止另一个属性用于
/// 展示与排序。使用 error_code 重载遵守禁异常约束。
/// @warning 低频准备路径：资源集合变化或布局缓存失效时读取文件状态。
FileMetadata queryFileMetadata(const std::filesystem::path& filePath)
{
    // 默认构造结果的两个有效位均为 false。
    FileMetadata metadata;

    // 文件大小查询失败时保留默认值但不设 hasSize。
    std::error_code filesystemError;
    // 使用 error_code 重载，路径不存在、权限不足等情况都不会抛出异常。
    const auto size = std::filesystem::file_size(filePath, filesystemError);
    if ( !filesystemError ) {
        // 只有错误码为空时，file_size 返回值才具备业务含义。
        metadata.size    = size;
        metadata.hasSize = true;
    }

    // 修改时间使用同一 error_code 前先清除大小查询错误。
    filesystemError.clear();
    // 修改时间查询独立执行，即使文件大小因特殊文件类型读取失败也可显示时间。
    const auto modifiedTime =
        std::filesystem::last_write_time(filePath, filesystemError);
    if ( !filesystemError ) {
        // 保存原始 file_time_type，格式化和排序分别按需使用它。
        metadata.lastWriteTime    = modifiedTime;
        metadata.hasLastWriteTime = true;
    }
    // 两个属性均尝试后返回部分有效结果。
    return metadata;
}

/// @brief 生成文件大小显示文本。
/// @param size 文件字节数。
/// @return 适合列表展示的大小文本。
///
/// 采用 1024 二进制进位，字节范围保持整数，大单位由翻译格式控制小数精度。
std::string formatFileSize(std::uintmax_t size)
{
    // 常量使用 double 便于后续无整数截断除法。
    constexpr double kibi = 1024.0;
    constexpr double mebi = kibi * 1024.0;
    constexpr double gibi = mebi * 1024.0;

    // 阈值使用严格小于，边界上的 1024 字节会自然提升到 KiB。
    if ( size < 1024 ) {
        // 小文件直接展示精确字节数。
        return TR_FMT("ui.file_manager.size_bytes", size);
    }
    // 统一转换为浮点后执行各单位除法，由翻译格式决定显示精度。
    const double value = static_cast<double>(size);
    if ( value < mebi ) {
        // KiB 区间不跨越 MiB 阈值。
        return TR_FMT("ui.file_manager.size_kib", value / kibi);
    }
    if ( value < gibi ) {
        // MiB 区间使用对应本地化单位。
        return TR_FMT("ui.file_manager.size_mib", value / mebi);
    }
    // GiB 是当前最高显示单位，更大文件仍以 GiB 数值表达。
    return TR_FMT("ui.file_manager.size_gib", value / gibi);
}

/// @brief 将文件系统时间转换为本地时间文本。
/// @param time 文件系统时间。
/// @return 本地时间文本，格式为 yyyy/mm/dd HH:MM。
///
/// file_time_type 与 system_clock
/// 可能使用不同纪元，先以两种时钟当前差值近似转换，
/// 再调用平台线程安全的本地时间 API。strftime 失败时显示未知。
std::string formatModifiedTime(std::filesystem::file_time_type time)
{
    // 当前时刻对齐可在不假设 epoch 的情况下转换文件时间。
    // 以两个时钟的当前时刻为锚点，平移输入时间到 system_clock 域。
    const auto systemTime =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            time - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    // 平台本地时间函数接受 time_t。
    const std::time_t timeValue =
        std::chrono::system_clock::to_time_t(systemTime);

    // 零初始化目标结构，平台函数会覆盖有效字段。
    std::tm localTime{};
#ifdef _WIN32
    // Windows 使用目标缓冲在前的安全版本。
    localtime_s(&localTime, &timeValue);
#else
    // POSIX 可重入版本避免共享静态 tm。
    localtime_r(&timeValue, &localTime);
#endif

    // 固定日期格式最大长度远小于缓冲区。
    char buffer[32]{};
    // 格式保持定长且不含秒，便于列宽预测和列表快速比较。
    if ( std::strftime(buffer, sizeof(buffer), "%Y/%m/%d %H:%M", &localTime) ==
         0 ) {
        // 零返回值表示格式化失败或缓冲区不足，使用本地化未知值。
        return TR("ui.file_manager.value_unknown").data();
    }
    // std::string 从栈缓冲复制结果，返回后不依赖局部数组生命周期。
    return buffer;
}

/// @brief 生成文件大小列文本。
/// @param hasSize 是否成功读取大小。
/// @param size 文件大小字节数。
/// @return 大小或未知占位。
///
/// 有效位来自独立文件系统查询，不能根据 size 是否为零推断成功，因为空文件合法。
std::string formatSizeColumn(bool hasSize, std::uintmax_t size)
{
    if ( !hasSize ) {
        // 查询失败显示未知而不是零字节。
        return TR("ui.file_manager.value_unknown").data();
    }
    // 有效零值会正确格式化为零字节。
    return formatFileSize(size);
}

/// @brief 生成修改时间列文本。
/// @param hasLastWriteTime 是否成功读取修改时间。
/// @param lastWriteTime 文件修改时间。
/// @return 修改时间或未知占位。
///
/// 默认构造 file_time_type 可能表示有效纪元值，因此必须依赖显式有效位。
std::string formatModifiedColumn(bool hasLastWriteTime,
                                 std::filesystem::file_time_type lastWriteTime)
{
    if ( !hasLastWriteTime ) {
        // 权限或文件消失造成的失败使用统一占位。
        return TR("ui.file_manager.value_unknown").data();
    }
    // 仅将已验证的时间值交给本地时间转换。
    return formatModifiedTime(lastWriteTime);
}

/// @brief 比较两个可选数值。
/// @tparam T 可比较数值类型。
/// @param lhs 左值。
/// @param lhsValid 左值是否有效。
/// @param rhs 右值。
/// @param rhsValid 右值是否有效。
/// @return 小于返回 -1，大于返回 1，相等返回 0。
///
/// 有效值在基础升序比较中排在未知值前，双方未知视为相等并交给资源 ID 回退排序。
template<typename T>
int compareOptionalValue(const T& lhs, bool lhsValid, const T& rhs,
                         bool rhsValid)
{
    if ( lhsValid != rhsValid ) {
        // 只有一侧有效时优先有效数据。
        return lhsValid ? -1 : 1;
    }
    if ( !lhsValid ) {
        // 双方均未知，不比较默认构造值。
        return 0;
    }
    // 使用严格弱序所需的双向小于比较，不要求 T 提供大于运算符。
    if ( lhs < rhs ) return -1;
    if ( rhs < lhs ) return 1;
    // 双向均不小于即按排序语义相等。
    return 0;
}

/// @brief 组合排序菜单项显示文本。
/// @param columnLabel 排序字段显示名。
/// @param ascending 是否升序。
/// @return 带方向后缀的菜单文本。
///
/// 本地化标签只用于显示，排序身份由调用点传入的枚举决定。
std::string makeSortMenuLabel(const char* columnLabel, bool ascending)
{
    // 升降序使用不同翻译模板，使语序可由目标语言自行调整。
    return ascending
               ? TR_FMT("ui.resource_table.sort_ascending_fmt", columnLabel)
               : TR_FMT("ui.resource_table.sort_descending_fmt", columnLabel);
}

/// @brief 查询表格列当前是否有效显示。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @return 当前帧列有效显示时返回 true。
///
/// IsEnabled 是当前布局后的实际可见状态，用于判断单列自动适配操作。
bool isTableColumnEnabled(const ImGuiTable* table, int column)
{
    // 表指针和索引必须同时有效才读取内部 Columns。
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsEnabled;
}

/// @brief 查询表格列的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @return 用户设置为显示时返回 true。
///
/// IsUserEnabled 不受本帧裁剪影响，适合列显隐菜单展示勾选态。
bool isTableColumnUserEnabled(const ImGuiTable* table, int column)
{
    // 防御检查与实际可见状态 helper 一致，调用者可安全传入上下文列。
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsUserEnabled;
}

/// @brief 排队设置表格列下一帧的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @param enabled 是否显示。
///
/// 通过 IsUserEnabledNextFrame 排队修改，避免在当前 Table
/// 布局中直接改内部状态。
void queueTableColumnEnabled(ImGuiTable* table, int column, bool enabled)
{
    if ( !table || column < 0 || column >= table->ColumnsCount ) {
        // 当前表不存在或列号越界时忽略请求。
        return;
    }
    // 写入 NextFrame 字段，让 ImGui 在下一次表格布局开始时统一应用。
    table->Columns[column].IsUserEnabledNextFrame = enabled;
}

/// @brief 绘制可裁剪、超宽自动滚动的表格单元格文本。
/// @param text 需要绘制的文本。
/// @param cursorPos 单元格起始屏幕坐标。
/// @param width 单元格可用宽度。
/// @param height 行高。
///
/// 文本短于列宽时保持静止；超宽时用正弦缓动在裁剪区内往返滚动，并为末尾增加
/// 40 像素停留余量。绘制后提交 Dummy 占位，保持 ImGui 行布局高度。
/// @warning UI 热路径：可见资源行每帧调用，只追加裁剪文本绘制命令。
///
/// 动画不保存逐行状态，所有溢出文本共享 ImGui 时间但按各自宽度计算范围。
/// 裁剪区使用屏幕坐标，调用方必须先切换到目标表格列。
/// 本函数不会提交交互控件，行点击与悬浮由外层跨列 Selectable 负责。
void renderScrollingTableText(const std::string& text, ImVec2 cursorPos,
                              float width, float height)
{
    // 列宽不能为负，极窄状态夹到零。
    const float textWidth = std::max(0.0f, width);
    // 完整测量文本决定是否启用动画，短文本不会产生任何水平偏移。
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const float  textH    = ImGui::GetFontSize();
    // 文本在行高中垂直居中。
    const float offsetY = (height - textH) * 0.5f;

    // 默认偏移为零，只有内容溢出时才计算随时间变化的滚动位置。
    float offset = 0.0f;
    if ( textSize.x > textWidth ) {
        // 额外范围让文本末端完全进入视区后稍作停留。
        const float scrollRange = textSize.x - textWidth + 40.0f;
        // 使用 ImGui 单调帧时间，使所有溢出单元格保持一致滚动节奏。
        const float time = static_cast<float>(ImGui::GetTime());
        // 正弦相位从左端开始，随后压缩两端区间形成停顿。
        float t = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
        t       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
        offset  = t * scrollRange;
    }

    // 裁剪矩形严格限制在当前单元格的可用宽高内。
    const ImVec2 textStartPos = cursorPos;
    ImGui::PushClipRect(
        textStartPos,
        ImVec2(textStartPos.x + textWidth, textStartPos.y + height),
        true);
    // 直接提交绘制命令可让文本移动而不改变 ImGui 的布局游标。
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(textStartPos.x - offset, textStartPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    // 每个单元格独立恢复裁剪栈，避免影响相邻列和后续行。
    ImGui::PopClipRect();
}

/// @brief 捕获音频管理器同步测量所需的当前帧快照。
/// @param dpiScale 当前窗口内容缩放。
/// @return 可用于布局缓存匹配与重建的值类型快照。
///
/// 快照只保存字体观察指针和轻量配置值，不拥有 SkinManager 或 ImGui 资源。
/// 调用方必须保证字体对象在准备和交换期间仍由皮肤系统保持有效。
/// 翻译版本用于识别同一语言内的文本热更新，语言字段则识别语言切换。
/// 字体偏好与两个缩放倍率共同覆盖字体重建后可能产生的尺寸变化。
/// FramePadding、FrameHeight 与 ItemSpacing 共同决定控制行和表格行尺寸。
/// 美学配置按原始值保存，具体 DPI 取整只在 buildLayoutMetrics 中进行。
UiFrameSnapshot captureAudioManagerUiFrameSnapshot(float dpiScale)
{
    // 布局测量只依赖当前帧可稳定读取的配置与 ImGui 状态。
    // 将这些输入集中复制到快照后，后台准备阶段不再访问全局单例。
    auto&       appConfig  = Config::AppConfig::instance();
    const auto& settings   = appConfig.getEditorSettings();
    const auto& aesthetics = settings.aesthetics;
    auto&       skinCfg    = Config::SkinManager::instance();
    const auto& style      = ImGui::GetStyle();

    UiFrameSnapshot snapshot;
    // 极小或无效的窗口缩放按 1 处理，避免后续固定尺寸退化。
    snapshot.dpiScale = std::max(1.0f, dpiScale);
    // 控件高度与内边距必须来自同一帧的样式，防止缓存混用。
    snapshot.framePadding           = style.FramePadding;
    snapshot.frameHeight            = ImGui::GetFrameHeight();
    snapshot.frameHeightWithSpacing = ImGui::GetFrameHeightWithSpacing();
    // 保留各语义字体指针，测量时按与实际绘制一致的回退顺序选择。
    snapshot.contentFont     = skinCfg.getFont("content");
    snapshot.menuFont        = skinCfg.getFont("menu");
    snapshot.fileManagerFont = skinCfg.getFont("filemanager");
    snapshot.fallbackFont    = ImGui::GetFont();
    snapshot.fontSize        = ImGui::GetFontSize();
    // 翻译版本和字体配置共同决定标签宽度，任一变化都应使缓存失效。
    snapshot.translationVersion = skinCfg.getTranslator().getVersion();
    snapshot.language           = settings.language;
    snapshot.preferredAsciiFont = settings.preferredAsciiFont;
    snapshot.preferredCjkFont   = settings.preferredCjkFont;
    snapshot.fontSizeMultiplier = settings.fontSizeMultiplier;
    snapshot.uiScaleMultiplier  = settings.uiScaleMultiplier;
    // 美学配置中的间距会参与最小尺寸推导，也属于缓存键。
    snapshot.windowPadding = aesthetics.windowPadding;
    snapshot.itemSpacing   = aesthetics.itemSpacing;
    return snapshot;
}
}  // namespace

/// @brief 创建音频管理器并订阅外部目录刷新通知。
/// @param subViewName 子视图名称。
///
/// 订阅回调可能不在 UI 绘制时刻执行，因此只通过原子布尔值传递刷新意图。
/// 多次目录事件可以安全合并为一次表格缓存重建，不需要保存事件数量。
/// @warning 回调不得读取项目、皮肤或 ImGui 状态，也不得直接重建资源表格。
AudioManagerView::AudioManagerView(const std::string& subViewName)
    : ISubView(subViewName)
{
    // 事件可能由文件导入或项目刷新流程发出；回调只设置原子脏标记。
    // 实际表格重建留到 UI 线程，避免跨线程读取项目资源容器。
    m_projectDirectoryRefreshedSubId =
        Event::EventBus::instance()
            .subscribe<Event::ProjectDirectoryRefreshedEvent>(
                [this](const Event::ProjectDirectoryRefreshedEvent&) {
                    m_projectDirectoryRefreshPending.store(
                        true, std::memory_order_release);
                });
}

/// @brief 取消音频管理器的目录刷新订阅。
///
/// 订阅 ID 与视图生命周期绑定；解除后事件总线不再持有捕获 this 的回调。
/// 析构过程不触发设备查询、项目命令或 UI 绘制。
AudioManagerView::~AudioManagerView()
{
    // 视图销毁前解除订阅，防止事件总线继续调用已失效的 this。
    Event::EventBus::instance()
        .unsubscribe<Event::ProjectDirectoryRefreshedEvent>(
            m_projectDirectoryRefreshedSubId);
}

/// @brief 捕获当前音频管理器布局输入。
/// @return 当前项目、皮肤音效数量和展开状态。
///
/// 项目指针只用于立即读取存在性和音频资源数量，不写入快照也不跨帧保存。
/// 资源内容变化由目录事件使表格标脏，布局层只关心影响高度的数量变化。
AudioManagerView::LayoutInputSnapshot
AudioManagerView::captureLayoutInput() const
{
    // 此处只采集影响行数和页脚展开高度的轻量状态。
    // 音频路径内容不参与布局测量，避免为求最小尺寸遍历资源。
    LayoutInputSnapshot input;
    auto&               engine  = Logic::EditorEngine::instance();
    auto*               project = engine.getCurrentProject();
    input.hasProject            = project != nullptr;
    input.permanentSfxCount =
        Config::SkinManager::instance().getData().audioPaths.size();
    input.showGlobalSettings = m_showGlobalSettings;
    input.projectAudioCount  = project ? project->m_audioResources.size() : 0;
    return input;
}

/// @brief 判断布局测量缓存是否匹配当前帧状态。
/// @param cache 需要检查的布局缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param input 当前布局输入。
/// @return 完全匹配时返回 true。
///
/// 有效位是首要条件；默认构造缓存即使数值恰好相等也不能命中。
/// 浮点比较覆盖 DPI、字号、间距和内边距，字符串与版本字段使用精确比较。
/// 本函数不访问全局状态，可在已捕获输入上进行纯值判断。
bool AudioManagerView::layoutMetricsMatch(const LayoutMetricsCache&  cache,
                                          const UiFrameSnapshot&     snapshot,
                                          const LayoutInputSnapshot& input)
{
    // 浮点输入来自缩放与样式计算，使用小容差屏蔽无意义的舍入抖动。
    auto floatEqual = [](float lhs, float rhs) {
        return std::abs(lhs - rhs) <= 0.0001f;
    };
    // 项目切换、资源数量或折叠状态改变都会影响布局占用。
    const bool inputMatch =
        cache.input.hasProject == input.hasProject &&
        cache.input.permanentSfxCount == input.permanentSfxCount &&
        cache.input.projectAudioCount == input.projectAudioCount &&
        cache.input.showGlobalSettings == input.showGlobalSettings;

    // 除业务输入外，任何会改变文字宽度或控件间距的 UI 输入都要匹配。
    return cache.valid && inputMatch &&
           floatEqual(cache.dpiScale, snapshot.dpiScale) &&
           floatEqual(cache.fontSize, snapshot.fontSize) &&
           floatEqual(cache.framePadding.x, snapshot.framePadding.x) &&
           floatEqual(cache.framePadding.y, snapshot.framePadding.y) &&
           floatEqual(cache.frameHeight, snapshot.frameHeight) &&
           floatEqual(cache.frameHeightWithSpacing,
                      snapshot.frameHeightWithSpacing) &&
           cache.language == snapshot.language &&
           cache.translationVersion == snapshot.translationVersion &&
           cache.preferredAsciiFont == snapshot.preferredAsciiFont &&
           cache.preferredCjkFont == snapshot.preferredCjkFont &&
           floatEqual(cache.fontSizeMultiplier, snapshot.fontSizeMultiplier) &&
           floatEqual(cache.uiScaleMultiplier, snapshot.uiScaleMultiplier) &&
           floatEqual(cache.windowPadding, snapshot.windowPadding) &&
           floatEqual(cache.itemSpacing, snapshot.itemSpacing);
}

/// @brief 构造音频管理器布局测量缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param input 当前布局输入。
/// @return 音频管理器布局测量结果。
///
/// 尺寸推导分为三层：基础样式尺寸、表格或空状态高度、页脚控制高度。
/// 所有派生值都写入 LayoutMetricsCache，实际绘制阶段只做区域切分。
/// 文字宽度使用与资源表格相同的字体回退顺序，避免测量字体与绘制字体不一致。
/// 最小宽度同时覆盖最长标签控制行和最长表头，窄窗口不会使固定控件重叠。
/// 最小高度根据资源数量与折叠状态变化，导入入口无论折叠与否均保留空间。
/// @warning 布局准备路径：仅在快照键变化时调用，不得读取文件或遍历音频内容。
AudioManagerView::LayoutMetricsCache AudioManagerView::buildLayoutMetrics(
    const UiFrameSnapshot& snapshot, const LayoutInputSnapshot& input)
{
    LayoutMetricsCache cache;
    // 先保存完整缓存键；调用方可在下帧直接执行逐项匹配。
    cache.valid                  = true;
    cache.input                  = input;
    cache.dpiScale               = snapshot.dpiScale;
    cache.fontSize               = snapshot.fontSize;
    cache.framePadding           = snapshot.framePadding;
    cache.frameHeight            = snapshot.frameHeight;
    cache.frameHeightWithSpacing = snapshot.frameHeightWithSpacing;
    cache.language               = snapshot.language;
    cache.translationVersion     = snapshot.translationVersion;
    cache.preferredAsciiFont     = snapshot.preferredAsciiFont;
    cache.preferredCjkFont       = snapshot.preferredCjkFont;
    cache.fontSizeMultiplier     = snapshot.fontSizeMultiplier;
    cache.uiScaleMultiplier      = snapshot.uiScaleMultiplier;
    cache.windowPadding          = snapshot.windowPadding;
    cache.itemSpacing            = snapshot.itemSpacing;

    // 下列尺寸统一从快照推导，保证同步路径与准备路径得到同一结果。
    const float scale       = std::max(1.0f, snapshot.dpiScale);
    const float frameH      = snapshot.frameHeight;
    const float frameWithSp = snapshot.frameHeightWithSpacing;
    // 像素间距取整，避免 Clay 与 ImGui 在亚像素边界出现累计错位。
    const float itemSpacing = std::floor(snapshot.itemSpacing * scale);
    const float rowSpacingY =
        std::ceil(std::max(4.0f * scale, itemSpacing * 0.5f));
    // 标签额外留白用于隔开固定标签列与右侧可伸缩控件。
    const float labelPad   = std::floor(12.0f * scale);
    const float rootPad    = std::floor(4.0f * scale);
    const float footerPadX = rootPad;
    // 分区间距大于行间距，使列表与全局设置在视觉上保持层次。
    const float sectionSpacing =
        std::ceil(std::max(12.0f * scale, itemSpacing));
    const float footerSpacing =
        std::ceil(std::max(2.0f * scale, itemSpacing * 0.25f));
    const float controlColGap = std::ceil(std::max(8.0f * scale, itemSpacing));
    const float labelColGap =
        std::ceil(std::max(4.0f * scale, itemSpacing * 0.5f));
    // 控件行高度同时覆盖字体、ImGui 框体和触控友好的最小高度。
    const float rowPaddingY = snapshot.framePadding.y * 2.0f;
    const float controlRowH = std::ceil(
        std::max({ frameH, snapshot.fontSize + rowPaddingY, 32.0f * scale }));
    // 静音按钮保持正方形，尺寸不小于普通 ImGui 控件高度。
    const float muteButtonSize = std::ceil(std::max(frameH, 30.0f * scale));
    // 表格行需要容纳字体和上下内边距，并保留最低可点击高度。
    const float audioItemHeight = std::ceil(
        std::max({ frameH, snapshot.fontSize + rowPaddingY, 28.0f * scale }));
    // 空状态使用独立占位高度，避免提示紧贴页脚标题。
    const float hintSpacerH = std::ceil(std::max(20.0f * scale, frameH * 0.5f));
    const float hintRowH    = std::ceil(std::max(30.0f * scale, frameH));
    const float importButtonH = std::ceil(std::max(32.0f * scale, frameH));
    const float importButtonGap =
        std::ceil(std::max(8.0f * scale, itemSpacing));
    // 把布局阶段需要的尺寸一次性写入缓存，绘制时不再重复测量。
    cache.rootPadding        = rootPad;
    cache.sectionSpacing     = sectionSpacing;
    cache.listRowSpacing     = rowSpacingY;
    cache.audioItemHeight    = audioItemHeight;
    cache.hintSpacerHeight   = hintSpacerH;
    cache.hintRowHeight      = hintRowH;
    cache.footerPaddingX     = footerPadX;
    cache.footerSpacing      = footerSpacing;
    cache.footerHeaderHeight = frameWithSp;
    cache.controlRowHeight   = controlRowH;
    cache.controlColumnGap   = controlColGap;
    cache.labelColumnGap     = labelColGap;
    cache.muteButtonSize     = muteButtonSize;
    cache.importButtonHeight = importButtonH;
    cache.importButtonGap    = importButtonGap;
    // 资源管理字体优先；缺失时依次回退到内容字体和当前 ImGui 字体。
    ImFont* font = snapshot.fileManagerFont
                       ? snapshot.fileManagerFont
                       : (snapshot.contentFont ? snapshot.contentFont
                                               : snapshot.fallbackFont);

    // 固定标签列按所有全局控制项中的最宽文本确定。
    const std::array<const char*, 5> controlLabels{
        TR("ui.audio_manager.output_device").data(),
        TR("ui.audio_manager.global_volume").data(),
        TR("ui.audio_manager.bgm_gain").data(),
        TR("ui.audio_manager.sfx_gain").data(),
        TR("ui.audio_manager.interaction_sfx_volume").data()
    };

    float labelWidth = 0.0f;
    for ( const char* label : controlLabels ) {
        // 翻译文本长度随语言变化，因此必须逐项取最大值。
        labelWidth =
            std::max(labelWidth,
                     measureAudioManagerText(label, font, snapshot.fontSize));
    }
    labelWidth += labelPad;
    cache.footerLabelWidth = labelWidth;

    // 滑条最小宽度需容纳数值文本、框内边距和可操作轨道。
    const float sliderValueW =
        std::max(measureAudioManagerText("0.00", font, snapshot.fontSize),
                 measureAudioManagerText("100%", font, snapshot.fontSize));
    const float sliderMinW = sliderValueW + snapshot.framePadding.x * 4.0f +
                             std::floor(48.0f * scale);

    // 表头和折叠标题共同约束窄窗口下仍可辨认的最小宽度。
    const std::array<const char*, 6> headers{
        TR("ui.audio_manager.column_id").data(),
        TR("ui.audio_manager.column_type").data(),
        TR("ui.audio_manager.column_path").data(),
        TR("ui.audio_manager.column_size").data(),
        TR("ui.audio_manager.column_modified_time").data(),
        TR("ui.audio_manager.global_settings").data(),
    };

    float headerWidth = 0.0f;
    for ( const char* header : headers ) {
        // 预留一个框高给排序箭头或折叠指示，再计入文字宽度。
        headerWidth = std::max(
            headerWidth,
            frameH + itemSpacing +
                measureAudioManagerText(header, font, snapshot.fontSize));
    }

    // 页脚控制行通常比表头更宽，两者取最大值作为内容下限。
    const float controlRowWidth = footerPadX * 2.0f + labelWidth +
                                  controlColGap + muteButtonSize +
                                  controlColGap + sliderMinW;
    float       minWidth =
        std::ceil(rootPad * 2.0f + std::max({ controlRowWidth, headerWidth }));

    // 列表最小高度按当前资源数量推导，空状态则计算提示占位。
    float        listHeight = 0.0f;
    size_t       listRows   = 0;
    const float  headerRowH = cache.footerHeaderHeight;
    const size_t audioResourceCount =
        input.permanentSfxCount + input.projectAudioCount;
    if ( input.hasProject || audioResourceCount > 0 ) {
        // 有项目时即使暂时无音频，也保留一行表格内容空间。
        const size_t visibleRows = std::max<size_t>(audioResourceCount, 1);
        addAudioManagerListRow(
            listHeight,
            listRows,
            headerRowH + static_cast<float>(visibleRows) * audioItemHeight,
            rowSpacingY);
    } else {
        // 完全无项目且无皮肤音效时展示居中的初始提示。
        addAudioManagerListRow(listHeight, listRows, hintSpacerH, rowSpacingY);
        addAudioManagerListRow(listHeight, listRows, hintRowH, rowSpacingY);
    }

    // 页脚标题始终存在，展开后再加入五个固定高度控制行。
    float footerH = cache.footerHeaderHeight;
    if ( input.showGlobalSettings ) {
        footerH += 5.0f * controlRowH + 5.0f * footerSpacing;
    }
    // 导入与项目工具按钮独立于折叠区域，始终计入总高度。
    footerH += importButtonH + importButtonGap;
    cache.globalControlsHeight = footerH - (importButtonH + importButtonGap);
    cache.footerHeight         = footerH;

    // 最终尺寸取整到完整像素，作为窗口不可再压缩的内容下限。
    float minHeight      = std::ceil(rootPad * 2.0f + listHeight + footerH);
    cache.minContentSize = ImVec2(minWidth, minHeight);
    return cache;
}

/// @brief 获取音频管理器布局测量缓存。
/// @param dpiScale 当前窗口内容缩放。
/// @return 与当前语言、字体、缩放和资源数量匹配的布局测量结果。
///
/// 本函数是同步兜底入口。若帧前准备已经生成相同快照，它只返回现有缓存；
/// 若语言、字体、样式、项目或资源数量在准备后变化，则在 UI 线程即时重建。
/// 返回引用仅在下一次缓存赋值前有效，调用方不得跨帧保存。
const AudioManagerView::LayoutMetricsCache& AudioManagerView::getLayoutMetrics(
    float dpiScale) const
{
    // 同步路径供实际绘制兜底；正常情况下可直接命中准备好的缓存。
    const LayoutInputSnapshot input = captureLayoutInput();
    const UiFrameSnapshot     snapshot =
        captureAudioManagerUiFrameSnapshot(dpiScale);
    if ( !layoutMetricsMatch(m_layoutMetricsCache, snapshot, input) ) {
        // 缓存键变化时才重新测量，避免每帧重复计算翻译文本宽度。
        m_layoutMetricsCache = buildLayoutMetrics(snapshot, input);
    }
    return m_layoutMetricsCache;
}

/// @brief 判断当前帧是否需要准备音频管理器布局数据。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要刷新布局缓存时返回 true。
///
/// 判断期间同步捕获业务输入，prepareUiFrameData 随后消费同一份输入，
/// 从而避免“判断使用旧项目、构建使用新项目”造成缓存键与结果不一致。
bool AudioManagerView::needsParallelUiPrepare(
    const UiFrameSnapshot& snapshot) const
{
    // 保存业务侧输入，保证随后准备阶段使用的是同一帧状态。
    m_prepareLayoutInput = captureLayoutInput();
    return !layoutMetricsMatch(
        m_layoutMetricsCache, snapshot, m_prepareLayoutInput);
}

/// @brief 在 UI 主线程准备音频管理器布局测量数据。
/// @param snapshot 当前帧 UI 快照。
///
/// 结果写入独立候选对象并设置完成标记，不会覆盖当前帧仍在使用的缓存。
/// 调用顺序要求 needsParallelUiPrepare 已先保存对应的布局输入。
void AudioManagerView::prepareUiFrameData(const UiFrameSnapshot& snapshot)
{
    // 结果先写入候选缓存，直到 UI 主线程安全交换后才对绘制可见。
    m_preparedLayoutMetricsCache =
        buildLayoutMetrics(snapshot, m_prepareLayoutInput);
    m_hasPreparedLayoutMetrics = true;
}

/// @brief 将准备好的布局测量数据切换给主线程使用。
///
/// 交换只移动值类型缓存，不持有项目或音频资源所有权。
/// 没有候选结果时保持幂等，允许 UIManager 每帧无条件调用。
void AudioManagerView::swapPreparedUiFrameData()
{
    if ( !m_hasPreparedLayoutMetrics ) {
        // 没有新结果时保留现有缓存，避免用默认值覆盖有效布局。
        return;
    }

    // 交换发生在帧边界，不与当前正在读取的布局数据交叉。
    m_layoutMetricsCache       = std::move(m_preparedLayoutMetricsCache);
    m_hasPreparedLayoutMetrics = false;
}

/// @brief 获取音频管理器中不可再换行控件所需的最小内容尺寸。
/// @param dpiScale 当前窗口内容缩放。
/// @return 缓存推导出的最小内容宽高。
///
/// 返回值包含列表、页脚和底部入口，不包含外层窗口标题栏或停靠装饰。
/// 调用会沿用与实际绘制相同的缓存匹配规则，因此约束与当前语言和项目一致。
ImVec2 AudioManagerView::getMinContentSize(float dpiScale) const
{
    return getLayoutMetrics(dpiScale).minContentSize;
}

/// @brief 使用 Clay 与 ImGui 渲染音频管理器主界面。
/// @param layoutContext 当前布局上下文。
/// @param sourceManager 当前 UI 管理器，用于打开音轨控制器。
/// @warning UI 热路径：每帧执行；只允许读取已缓存表格数据和响应用户交互，
/// 资源扫描必须通过脏标记触发。
///
/// 绘制分为四个稳定区域：资源列表、全局设置、底部入口和确认弹窗。
/// Clay 负责计算区域包围盒，ImGui 回调负责在最终屏幕坐标提交控件与绘制命令。
/// 项目和皮肤资源先合并到排序缓存，再按分类连续分组，长列表由裁剪器限制工作量。
/// 文件大小与修改时间只在对应列可见或作为排序键时读取，并缓存失败结果。
/// 所有模型修改均通过 EditorEngine 命令入口提交；视图只直接修改音频运行参数。
/// 临时字体、颜色、样式、子窗口与弹窗作用域必须在本函数内保持严格成对。
/// 输出设备枚举使用后端身份参与缓存，后端切换后不会展示旧设备列表。
/// 表格排序状态由视图持有，ImGui 排序规格只作为用户操作输入同步进来。
/// 分组展开状态不随排序缓存重建丢失，用户可连续浏览资源更新后的同一分类。
/// 项目切换会通过缓存中的项目指针身份触发重建，不复用前一项目的路径数据。
/// @warning 目录事件的原子标记只用于跨线程合并通知，不承载资源数据。
void AudioManagerView::onUpdate(LayoutContext& layoutContext,
                                UIManager*     sourceManager)
{
    // 目录事件回调只负责发布标记；本帧在 UI 线程消费并使排序缓存失效。
    if ( m_projectDirectoryRefreshPending.exchange(
             false, std::memory_order_acq_rel) ) {
        m_audioTableSortCacheDirty = true;
    }

    // 所有全局对象只在本帧入口解析一次，后续局部绘制器借用这些引用。
    auto& engine       = Logic::EditorEngine::instance();
    auto* project      = engine.getCurrentProject();
    auto& skinCfg      = Config::SkinManager::instance();
    auto& audioManager = Audio::AudioManager::instance();

    // 文件管理字体与资源表格的视觉语义一致；缺失时沿用当前字体。
    float   dpiScale        = layoutContext.m_dpiScale;
    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) {
        ImGui::PushFont(fileManagerFont, fileManagerFont->LegacySize);
    }

    // 绘制只读取缓存后的尺寸，避免在多个 Clay 回调中重复测量。
    const auto& layoutMetrics = getLayoutMetrics(dpiScale);
    const float maxLabelW     = layoutMetrics.footerLabelWidth;
    // Clay 的像素尺寸使用无符号整数；先夹到非负值再向上取整。
    auto toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    // 输出设备枚举可能涉及后端查询，仅在后端变化或显式标脏时刷新。
    const auto playbackBackend = audioManager.getPlaybackBackend();
    if ( m_outputDevicesDirty ||
         m_cachedOutputDeviceBackend != playbackBackend ) {
        m_cachedOutputDevices       = audioManager.listOutputDevices();
        m_cachedOutputDeviceBackend = playbackBackend;
        m_outputDevicesDirty        = false;
    }

    // 每帧从对象池起点复用 Clay 行容器，索引保证同帧实例不冲突。
    size_t rowIndex     = 0;
    size_t subHBoxIndex = 0;

    // 根布局仅承载上方可伸缩列表；页脚在后面按固定高度单独渲染。
    CLayVBox rootVBox;

    /// @brief 向页脚加入标签、静音按钮和滑条组成的音量控制行。
    /// @param parent 接收控制行的纵向 Clay 容器。
    /// @param id 本行所有 ImGui 与 Clay 元素共享的稳定标识前缀。
    /// @param label 左侧本地化标签。
    /// @param labelWidth 所有控制行共用的固定标签列宽。
    /// @param volume 当前音量值。
    /// @param muted 当前静音状态。
    /// @param levelL 左声道电平，保留给控制行视觉扩展。
    /// @param levelR 右声道电平，保留给控制行视觉扩展。
    /// @param minVal 滑条下限。
    /// @param maxVal 滑条上限。
    /// @param tooltip 控件悬浮说明。
    /// @param format 滑条数值格式。
    /// @param onVolumeChange 音量变化回调。
    /// @param onMuteChange 静音变化回调。
    ///
    /// 左侧固定宽度保证不同控制行标签对齐，右侧使用 Grow 吸收剩余宽度。
    /// 回调只在对应控件产生交互时写入音频管理器状态。
    /// Clay 容器来自视图对象池，每帧 clear 后复用，避免持有上帧元素。
    /// ImGui 控件 ID 由调用方传入的稳定前缀派生，不使用本地化标签作为身份。
    /// 标签和按钮固定宽度，滑条承担窗口缩放产生的全部横向余量。
    /// 静音图标反映当前状态和音量区间，点击回调接收取反后的目标状态。
    /// 每个元素回调都只借用本帧捕获值，不得延迟到当前 onUpdate 返回之后执行。
    auto addControlRow = [&](CLayVBox&   parent,
                             const char* id,
                             const char* label,
                             float       labelWidth,
                             float       volume,
                             bool        muted,
                             float       levelL,
                             float       levelR,
                             float       minVal,
                             float       maxVal,
                             const char* tooltip,
                             const char* format,
                             auto        onVolumeChange,
                             auto        onMuteChange) {
        // 从视图拥有的布局对象池取得一行，避免每帧动态创建长期对象。
        CLayHBox& row = this->getRow(rowIndex++);
        row.clear();
        // 主行只提供左右列间距，垂直对齐由固定行高统一控制。
        row.setPadding(0, 0, 0, 0)
            .setSpacing(toLayoutPixels(layoutMetrics.controlColumnGap))
            .setAlignment(Alignment::Center());

        // 左列固定为最宽标签宽度，短标签后的弹簧填满剩余空间。
        CLayHBox& leftBox = this->getSubHBox(subHBoxIndex++);
        leftBox.clear();
        leftBox.setSpacing(toLayoutPixels(layoutMetrics.labelColumnGap))
            .setAlignment(Alignment::Center());

        // 标签元素按真实文本宽度占位，并在控制行中垂直居中。
        leftBox.addElement(std::string(id) + "_lbl",
                           Sizing::Fixed(ImGui::CalcTextSize(label).x),
                           Sizing::Grow(),
                           [label](Clay_BoundingBox r, bool) {
                               // Clay 给出最终包围盒后再设置 ImGui 屏幕坐标。
                               float textH  = ImGui::CalcTextSize(label).y;
                               float offset = (r.height - textH) * 0.5f;
                               ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                               ImGui::Text("%s", label);
                           });

        // 弹簧将标签推到左侧，同时维持所有行的右边界一致。
        leftBox.addSpring();

        // 左列禁止横向增长，避免长滑条反向挤压标签对齐线。
        row.addLayout((std::string(id) + "_left").c_str(),
                      leftBox,
                      Sizing::Fixed(labelWidth),
                      Sizing::Grow());

        // 右列承载固定尺寸静音按钮和占据余量的滑条。
        CLayHBox& rightBox = this->getSubHBox(subHBoxIndex++);
        rightBox.clear();
        rightBox.setSpacing(toLayoutPixels(layoutMetrics.controlColumnGap))
            .setAlignment(Alignment::Center());

        // 静音按钮使用稳定方形点击区，图标按音量区间选择。
        rightBox.addElement(
            std::string(id) + "_mute",
            Sizing::Fixed(layoutMetrics.muteButtonSize),
            Sizing::Fixed(layoutMetrics.muteButtonSize),
            [&, id, muted, volume, tooltip, onMuteChange](Clay_BoundingBox r,
                                                          bool isHovered) {
                // 静音状态优先；未静音时按三个音量区间提供视觉反馈。
                const char* icon = ICON_MMM_VOLUME_MUTE;
                if ( !muted ) {
                    if ( volume <= 0.33f )
                        icon = ICON_MMM_VOLUME_OFF;
                    else if ( volume <= 0.66f )
                        icon = ICON_MMM_VOLUME_LOW;
                    else
                        icon = ICON_MMM_VOLUME_HIGH;
                }

                // 方形按钮在 Clay 行包围盒内垂直居中。
                ImGui::SetCursorScreenPos(
                    { r.x,
                      r.y + (r.height - layoutMetrics.muteButtonSize) * 0.5f });
                if ( muted ) {
                    // 静音图标使用危险色，但不改变按钮背景语义。
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getDangerColor());
                }
                Utils::pushFixedButtonStyleVars();
                // 统一反馈按钮负责悬浮与点击音效，回调只提交新状态。
                if ( ::MMM::UI::FeedbackButton(
                         (std::string(icon) + "##Btn" + id).c_str(),
                         ImVec2(layoutMetrics.muteButtonSize,
                                layoutMetrics.muteButtonSize)) ) {
                    onMuteChange(!muted);
                }
                Utils::popFixedButtonStyleVars();
                if ( muted ) {
                    // 只弹出本分支压入的文本颜色，维持 ImGui 样式栈平衡。
                    ImGui::PopStyleColor();
                }
                if ( ImGui::IsItemHovered() ) {
                    // 提示同时描述控制对象与点击后将执行的动作。
                    const std::string tooltipText =
                        fmt::format("{} ({})",
                                    tooltip,
                                    muted ? TR("ui.audio_manager.unmute").data()
                                          : TR("ui.audio_manager.mute").data());
                    Utils::renderTooltip(tooltipText.c_str());
                }
            });

        // 滑条横向增长，并使用固定行高扩大稳定的交互区域。
        rightBox.addElement(
            std::string(id) + "_slider",
            Sizing::Grow(),
            Sizing::Fixed(layoutMetrics.controlRowHeight),
            [&, id, volume, minVal, maxVal, format, tooltip, onVolumeChange](
                Clay_BoundingBox r, bool isHovered) {
                // ImGui 框体在 Clay 分配的较高区域内垂直居中。
                float frameH = ImGui::GetFrameHeight();
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - frameH) * 0.5f });
                ImGui::SetNextItemWidth(r.width);
                // 使用局部副本让 ImGui 编辑；产生变化后才调用业务回调。
                float val = volume;
                if ( ::MMM::UI::FeedbackSliderFloat(
                         (std::string("##Slider") + id).c_str(),
                         &val,
                         minVal,
                         maxVal,
                         format) ) {
                    onVolumeChange(val);
                }
                if ( ImGui::IsItemHovered() ) {
                    // 滑条与静音按钮复用同一控制项说明。
                    Utils::renderTooltip(tooltip);
                }
            });

        // 右列获得主行除固定标签列外的全部剩余宽度。
        row.addLayout((std::string(id) + "_right").c_str(),
                      rightBox,
                      Sizing::Grow(),
                      Sizing::Grow());

        // 最后把完整控制行加入页脚，并锁定缓存计算出的行高。
        parent.addLayout((std::string(id) + "_row").c_str(),
                         row,
                         Sizing::Grow(),
                         Sizing::Fixed(layoutMetrics.controlRowHeight));
    };

    /// @brief 向页脚加入输出设备选择行。
    /// @param parent 接收设备行的纵向 Clay 容器。
    /// @param id 行内控件稳定标识前缀。
    /// @param label 左侧本地化标签。
    /// @param labelWidth 与其他控制行一致的固定标签列宽。
    ///
    /// 设备列表来自低频缓存；选择成功后标脏，下一帧重新枚举以反映后端状态。
    /// 空设备名表示系统默认输出；显示标签与后端保存的真实设备名相互独立。
    /// 下拉选项使用缓存顺序，不在 UI 层额外排序，以保留后端提供的优先级。
    /// 只有 AudioManager 确认切换成功才使设备列表失效，失败时保留当前选择。
    /// 组合框与音量控制共享标签列和固定行高，因此展开设置时不会左右跳动。
    auto addDeviceComboRow = [&](CLayVBox&   parent,
                                 const char* id,
                                 const char* label,
                                 float       labelWidth) {
        // 设备行复用与音量行相同的左右列布局，保持视觉基线一致。
        CLayHBox& row = this->getRow(rowIndex++);
        row.clear();
        row.setPadding(0, 0, 0, 0)
            .setSpacing(toLayoutPixels(layoutMetrics.controlColumnGap))
            .setAlignment(Alignment::Center());

        // 左列使用文本宽度加弹簧，外层仍固定到统一标签宽度。
        CLayHBox& leftBox = this->getSubHBox(subHBoxIndex++);
        leftBox.clear();
        leftBox.setSpacing(toLayoutPixels(layoutMetrics.labelColumnGap))
            .setAlignment(Alignment::Center());
        leftBox.addElement(std::string(id) + "_lbl",
                           Sizing::Fixed(ImGui::CalcTextSize(label).x),
                           Sizing::Grow(),
                           [label](Clay_BoundingBox r, bool) {
                               // 设备标签按最终包围盒高度居中绘制。
                               const float textH = ImGui::CalcTextSize(label).y;
                               ImGui::SetCursorScreenPos(
                                   { r.x, r.y + (r.height - textH) * 0.5f });
                               ImGui::Text("%s", label);
                           });
        leftBox.addSpring();

        row.addLayout((std::string(id) + "_left").c_str(),
                      leftBox,
                      Sizing::Fixed(labelWidth),
                      Sizing::Grow());

        // 右列只包含组合框，因此允许它占满所有剩余宽度。
        CLayHBox& rightBox = this->getSubHBox(subHBoxIndex++);
        rightBox.clear();
        rightBox.setAlignment(Alignment::Center());
        rightBox.addElement(
            std::string(id) + "_combo",
            Sizing::Grow(),
            Sizing::Fixed(layoutMetrics.controlRowHeight),
            [&, id](Clay_BoundingBox r, bool) {
                // 空设备名代表跟随后端默认设备，显示本地化占位而非空串。
                const std::string defaultDeviceLabel =
                    TR("ui.audio_manager.output_device_default").data();
                const std::string currentDeviceName =
                    audioManager.getOutputDeviceName();
                const std::string previewName = currentDeviceName.empty()
                                                    ? defaultDeviceLabel
                                                    : currentDeviceName;

                // 组合框按当前框高在 Clay 行内垂直居中。
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo(
                         (std::string("##Combo") + id).c_str(),
                         previewName.c_str()) ) {
                    // 仅遍历已缓存设备，避免打开下拉框时触发后端枚举。
                    for ( const auto& device : m_cachedOutputDevices ) {
                        const std::string optionLabel =
                            device.isDefault ? defaultDeviceLabel : device.name;
                        // 业务状态仍以真实设备名比较，默认项也不混用显示标签。
                        const bool selected = currentDeviceName == device.name;
                        if ( ::MMM::UI::FeedbackSelectable(optionLabel.c_str(),
                                                           selected) ) {
                            // 后端确认切换成功后才刷新设备缓存。
                            if ( audioManager.setOutputDeviceName(
                                     device.name) ) {
                                m_outputDevicesDirty = true;
                            }
                        }
                        if ( selected ) {
                            // 下拉框打开时让当前设备获得默认键盘焦点。
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.audio_manager.output_device_tooltip").data());
                }
            });

        row.addLayout((std::string(id) + "_right").c_str(),
                      rightBox,
                      Sizing::Grow(),
                      Sizing::Grow());
        parent.addLayout((std::string(id) + "_row").c_str(),
                         row,
                         Sizing::Grow(),
                         Sizing::Fixed(layoutMetrics.controlRowHeight));
    };

    // 皮肤音效与项目音频会合并到同一只读表格，但保留来源分类。
    const auto& skinData = Config::SkinManager::instance().getData();

    /// @brief 将内部资源分类转换为本地化显示文本。
    auto audioTableTypeLabel = [](AudioTableRowKind kind) -> std::string {
        switch ( kind ) {
        case AudioTableRowKind::InteractionSfx:
            return TR("ui.audio_manager.type_interaction_sfx").data();
        case AudioTableRowKind::PermanentSfx:
            return TR("ui.audio_manager.type_permanent_sfx").data();
        case AudioTableRowKind::MainTrack:
            return TR("ui.audio_manager.type_main_track").data();
        case AudioTableRowKind::ProjectSfx:
            return TR("ui.audio_manager.type_project_sfx").data();
        }
        // 防御未知枚举值，避免表格出现空白单元格。
        return TR("ui.file_manager.value_unknown").data();
    };

    /// @brief 返回资源分类在分组表格中的稳定顺序。
    ///
    /// 排名同时用于排序、分组起点数组和展开状态数组，枚举映射必须保持一致。
    auto audioTableTypeSortRank = [](AudioTableRowKind kind) {
        switch ( kind ) {
        case AudioTableRowKind::PermanentSfx: return 0;
        case AudioTableRowKind::InteractionSfx: return 1;
        case AudioTableRowKind::MainTrack: return 2;
        case AudioTableRowKind::ProjectSfx: return 3;
        }
        // 未知枚举排到所有已知分类之后，且不会索引四元素分组数组。
        return 4;
    };

    /// @brief 按需读取单行资源的文件大小和修改时间。
    /// @param row 需要补充元数据的缓存行。
    ///
    /// 文件系统访问只发生在缓存重建或元数据列首次可见时，不在每帧重复执行。
    auto loadAudioTableRowMetadata = [](AudioTableRow& row) {
        if ( row.m_metadataLoaded ) {
            // 已尝试过的失败结果也会缓存，避免持续查询不可访问路径。
            return;
        }
        const FileMetadata metadata = queryFileMetadata(row.m_absolutePath);
        row.m_size                  = metadata.size;
        row.m_hasSize               = metadata.hasSize;
        row.m_lastWriteTime         = metadata.lastWriteTime;
        row.m_hasLastWriteTime      = metadata.hasLastWriteTime;
        // 有效位区分真实零值与查询失败；完成标记必须最后写入。
        row.m_metadataLoaded = true;
    };

    /// @brief 从皮肤配置和当前项目重建排序后的音频表格缓存。
    ///
    /// 此操作包含容器遍历、可选文件查询和稳定排序，只能在明确标脏时执行。
    /// 皮肤音效路径直接来自 SkinData，项目音频路径相对项目根目录解析。
    /// 每行保存显示文本、绝对查询路径、音轨类型和来源分类，不拥有音频对象。
    /// 分类始终是第一排序层，以确保同类资源在数组中形成连续区间。
    /// 用户字段只在同一分类内部排序，ID 与路径提供最终确定性回退顺序。
    /// 大小和时间采用独立有效位，未知元数据在基础升序中位于有效值之后。
    /// 重建结束后同步保存项目身份和两类资源数量，作为后续低成本缓存键。
    auto rebuildAudioTableRows = [&]() {
        // 旧行携带的路径与元数据均与当前资源集合绑定，整体丢弃最安全。
        m_audioTableRows.clear();
        const size_t projectAudioCount =
            project ? project->m_audioResources.size() : 0;
        // 一次预留总容量，避免两类资源追加过程中反复扩容。
        m_audioTableRows.reserve(skinData.audioPaths.size() +
                                 projectAudioCount);

        // 皮肤资源使用绝对路径，按键名前缀进一步区分交互音效。
        for ( const auto& [key, path] : skinData.audioPaths ) {
            AudioTableRow row;
            row.m_id           = key;
            row.m_path         = Config::pathToUtf8(path);
            row.m_absolutePath = path;
            row.m_type         = AudioTrackType::Effect;
            // 分类只影响显示分组，不改变其 Effect 播放类型。
            row.m_kind = isInteractionSfxKey(key)
                             ? AudioTableRowKind::InteractionSfx
                             : AudioTableRowKind::PermanentSfx;
            m_audioTableRows.push_back(std::move(row));
        }

        if ( project ) {
            // 项目资源保存相对路径，文件查询前由项目根目录解析为绝对路径。
            for ( const auto& audio : project->m_audioResources ) {
                AudioTableRow row;
                row.m_id   = audio.m_id;
                row.m_path = audio.m_path;
                row.m_absolutePath =
                    project->m_projectRoot / Config::utf8ToPath(audio.m_path);
                row.m_type = audio.m_type;
                // 主轨与项目音效分别成组，便于识别可用于时间轴的资源。
                row.m_kind = audio.m_type == AudioTrackType::Main
                                 ? AudioTableRowKind::MainTrack
                                 : AudioTableRowKind::ProjectSfx;
                m_audioTableRows.push_back(std::move(row));
            }
        }

        // 只有按元数据排序时才需在排序前加载所有行的对应字段。
        if ( m_audioTableSortKey == AudioTableSortKey::Size ||
             m_audioTableSortKey == AudioTableSortKey::ModifiedTime ) {
            for ( auto& row : m_audioTableRows ) {
                loadAudioTableRowMetadata(row);
            }
        }

        // 稳定排序先维持分类连续，再在分类内部应用用户选择的字段。
        std::stable_sort(
            m_audioTableRows.begin(),
            m_audioTableRows.end(),
            [&](const AudioTableRow& lhs, const AudioTableRow& rhs) {
                // 分组排名是第一排序键，保证每组能用连续区间记录。
                const int lhsGroupRank = audioTableTypeSortRank(lhs.m_kind);
                const int rhsGroupRank = audioTableTypeSortRank(rhs.m_kind);
                if ( lhsGroupRank != rhsGroupRank ) {
                    int groupCompareResult = lhsGroupRank - rhsGroupRank;
                    // 仅按类型降序时反转组序；其他字段不会打散固定组序。
                    if ( m_audioTableSortKey == AudioTableSortKey::Type &&
                         m_audioTableSortDirection ==
                             SortDirection::Descending ) {
                        groupCompareResult = -groupCompareResult;
                    }
                    return groupCompareResult < 0;
                }

                // 同组内再计算实际字段比较结果。
                int compareResult = 0;
                switch ( m_audioTableSortKey ) {
                case AudioTableSortKey::Id:
                    compareResult = compareAsciiText(lhs.m_id, rhs.m_id);
                    break;
                case AudioTableSortKey::Type: break;
                case AudioTableSortKey::Path:
                    compareResult = compareAsciiText(lhs.m_path, rhs.m_path);
                    break;
                case AudioTableSortKey::Size:
                    compareResult = compareOptionalValue(
                        lhs.m_size, lhs.m_hasSize, rhs.m_size, rhs.m_hasSize);
                    break;
                case AudioTableSortKey::ModifiedTime:
                    compareResult =
                        compareOptionalValue(lhs.m_lastWriteTime,
                                             lhs.m_hasLastWriteTime,
                                             rhs.m_lastWriteTime,
                                             rhs.m_hasLastWriteTime);
                    break;
                }

                // 相同主键依次回退到 ID 和路径，形成跨帧稳定的全序。
                if ( compareResult == 0 ) {
                    compareResult = compareAsciiText(lhs.m_id, rhs.m_id);
                }
                if ( compareResult == 0 ) {
                    compareResult = compareAsciiText(lhs.m_path, rhs.m_path);
                }
                // 排序方向最后统一翻转，避免各字段重复分支。
                if ( m_audioTableSortDirection == SortDirection::Descending ) {
                    compareResult = -compareResult;
                }
                return compareResult < 0;
            });

        // 排序完成后建立每组连续区间，渲染裁剪器据此映射可见行。
        m_audioTableGroupStarts.fill(0);
        m_audioTableGroupSizes.fill(0);
        for ( size_t rowIndex = 0; rowIndex < m_audioTableRows.size();
              ++rowIndex ) {
            const size_t groupIndex = static_cast<size_t>(
                audioTableTypeSortRank(m_audioTableRows[rowIndex].m_kind));
            if ( m_audioTableGroupSizes[groupIndex] == 0 ) {
                // 首次遇到该分类时记录它在排序数组中的起点。
                m_audioTableGroupStarts[groupIndex] = rowIndex;
            }
            m_audioTableGroupSizes[groupIndex]++;
        }

        // 同步记录本次快照身份，供下一帧低成本判断资源集合是否变化。
        m_cachedPermanentSfxCount  = skinData.audioPaths.size();
        m_cachedProjectAudioCount  = projectAudioCount;
        m_cachedAudioTableProject  = project;
        m_audioTableSortCacheDirty = false;
    };

    /// @brief 把 ImGui 表头排序规格同步到视图持有的排序状态。
    ///
    /// 仅消费主排序键；更新后清除 SpecsDirty，避免同一请求被重复处理。
    auto syncAudioTableSortSpecs = [&]() {
        ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        if ( !sortSpecs || sortSpecs->SpecsCount <= 0 ||
             !sortSpecs->SpecsDirty ) {
            // 表格未启用排序或用户没有产生新排序请求时保持缓存。
            return;
        }

        // 本表使用单列排序，第一项即为当前生效的主规格。
        const ImGuiTableColumnSortSpecs& primarySpec = sortSpecs->Specs[0];
        AudioTableSortKey                newSortKey  = AudioTableSortKey::Id;
        // ID 是默认值，其余列索引显式映射到内部枚举。
        if ( primarySpec.ColumnIndex == AudioTableColumnType ) {
            newSortKey = AudioTableSortKey::Type;
        } else if ( primarySpec.ColumnIndex == AudioTableColumnPath ) {
            newSortKey = AudioTableSortKey::Path;
        } else if ( primarySpec.ColumnIndex == AudioTableColumnSize ) {
            newSortKey = AudioTableSortKey::Size;
        } else if ( primarySpec.ColumnIndex == AudioTableColumnModifiedTime ) {
            newSortKey = AudioTableSortKey::ModifiedTime;
        }

        // ImGui 未明确给出降序时按升序处理，与默认排序约定一致。
        const SortDirection newDirection =
            primarySpec.SortDirection == ImGuiSortDirection_Descending
                ? SortDirection::Descending
                : SortDirection::Ascending;
        // 只有状态真正变化才触发昂贵的表格缓存重建。
        if ( newSortKey != m_audioTableSortKey ||
             newDirection != m_audioTableSortDirection ) {
            m_audioTableSortKey        = newSortKey;
            m_audioTableSortDirection  = newDirection;
            m_audioTableSortCacheDirty = true;
        }
        // 无论内部状态是否变化，本次 ImGui 请求都已经消费完毕。
        sortSpecs->SpecsDirty = false;
    };

    /// @brief 将表格排序恢复为 ID 升序。
    ///
    /// 默认状态同时用于完整重置和仅重置排序两个菜单动作。
    auto resetAudioTableSort = [&]() {
        if ( m_audioTableSortKey != AudioTableSortKey::Id ||
             m_audioTableSortDirection != SortDirection::Ascending ) {
            m_audioTableSortKey        = AudioTableSortKey::Id;
            m_audioTableSortDirection  = SortDirection::Ascending;
            m_audioTableSortCacheDirty = true;
        }
    };

    /// @brief 应用菜单选择的显式排序字段和方向。
    /// @param sortKey 目标排序字段。
    /// @param direction 目标排序方向。
    ///
    /// 与当前状态相同的重复点击不使缓存失效。
    auto applyAudioTableSort = [&](AudioTableSortKey sortKey,
                                   SortDirection     direction) {
        if ( m_audioTableSortKey != sortKey ||
             m_audioTableSortDirection != direction ) {
            m_audioTableSortKey        = sortKey;
            m_audioTableSortDirection  = direction;
            m_audioTableSortCacheDirty = true;
        }
    };

    /// @brief 绘制资源表头右键菜单。
    ///
    /// 菜单提供列宽、排序、重置和列显隐操作，并保持至少一列可见。
    /// 右键具体列时可单独自动适配；右键空白区域时只提供全局操作。
    /// 排序菜单不要求目标列当前可见，便于按隐藏元数据列组织资源。
    /// 重置全部会同时恢复 ImGui 表格设置和本视图维护的默认排序。
    /// 列显隐修改排队到下一帧，由 ImGui 在完整布局边界统一应用。
    /// 所有菜单项使用反馈封装，保证悬浮与点击效果和项目其他 UI 一致。
    auto renderAudioTableHeaderContextMenu = [&]() {
        ImGuiTable* table = ImGui::GetCurrentTable();
        // 该回调只允许在 BeginTable 成功后的表格上下文中调用。
        if ( !table ) return;

        // 给紧凑菜单设置可点击的最小留白，不永久修改全局样式。
        ImGuiStyle&  style = ImGui::GetStyle();
        const ImVec2 popupPadding(std::max(style.WindowPadding.x, 8.0f),
                                  std::max(style.WindowPadding.y, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(std::max(style.ItemSpacing.x, 8.0f),
                                   std::max(style.ItemSpacing.y, 4.0f)));
        // 使用 ImGui 表格内部记录的上下文列，兼容列重排后的索引。
        const bool popupOpen = ImGui::TableBeginContextMenuPopup(table);
        if ( !popupOpen ) {
            // 未打开弹窗时也必须恢复两项临时样式。
            ImGui::PopStyleVar(2);
            return;
        }

        // 空白表头区域触发菜单时没有目标列，单列自适应项因此隐藏。
        const int contextColumn =
            table->ContextPopupColumn >= 0 &&
                    table->ContextPopupColumn < table->ColumnsCount
                ? table->ContextPopupColumn
                : -1;
        // 只允许对当前实际显示的列执行单列自动适配。
        if ( contextColumn >= 0 && isTableColumnEnabled(table, contextColumn) &&
             ::MMM::UI::FeedbackMenuItem(
                 TR("ui.audio_manager.table_menu.size_column_fit").data()) ) {
            ImGui::TableSetColumnWidthAutoSingle(table, contextColumn);
        }

        // 全列默认宽度交由 ImGui 根据标题和内容重新计算。
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.audio_manager.table_menu.size_all_default").data()) ) {
            ImGui::TableSetColumnWidthAutoAll(table);
        }

        /// @brief 绘制一项具体字段和方向的排序菜单。
        /// @param sortKey 对应内部排序字段。
        /// @param direction 对应排序方向。
        /// @param columnLabel 用于构造本地化菜单文本的列名。
        auto sortMenuItem = [&](AudioTableSortKey sortKey,
                                SortDirection     direction,
                                const char*       columnLabel) {
            const std::string label = makeSortMenuLabel(
                columnLabel, direction == SortDirection::Ascending);
            // 当前排序项仅显示勾选态，仍允许用户重复选择。
            const bool selected = m_audioTableSortKey == sortKey &&
                                  m_audioTableSortDirection == direction;
            if ( ::MMM::UI::FeedbackMenuItem(
                     label.c_str(), nullptr, selected) ) {
                applyAudioTableSort(sortKey, direction);
            }
        };
        // 每个可排序字段均提供升序和降序，行为不依赖当前列显隐。
        if ( ::MMM::UI::FeedbackBeginMenu(
                 TR("ui.resource_table.sort").data()) ) {
            sortMenuItem(AudioTableSortKey::Id,
                         SortDirection::Ascending,
                         TR("ui.audio_manager.column_id").data());
            sortMenuItem(AudioTableSortKey::Id,
                         SortDirection::Descending,
                         TR("ui.audio_manager.column_id").data());
            sortMenuItem(AudioTableSortKey::Type,
                         SortDirection::Ascending,
                         TR("ui.audio_manager.column_type").data());
            sortMenuItem(AudioTableSortKey::Type,
                         SortDirection::Descending,
                         TR("ui.audio_manager.column_type").data());
            sortMenuItem(AudioTableSortKey::Path,
                         SortDirection::Ascending,
                         TR("ui.audio_manager.column_path").data());
            sortMenuItem(AudioTableSortKey::Path,
                         SortDirection::Descending,
                         TR("ui.audio_manager.column_path").data());
            sortMenuItem(AudioTableSortKey::Size,
                         SortDirection::Ascending,
                         TR("ui.audio_manager.column_size").data());
            sortMenuItem(AudioTableSortKey::Size,
                         SortDirection::Descending,
                         TR("ui.audio_manager.column_size").data());
            sortMenuItem(AudioTableSortKey::ModifiedTime,
                         SortDirection::Ascending,
                         TR("ui.audio_manager.column_modified_time").data());
            sortMenuItem(AudioTableSortKey::ModifiedTime,
                         SortDirection::Descending,
                         TR("ui.audio_manager.column_modified_time").data());
            ::MMM::UI::FeedbackEndMenu();
        }

        // 重置子菜单区分完整表格设置、列宽、列显隐和排序状态。
        if ( ::MMM::UI::FeedbackBeginMenu(
                 TR("ui.audio_manager.table_menu.reset").data()) ) {
            if ( ::MMM::UI::FeedbackMenuItem(
                     TR("ui.audio_manager.table_menu.reset_all").data()) ) {
                // 完整重置同时清除 ImGui 持久化列配置和内部排序状态。
                ImGui::TableResetSettings(table);
                resetAudioTableSort();
            }
            if ( ::MMM::UI::FeedbackMenuItem(
                     TR("ui.audio_manager.table_menu.reset_columns").data()) ) {
                // 仅恢复宽度时保留列顺序、显隐和用户排序。
                ImGui::TableSetColumnWidthAutoAll(table);
            }
            if ( ::MMM::UI::FeedbackMenuItem(
                     TR("ui.audio_manager.table_menu.show_all_columns")
                         .data()) ) {
                // 显示全部列通过下一帧状态排队，避免当前布局中途变化。
                for ( int column = 0; column < table->ColumnsCount; ++column ) {
                    queueTableColumnEnabled(table, column, true);
                }
            }
            if ( ::MMM::UI::FeedbackMenuItem(
                     TR("ui.audio_manager.table_menu.reset_sort").data()) ) {
                resetAudioTableSort();
            }
            ::MMM::UI::FeedbackEndMenu();
        }

        // 分隔线以下提供与 ImGui 默认表头菜单一致的列显隐列表。
        ImGui::Separator();

        // 标签数组顺序必须与 AudioTableColumn 枚举及表格列注册一致。
        const std::array<const char*, 5> columnLabels{
            TR("ui.audio_manager.column_id").data(),
            TR("ui.audio_manager.column_type").data(),
            TR("ui.audio_manager.column_path").data(),
            TR("ui.audio_manager.column_size").data(),
            TR("ui.audio_manager.column_modified_time").data()
        };
        // 先统计用户启用列数，用于阻止隐藏最后一列。
        int enabledColumnCount = 0;
        for ( int column = 0; column < table->ColumnsCount; ++column ) {
            if ( isTableColumnUserEnabled(table, column) ) {
                enabledColumnCount++;
            }
        }
        for ( int column = 0; column < table->ColumnsCount; ++column ) {
            const bool enabled = isTableColumnUserEnabled(table, column);
            // 已隐藏列总能恢复；当前唯一可见列不能被关闭。
            const bool canToggle = !enabled || enabledColumnCount > 1;
            if ( ::MMM::UI::FeedbackMenuItem(
                     columnLabels[column], nullptr, enabled, canToggle) ) {
                queueTableColumnEnabled(table, column, !enabled);
            }
        }

        // 弹窗和样式栈由本函数成对收尾。
        ImGui::EndPopup();
        ImGui::PopStyleVar(2);
    };

    /// @brief 在给定区域绘制分组音频资源表格。
    /// @param r Clay 分配的最终屏幕包围盒。
    ///
    /// 表格行来自脏标记缓存；文件元数据按列可见性延迟读取，长列表使用裁剪器。
    /// @warning UI 热路径：每帧调用，不得无条件扫描项目或查询文件系统。
    /// 表头冻结在纵向滚动区域顶部，列宽、顺序和显隐由 ImGui 持久化。
    /// 表格默认展示 ID 与路径，类型、大小和修改时间按需由用户打开。
    /// 自定义表头菜单替代 ImGui 默认菜单，但继续使用其内部列状态字段。
    /// 分组头和资源行使用相同固定高度，使 ImGuiListClipper 可准确估算范围。
    /// 每个资源行先提交跨列命中区，再在各可见列直接绘制滚动文本。
    /// 展开状态在裁剪遍历结束后更新，确保当前帧的逻辑行映射保持不变。
    auto renderAudioResourcesTable = [&](Clay_BoundingBox r, bool) {
        // Clay 与 ImGui 共用屏幕坐标，绘制前把游标移动到分配区域左上角。
        ImGui::SetCursorScreenPos({ r.x, r.y });
        // 纵向滚动条样式只覆盖该表格，析构时自动恢复。
        Utils::VerticalScrollbarStyleScope verticalScrollbarStyle(dpiScale);
        // 支持用户调整、重排、隐藏与排序列，并固定表头进行纵向滚动。
        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp;

        // BeginTable 失败时不能调用任何 Table API，也无需显式 EndTable。
        if ( ImGui::BeginTable("AudioManagerGroupedTable",
                               5,
                               tableFlags,
                               { r.width, r.height }) ) {
            // 第一行表头冻结，横向列均可随用户设置重排。
            ImGui::TableSetupScrollFreeze(0, 1);
            // ID 为默认排序列并优先拉伸，适合多数窗口宽度。
            ImGui::TableSetupColumn(
                TR("ui.audio_manager.column_id").data(),
                ImGuiTableColumnFlags_DefaultSort |
                    ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            // 类型列默认隐藏，但提供固定宽度以避免显示后挤压不稳定。
            ImGui::TableSetupColumn(
                TR("ui.audio_manager.column_type").data(),
                ImGuiTableColumnFlags_DefaultHide |
                    ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(96.0f, 116.0f * ImGui::GetFontSize() / 17.0f));
            // 路径列承担剩余宽度，超长内容由单元格滚动绘制器处理。
            ImGui::TableSetupColumn(
                TR("ui.audio_manager.column_path").data(),
                ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            // 大小列涉及文件查询，默认隐藏以避免无需求值。
            ImGui::TableSetupColumn(
                TR("ui.audio_manager.column_size").data(),
                ImGuiTableColumnFlags_DefaultHide |
                    ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(96.0f, 104.0f * ImGui::GetFontSize() / 17.0f));
            // 修改时间按固定格式预留宽度，并偏好最近时间在前。
            ImGui::TableSetupColumn(
                TR("ui.audio_manager.column_modified_time").data(),
                ImGuiTableColumnFlags_DefaultHide |
                    ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortDescending,
                std::max(168.0f,
                         ImGui::CalcTextSize("0000/00/00 00:00").x +
                             ImGui::GetStyle().CellPadding.x * 4.0f +
                             ImGui::GetFrameHeight()));
            if ( ImGuiTable* table = ImGui::GetCurrentTable() ) {
                // 自定义菜单覆盖默认菜单，以提供统一反馈和扩展重置动作。
                table->DisableDefaultContextMenu = true;
            }
            // 表头必须先提交，随后才能读取用户产生的排序规格。
            ImGui::TableHeadersRow();
            syncAudioTableSortSpecs();
            renderAudioTableHeaderContextMenu();

            // 除显式脏标记外，项目身份和两类资源数量变化也会使缓存失效。
            const size_t projectAudioCount =
                project ? project->m_audioResources.size() : 0;
            if ( m_audioTableSortCacheDirty ||
                 m_cachedPermanentSfxCount != skinData.audioPaths.size() ||
                 m_cachedProjectAudioCount != projectAudioCount ||
                 m_cachedAudioTableProject != project ) {
                rebuildAudioTableRows();
            }

            // 元数据列的实际显示状态决定本帧是否需要惰性查询。
            ImGuiTable* table = ImGui::GetCurrentTable();
            const bool  sizeColumnVisible =
                isTableColumnEnabled(table, AudioTableColumnSize);
            const bool modifiedColumnVisible =
                isTableColumnEnabled(table, AudioTableColumnModifiedTime);
            // 跨列 Selectable 必须从显示顺序中的第一列提交，才能覆盖整行。
            int firstEnabledColumn = AudioTableColumnId;
            if ( table ) {
                // DisplayOrderToIndex 处理用户重排列后的真实显示顺序。
                for ( int displayOrder = 0; displayOrder < table->ColumnsCount;
                      ++displayOrder ) {
                    const int column = table->DisplayOrderToIndex[displayOrder];
                    if ( isTableColumnEnabled(table, column) ) {
                        firstEnabledColumn = column;
                        break;
                    }
                }
            }

            // 行视觉高度包含单元格内边距，可点击内容高度需扣除上下内边距。
            const float rowHeight = layoutMetrics.audioItemHeight;
            const float rowSelectableHeight =
                std::max(ImGui::GetFontSize(),
                         rowHeight - ImGui::GetStyle().CellPadding.y * 2.0f);
            /// @brief 绘制一个缓存资源行及其交互。
            /// @param rowIndex 排序缓存中的行索引。
            ///
            /// 项目资源支持拖放、类型修改、移除和打开控制器；皮肤资源只读。
            /// 拖放载荷使用固定布局 DTO，只有可安全存储的资源 ID 才开放拖动。
            /// 右键菜单通过命令系统修改项目模型，单击则只打开对应控制器窗口。
            /// 行悬浮提示展示未裁剪路径，并在载荷受限时解释拖动不可用原因。
            /// 元数据格式化只对可见列执行，避免为隐藏列创建临时字符串。
            /// 该回调接收排序缓存索引，调用期间不得改变缓存容器的大小和顺序。
            auto renderAudioResourceRow = [&](size_t rowIndex) {
                auto& rowData = m_audioTableRows[rowIndex];
                if ( sizeColumnVisible || modifiedColumnVisible ) {
                    // 任一元数据列可见时一次加载两项属性，后续帧直接复用。
                    loadAudioTableRowMetadata(rowData);
                }
                const std::string typeText =
                    audioTableTypeLabel(rowData.m_kind);

                // 先创建固定高度表格行，再在第一可见列提交跨列选择区。
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                (void)ImGui::TableSetColumnIndex(firstEnabledColumn);

                // 保存统一 Y 坐标，确保重排列后各单元格文字仍在同一基线。
                const float rowCursorY = ImGui::GetCursorScreenPos().y;
                // ID 同时包含资源标识、路径和索引，区分潜在同名条目。
                const std::string rowId   = fmt::format("##AudioRow_{}_{}_{}",
                                                        rowData.m_id,
                                                        rowData.m_path,
                                                        rowIndex);
                const bool        clicked = ::MMM::UI::FeedbackSelectable(
                    rowId.c_str(),
                    false,
                    ImGuiSelectableFlags_SpanAllColumns,
                    { 0.0f, rowSelectableHeight });
                // 在移动到其他列之前捕获整行悬浮态。
                const bool hovered = ImGui::IsItemHovered();
                const bool isProjectAudioResource =
                    rowData.m_kind == AudioTableRowKind::MainTrack ||
                    rowData.m_kind == AudioTableRowKind::ProjectSfx;
                // 仅项目资源可拖放，且载荷格式要求 ID 能装入固定缓冲区。
                const bool canDragAudioResource =
                    isProjectAudioResource &&
                    Common::canStoreAudioResourceDragId(rowData.m_id);
                if ( canDragAudioResource &&
                     ImGui::BeginDragDropSource(ImGuiDragDropFlags_None) ) {
                    // 由公共 helper 构造固定布局载荷，避免传递临时字符串指针。
                    const auto payload = Common::makeAudioResourceDragPayload(
                        rowData.m_id, rowData.m_type);
                    if ( payload ) {
                        // 载荷按值复制给 ImGui，并在拖动预览中显示资源 ID。
                        ImGui::SetDragDropPayload(
                            Common::AUDIO_RESOURCE_DRAG_PAYLOAD_TYPE,
                            &*payload,
                            sizeof(*payload));
                        ImGui::TextUnformatted(
                            TR("ui.audio_manager.drag_to_bgm").data());
                        ImGui::TextDisabled("%s", rowData.m_id.c_str());
                    }
                    ImGui::EndDragDropSource();
                }
                if ( isProjectAudioResource ) {
                    // 类型修改和移除只属于项目模型，皮肤资源不开放右键菜单。
                    const std::string contextMenuId =
                        fmt::format("AudioTrackContext_{}_{}_{}",
                                    rowData.m_id,
                                    rowData.m_path,
                                    rowIndex);
                    // 临时扩大弹窗留白，提高菜单项可点击性。
                    const auto&  style = ImGui::GetStyle();
                    const ImVec2 popupPadding(
                        std::max(style.WindowPadding.x, 8.0f),
                        std::max(style.WindowPadding.y, 6.0f));
                    const ImVec2 popupItemSpacing(
                        std::max(style.ItemSpacing.x, 8.0f),
                        std::max(style.ItemSpacing.y, 4.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                        popupPadding);
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                        popupItemSpacing);
                    // 使用独立 ID 打开行级右键菜单，不与表头菜单冲突。
                    const bool contextMenuOpen = ImGui::BeginPopupContextItem(
                        contextMenuId.c_str(),
                        ImGuiPopupFlags_MouseButtonRight);
                    if ( contextMenuOpen ) {
                        /// @brief 绘制一种目标音轨类型并提交撤销命令。
                        auto changeTrackType = [&](AudioTrackType targetType,
                                                   const char*    label) {
                            // 当前类型显示勾选且不重复提交相同命令。
                            const bool selected = rowData.m_type == targetType;
                            if ( ::MMM::UI::FeedbackMenuItem(
                                     label, nullptr, selected) &&
                                 !selected ) {
                                // 类型变化会改变分组，命令入队前先使缓存失效。
                                m_audioTableSortCacheDirty = true;
                                engine.pushCommand(
                                    Logic::CmdUpdateAudioResource{
                                        rowData.m_id, targetType });
                            }
                        };
                        changeTrackType(
                            AudioTrackType::Main,
                            TR("ui.audio_manager.type_main_track").data());
                        changeTrackType(
                            AudioTrackType::Effect,
                            TR("ui.audio_manager.type_project_sfx").data());

                        // 危险的移除动作与类型切换分组显示，并要求二次确认。
                        ImGui::Separator();
                        if ( ::MMM::UI::FeedbackMenuItem(
                                 TR("ui.audio_manager.remove_track").data()) ) {
                            // 只暂存稳定资源 ID，实际删除由模态框确认后执行。
                            m_removeTrackId   = rowData.m_id;
                            m_openRemoveModal = true;
                        }
                        ImGui::EndPopup();
                    }
                    // 无论弹窗是否打开，本行压入的两项样式都必须恢复。
                    ImGui::PopStyleVar(2);
                }
                if ( clicked ) {
                    // 点击行按资源类型打开对应控制器，不直接修改播放状态。
                    const auto controllerType =
                        rowData.m_type == AudioTrackType::Main
                            ? AudioTrackControllerUI::TrackType::Main
                            : AudioTrackControllerUI::TrackType::Effect;
                    // sourceManager 由 UIManager 调用路径保证有效。
                    sourceManager->openAudioTrackController(
                        rowData.m_id, rowData.m_id, controllerType);
                }

                if ( hovered ) {
                    // 悬浮提示展示完整路径和分类，弥补单元格裁剪的信息损失。
                    std::string tooltipText =
                        fmt::format("{}\n{}: {}",
                                    rowData.m_path.c_str(),
                                    TR("ui.audio_manager.column_type").data(),
                                    typeText.c_str());
                    if ( isProjectAudioResource && !canDragAudioResource ) {
                        // 过长 ID 无法写入固定载荷时明确说明拖放不可用。
                        tooltipText += "\n";
                        tooltipText +=
                            TR("ui.audio_manager.drag_unavailable").data();
                    }
                    Utils::renderTooltip(tooltipText.c_str());
                }

                /// @brief 在当前行的指定可见列绘制可滚动文本。
                auto renderColumnText = [&](int                column,
                                            const std::string& text) {
                    if ( !ImGui::TableSetColumnIndex(column) ) {
                        // 隐藏列不参与布局，也不提交任何文本绘制命令。
                        return;
                    }
                    // 恢复整行记录的 Y 坐标，避免 Selectable 改变后续游标高度。
                    ImVec2 cellPos = ImGui::GetCursorScreenPos();
                    cellPos.y      = rowCursorY;
                    renderScrollingTableText(text,
                                             cellPos,
                                             ImGui::GetContentRegionAvail().x,
                                             rowSelectableHeight);
                };

                // ID 列增加音乐图标，其他列保持可直接复制理解的纯文本。
                const std::string idText =
                    std::string(ICON_MMM_MUSIC) + "  " + rowData.m_id;
                renderColumnText(AudioTableColumnId, idText);
                renderColumnText(AudioTableColumnType, typeText);
                renderColumnText(AudioTableColumnPath, rowData.m_path);
                if ( sizeColumnVisible ) {
                    // 只为实际显示的元数据列构造格式化字符串。
                    renderColumnText(
                        AudioTableColumnSize,
                        formatSizeColumn(rowData.m_hasSize, rowData.m_size));
                }
                if ( modifiedColumnVisible ) {
                    // 查询失败由格式化 helper 显示统一未知占位。
                    renderColumnText(
                        AudioTableColumnModifiedTime,
                        formatModifiedColumn(rowData.m_hasLastWriteTime,
                                             rowData.m_lastWriteTime));
                }
            };

            /// @brief 绘制一个可折叠资源分组的表头行。
            /// @param groupKind 分组对应的资源分类。
            /// @param groupSize 分组内资源数量。
            /// @param expanded 当前展开状态。
            /// @return 用户点击表头时返回 true。
            ///
            /// 表头跨越所有可见列，并用箭头、分类文本和数量表达分组状态。
            /// 即使分类数量为零也保留表头，用户可预先设置各分类展开偏好。
            /// 箭头和文字通过 DrawList 绘制，不会推进表格单元格的布局游标。
            /// 点击结果由外层延迟应用，回调本身不修改展开状态数组。
            /// 分组标题使用内部分类映射，不把显示语言文本作为持久标识。
            auto renderAudioGroupHeader = [&](AudioTableRowKind groupKind,
                                              size_t            groupSize,
                                              bool              expanded) {
                // 分类排名同时作为展开状态和分组区间数组的索引。
                const size_t groupIndex =
                    static_cast<size_t>(audioTableTypeSortRank(groupKind));

                // 分组头与普通资源行保持相同高度，确保裁剪器步长恒定。
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                // 半透明 Header 色区分分组头，同时保留表格交替行背景。
                ImGui::TableSetBgColor(
                    ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(ImGuiCol_Header, 0.30f));
                // 跨列选择区从用户显示顺序中的第一列开始提交。
                (void)ImGui::TableSetColumnIndex(firstEnabledColumn);

                // 包围盒起点用于手工绘制箭头、标签和数量。
                const ImVec2 headerPos = ImGui::GetCursorScreenPos();
                // 分组索引足以构成稳定且不受本地化文本影响的 ImGui ID。
                const std::string headerId =
                    fmt::format("##AudioGroup_{}", groupIndex);
                // 整行选择区只报告点击，不持有 ImGui 选择状态。
                const bool clicked = ::MMM::UI::FeedbackSelectable(
                    headerId.c_str(),
                    false,
                    ImGuiSelectableFlags_SpanAllColumns,
                    { 0.0f, rowSelectableHeight });

                // 箭头略小于字体高度，为左侧和文字之间保留视觉呼吸空间。
                const float arrowScale = 0.70f;
                const float arrowSize  = ImGui::GetFontSize() * arrowScale;
                // 所有表头内容以当前字体高度为共同垂直基线。
                const float contentY =
                    headerPos.y +
                    (rowSelectableHeight - ImGui::GetFontSize()) * 0.5f;
                // 展开时箭头向下，折叠时向右，符合树形列表惯例。
                ImGui::RenderArrow(
                    ImGui::GetWindowDrawList(),
                    ImVec2(
                        headerPos.x,
                        contentY + (ImGui::GetFontSize() - arrowSize) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text),
                    expanded ? ImGuiDir_Down : ImGuiDir_Right,
                    arrowScale);

                // 分类标题在箭头右侧绘制，间距按字体大小随 DPI 缩放。
                const std::string headerText = audioTableTypeLabel(groupKind);
                const ImVec2 textPos(headerPos.x + ImGui::GetFontSize() * 1.25f,
                                     contentY);
                // 使用绘制列表而非 Text，避免改变当前表格单元格游标。
                ImGui::GetWindowDrawList()->AddText(
                    textPos,
                    ImGui::GetColorU32(ImGuiCol_Text),
                    headerText.c_str());
                // 数量使用弱化颜色紧随标题，零资源分组仍然明确可见。
                const std::string countText = fmt::format("{}", groupSize);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(textPos.x +
                               ImGui::CalcTextSize(headerText.c_str()).x +
                               ImGui::GetStyle().ItemInnerSpacing.x,
                           contentY),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    countText.c_str());

                // 展开状态由外层在裁剪遍历结束后统一切换。
                return clicked;
            };

            // 默认分组顺序与排名函数一致，便于直接读取连续缓存区间。
            std::array<AudioTableRowKind, 4> groupOrder{
                AudioTableRowKind::PermanentSfx,
                AudioTableRowKind::InteractionSfx,
                AudioTableRowKind::MainTrack,
                AudioTableRowKind::ProjectSfx
            };
            // 类型降序只反转分组顺序，组内字段顺序仍由缓存排序决定。
            if ( m_audioTableSortKey == AudioTableSortKey::Type &&
                 m_audioTableSortDirection == SortDirection::Descending ) {
                std::reverse(groupOrder.begin(), groupOrder.end());
            }

            // 冻结本帧展开状态，防止点击中途改变可见行到缓存行映射。
            const auto expandedGroups = m_audioTableGroupExpanded;
            // 每个分组头始终占一行，展开组额外贡献其资源行数。
            size_t visibleRowCount = groupOrder.size();
            for ( const auto groupKind : groupOrder ) {
                const size_t groupIndex =
                    static_cast<size_t>(audioTableTypeSortRank(groupKind));
                if ( expandedGroups[groupIndex] ) {
                    // 折叠组资源不进入裁剪器的逻辑行空间。
                    visibleRowCount += m_audioTableGroupSizes[groupIndex];
                }
            }

            // 同一帧最多记录一个最后点击的分组，并在遍历后应用。
            int              toggledGroupIndex = -1;
            ImGuiListClipper clipper;
            // 由 ImGui 根据首行实际占用高度计算裁剪步长，避免表格单元格
            // 内边距造成累计误差，使列表底部提前到达滚动上限。
            // 裁剪器只遍历当前滚动区域附近的逻辑行，控制长列表开销。
            clipper.Begin(static_cast<int>(visibleRowCount));
            while ( clipper.Step() ) {
                // DisplayStart 和 DisplayEnd 已根据滚动位置收窄。
                for ( int visibleRow = clipper.DisplayStart;
                      visibleRow < clipper.DisplayEnd;
                      ++visibleRow ) {
                    // 将全局可见行索引逐组扣减，定位到表头或资源行。
                    size_t groupRelativeRow = static_cast<size_t>(visibleRow);
                    for ( const auto groupKind : groupOrder ) {
                        const size_t groupIndex = static_cast<size_t>(
                            audioTableTypeSortRank(groupKind));
                        const size_t groupSize =
                            m_audioTableGroupSizes[groupIndex];
                        if ( groupRelativeRow == 0 ) {
                            // 每组区间的第零行固定为可点击表头。
                            if ( renderAudioGroupHeader(
                                     groupKind,
                                     groupSize,
                                     expandedGroups[groupIndex]) ) {
                                // 延迟切换，维持当前裁剪遍历使用的冻结快照。
                                toggledGroupIndex =
                                    static_cast<int>(groupIndex);
                            }
                            break;
                        }

                        // 越过当前组表头后，剩余索引才可能指向资源。
                        groupRelativeRow--;
                        if ( expandedGroups[groupIndex] ) {
                            if ( groupRelativeRow < groupSize ) {
                                // 分组起点加相对索引得到排序缓存中的真实行。
                                renderAudioResourceRow(
                                    m_audioTableGroupStarts[groupIndex] +
                                    groupRelativeRow);
                                break;
                            }
                            // 未命中时跳过整个展开组，继续检查下一分类。
                            groupRelativeRow -= groupSize;
                        }
                    }
                }
            }
            if ( toggledGroupIndex >= 0 ) {
                // 裁剪遍历完成后再写回状态，下一帧重新计算可见行总数。
                const size_t groupIndex =
                    static_cast<size_t>(toggledGroupIndex);
                m_audioTableGroupExpanded[groupIndex] =
                    !m_audioTableGroupExpanded[groupIndex];
            }
            // 仅在 BeginTable 成功分支中结束表格。
            ImGui::EndTable();
        }
    };

    /// @brief 在没有项目和皮肤音效时绘制居中初始提示。
    /// @param r 提示可使用的屏幕包围盒。
    ///
    /// 横纵坐标均夹到非负余量，文本大于区域时从左上角开始而不反向偏移。
    /// 此状态只在当前没有项目且皮肤未提供任何音效时出现。
    /// 提示使用弱化文字颜色，不创建按钮或隐含可点击区域。
    auto renderInitialHint = [&](Clay_BoundingBox r, bool) {
        const char* text = TR("ui.audio_manager.initial_hint").data();
        // 先测量本地化文本，保证不同语言均按自身尺寸居中。
        const auto textSize = ImGui::CalcTextSize(text);
        ImGui::SetCursorScreenPos(
            { r.x + std::max(0.0f, r.width - textSize.x) * 0.5f,
              r.y + std::max(0.0f, r.height - textSize.y) * 0.5f });
        // 弱化颜色表示当前无可操作资源，而不是错误状态。
        ImGui::TextDisabled("%s", text);
    };

    // 页脚容器始终显示折叠标题，具体控制行由展开状态决定。
    CLayVBox footerVBox;
    // 左右使用对称内边距，避免手工缩进造成不同宽度下偏心。
    footerVBox
        .setPadding(toLayoutPixels(layoutMetrics.footerPaddingX),
                    toLayoutPixels(layoutMetrics.footerPaddingX),
                    0,
                    0)
        .setSpacing(toLayoutPixels(layoutMetrics.footerSpacing));

    // 折叠标题占固定单行高度，并直接切换视图持有的展开状态。
    footerVBox.addElement("FooterHeader",
                          Sizing::Grow(),
                          Sizing::Fixed(layoutMetrics.footerHeaderHeight),
                          [&](Clay_BoundingBox r, bool isHovered) {
                              // 公共折叠标题负责箭头和整行命中区绘制。
                              Utils::renderCollapsingHeader(
                                  TR("ui.audio_manager.global_settings").data(),
                                  &m_showGlobalSettings,
                                  r);
                          });

    if ( m_showGlobalSettings ) {
        // 设备选择置于所有音量控制之前，明确这些值作用于当前输出。
        addDeviceComboRow(footerVBox,
                          "OutputDevice",
                          TR("ui.audio_manager.output_device").data(),
                          maxLabelW);

        // 全局音量控制最终混音输出，并显示当前左右输出电平输入。
        addControlRow(
            footerVBox,
            "Global",
            TR("ui.audio_manager.global_volume").data(),
            maxLabelW,
            audioManager.getGlobalVolume(),
            audioManager.isGlobalMuted(),
            audioManager.getOutputLevelL(),
            audioManager.getOutputLevelR(),
            0.0f,
            1.0f,
            TR("ui.audio_manager.global_volume").data(),
            "%.4f",
            // 回调直接更新轻量音频参数，不触发资源重载。
            [&](float v) { audioManager.setGlobalVolume(v); },
            [&](bool m) { audioManager.setGlobalMute(m); });

        // BGM 增益只影响主音轨，并复用主轨实时电平输入。
        addControlRow(
            footerVBox,
            "BGMGain",
            TR("ui.audio_manager.bgm_gain").data(),
            maxLabelW,
            audioManager.getBGMGain(),
            audioManager.isBGMGainMuted(),
            audioManager.getMainTrackLevelL(),
            audioManager.getMainTrackLevelR(),
            0.0f,
            1.0f,
            TR("ui.audio_manager.bgm_gain").data(),
            "%.4f",
            // 增益和静音由 AudioManager 维护，视图不保存重复状态。
            [&](float v) { audioManager.setBGMGain(v); },
            [&](bool m) { audioManager.setBGMGainMute(m); });

        // 普通音效增益当前没有独立电平展示，因此传入零值占位。
        addControlRow(
            footerVBox,
            "SFXGain",
            TR("ui.audio_manager.sfx_gain").data(),
            maxLabelW,
            audioManager.getSFXGain(),
            audioManager.isSFXGainMuted(),
            0.0f,
            0.0f,
            0.0f,
            1.0f,
            TR("ui.audio_manager.sfx_gain").data(),
            "%.4f",
            // 交互结束前每次滑条变化都即时写入，保持听觉反馈连续。
            [&](float v) { audioManager.setSFXGain(v); },
            [&](bool m) { audioManager.setSFXGainMute(m); });

        // 交互音效使用独立总线，避免 UI 反馈音受普通谱面音效控制。
        addControlRow(
            footerVBox,
            "InteractionSFXGain",
            TR("ui.audio_manager.interaction_sfx_volume").data(),
            maxLabelW,
            audioManager.getInteractionSFXGain(),
            audioManager.isInteractionSFXGainMuted(),
            0.0f,
            0.0f,
            0.0f,
            1.0f,
            TR("ui.audio_manager.interaction_sfx_volume").data(),
            "%.4f",
            // 静音与增益分别提交，保留取消静音后的原增益值。
            [&](float v) { audioManager.setInteractionSFXGain(v); },
            [&](bool m) { audioManager.setInteractionSFXGainMute(m); });
    }

    // 布局分为可伸缩列表、固定页脚控制和底部等宽按钮三个区域。
    // 页脚高度已由缓存结合折叠状态计算，本帧不再重新测量子控件。
    float footerH = layoutMetrics.footerHeight;

    // 根容器只描述顶部列表区域，它自动占据扣除页脚后的全部高度。
    rootVBox
        .setPadding(toLayoutPixels(layoutMetrics.rootPadding),
                    toLayoutPixels(layoutMetrics.rootPadding),
                    toLayoutPixels(layoutMetrics.rootPadding),
                    toLayoutPixels(layoutMetrics.rootPadding))
        .setSpacing(toLayoutPixels(layoutMetrics.sectionSpacing))
        .addElement("listContentArea",
                    Sizing::Grow(),
                    Sizing::Grow(),
                    [&](Clay_BoundingBox r, bool isHovered) {
                        // 子窗口去除自身内边距，让表格边界严格贴合 Clay 区域。
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                            ImVec2(0.0f, 0.0f));
                        // 独立子窗口承载表格滚动，避免推动外层页脚位置。
                        ImGui::BeginChild("AudioListChild",
                                          { r.width, r.height },
                                          false,
                                          ImGuiWindowFlags_None);
                        // BeginChild 后重新读取实际内容区域，考虑滚动条占宽。
                        const ImVec2 childPos = ImGui::GetCursorScreenPos();
                        const ImVec2 childAvail =
                            ImGui::GetContentRegionAvail();
                        // 将 ImGui 内容区域重新包装为局部绘制器所需的 Clay
                        // 包围盒。
                        Clay_BoundingBox childBox{ .x      = childPos.x,
                                                   .y      = childPos.y,
                                                   .width  = childAvail.x,
                                                   .height = childAvail.y };
                        if ( project || !skinData.audioPaths.empty() ) {
                            // 有任一资源来源时展示表格；空项目也可查看皮肤音效。
                            renderAudioResourcesTable(childBox, isHovered);
                        } else {
                            // 两个来源都为空时不创建无意义的空表格。
                            renderInitialHint(childBox, isHovered);
                        }

                        // 子窗口和临时样式必须在同一回调内成对恢复。
                        ImGui::EndChild();
                        ImGui::PopStyleVar();
                    });

    // 极窄窗口下页脚可能超过可用高度，列表高度夹到零。
    const float listAreaHeight =
        std::max(0.0f, layoutContext.m_avail.y - footerH);
    if ( listAreaHeight > 0.0f ) {
        // 零高度时跳过 Clay 渲染，避免创建退化包围盒。
        rootVBox.renderInCurrent(layoutContext.m_startPos,
                                 { layoutContext.m_avail.x, listAreaHeight });
    }

    // 页脚紧接列表区域，其屏幕坐标由窗口起点和列表高度确定。
    ImVec2 footerPos = { layoutContext.m_startPos.x,
                         layoutContext.m_startPos.y + listAreaHeight };

    // 控制区高度不包含下方导入按钮及两区之间的间隔。
    float controlH = layoutMetrics.globalControlsHeight;
    // 折叠时仍渲染标题行，展开时容器内包含全部控制项。
    footerVBox.renderInCurrent(footerPos,
                               { layoutContext.m_avail.x, controlH });

    // 最底部两项入口按 Grow 等分宽度，保持导入与项目工具权重一致。
    // 容器沿用根区域左右内边距，两按钮之间的间距也按 DPI 后像素取整。
    // 该区域不进入 footerVBox，使折叠全局设置不会隐藏主要资源入口。
    CLayHBox bottomBtnHBox;
    bottomBtnHBox
        .setPadding(toLayoutPixels(layoutMetrics.rootPadding),
                    toLayoutPixels(layoutMetrics.rootPadding),
                    0,
                    0)
        .setSpacing(toLayoutPixels(layoutMetrics.rootPadding))
        .setAlignment(Alignment::Center())
        // 左侧按钮始终可用，用于从外部文件创建项目音频资源。
        .addElement(
            "Audio_ImportNew",
            Sizing::Grow(),
            Sizing::Fixed(layoutMetrics.importButtonHeight),
            [&engine](Clay_BoundingBox r, bool isHovered) {
                // 回调在 Clay 完成本帧布局后立即执行，engine 引用不会跨帧保存。
                // 原生与内置选择器最终都通过 AudioImportTriggerEvent
                // 汇入导入流程。
                // 本视图只负责选择路径，不决定目标文件名、复制位置或资源 ID。
                // 原生路径所有权遵循 NFD API；内置对话框由单例自行管理状态。
                // 文本与透明按钮样式仅覆盖本按钮，结束前按相反顺序恢复。
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(1, 1, 1, 0.2f));
                Utils::pushFixedButtonStyleVars();

                // 先绘制轻透明自定义背景，再在同一区域叠加反馈按钮。
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImDrawList* dl    = ImGui::GetWindowDrawList();
                ImVec4      bgCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
                // 默认背景降低不透明度，悬浮时再提高以提供视觉反馈。
                bgCol.w *= 0.5f;
                float rounding = ImGui::GetStyle().FrameRounding;

                if ( isHovered ) bgCol.w *= 1.5f;

                // 背景圆角沿用主题 FrameRounding，与其他输入控件一致。
                dl->AddRectFilled({ r.x, r.y },
                                  { r.x + r.width, r.y + r.height },
                                  ImGui::ColorConvertFloat4ToU32(bgCol),
                                  rounding);

                // 可见标签只显示加号，隐藏 ID 保证同帧控件标识唯一。
                if ( ::MMM::UI::FeedbackButton(
                         fmt::format("{}##ImportAudio", ICON_MMM_PLUS).c_str(),
                         ImVec2(r.width, r.height)) ) {
                    // 文件选择器类型和上次路径从当前编辑器配置快照读取。
                    const auto settings = engine.getEditorConfig().settings;
                    if ( settings.filePickerStyle ==
                         Config::FilePickerStyle::Native ) {
                        // 原生对话框打开前播放统一弹窗反馈。
                        ::MMM::UI::PlayPopupOpenFeedback();
                        nfdu8char_t* outPath = nullptr;
                        // 原生过滤器限制为音频导入流程支持的格式。
                        nfdu8filteritem_t filters[1] = {
                            { "Audio Files", "mp3,ogg,wav,flac,opus,aac,m4a" }
                        };
                        nfdresult_t result = NativeFileDialog::openFile(
                            &outPath, filters, 1, nullptr);

                        if ( result == NFD_OKAY ) {
                            // 发布事件转交导入流程处理；本视图不直接复制文件。
                            Event::EventBus::instance().publish(
                                Event::AudioImportTriggerEvent{ outPath });
                            // NFD 返回的 UTF-8 路径由库分配，成功分支负责释放。
                            NFD_FreePathU8(outPath);
                        } else if ( result == NFD_ERROR ) {
                            // 取消选择是正常交互，只有库错误需要记录日志。
                            XERROR("NFD Error: {}", NFD_GetError());
                        }
                    } else {
                        // 内置选择器使用只读文件名字段，并恢复上次浏览目录。
                        IGFD::FileDialogConfig fdConfig;
                        fdConfig.path     = settings.lastFilePickerPath;
                        fdConfig.fileName = "";
                        fdConfig.flags =
                            ImGuiFileDialogFlags_Modal |
                            ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_ReadOnlyFileNameField;
                        // 记录调用前状态，确保弹窗音效只在首次打开时播放。
                        const bool wasOpen =
                            ImGuiFileDialog::Instance()->IsOpened(
                                "AudioImportPicker");
                        // 重复调用 OpenDialog
                        // 由实例内部去重，不会重建已开弹窗。
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "AudioImportPicker",
                            TR("ui.audio_manager.import_audio").data(),
                            ".mp3,.ogg,.wav,.flac,.opus,.aac,.m4a",
                            fdConfig);
                        if ( !wasOpen && ImGuiFileDialog::Instance()->IsOpened(
                                             "AudioImportPicker") ) {
                            // 仅从关闭到打开的边沿产生一次听觉反馈。
                            ::MMM::UI::PlayPopupOpenFeedback();
                        }
                    }
                }

                // 恢复按钮样式后再恢复四项颜色，维持外层 UI 样式栈。
                Utils::popFixedButtonStyleVars();
                ImGui::PopStyleColor(4);
                if ( ImGui::IsItemHovered() ) {
                    // 图标按钮没有可见文字，悬浮提示补充完整动作名称。
                    Utils::renderTooltip(
                        TR("ui.audio_manager.import_audio").data());
                }
            })
        // 右侧按钮打开项目级音频工具，没有项目时保持可见但禁用。
        .addElement(
            "Audio_ProjectTool",
            Sizing::Grow(),
            Sizing::Fixed(layoutMetrics.importButtonHeight),
            [sourceManager, project](Clay_BoundingBox r, bool isHovered) {
                // project 只作为本帧可用性快照，不在回调外延长项目生命周期。
                // sourceManager 为空时按钮仍可绘制，但点击不会尝试打开窗口。
                // 禁用态允许悬浮提示，帮助无项目状态下解释入口前置条件。
                // 打开动作由 UIManager 去重并管理焦点，本视图不保存窗口指针。
                // 禁用态只弱化文字，仍绘制与左按钮对称的背景轮廓。
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    project ? ImGui::GetStyle().Colors[ImGuiCol_Text]
                            : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(1, 1, 1, 0.2f));
                Utils::pushFixedButtonStyleVars();

                // 背景透明度直接由 Clay 悬浮结果决定，不依赖禁用按钮命中。
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec4 background = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
                background.w *= isHovered ? 0.75F : 0.5F;
                // 使用主题圆角和框体颜色维持两个入口的一致外观。
                drawList->AddRectFilled(
                    { r.x, r.y },
                    { r.x + r.width, r.y + r.height },
                    ImGui::ColorConvertFloat4ToU32(background),
                    ImGui::GetStyle().FrameRounding);

                // 项目是工具的数据源；不存在项目时阻止点击提交。
                ImGui::BeginDisabled(!project);
                if ( ::MMM::UI::FeedbackButton(
                         fmt::format("{}##ProjectAudioTool", ICON_MMM_MUSIC)
                             .c_str(),
                         ImVec2(r.width, r.height)) &&
                     sourceManager ) {
                    // UIManager 负责窗口实例和焦点管理，视图不直接构造工具。
                    sourceManager->openProjectAudioTool();
                }
                ImGui::EndDisabled();

                // 禁用作用域、按钮样式与颜色栈均在局部回调内闭合。
                Utils::popFixedButtonStyleVars();
                ImGui::PopStyleColor(4);
                if ( ImGui::IsItemHovered(
                         ImGuiHoveredFlags_AllowWhenDisabled) ) {
                    // 即使无项目也显示提示，让用户理解入口用途。
                    Utils::renderTooltip(
                        TR("ui.audio_manager.open_project_audio_tool").data());
                }
            });

    // 按钮区域位于控制区下方，并保留缓存计算的垂直间隔。
    ImVec2 btnPos = { footerPos.x,
                      footerPos.y + controlH + layoutMetrics.importButtonGap };
    // 两按钮共用窗口可用宽度和固定高度，不受页脚折叠状态影响。
    bottomBtnHBox.renderInCurrent(
        btnPos, { layoutContext.m_avail.x, layoutMetrics.importButtonHeight });

    // 行级移除请求先转换为一次 OpenPopup 调用，再清除边沿标志。
    // 待删除 ID 独立于表格行引用，因此缓存重建或滚动不会使确认目标悬空。
    // 确认和取消都会清空该 ID；弹窗关闭后没有残留请求进入下一帧。
    if ( m_openRemoveModal ) {
        ::MMM::UI::FeedbackOpenPopup("RemoveTrackConfirm");
        // 弹窗打开状态由 ImGui 接管，避免后续帧反复重开。
        m_openRemoveModal = false;
    }
    if ( !m_removeTrackId.empty() ) {
        // 仅在存在待处理资源 ID 时维持确认弹窗生命周期。
        Utils::CenteredModalPopupScope removeModalScope(dpiScale);
        if ( removeModalScope.begin("RemoveTrackConfirm") ) {
            // 模态框阻止误操作落到后方资源表格。
            ImGui::Text("%s", TR("ui.audio_manager.remove_confirm").data());
            ImGui::Spacing();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.confirm").data(),
                                           { 100 * dpiScale, 0 }) ) {
                // 删除改变资源集合和分组范围，必须使排序缓存失效。
                m_audioTableSortCacheDirty = true;
                // 通过命令系统提交删除，以保留项目编辑历史的一致入口。
                engine.pushCommand(
                    Logic::CmdRemoveAudioResource{ m_removeTrackId });
                // 清除待处理 ID 后关闭弹窗，防止下一帧再次提交。
                m_removeTrackId.clear();
                ImGui::CloseCurrentPopup();
            }
            // 取消按钮与确认按钮同排，尺寸一致且不修改项目。
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           { 100 * dpiScale, 0 }) ) {
                // 取消只清除暂存请求，并显式关闭当前弹窗。
                m_removeTrackId.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // 只在入口实际压入字体时恢复，保持调用者的字体栈不变。
    if ( fileManagerFont ) ImGui::PopFont();
}
}  // namespace MMM::UI
