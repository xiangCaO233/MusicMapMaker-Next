#include "ui/imgui/manager/ToolbarView.h"

#include "audio/AudioManager.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/ColorPaletteFile.h"
#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/ICanvasView.h"
#include "ui/IEditorApplicationService.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/manager/SoundEffectToolTrackLayout.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <iterator>
#include <limits>
#include <mutex>
#include <nfd.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace MMM::UI
{

namespace
{
/// @brief 绘制文本空间不足时自动滚动的工具栏方形按钮。
/// @param id 不显示的 ImGui ID。
/// @param text 按钮显示文本。
/// @param size 按钮尺寸。
/// @return 本帧按钮被激活时返回 true。
/// @warning UI 热路径：只复用反馈按钮并追加一次局部裁剪文本绘制。
///
/// 按钮本体只使用隐藏 ID，显示文本由绘制列表单独提交，便于在窄区域滚动。
/// 文本裁剪严格限制在按钮内边距内，不会覆盖相邻工具栏项目。
///
/// 该 helper 专用于倍速等动态短文本。隐藏 ID 保证显示值变化不重建控件，
/// scrolling helper 则只在测量宽度超过可用区域时启用动画。按钮矩形取自
/// ImGui 最终布局结果，因此主题边距、DPI 和窗口约束变化都能自动适配。
/// 返回值保留 FeedbackButton 的统一悬浮与点击反馈语义。
bool drawToolbarScrollingButton(const char* id, std::string_view text,
                                ImVec2 size)
{
    // 统一反馈按钮先建立命中区，并保存点击结果供绘制结束后返回。
    const bool clicked = ::MMM::UI::FeedbackButton(id, size);
    // 读取刚提交按钮的实际屏幕矩形，避免重复推导布局坐标。
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    // 水平留白至少两像素，紧凑主题下文字仍不贴边。
    const float padding = std::max(2.0f, ImGui::GetStyle().FramePadding.x);
    const float availableWidth =
        std::max(0.0f, itemMax.x - itemMin.x - padding * 2.0f);
    // 公共滚动文本 helper 在溢出时动画，空间足够时保持居中静止。
    Utils::drawScrollingText(text,
                             ImVec2(itemMin.x + padding, itemMin.y),
                             availableWidth,
                             itemMax.y - itemMin.y,
                             true);
    // 装饰绘制不改变最初由 FeedbackButton 捕获的点击结果。
    return clicked;
}

/// @brief 将颜色槽位转换为数组索引。
/// @param slot 笔记颜色槽位枚举。
/// @return 与颜色数组顺序一致的无符号索引。
///
/// NoteColorSlot 枚举值必须保持从零连续，调用方随后用于定长数组访问。
/// 转换本身不做范围检查，因为调用点要么遍历 NOTE_COLOR_SLOT_COUNT，要么使用
/// 类成员中已经验证的活动槽位。若枚举改为非连续值，必须同步重构数组映射，
/// 不能仅在此函数夹取后掩盖 ABI 与序列化顺序差异。
std::size_t colorSlotIndex(Logic::NoteColorSlot slot)
{
    return static_cast<std::size_t>(slot);
}

/// @brief 获取颜色槽位对应的翻译键。
/// @param slot 笔记颜色槽位。
/// @return 对应调色盘标签的稳定翻译键。
///
/// 未知枚举回退 Tap 标签，避免弹窗展示空白名称。
///
/// 返回的是静态翻译键而非本地化文本，调用方在绘制时调用 TR，从而支持语言
/// 热切换。switch 显式列出每个槽位，使编译器警告能提示新增枚举未完成映射。
/// 回退仅用于防御损坏状态，不应作为新增槽位的正常显示策略。
const char* colorSlotLabelKey(Logic::NoteColorSlot slot)
{
    switch ( slot ) {
    case Logic::NoteColorSlot::Tap: return "ui.toolbar.note_palette.note";
    case Logic::NoteColorSlot::Head: return "ui.toolbar.note_palette.note_head";
    case Logic::NoteColorSlot::Hold: return "ui.toolbar.note_palette.note_hold";
    case Logic::NoteColorSlot::End: return "ui.toolbar.note_palette.note_end";
    case Logic::NoteColorSlot::FlickArrow:
        return "ui.toolbar.note_palette.note_flick_arrow";
    case Logic::NoteColorSlot::Node: return "ui.toolbar.note_palette.note_node";
    }
    // 防御未来枚举扩展未同步映射的情况。
    return "ui.toolbar.note_palette.note";
}

/// @brief 获取分拍线颜色槽位对应的翻译键。
/// @param slotIndex 分拍线颜色数组索引。
/// @return 具体分母或默认线颜色的翻译键。
///
/// 索引顺序与 BEAT_LINE_COLOR_PALETTE_DENOMINATORS 保持一致。
///
/// 翻译键显式展开是为了让每个分母拥有独立可本地化名称，同时保持存储数组
/// 的稳定顺序。default 分支覆盖越界和未来新增槽位，在配置与翻译尚未同步时
/// 仍给出可理解标签。此 helper 不访问皮肤颜色或运行时配置。
const char* beatLineColorSlotLabelKey(std::size_t slotIndex)
{
    switch ( slotIndex ) {
    case 0: return "ui.toolbar.note_palette.beat_line_1";
    case 1: return "ui.toolbar.note_palette.beat_line_2";
    case 2: return "ui.toolbar.note_palette.beat_line_3";
    case 3: return "ui.toolbar.note_palette.beat_line_4";
    case 4: return "ui.toolbar.note_palette.beat_line_6";
    case 5: return "ui.toolbar.note_palette.beat_line_8";
    case 6: return "ui.toolbar.note_palette.beat_line_12";
    case 7: return "ui.toolbar.note_palette.beat_line_16";
    // 越界索引使用默认分拍线标签，调用方仍可给出可理解提示。
    default: return "ui.toolbar.note_palette.beat_line_default";
    }
}

/// @brief 获取颜色槽位对应的皮肤颜色键。
/// @param slot 笔记颜色槽位。
/// @return SkinData colors 表中的稳定键名。
///
/// 映射只描述新格式键名，旧皮肤兼容由 skinColorForSlot 统一处理。
///
/// 键名不带 colors. 前缀，因为 SkinManager 的 getColor 接口从颜色表根节点
/// 解析。把兼容逻辑留在读取 helper，可保证导出、自定义方案和 UI 标签继续
/// 使用统一槽位身份。未知值回退 Tap 只防止无效字符串查询。
const char* colorSlotSkinKey(Logic::NoteColorSlot slot)
{
    switch ( slot ) {
    case Logic::NoteColorSlot::Tap: return "note_tap";
    case Logic::NoteColorSlot::Head: return "note_head";
    case Logic::NoteColorSlot::Hold: return "note_hold";
    case Logic::NoteColorSlot::End: return "note_end";
    case Logic::NoteColorSlot::FlickArrow: return "note_flick_arrow";
    case Logic::NoteColorSlot::Node: return "note_node";
    }
    // 未知枚举回退最基础的 Tap 颜色。
    return "note_tap";
}

/// @brief 获取颜色槽位对应的皮肤颜色，兼容旧皮肤的头部和尾部颜色。
/// @param slot 笔记颜色槽位。
/// @return 皮肤定义或兼容回退后的 RGBA 颜色。
///
/// 旧皮肤可能只提供 note_hold，Head 与 End 缺失时沿用该颜色。
///
/// 缺失检测使用 SkinData 的键存在性，而不是颜色值比较，因此用户主动设置
/// 洋红色不会在这两个兼容分支被误判。其他新槽位没有历史等价项，仍由
/// SkinManager 的标准缺失策略处理，便于主题作者发现未配置键。
/// 返回值按值复制，不延长 SkinData 内部对象引用的生命周期。
Config::Color skinColorForSlot(Logic::NoteColorSlot slot)
{
    // SkinManager 拥有当前皮肤数据，本函数只同步读取。
    auto& skin = Config::SkinManager::instance();
    if ( slot == Logic::NoteColorSlot::Head &&
         !skin.getData().colors.contains("note_head") ) {
        // 头部缺失时使用持续段颜色，保持旧主题视觉连贯。
        return skin.getColor("note_hold");
    }
    if ( slot == Logic::NoteColorSlot::End &&
         !skin.getData().colors.contains("note_end") ) {
        // 尾部采用同一兼容规则，不把洋红缺失占位暴露给用户。
        return skin.getColor("note_hold");
    }
    // 新皮肤或其他槽位直接读取专用键。
    return skin.getColor(colorSlotSkinKey(slot));
}

/// @brief 将皮肤颜色转换为 glm 颜色。
/// @param color 配置层颜色值。
/// @return 通道顺序不变的 glm::vec4。
///
/// 转换不执行色域或 Alpha 预乘，调色盘统一使用直通 RGBA。
/// 两个类型都以 float 保存通道，逐字段构造可避免依赖聚合内存布局或别名规则。
/// 调用方负责保证颜色范围；皮肤配置的验证和编辑控件已经执行约束。
glm::vec4 toVec4(const Config::Color& color)
{
    return { color.r, color.g, color.b, color.a };
}

/// @brief 将 glm 颜色转换为 ImGui 颜色。
/// @param color 调色盘 RGBA 值。
/// @return 可直接传给 ImGui 的同通道颜色。
///
/// 两种类型都使用浮点 0 至 1 表示，无需缩放。
/// ImGui 在此路径接收非预乘 Alpha，和 glm 调色盘存储约定一致。
/// 返回临时值适合直接传给绘制 API，不暴露底层数组指针。
ImVec4 toImVec4(glm::vec4 color)
{
    return { color.r, color.g, color.b, color.a };
}

/// @brief 将 glm 颜色转换为配置存储颜色。
/// @param color 调色盘 RGBA 值。
/// @return 固定四元素数组，顺序为红、绿、蓝、透明度。
///
/// 值不在此处夹取，编辑控件和解析入口负责保证合法范围。
/// 固定数组是配置序列化层的稳定表示，避免让 nlohmann/json 依赖 glm 类型。
/// 顺序始终为 RGBA，不能与 ImGui 的打包色整数通道顺序混用。
std::array<float, 4> toStoredColor(glm::vec4 color)
{
    return { color.r, color.g, color.b, color.a };
}

/// @brief 将配置存储颜色转换为 glm 颜色。
/// @param color 固定四通道存储数组。
/// @return 对应的 glm RGBA 值。
///
/// 此函数与 toStoredColor 互为无损值转换。
/// 参数使用常量引用避免复制存储数组，返回 glm 值供 UI 安全修改。
/// 不做 Alpha 预乘或色彩空间转换，确保方案导入导出往返保持数值。
glm::vec4 fromStoredColor(const std::array<float, 4>& color)
{
    return { color[0], color[1], color[2], color[3] };
}

/// @brief 判断皮肤颜色查询结果是否为缺失键占位色。
/// @param color SkinManager 返回的颜色。
/// @return 精确等于不透明洋红占位值时返回 true。
///
/// 该判断只用于皮肤键兼容，用户主动设置同色仍可能被视作占位。
///
/// SkinManager 以精确 RGBA 洋红表示未找到嵌套键；这里不采用近似比较，避免
/// 把主题作者配置的相近颜色误判为缺失。调用点仅用于分拍线具体分母回退，
/// 普通笔记兼容通过键存在性判断处理。
bool isMissingSkinColor(const Config::Color& color)
{
    return color.r == 1.0f && color.g == 0.0f && color.b == 1.0f &&
           color.a == 1.0f;
}

/// @brief 获取分拍线槽位对应的皮肤颜色。
/// @param slotIndex 分拍线调色盘槽位索引。
/// @return 具体分母颜色，缺失或越界时回退默认分拍线颜色。
///
/// 具体键由 denominator 数组生成，避免槽位与分母映射在多处重复。
///
/// 分母数组是槽位语义的权威来源，字符串只在读取时临时构造。具体键缺失时
/// 回退 beat_lines.default，使旧皮肤无需声明每个细分颜色。越界同样回退
/// 默认键，但不会尝试构造不存在分母的路径。
/// 返回配置颜色值，后续统一转换为运行时 glm 表示。
Config::Color skinBeatLineColor(std::size_t slotIndex)
{
    // 越界输入不能索引分母数组，直接读取默认键。
    auto& skin = Config::SkinManager::instance();
    if ( slotIndex >= Config::BEAT_LINE_COLOR_PALETTE_DENOMINATORS.size() ) {
        return skin.getColor("beat_lines.default");
    }

    // 合法槽位的分母值决定 beat_lines.beat_N 键名。
    const int denominator =
        Config::BEAT_LINE_COLOR_PALETTE_DENOMINATORS[slotIndex];
    // 缺失键由 SkinManager 返回约定洋红色，随后转换为默认颜色。
    Config::Color color =
        skin.getColor("beat_lines.beat_" + std::to_string(denominator));
    return isMissingSkinColor(color) ? skin.getColor("beat_lines.default")
                                     : color;
}

/// @brief 用皮肤默认颜色填充调色盘缓存。
/// @param colors 接收所有笔记槽位颜色的定长数组。
///
/// 枚举连续性由 NOTE_COLOR_SLOT_COUNT 约束，每个槽位都经过旧皮肤兼容。
///
/// 函数完整覆盖输出数组，不依赖其先前内容，适用于首次初始化和换肤刷新。
/// 循环上界来自目标语义常量而不是数组魔数，新增槽位时编译期尺寸会同步。
/// 读取皮肤发生在明确的初始化或皮肤刷新路径，不在每个色块绘制中批量执行。
void fillPaletteWithSkinDefaults(
    std::array<glm::vec4, Logic::NOTE_COLOR_SLOT_COUNT>& colors)
{
    for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
        // 数组索引按枚举底层值恢复为具体槽位。
        auto slot = static_cast<Logic::NoteColorSlot>(i);
        colors[i] = toVec4(skinColorForSlot(slot));
    }
}

/// @brief 用皮肤默认颜色填充分拍线调色盘缓存。
/// @param colors 接收全部分拍线槽位颜色的定长数组。
///
/// 每个槽位都允许具体皮肤键缺失并回退 beat_lines.default。
///
/// 使用输出数组自身 size 作为上界，确保与配置定义的固定槽位数量一致。
/// 完整覆盖保证关闭 override 时 UI 预览仍准确反映当前皮肤，而不是残留旧方案
/// 的部分颜色。函数不修改覆盖标志或提交视觉配置。
void fillBeatLinePaletteWithSkinDefaults(
    std::array<glm::vec4, Config::BEAT_LINE_COLOR_PALETTE_SLOT_COUNT>& colors)
{
    for ( std::size_t i = 0; i < colors.size(); ++i ) {
        // 配置颜色统一转换到运行时 glm 表示。
        colors[i] = toVec4(skinBeatLineColor(i));
    }
}

/// @brief 在皮肤默认颜色基础上应用一个自定义调色盘方案。
/// @param colors 接收笔记颜色的运行时数组。
/// @param beatLineColors 接收分拍线颜色的运行时数组。
/// @param scheme 已验证的存储方案。
///
/// 方案包含完整定长数组，因此这里覆盖全部槽位而非仅处理差异项。
///
/// 配置加载阶段已经验证数组尺寸和数值结构，本 helper 只负责表示转换。
/// 两个循环独立使用各自目标 size，避免未来笔记槽和分拍线槽数量变化时耦合。
/// 输出数组在函数结束后形成完整一致快照，调用方随后再统一推送下游状态。
void applyStoredPaletteScheme(
    std::array<glm::vec4, Logic::NOTE_COLOR_SLOT_COUNT>& colors,
    std::array<glm::vec4, Config::BEAT_LINE_COLOR_PALETTE_SLOT_COUNT>&
                                      beatLineColors,
    const Config::ColorPaletteScheme& scheme)
{
    // 笔记颜色与存储数组按相同固定索引逐项转换。
    for ( std::size_t i = 0; i < colors.size(); ++i ) {
        colors[i] = fromStoredColor(scheme.noteColors[i]);
    }
    // 分拍线颜色使用独立数组，避免与笔记槽位数量耦合。
    for ( std::size_t i = 0; i < beatLineColors.size(); ++i ) {
        beatLineColors[i] = fromStoredColor(scheme.beatLineColors[i]);
    }
}

/// @brief 将 0 到 1 的颜色通道转换为 8 位整数。
/// @param value 浮点颜色通道。
/// @return 四舍五入并夹取到 0 至 255 的整数。
///
/// 先乘 255 再 round，保证 0.5 等中间值映射到最近字节。
/// 输入先四舍五入再夹取，可容忍浮点计算产生的极小越界并保证 snprintf 参数
/// 始终处于两位十六进制范围。该转换只用于文本显示，不回写原浮点颜色。
int colorChannelToByte(float value)
{
    return static_cast<int>(
        std::clamp(std::round(value * 255.0f), 0.0f, 255.0f));
}

/// @brief 将颜色转换为 #RRGGBBAA 文本。
/// @param color 浮点 RGBA 颜色。
/// @return 大写十六进制八通道文本，始终包含 Alpha。
///
/// 固定缓冲大小覆盖井号、八位数字和结尾空字符。
///
/// Alpha 始终输出，避免透明颜色在编辑往返中丢失信息。使用大写格式提供稳定
/// 比较和复制文本；解析器仍接受大小写输入。每个通道先通过统一量化 helper，
/// 保证负值或略大于一的外部配置也不会生成超宽文本。
std::string colorToHexString(glm::vec4 color)
{
    // 零初始化保证 snprintf 异常截断时仍有终止空字符。
    char buffer[10]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "#%02X%02X%02X%02X",
                  colorChannelToByte(color.r),
                  colorChannelToByte(color.g),
                  colorChannelToByte(color.b),
                  colorChannelToByte(color.a));
    // 返回字符串复制栈缓冲内容，不依赖局部生命周期。
    return std::string(buffer);
}

/// @brief 判断字符是否为空白。
/// @param ch 单个输入字节。
/// @return 属于十六进制输入允许剔除的 ASCII 空白时返回 true。
///
/// 只处理首尾 ASCII 空白，不依赖当前区域设置。
/// 不调用 ctype 可避免负 char 值和区域设置带来的未定义或平台差异。
/// 十六进制主体内部的空白不会被接受，防止看似合法但含分隔符的颜色串。
bool isHexInputSpace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

/// @brief 获取单个十六进制字符值。
/// @param ch 待解析字符。
/// @return 0 至 15，非十六进制字符返回 -1。
///
/// 同时接受大小写字母，输出统一为数值。
/// 分支只覆盖 ASCII 范围，UTF-8 高位字节和其他字符稳定返回失败。
/// 返回 int 的 -1 哨兵便于 parseHexByte 在写输出前验证两个半字节。
int hexDigitValue(char ch)
{
    if ( ch >= '0' && ch <= '9' ) return ch - '0';
    if ( ch >= 'a' && ch <= 'f' ) return ch - 'a' + 10;
    if ( ch >= 'A' && ch <= 'F' ) return ch - 'A' + 10;
    // 其他标点和非 ASCII 字节均视为解析失败。
    return -1;
}

/// @brief 解析两个十六进制字符为 8 位通道值。
/// @param text 已验证长度的十六进制主体。
/// @param offset 高半字节位置。
/// @param value 接收 0 至 255 的通道值。
/// @return 两个字符都合法时返回 true。
///
/// 调用方保证 offset 与 offset+1 位于字符串范围内。
/// 两个半字节先全部解析，任一失败都不修改 value，保持调用方的事务式解析。
/// 组合使用算术而非位操作只是表达十六进制位权，结果范围等价为 0..255。
bool parseHexByte(std::string_view text, std::size_t offset, int& value)
{
    int hi = hexDigitValue(text[offset]);
    int lo = hexDigitValue(text[offset + 1]);
    // 任一半字节无效时不写入输出值。
    if ( hi < 0 || lo < 0 ) return false;
    // 高半字节乘十六后与低半字节组合。
    value = hi * 16 + lo;
    return true;
}

/// @brief 解析 #RRGGBB 或 #RRGGBBAA 颜色文本。
/// @param text 用户输入颜色文本，可含首尾 ASCII 空白和可选井号。
/// @param color 成功时接收归一化 RGBA 值。
/// @return 格式与所有数字合法时返回 true。
///
/// 六位格式默认 Alpha 为 255，八位格式显式读取最后一通道。
/// 失败路径不修改输出颜色，调用方可保留当前调色盘值。
///
/// 解析只创建 string_view 子视图，不分配临时字符串，适合输入框每次变化时
/// 即时调用。可选井号只允许一个且必须位于去除首尾空白后的开头。三位、四位
/// 简写和其他通道顺序明确不支持，以保持导出格式与编辑格式一一对应。
/// 所有字节验证完成后才写 color，保证失败不会造成部分通道更新。
bool parseHexColor(std::string_view text, glm::vec4& color)
{
    // 先定位去空白后的半开区间，不分配临时字符串。
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while ( begin < end && isHexInputSpace(text[begin]) ) {
        ++begin;
    }
    while ( end > begin && isHexInputSpace(text[end - 1]) ) {
        --end;
    }
    // 子视图只调整指针和长度，仍引用原输入缓冲区。
    text = text.substr(begin, end - begin);
    if ( !text.empty() && text.front() == '#' ) {
        // 井号只允许位于去空白后的首字符。
        text.remove_prefix(1);
    }
    // 只接受完整 RGB 或 RGBA 字节，不支持三位缩写。
    if ( text.size() != 6 && text.size() != 8 ) return false;

    int r = 0;
    int g = 0;
    int b = 0;
    // 六位格式预设完全不透明 Alpha。
    int a = 255;
    if ( !parseHexByte(text, 0, r) || !parseHexByte(text, 2, g) ||
         !parseHexByte(text, 4, b) ) {
        // RGB 任一通道失败时保持调用方颜色不变。
        return false;
    }
    if ( text.size() == 8 && !parseHexByte(text, 6, a) ) {
        // 显式 Alpha 非法同样使整个输入失败。
        return false;
    }

    // 所有通道验证完成后一次性写入输出，避免部分更新。
    color = { static_cast<float>(r) / 255.0f,
              static_cast<float>(g) / 255.0f,
              static_cast<float>(b) / 255.0f,
              static_cast<float>(a) / 255.0f };
    return true;
}

/// @brief 获取默认调色盘方案名。
/// @return 当前语言的皮肤默认方案显示名。
///
/// 此文本仅用于 UI 展示和保留名校验，持久化身份使用固定内部 ID。
/// 每次调用在当前语言环境下翻译，因此运行时换语言后菜单无需重建缓存。
/// 返回拥有字符串而不是翻译视图，调用方可安全跨表达式使用。
std::string defaultPaletteSchemeName()
{
    return TR("ui.toolbar.note_palette.skin_default_scheme").data();
}

/// @brief 获取继承软件默认调色盘方案名。
/// @return 当前语言的项目继承选项显示名。
///
/// 空项目偏好在界面中以该文本表达，不作为自定义方案存储。
/// 与皮肤默认显示名一样，该名称被保留名校验拒绝，避免自定义项在组合框中
/// 与语义选项无法区分。实际项目继承由空字段表达，不持久化此翻译文本。
std::string inheritedPaletteSchemeName()
{
    return TR("ui.settings.project.note_palette.inherit").data();
}

/// @brief 判断方案名是否保留给内置调色盘选项。
/// @param name 待检查方案名。
/// @return 空名、皮肤默认内部 ID 或当前本地化内置名称时返回 true。
///
/// 自定义方案禁止使用这些名称，避免菜单项身份和保存目标产生歧义。
///
/// 检查同时覆盖稳定 ID 和当前语言的两个显示名。旧语言下保存的同义名称
/// 可能不会在新语言中被识别，这是保留本地化文本作为 UI 约束的既有边界；
/// 稳定皮肤 ID 始终不可被占用。空字符串单独拒绝，避免名称关联失效。
bool isReservedPaletteSchemeName(const std::string& name)
{
    // 同时检查稳定 ID 和两个可见名称，覆盖导入文件与手工输入来源。
    return name.empty() ||
           name == Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ||
           name == defaultPaletteSchemeName() ||
           name == inheritedPaletteSchemeName();
}

/// @brief 将方案名转换为可用作推荐导出文件名的文本。
/// @param name 当前调色方案名。
/// @return 已替换路径非法字符且移除结尾空格和点号的文件名主体。
///
/// 非法控制字节和 Windows 保留路径字符统一替换为下划线。
/// UTF-8 高位字节保持不变，允许本地语言方案名作为建议文件名。
/// 输出始终追加项目专用扩展名，不包含目录部分。
///
/// 这是建议名清理而非完整路径安全验证：文件选择器仍决定最终目录和名称，
/// 导出入口随后规范扩展名。逐字节扫描只替换 ASCII 保留字符，不会拆分合法
/// UTF-8 序列。尾部点号和空格按 Windows 最严格规则删除，使建议名跨平台。
/// 若清理后只剩相对目录标识，则使用稳定英文主体避免路径解析歧义。
std::string sanitizePaletteExportFileName(std::string name)
{
    // 逐字节只处理 ASCII 控制符和明确保留字符，不拆解 UTF-8 序列。
    for ( char& character : name ) {
        const auto byte = static_cast<unsigned char>(character);
        if ( byte < 32 || character == '<' || character == '>' ||
             character == ':' || character == '"' || character == '/' ||
             character == '\\' || character == '|' || character == '?' ||
             character == '*' ) {
            // 使用下划线保留原名称大致结构，而不是直接删除字符。
            character = '_';
        }
    }
    // Windows 路径不允许结尾空格或点号，连续尾部字符全部移除。
    while ( !name.empty() && (name.back() == ' ' || name.back() == '.') ) {
        name.pop_back();
    }
    // 空结果和相对目录标识使用安全通用名称回退。
    if ( name.empty() || name == "." || name == ".." ) {
        name = "palette";
    }
    // 扩展名由配置模块集中定义，保持导入过滤器和序列化格式一致。
    name += Config::COLOR_PALETTE_FILE_EXTENSION;
    return name;
}

/// @brief 规范化调色方案导出路径的扩展名。
/// @param path 文件选择器返回的 UTF-8 路径。
/// @return 使用 `.mmpalette` 扩展名的 UTF-8 路径。
///
/// 无论用户是否输入扩展名，都用项目定义扩展名替换末级后缀。
/// 路径先转换为平台 filesystem 表示，再安全恢复 UTF-8。
///
/// replace_extension 只修改最后一个路径组件，父目录中的点号不受影响。
/// 空路径代表取消或异常结果并保持为空，调用方据此拒绝写文件。统一扩展名
/// 防止文件选择器在用户手工输入其他后缀时生成无法被导入过滤器发现的文件。
std::string normalizePaletteExportPath(const std::string& path)
{
    // 取消文件选择器返回空串，调用方无需尝试创建文件。
    if ( path.empty() ) {
        return {};
    }
    // 平台路径 API 负责识别最后一个扩展名边界。
    std::filesystem::path normalized = Config::utf8ToPath(path);
    normalized.replace_extension(Config::COLOR_PALETTE_FILE_EXTENSION);
    return Config::pathToUtf8(normalized);
}
}  // namespace

/// @brief 创建工具栏视图。
/// @param name 视图名称。
///
/// 调色盘和弹窗状态按成员默认值初始化，实际皮肤颜色在首次 update 时加载。
/// 构造阶段不访问 SkinManager、AppConfig 或 EditorEngine，确保视图可以在应用
/// 服务和图形上下文完全就绪前创建。name 只交给 IUIView 管理窗口身份。
/// 所有跨帧 UI 缓冲使用类内默认初始化，首次绘制再同步外部权威状态。
ToolbarView::ToolbarView(const std::string& name) : IUIView(name) {}

/// @brief 绑定工具栏使用的编辑器应用服务观察指针。
/// @param service 当前 UIManager 提供的服务；可为空。
///
/// 指针不转移所有权且只在当前 UI 生命周期内使用，空值触发配置读取回退。
///
/// update 每帧从 UIManager 重新绑定该观察指针，因此应用服务被替换或销毁后
/// 不会长期保留旧地址。函数不读取 service，也不触发配置同步；实际访问只在
/// 同一 UI 更新调用链中发生。调用者必须保证服务至少存活到本帧绘制结束。
void ToolbarView::setEditorApplicationService(
    IEditorApplicationService* service)
{
    // 每帧从 UIManager 同步，避免保存已替换应用服务的旧指针。
    // 服务实例变化时下一次打开音效工具必须重新取得权威配置。
    if ( m_editorApplicationService != service ) {
        m_soundEffectEditorConfigRevision =
            std::numeric_limits<std::uint64_t>::max();
    }
    m_editorApplicationService = service;
}

/// @brief 获取工具栏当前应读取的编辑器配置快照。
/// @return 应用服务配置；未绑定服务时回退 AppConfig。
///
/// 返回值语义隔离调用方修改，任何变更都必须显式交给 updateEditorConfig。
/// 应用服务存在时它是权威来源，可包含尚未反映到 AppConfig 引用中的新状态；
/// 回退只服务于初始化或测试环境，不建立额外所有权。
///
/// 返回值按值复制，调用方可修改后通过 updateEditorConfig 提交。
Config::EditorConfig ToolbarView::currentEditorConfig() const
{
    // UIManager 服务优先，保证运行中临时配置与持久化入口一致。
    return m_editorApplicationService
               ? m_editorApplicationService->editorConfig()
               : Config::AppConfig::instance().getEditorConfig();
}

/// @brief 通过编辑器应用服务提交新的编辑器配置。
/// @param config 已修改的完整配置快照。
///
/// 未绑定服务时不直接写 AppConfig，避免绕过应用层的变更通知与持久化流程。
///
/// 完整快照提交允许应用层统一比较字段、通知渲染消费者并决定保存策略。
/// 工具栏不保留提交后的引用，也不假设更新同步生效；下一帧重新读取权威值。
/// 空服务属于合法降级状态，调用者无需在每个控件分支重复检查。
void ToolbarView::updateEditorConfig(const Config::EditorConfig& config) const
{
    if ( m_editorApplicationService ) {
        // 服务负责应用、保存并广播配置变化。
        m_editorApplicationService->updateEditorConfig(config);
    }
}

/// @brief 绘制固定宽度工具栏并处理工具、快捷开关和各独立弹窗入口。
/// @param sourceManager 当前 UI 管理器，用于应用服务、画布焦点与窗口状态。
///
/// 工具栏先同步引擎状态和用户可见性配置，再按固定顺序绘制状态工具与独立按钮。
/// 所有互斥浮层在入口打开时关闭其他浮层，避免多个锚定窗口同时占据画布边缘。
/// 配置快捷键只在无文本输入、无活动控件、无录制且无弹窗时处理，并且每帧至多一个。
/// 调色盘按项目偏好增量应用，皮肤版本变化时仅刷新依赖皮肤的活动选择。
///
/// 绘制顺序分为三层：
/// - 顶部状态工具决定鼠标在画布上的编辑语义；
/// - 中部独立按钮控制调色盘、磁吸、时序、分拍线和音效工具；
/// - 底部按钮显示播放、倍速、轨道数和分拍数等高频状态。
/// 可见性设置只移除对应控件，不保留空位；底部组单独测量并贴齐窗口底边。
///
/// 工具栏本体不承担配置文件读写或谱面直接修改。配置变化交给应用服务，
/// 谱面和运行时变化以编辑命令投递。唯一的直接运行时开关是同主音频画布
/// 同步状态，它由 EditorEngine 提供专用接口且不属于 EditorConfig。
///
/// 锚定弹层在工具栏窗口结束后绘制。各入口保存按钮屏幕 Y 坐标，各弹层
/// 使用主视口、上帧尺寸与右上角 Pivot 完成边界夹取。窗口 ID 均为稳定隐藏
/// 标识，显示数值、语言和图标变化不会丢失 ImGui 状态。
///
/// 快捷键采用固定优先级链。若用户把多个动作绑定到同一组合键，只执行链中
/// 第一项，避免从同一个 editorCfg 快照提交互相覆盖的多个完整配置。文本输入、
/// 快捷键录制、活动控件和任何弹窗都会屏蔽该链，防止编辑操作误触全局开关。
///
/// 样式栈约定：
/// - 工具栏窗口压入四项基础样式和固定按钮样式；
/// - 窗口自身再压入三项窗口样式；
/// - 每个按钮恰好压入三项状态颜色并在本分支恢复；
/// - 图标字体只在皮肤提供 pure_icons 时压入；
/// - 各独立弹层拥有自己的样式作用域，不继承工具栏覆盖。
/// 任一可见性组合都必须维持这些 Push/Pop 成对关系。
///
/// 状态读取约定：
/// - EditorConfig 在工具栏窗口开始时取得一次只读快照；
/// - AudioManager 和应用服务运行态在对应控件绘制前读取；
/// - 谱面元数据只在需要倍速或轨道数控件时持短作用域会话锁；
/// - 所有修改均生成新配置或命令，不原地修改该只读快照。
/// 这保证同一帧所有按钮显示基于一致起点，下一帧再汇合已应用变化。
/// @warning UI 热路径：每帧执行；不得引入文件操作、阻塞等待或无条件资源遍历。
void ToolbarView::update(UIManager* sourceManager)
{
    // 应用服务观察指针每帧同步，UIManager 为空时安全回退只读配置路径。
    setEditorApplicationService(
        sourceManager ? sourceManager->getEditorApplicationService() : nullptr);
    // 皮肤字体和窗口内容缩放在本帧入口各读取一次。
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();

    // 应用服务是当前工具状态权威来源，避免工具栏保留旧选择。
    if ( m_editorApplicationService ) {
        m_currentTool = m_editorApplicationService->currentTool();
    }
    // 布局浮层只允许与 Layout 工具共同存在。
    if ( m_currentTool != Logic::EditTool::Layout ) {
        m_showLayoutPopup = false;
    }

    // 可见性和美学设置使用同一编辑器设置引用，贯穿本帧绘制。
    //
    // toolbarVisibility 把控件分为状态工具和独立按钮，两组各自保持配置顺序。
    // 隐藏某项只影响本帧布局，不关闭其对应编辑器功能；若某个已打开弹层入口
    // 被设置隐藏，入口互斥与后续渲染函数仍会根据成员状态处理现有窗口。
    // aesthetics 引用仅用于读取尺寸，不在工具栏内修改主题配置。
    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    auto& aesthetics     = editorSettings.aesthetics;
    const auto& toolbarVisibility   = editorSettings.toolbarVisibility;
    const auto& stateToolVisibility = toolbarVisibility.stateTools;
    const auto& independentButtonVisibility =
        toolbarVisibility.independentButtons;
    // 标签开关改变按钮高度，固定窗口开关改变停靠与标题栏行为。
    const bool showToolLabels  = editorSettings.showToolLabels;
    const bool fixedToolWindow = editorSettings.fixedToolWindow;

    // 按钮被配置隐藏时同步关闭其浮层，防止出现无锚点窗口。
    if ( !stateToolVisibility.layout ) m_showLayoutPopup = false;
    if ( !independentButtonVisibility.notePalette ) m_showColorPopup = false;
    if ( !independentButtonVisibility.magnet ) m_showMagnetPopup = false;
    if ( !independentButtonVisibility.beatLineDisplay ) {
        m_showBeatLinePopup = false;
    }
    if ( !independentButtonVisibility.soundEffectTool ) {
        m_showSoundEffectTool = false;
    }
    if ( !independentButtonVisibility.playbackSpeed ) {
        m_showSpeedPopup = false;
    }
    if ( !independentButtonVisibility.trackCount ) m_showKeyPopup = false;
    if ( !independentButtonVisibility.beatDivisor ) {
        m_showDivisorPopup = false;
    }

    // 所有窗口与按钮尺寸在屏幕像素域按 DPI 取整。
    float windowPadding = std::floor(aesthetics.windowPadding * dpiScale);

    // 工具栏内容列固定为 32 逻辑像素，窗口宽度额外包含左右内边距。
    //
    // toolbarBaseW 保留未取整的 DPI 乘积供按钮尺寸使用，fixedW 则为窗口约束
    // 的像素宽度。窗口总宽加入两侧主题内边距，保证内容列不因样式变化缩窄。
    // 短标签模式只增加高度，不扩大宽度，以维持固定工具停靠栏布局。
    float fixedBaseW   = 32.0f;
    float toolbarBaseW = fixedBaseW * dpiScale;
    float fixedW       = std::floor(fixedBaseW * dpiScale);
    float btnSize      = toolbarBaseW;
    // 显示短标签时增高按钮，否则保持方形。
    float btnHeight   = showToolLabels ? std::floor(46.0f * dpiScale) : btnSize;
    float totalFixedW = fixedW + 2.0f * windowPadding;

    // 最小和最大宽度相同，只允许窗口纵向随内容变化。
    ImGui::SetNextWindowSizeConstraints(ImVec2(totalFixedW, -1),
                                        ImVec2(totalFixedW, -1));

    // 工具栏禁止滚动、键盘导航和用户缩放，内容按可见按钮确定高度。
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize;
    if ( fixedToolWindow ) {
        // 固定模式移除标题、移动与停靠，使工具栏保持预设位置。
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoDocking;
    } else if ( ImGuiID toolDockId = MainDockSpaceUI::getToolDockId();
                toolDockId != 0 ) {
        // 非固定模式持续绑定专用工具停靠节点。
        ImGui::SetNextWindowDockID(toolDockId, ImGuiCond_Always);
    }

    float rounding = std::floor(aesthetics.frameRounding * dpiScale);
    // 工具按钮去除间距和边框，形成紧凑垂直工具条。
    //
    // 项目间距随后由 advanceItem 显式插入，因为部分控件隐藏时需要只在实际
    // 相邻项之间留白。固定按钮样式统一处理中英文标签可能带来的主题内边距，
    // 窗口样式与按钮样式分别成对恢复，不能把这些覆盖泄漏到其他 UI 视图。
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    Utils::pushFixedButtonStyleVars();

    /// @brief 压入活动实色或非活动透明按钮三态颜色。
    auto pushBtnStyle = [&](bool active) {
        if ( active ) {
            // 活动按钮三态保持同色，悬浮不会改变开关认知。
            ImVec4 activeCol = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
        } else {
            Utils::UIThemeUtils::pushTransparentButtonStyles();
        }
    };

    // 窗口自身圆角与控件圆角分别取自美学设置。
    float windowRound = std::floor(aesthetics.windowRounding * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(windowPadding, windowPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    // 可见标题为空格，###Toolbar 提供稳定停靠和查找 ID。
    if ( ImGui::Begin(" ###Toolbar", nullptr, flags) ) {
        // 工具栏介绍覆盖当前实际显示区域，隐藏 Dock 标签不提交过期几何。
        // 完整窗口矩形同时涵盖当前设置允许显示的全部工具按钮。
        // 独立弹层不属于工具栏简介，保持在后续绘制阶段之外。
        // 多视口时锚点沿用工具栏窗口自身视口，避免提示跳回主屏。
        if ( auto* window = ImGui::GetCurrentWindow();
             sourceManager && window &&
             (!window->DockIsActive || window->DockTabIsVisible) ) {
            sourceManager->walkthroughSpotlight().reportTarget(
                "editor.toolbar",
                window->Pos,
                { window->Pos.x + window->Size.x,
                  window->Pos.y + window->Size.y },
                window->Viewport);
        }
        bool pushedIconFont = false;
        if ( auto f = skinCfg.getFont("pure_icons") ) {
            // 工具图标优先使用纯图标字体，缺失时保持当前字体栈。
            ImGui::PushFont(f, f->LegacySize);
            pushedIconFont = true;
        }

        // 垂直项目间距由显式游标推进控制，ImGui ItemSpacing 已设为零。
        const float itemSpacing = std::floor(aesthetics.itemSpacing * dpiScale);
        auto&       engine      = Logic::EditorEngine::instance();
        const auto  editorCfg   = currentEditorConfig();
        const auto& shortcutConfig = editorCfg.settings.shortcutConfig;
        // 每帧观察当前模式，使快捷切换知道最近非隐藏模式。
        m_beatLineDisplayModeHistory.observe(
            editorCfg.visual.beatLineDisplayMode);

        /// @brief 给本地化提示追加格式化快捷键。
        auto tooltipWithShortcut =
            [](const char*                    tooltip,
               const Config::ShortcutBinding& binding) -> std::string {
            std::string tooltipText  = tooltip ? tooltip : "";
            std::string shortcutText = ShortcutUtils::formatShortcut(binding);
            if ( !shortcutText.empty() ) {
                // 未绑定快捷键时不追加空括号。
                tooltipText += " (";
                tooltipText += shortcutText;
                tooltipText += ")";
            }
            return tooltipText;
        };

        /// @brief 检测一个配置快捷键并提交调用方定义的配置翻转。
        auto applyConfigToggleShortcut =
            [&](const Config::ShortcutBinding& binding,
                auto                           applyChange) -> bool {
            if ( !ShortcutUtils::isShortcutPressed(binding) ) {
                // 未触发时不复制配置。
                return false;
            }
            // 所有快捷键基于本帧同一原始快照，外层限制每帧只处理一个。
            auto newConfig = editorCfg;
            applyChange(newConfig);
            if ( m_editorApplicationService ) {
                m_editorApplicationService->updateEditorConfig(newConfig);
            }
            return true;
        };

        // 任何文本、录制、活动控件或弹窗上下文都会屏蔽全局切换快捷键。
        //
        // WantTextInput 覆盖文本框和输入法，录制标志覆盖快捷键设置捕获流程，
        // IsAnyItemActive 防止滑条/拖拽期间误触，AnyPopupLevel 则涵盖本视图与
        // 其他窗口弹层。四项共同构成全局快捷键可安全消费的前置条件。
        if ( !ImGui::GetIO().WantTextInput &&
             !ShortcutUtils::isShortcutRecordingActive() &&
             !ImGui::IsAnyItemActive() &&
             !ImGui::IsPopupOpen(
                 nullptr,
                 ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ) {
            // 固定优先级链保证重叠绑定每帧只执行第一项。
            bool handledShortcut = false;
            /// @brief 尝试尚未处理的下一项快捷键。
            auto tryToggleShortcut = [&](const Config::ShortcutBinding& binding,
                                         auto applyChange) {
                if ( handledShortcut ) {
                    // 前一项已消费后立即跳过余下候选。
                    return;
                }
                handledShortcut =
                    applyConfigToggleShortcut(binding, applyChange);
            };
            // 反向滚动优先级最高，保证导航方向切换不会与吸附选项同时发生。
            tryToggleShortcut(shortcutConfig.toggleReverseScroll,
                              [](Config::EditorConfig& config) {
                                  config.settings.reverseScroll =
                                      !config.settings.reverseScroll;
                              });
            // 滚动吸附控制时间游标是否落在分拍网格。
            tryToggleShortcut(shortcutConfig.toggleScrollSnap,
                              [](Config::EditorConfig& config) {
                                  config.settings.scrollSnap =
                                      !config.settings.scrollSnap;
                              });
            // 向下取整吸附只在吸附算法内部改变舍入方向。
            tryToggleShortcut(shortcutConfig.toggleSnapFloor,
                              [](Config::EditorConfig& config) {
                                  config.settings.snapFloor =
                                      !config.settings.snapFloor;
                              });
            // 时序映射开关在视觉配置中保存线性映射的反向语义。
            tryToggleShortcut(shortcutConfig.toggleScrollTimingMapping,
                              [](Config::EditorConfig& config) {
                                  config.visual.enableLinearScrollMapping =
                                      !config.visual.enableLinearScrollMapping;
                              });
            // 分拍线快捷键在隐藏态和最近可见模式之间切换。
            tryToggleShortcut(shortcutConfig.toggleBeatLines,
                              [this](Config::EditorConfig& config) {
                                  config.visual.beatLineDisplayMode =
                                      m_beatLineDisplayModeHistory.toggleTarget(
                                          config.visual.beatLineDisplayMode);
                              });
            // 滚动停止播放是编辑交互偏好，不直接发播放命令。
            tryToggleShortcut(shortcutConfig.toggleStopPlaybackOnScroll,
                              [](Config::EditorConfig& config) {
                                  config.settings.stopPlaybackOnScroll =
                                      !config.settings.stopPlaybackOnScroll;
                              });
            // 击打音开关写入音效配置，由应用层同步到运行时。
            tryToggleShortcut(shortcutConfig.toggleHitSfx,
                              [](Config::EditorConfig& config) {
                                  config.settings.sfxConfig.enableHitSfx =
                                      !config.settings.sfxConfig.enableHitSfx;
                              });
            // 击打视觉特效独立于声音总开关。
            tryToggleShortcut(shortcutConfig.toggleHitEffects,
                              [](Config::EditorConfig& config) {
                                  config.visual.enableHitEffects =
                                      !config.visual.enableHitEffects;
                              });
            // 运行时同步主音频不是 EditorConfig 字段，单独调用引擎接口。
            if ( !handledShortcut &&
                 ShortcutUtils::isShortcutPressed(
                     shortcutConfig.toggleSyncSameMainAudio) ) {
                engine.setSyncSameMainAudioCanvases(
                    !engine.isSyncSameMainAudioCanvasesEnabled());
            }
        }

        /// @brief 按配置的垂直间距推进到下一个工具栏项目。
        auto advanceItem = [&]() {
            if ( itemSpacing > 0.0f ) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + itemSpacing);
            }
        };

        /// @brief 绘制基于 EditorConfig 快照的布尔切换按钮。
        auto drawToggleButton = [&](const char*                    icon,
                                    bool                           active,
                                    const char*                    tooltip,
                                    const char*                    shortLabel,
                                    const Config::ShortcutBinding& binding,
                                    auto applyChange) {
            pushBtnStyle(active);
            ImGui::PushID(tooltip);
            if ( drawIconButton(icon,
                                "##ToolbarToggleButton",
                                shortLabel,
                                btnSize,
                                btnHeight,
                                showToolLabels) ) {
                // 点击从本帧配置快照派生新值，并交给应用服务提交。
                auto newConfig = editorCfg;
                applyChange(newConfig);
                if ( m_editorApplicationService ) {
                    m_editorApplicationService->updateEditorConfig(newConfig);
                }
            }
            ImGui::PopID();
            // 提示统一附加当前快捷绑定。
            std::string tooltipText = tooltipWithShortcut(tooltip, binding);
            drawTooltip(tooltipText.c_str());
            ImGui::PopStyleColor(3);
            advanceItem();
        };

        /// @brief 绘制不存于 EditorConfig 的运行时布尔开关按钮。
        auto drawRuntimeToggleButton =
            [&](const char*                    icon,
                bool                           active,
                const char*                    tooltip,
                const char*                    shortLabel,
                const Config::ShortcutBinding& binding,
                auto                           applyChange) {
                pushBtnStyle(active);
                ImGui::PushID(tooltip);
                if ( drawIconButton(icon,
                                    "##ToolbarRuntimeToggleButton",
                                    shortLabel,
                                    btnSize,
                                    btnHeight,
                                    showToolLabels) ) {
                    // 回调直接接收目标状态，避免再次读取运行时值。
                    applyChange(!active);
                }
                ImGui::PopID();
                std::string tooltipText = tooltipWithShortcut(tooltip, binding);
                drawTooltip(tooltipText.c_str());
                ImGui::PopStyleColor(3);
            };

        // Layout 模式下禁用普通编辑工具，要求先退出布局编辑。
        //
        // 禁用仍绘制所有可见普通工具，使布局不因模式切换而跳动。用户通过
        // Layout 按钮再次点击退出并恢复进入前工具，而不是直接点击被禁用项。
        // 每个工具后调用 advanceItem，只在该项确实可见时产生间距。
        const bool isLayoutEditing = m_currentTool == Logic::EditTool::Layout;
        ImGui::BeginDisabled(isLayoutEditing);
        if ( stateToolVisibility.move ) {
            // 移动工具负责平移视图和直接拖动物件。
            drawToolButton(ICON_MMM_HAND,
                           Logic::EditTool::Move,
                           TR("ui.toolbar.move").data(),
                           btnSize,
                           btnHeight,
                           TR("ui.toolbar.short.move").data(),
                           showToolLabels,
                           sourceManager);
            advanceItem();
        }
        if ( stateToolVisibility.marquee ) {
            // 框选工具进入矩形选择状态机。
            drawToolButton(ICON_MMM_SQUARE_SELECT,
                           Logic::EditTool::Marquee,
                           TR("ui.toolbar.marquee").data(),
                           btnSize,
                           btnHeight,
                           TR("ui.toolbar.short.marquee").data(),
                           showToolLabels,
                           sourceManager);
            advanceItem();
        }
        if ( stateToolVisibility.draw ) {
            // 绘制工具创建和编辑音符几何。
            drawToolButton(ICON_MMM_PEN,
                           Logic::EditTool::Draw,
                           TR("ui.toolbar.draw").data(),
                           btnSize,
                           btnHeight,
                           TR("ui.toolbar.short.draw").data(),
                           showToolLabels,
                           sourceManager);
            advanceItem();
        }
        if ( stateToolVisibility.colorBrush ) {
            // 颜色画笔把当前调色槽应用到命中物件。
            drawToolButton(ICON_MMM_PAINT_BRUSH,
                           Logic::EditTool::ColorBrush,
                           TR("ui.toolbar.color_brush").data(),
                           btnSize,
                           btnHeight,
                           TR("ui.toolbar.short.color_brush").data(),
                           showToolLabels,
                           sourceManager);
            advanceItem();
        }
        if ( stateToolVisibility.colorEraser ) {
            // 颜色橡皮清除对象级覆盖并恢复方案颜色。
            drawToolButton(ICON_MMM_ERASER,
                           Logic::EditTool::ColorEraser,
                           TR("ui.toolbar.color_eraser").data(),
                           btnSize,
                           btnHeight,
                           TR("ui.toolbar.short.color_eraser").data(),
                           showToolLabels,
                           sourceManager);
            advanceItem();
        }
        ImGui::EndDisabled();

        if ( stateToolVisibility.layout ) {
            // Layout 按钮不受自身编辑态禁用，可再次点击退出该模式。
            drawLayoutButton(btnSize, btnHeight, showToolLabels, sourceManager);
            advanceItem();
        }

        // 分隔线只在上下两组均至少有一个可见项时绘制。
        const bool hasVisibleStateTool =
            stateToolVisibility.move || stateToolVisibility.marquee ||
            stateToolVisibility.draw || stateToolVisibility.colorBrush ||
            stateToolVisibility.colorEraser || stateToolVisibility.layout;
        const bool hasVisibleUpperIndependentButton =
            independentButtonVisibility.notePalette ||
            independentButtonVisibility.magnet ||
            independentButtonVisibility.scrollTimingMapping ||
            independentButtonVisibility.beatLineDisplay ||
            independentButtonVisibility.soundEffectTool;
        if ( hasVisibleStateTool && hasVisibleUpperIndependentButton ) {
            // 分隔线水平内缩，不接触工具栏窗口边缘。
            ImVec2 sepPos = ImGui::GetCursorScreenPos();
            float  sepH   = 2.0f * dpiScale;
            ImGui::GetWindowDrawList()->AddLine(
                { sepPos.x + 4.0f * dpiScale, sepPos.y + sepH * 0.5f },
                { sepPos.x + btnSize - 4.0f * dpiScale,
                  sepPos.y + sepH * 0.5f },
                IM_COL32(100, 100, 100, 150),
                1.0f * dpiScale);
            ImGui::Dummy(ImVec2(btnSize, sepH));
            advanceItem();
        }

        // 调色盘首次初始化后，每帧只增量解析项目偏好应用键。
        //
        // 初始化建立皮肤颜色数组；项目偏好解析随后可能替换为软件继承或项目
        // 自定义方案。应用键命中后为常数时间返回，因此这里可处于每帧路径。
        // 顺序必须在色块按钮绘制前完成，保证按钮始终显示当前权威活动颜色。
        if ( !m_colorPaletteInitialized ) initializeColorPalette();
        applyProjectPalettePreference();

        // 调色盘入口的活动态只表示弹层可见，按钮色块本身始终反映当前槽颜色。
        // 打开后关闭所有其他锚定弹层，避免多个窗口竞争工具栏左侧同一空间。
        // 颜色方案编辑不改变当前编辑工具，因此可与 Move、Draw 等模式共存。
        if ( independentButtonVisibility.notePalette ) {
            // 调色盘按钮活动样式对应浮层打开状态。
            pushBtnStyle(m_showColorPopup);
            if ( ::MMM::UI::FeedbackButton("##ToolbarNoteColor",
                                           ImVec2(btnSize, btnHeight)) ) {
                m_showColorPopup = !m_showColorPopup;
                if ( m_showColorPopup ) {
                    // 打开任一独立浮层时关闭其余互斥弹窗。
                    m_showDivisorPopup    = false;
                    m_showKeyPopup        = false;
                    m_showSpeedPopup      = false;
                    m_showBeatLinePopup   = false;
                    m_showMagnetPopup     = false;
                    m_showSoundEffectTool = false;
                }
            }
            {
                // 按钮内容使用当前活动笔记槽颜色绘制自定义色块。
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2      minPos   = ImGui::GetItemRectMin();
                ImVec2      maxPos   = ImGui::GetItemRectMax();
                // 有短标签时缩小色块并上移，否则在方形按钮中居中。
                const float  swatchSize = showToolLabels
                                              ? std::floor(btnSize * 0.62f)
                                              : std::floor(btnSize * 0.72f);
                const ImVec2 swatchMin  = {
                    minPos.x + (btnSize - swatchSize) * 0.5f,
                    minPos.y + (showToolLabels
                                    ? std::floor(5.0f * dpiScale)
                                    : (btnHeight - swatchSize) * 0.5f),
                };
                const ImVec2 swatchMax = { swatchMin.x + swatchSize,
                                           swatchMin.y + swatchSize };
                // 色块填充取当前调色盘活动槽，并绘制主题文本色边框。
                drawList->AddRectFilled(
                    swatchMin,
                    swatchMax,
                    ImGui::ColorConvertFloat4ToU32(toImVec4(
                        m_paletteColors[colorSlotIndex(m_activeColorSlot)])),
                    rounding);
                drawList->AddRect(swatchMin,
                                  swatchMax,
                                  ImGui::GetColorU32(ImGuiCol_Text),
                                  rounding,
                                  0,
                                  std::floor(1.0f * dpiScale));
                if ( showToolLabels ) {
                    // 短标签使用菜单字体贴近按钮底部。
                    if ( ImFont* labelFont = skinCfg.getFont("menu") ) {
                        const char* label =
                            TR("ui.toolbar.short.note_palette").data();
                        const float  labelFontSize = std::floor(std::min(
                            btnSize * 0.38f, ImGui::GetFontSize() * 0.72f));
                        const ImVec2 labelSize     = labelFont->CalcTextSizeA(
                            labelFontSize,
                            std::numeric_limits<float>::max(),
                            0.0f,
                            label);
                        const ImVec2 labelPos = {
                            minPos.x + (btnSize - labelSize.x) * 0.5f,
                            maxPos.y - labelSize.y -
                                std::floor(3.0f * dpiScale),
                        };
                        drawList->AddText(labelFont,
                                          labelFontSize,
                                          labelPos,
                                          ImGui::GetColorU32(ImGuiCol_Text),
                                          label);
                    }
                }
            }
            // 缓存按钮顶部作为调色盘浮层垂直锚点。
            m_lastColorBtnY = ImGui::GetItemRectMin().y;
            drawTooltip(TR("ui.toolbar.note_palette").data());
            ImGui::PopStyleColor(3);
            advanceItem();
        }

        // 磁吸入口本身不是布尔配置开关：其弹层包含多个独立吸附选项，所以按钮
        // 始终使用入口强调样式。实际启用状态由弹层复选框分别表达。
        if ( independentButtonVisibility.magnet ) {
            // 磁吸按钮始终使用强调样式，点击只切换设置浮层。
            pushBtnStyle(true);
            ImGui::PushID("MagnetTool");
            if ( drawIconButton(ICON_MMM_MAGNET,
                                "##ToolbarMagnetTool",
                                TR("ui.toolbar.short.magnet_tool").data(),
                                btnSize,
                                btnHeight,
                                showToolLabels) ) {
                m_showMagnetPopup = !m_showMagnetPopup;
                if ( m_showMagnetPopup ) {
                    // 磁吸浮层打开时关闭其他独立浮层。
                    m_showColorPopup      = false;
                    m_showDivisorPopup    = false;
                    m_showKeyPopup        = false;
                    m_showSpeedPopup      = false;
                    m_showBeatLinePopup   = false;
                    m_showSoundEffectTool = false;
                }
            }
            // 记录本帧按钮 Y 供浮层锚定。
            m_lastMagnetBtnY = ImGui::GetItemRectMin().y;
            ImGui::PopID();
            drawTooltip(TR("ui.toolbar.magnet_tool").data());
            ImGui::PopStyleColor(3);
            advanceItem();
        }

        // 时序映射是单一布尔配置，可直接使用通用切换按钮。图标活动态显示
        // “非线性时序映射已启用”，因此与存储字段 enableLinearScrollMapping
        // 取反；点击闭包同样翻转底层字段。
        if ( independentButtonVisibility.scrollTimingMapping ) {
            // 图标 active 表示非线性滚动时序映射正在生效。
            drawToggleButton(
                ICON_MMM_EYE,
                !editorCfg.visual.enableLinearScrollMapping,
                TR("ui.toolbar.scroll_timing_mapping").data(),
                TR("ui.toolbar.short.scroll_timing_mapping").data(),
                shortcutConfig.toggleScrollTimingMapping,
                [](Config::EditorConfig& config) {
                    config.visual.enableLinearScrollMapping =
                        !config.visual.enableLinearScrollMapping;
                });
        }

        // 分拍线入口打开三态模式弹层。按钮不直接以当前模式着色，因为 Always、
        // NearCursor 和 Hidden 不能用单一布尔 active 准确表达；详细状态留在
        // 单选控件和快捷键提示中。
        if ( independentButtonVisibility.beatLineDisplay ) {
            // 分拍线入口打开模式浮层，具体显示状态由浮层单选控制。
            pushBtnStyle(true);
            ImGui::PushID("BeatLineDisplayMode");
            if ( drawIconButton(ICON_MMM_BARS,
                                "##ToolbarBeatLineDisplayMode",
                                TR("ui.toolbar.short.draw_beat_lines").data(),
                                btnSize,
                                btnHeight,
                                showToolLabels) ) {
                m_showBeatLinePopup = !m_showBeatLinePopup;
                if ( m_showBeatLinePopup ) {
                    // 打开后维持独占锚定浮层。
                    m_showColorPopup      = false;
                    m_showDivisorPopup    = false;
                    m_showKeyPopup        = false;
                    m_showSpeedPopup      = false;
                    m_showMagnetPopup     = false;
                    m_showSoundEffectTool = false;
                }
            }
            // 记录按钮顶部作为分拍线设置浮层锚点。
            m_lastBeatLineBtnY = ImGui::GetItemRectMin().y;
            ImGui::PopID();
            {
                const std::string tooltipText =
                    tooltipWithShortcut(TR("ui.toolbar.draw_beat_lines").data(),
                                        shortcutConfig.toggleBeatLines);
                drawTooltip(tooltipText.c_str());
            }
            ImGui::PopStyleColor(3);
            advanceItem();
        }

        // 音效工具是可滚动的独立混音窗口。重新打开时让两个分类增益草稿从
        // EditorConfig 权威值初始化，避免沿用上次未完成手势的局部数值。
        // 它与 Layout 大弹层互斥，也会关闭所有小型锚定弹层。
        if ( independentButtonVisibility.soundEffectTool ) {
            // 音效工具入口控制独立混音窗口，并与布局设置互斥。
            pushBtnStyle(true);
            ImGui::PushID("SoundEffectTool");
            if ( drawIconButton(ICON_MMM_HIT_SFX,
                                "##ToolbarSoundEffectTool",
                                TR("ui.toolbar.short.key_sound_tool").data(),
                                btnSize,
                                btnHeight,
                                showToolLabels) ) {
                m_showSoundEffectTool = !m_showSoundEffectTool;
                if ( m_showSoundEffectTool ) {
                    // 每次打开重置增益草稿初始化，使其从运行时状态重新同步。
                    m_soundEffectGainDraftInitialized = false;
                    // 强制首次显示复制一次权威配置，防止引擎重启后修订号
                    // 恰好与上次相同而复用旧会话的音效设置。
                    m_soundEffectEditorConfigRevision =
                        std::numeric_limits<std::uint64_t>::max();
                    m_showLayoutPopup   = false;
                    m_showColorPopup    = false;
                    m_showDivisorPopup  = false;
                    m_showKeyPopup      = false;
                    m_showSpeedPopup    = false;
                    m_showBeatLinePopup = false;
                    m_showMagnetPopup   = false;
                }
            }
            // 音效窗口固定宽度，但仍以按钮顶部作为垂直锚点并执行视口夹取。
            m_lastSoundEffectToolBtnY = ImGui::GetItemRectMin().y;
            ImGui::PopID();
            {
                // 音效提示额外说明快捷键是“切换击打音”而非单纯打开工具窗口，
                // 因此使用专用提示模板而不是通用括号拼接。
                std::string tooltipText =
                    TR("ui.toolbar.key_sound_tool").data();
                const auto shortcutText =
                    ShortcutUtils::formatShortcut(shortcutConfig.toggleHitSfx);
                if ( !shortcutText.empty() ) {
                    tooltipText += "\n";
                    tooltipText +=
                        TR("ui.toolbar.key_sound_tool_shortcut_hint").data();
                    tooltipText += " (";
                    tooltipText += shortcutText;
                    tooltipText += ")";
                }
                drawTooltip(tooltipText.c_str());
            }
            ImGui::PopStyleColor(3);
            advanceItem();
        }

        // 底部控件与上方工具组采用不同布局策略：它们应始终贴近窗口底边，
        // 即使用户隐藏了部分中间工具，也不能随内容整体上移。
        // 这里先计算实际可见按钮数量和精确高度，再一次性移动游标，避免
        // 在各按钮分支中重复推导剩余空间而产生一像素级累积误差。
        // 底部组只包含频繁观察的时间和谱面量化状态，布局上与工具入口分离。
        // 播放按钮通过应用服务读取运行态，倍速通过 AudioManager 读取，轨道数
        // 从会话元数据读取，分拍数从 EditorConfig 读取；各自权威来源不同，
        // 点击后也必须回到对应命令或配置入口，不能统一写某个局部缓存。
        const int bottomButtonCount =
            static_cast<int>(independentButtonVisibility.playback) +
            static_cast<int>(independentButtonVisibility.playbackSpeed) +
            static_cast<int>(independentButtonVisibility.trackCount) +
            static_cast<int>(independentButtonVisibility.beatDivisor);
        // 播放按钮可能包含文字，因此使用 btnHeight；其余数值按钮保持方形。
        float bottomButtonsH = 0.0f;
        // 播放按钮图标随运行态在播放与暂停间切换，active 样式对应正在播放。
        // 快捷键和点击最终都分发 CmdSetPlayState，保持状态机入口一致。
        if ( independentButtonVisibility.playback ) {
            bottomButtonsH += btnHeight;
        }
        bottomButtonsH +=
            btnSize *
            static_cast<float>(
                static_cast<int>(independentButtonVisibility.playbackSpeed) +
                static_cast<int>(independentButtonVisibility.trackCount) +
                static_cast<int>(independentButtonVisibility.beatDivisor));
        // 间距只存在于相邻可见项之间，隐藏项不应留下空槽。
        if ( bottomButtonCount > 1 ) {
            bottomButtonsH +=
                itemSpacing * static_cast<float>(bottomButtonCount - 1);
        }
        if ( bottomButtonCount > 0 ) {
            // 只在下方仍有余量时移动游标；窗口过矮时保持自然流式布局，
            // 让 ImGui 裁剪内容，而不是把按钮反向压到已绘制控件之上。
            float bottomStartY = ImGui::GetCursorPosY() +
                                 ImGui::GetContentRegionAvail().y -
                                 bottomButtonsH;
            if ( bottomStartY > ImGui::GetCursorPosY() ) {
                ImGui::SetCursorPosY(bottomStartY);
            }
        }

        // 该计数器只负责在底部组内部插入间距，最后一项后不追加空白。
        int  renderedBottomButtonCount = 0;
        auto advanceBottomButton       = [&]() {
            ++renderedBottomButtonCount;
            if ( renderedBottomButtonCount < bottomButtonCount ) {
                advanceItem();
            }
        };

        if ( independentButtonVisibility.playback ) {
            // 播放态来自应用服务快照；按钮提交命令而不直接操作音频对象，
            // 以保持菜单、快捷键和工具栏共用同一状态转换入口。
            const bool playbackPlaying =
                m_editorApplicationService &&
                m_editorApplicationService->isPlaybackPlaying();
            drawRuntimeToggleButton(
                playbackPlaying ? ICON_MMM_PAUSE : ICON_MMM_PLAY,
                playbackPlaying,
                TR("ui.toolbar.play_pause").data(),
                TR("ui.toolbar.short.play_pause").data(),
                shortcutConfig.togglePlayback,
                [](bool shouldPlay) {
                    MenuUtil::dispatchCommand(
                        Logic::CmdSetPlayState{ shouldPlay });
                });
            advanceBottomButton();
        }

        if ( independentButtonVisibility.playbackSpeed ||
             independentButtonVisibility.trackCount ) {
            // 倍速和轨道数都依赖活动谱面，因此共享一次短作用域会话锁。
            // 锁覆盖两项绘制是既有约束：控件回调只入队命令，不在此同步执行。
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();

            // 无谱面时仍绘制占位按钮以保持底部工具栏布局稳定。
            bool hasBeatmap = session && session->getContext().currentBeatmap;
            int  currentTracks = 4;
            if ( hasBeatmap ) {
                currentTracks =
                    session->getContext()
                        .currentBeatmap->m_baseMapMetadata.track_count;
            }

            if ( !hasBeatmap ) {
                // 禁用整个依赖谱面的控件区，避免占位文本仍能打开空弹层。
                ImGui::BeginDisabled();
            }

            // 倍速按钮在无谱面时显示“--”且由外层 BeginDisabled 禁止交互；
            // 有谱面时数值来自 AudioManager，弹层显示态决定按钮背景。
            if ( independentButtonVisibility.playbackSpeed ) {
                // 活动配色表示倍速弹层已打开，而不是倍速是否为默认值。
                pushBtnStyle(m_showSpeedPopup);
                ImFont* contentFont = skinCfg.getFont("content");
                if ( contentFont ) {
                    ImGui::PushFont(contentFont, contentFont->LegacySize);
                }

                // 显示值直接读取音频运行时，确保异步命令生效后能立即回显。
                const double currentSpeed =
                    Audio::AudioManager::instance().getPlaybackSpeed();
                char speedText[32];
                if ( hasBeatmap ) {
                    // 两位有效数字足以区分预设，同时适配窄方形按钮。
                    snprintf(
                        speedText, sizeof(speedText), "%.2g", currentSpeed);
                } else {
                    snprintf(speedText, sizeof(speedText), "--");
                }

                // 稳定 ID 与显示文本分离，数值变化不会破坏 ImGui 状态。
                if ( drawToolbarScrollingButton("###ToolbarPlaybackSpeed",
                                                speedText,
                                                ImVec2(btnSize, btnSize)) ) {
                    m_showSpeedPopup = !m_showSpeedPopup;
                    if ( m_showSpeedPopup ) {
                        // 数值弹层共享工具栏左侧空间，任一时刻只保留一个。
                        m_showKeyPopup        = false;
                        m_showDivisorPopup    = false;
                        m_showBeatLinePopup   = false;
                        m_showMagnetPopup     = false;
                        m_showSoundEffectTool = false;
                    }
                }
                // 弹层下一阶段以本帧按钮顶部为锚点。
                m_lastSpeedBtnY = ImGui::GetItemRectMin().y;

                if ( hasBeatmap && ImGui::IsItemHovered() ) {
                    // 滚轮调节是可选交互；关闭设置后悬浮仅显示说明。
                    if ( editorCfg.settings
                             .enableToolbarValueWheelAdjustment ) {
                        const float wheel = ImGui::GetIO().MouseWheel;
                        if ( std::abs(wheel) > 0.1F ) {
                            // 先吸附到最近预设再沿滚轮方向移动一档，避免
                            // 外部设置的非预设倍速导致首次滚动方向不直观。
                            constexpr std::array<double, 8> presets{
                                0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0
                            };
                            std::size_t bestIndex = 0;
                            double      minimumDifference =
                                std::abs(currentSpeed - presets.front());
                            for ( std::size_t index = 1; index < presets.size();
                                  ++index ) {
                                // 线性扫描固定八项比维护额外查找结构更直接。
                                const double difference =
                                    std::abs(currentSpeed - presets[index]);
                                if ( difference < minimumDifference ) {
                                    minimumDifference = difference;
                                    bestIndex         = index;
                                }
                            }

                            // 边界档位保持不变，不产生无效命令和反馈音效。
                            if ( wheel > 0.0F &&
                                 bestIndex + 1 < presets.size() ) {
                                ++bestIndex;
                            } else if ( wheel < 0.0F && bestIndex > 0 ) {
                                --bestIndex;
                            }

                            const double newSpeed = presets[bestIndex];
                            if ( std::abs(newSpeed - currentSpeed) > 0.0001 ) {
                                // 命令队列负责跨线程应用；反馈只在值确实变化时播放。
                                engine.pushCommand(
                                    Logic::CmdSetPlaybackSpeed{ newSpeed });
                                ::MMM::UI::PlayInteractionMouseUpFeedback();
                            }
                        }
                    }
                    drawTooltip(TR("ui.toolbar.playback_speed").data());
                }

                if ( contentFont ) ImGui::PopFont();
                ImGui::PopStyleColor(3);
                advanceBottomButton();
            }

            // 轨道数按钮显示谱面元数据的一基 K 标记。外层共享会话快照保证
            // 同帧倍速和轨道控件对“是否存在谱面”判断一致。
            if ( independentButtonVisibility.trackCount ) {
                // 轨道数按钮复用同一会话快照，显示文本包含稳定隐藏 ID。
                pushBtnStyle(m_showKeyPopup);
                ImFont* contentFont = skinCfg.getFont("content");
                if ( contentFont ) {
                    ImGui::PushFont(contentFont, contentFont->LegacySize);
                }

                char keyBuf[64];
                if ( hasBeatmap ) {
                    snprintf(keyBuf,
                             sizeof(keyBuf),
                             "%dK###ToolbarKeyCount",
                             currentTracks);
                } else {
                    snprintf(keyBuf, sizeof(keyBuf), "--###ToolbarKeyCount");
                }

                // 点击只控制弹层；元数据修改由弹层或滚轮提交命令。
                if ( ::MMM::UI::FeedbackButton(keyBuf,
                                               ImVec2(btnSize, btnSize)) ) {
                    m_showKeyPopup = !m_showKeyPopup;
                    if ( m_showKeyPopup ) {
                        // 轨道数弹层与其他锚定弹层互斥。
                        m_showDivisorPopup    = false;
                        m_showSpeedPopup      = false;
                        m_showBeatLinePopup   = false;
                        m_showMagnetPopup     = false;
                        m_showSoundEffectTool = false;
                    }
                }
                // 保存锚点时使用屏幕坐标，后续可跨窗口定位。
                m_lastKeyBtnY = ImGui::GetItemRectMin().y;

                if ( hasBeatmap && ImGui::IsItemHovered() ) {
                    // 滚轮每格调整一轨，范围与元数据编辑页保持一致。
                    if ( editorCfg.settings
                             .enableToolbarValueWheelAdjustment ) {
                        const float wheel = ImGui::GetIO().MouseWheel;
                        if ( std::abs(wheel) > 0.1F ) {
                            const int newTracks = std::clamp(
                                currentTracks + (wheel > 0.0F ? 1 : -1), 1, 32);
                            if ( newTracks != currentTracks ) {
                                // 复制完整元数据，只替换轨道数，避免丢失其他字段。
                                auto metadata =
                                    session->getContext()
                                        .currentBeatmap->m_baseMapMetadata;
                                metadata.track_count = newTracks;
                                engine.pushCommand(
                                    Logic::CmdUpdateBeatmapMetadata{
                                        metadata });
                                ::MMM::UI::PlayInteractionMouseUpFeedback();
                            }
                        }
                    }
                    drawTooltip(TR("ui.settings.beatmap.tracks").data());
                }

                if ( contentFont ) ImGui::PopFont();
                ImGui::PopStyleColor(3);
                advanceBottomButton();
            }

            if ( !hasBeatmap ) {
                ImGui::EndDisabled();
            }
        }

        // 分拍数即使没有活动谱面也可编辑，因为它是软件级编辑器偏好，会成为
        // 后续谱面画布的吸附基准。按钮值和弹层锚点均来自本帧配置快照。
        if ( independentButtonVisibility.beatDivisor ) {
            // 分拍数属于编辑器配置，不依赖活动谱面，因此始终可调整。
            int currentDivisor = editorCfg.settings.beatDivisor;
            pushBtnStyle(m_showDivisorPopup);
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) {
                ImGui::PushFont(contentFont, contentFont->LegacySize);
            }
            // 可见数字与固定 ID 分离，改变分拍不会重建控件状态。
            char divisorBuf[64];
            snprintf(divisorBuf,
                     sizeof(divisorBuf),
                     "%d###ToolbarBeatDivisor",
                     currentDivisor);
            if ( ::MMM::UI::FeedbackButton(divisorBuf,
                                           ImVec2(btnSize, btnSize)) ) {
                m_showDivisorPopup = !m_showDivisorPopup;
                if ( m_showDivisorPopup ) {
                    // 打开分拍弹层时关闭其他数值和工具弹层。
                    m_showKeyPopup        = false;
                    m_showSpeedPopup      = false;
                    m_showBeatLinePopup   = false;
                    m_showMagnetPopup     = false;
                    m_showSoundEffectTool = false;
                }
            }
            // 历史成员名 m_lastBtnY 专用于分拍弹层锚点。
            m_lastBtnY = ImGui::GetItemRectMin().y;
            if ( ImGui::IsItemHovered() ) {
                if ( editorCfg.settings.enableToolbarValueWheelAdjustment ) {
                    const auto& io    = ImGui::GetIO();
                    const float wheel = io.MouseWheel;
                    if ( std::abs(wheel) > 0.1F ) {
                        // Shift 使用用户配置的滚动倍率，普通滚轮只移动一格。
                        int step = wheel > 0.0F ? 1 : -1;
                        if ( io.KeyShift ) {
                            step *= static_cast<int>(
                                editorCfg.settings.scrollSpeedMultiplier);
                        }
                        // 1..64 与弹层滑条范围保持一致。
                        const int newDivisor =
                            std::clamp(currentDivisor + step, 1, 64);
                        if ( newDivisor != currentDivisor ) {
                            // 值语义副本经统一入口应用并持久化。
                            auto newConfig                 = editorCfg;
                            newConfig.settings.beatDivisor = newDivisor;
                            updateEditorConfig(newConfig);
                            ::MMM::UI::PlayInteractionMouseUpFeedback();
                        }
                    }
                }
                drawTooltip(TR("ui.toolbar.beat_divisor").data());
            }
            if ( contentFont ) ImGui::PopFont();
            ImGui::PopStyleColor(3);
            advanceBottomButton();
        }

        // 字体栈只在图标字体实际存在时恢复。
        if ( pushedIconFont ) ImGui::PopFont();
    }
    ImGui::End();
    // 与前面的 PushStyleVar/固定按钮样式严格成对恢复，防止污染其他窗口。
    ImGui::PopStyleVar(3);  // 窗口内边距、窗口边框大小、窗口圆角
    Utils::popFixedButtonStyleVars();
    ImGui::PopStyleVar(4);

    // 主窗口结束后再绘制独立弹层，避免它们继承工具栏裁剪区和滚动设置。
    // 每个渲染函数自行检查显示状态，因此调用顺序只决定窗口层叠顺序。
    renderColorPalettePopup(dpiScale);
    renderPaletteExportFileDialog(dpiScale);
    renderPaletteImportFileDialog(dpiScale);
    renderLayoutPopup(dpiScale, sourceManager);
    renderMagnetPopup(dpiScale);
    renderBeatLinePopup(dpiScale);
    renderSoundEffectTool(dpiScale);

    // --- 绘制分拍数量设置悬浮窗 ---
    //
    // 该弹层提供连续滑条和皮肤可配置预设。所有入口更新同一
    // EditorConfig 字段，范围统一为 1..64。位置缓存与样式栈仅属于此弹层，
    // 不与倍速或轨道数弹层共享，以免内容变化造成相互跳动。
    //
    // 定位协议：
    // - m_lastBtnY 是分拍按钮本帧的屏幕顶部；
    // - m_popupWidth/Height 是上一帧自动尺寸结果；
    // - targetX 代表弹层右边缘，因此左边界检查需要加完整宽度；
    // - targetY 同时夹取顶部留白和底部减窗口高度；
    // - 首帧估计只影响一次定位，实际尺寸会在窗口内容绘制后回填。
    //
    // 滑条适合任意合法值，预设列表只提供皮肤认为常用的分母。两种交互都
    // 从本帧 editorCfg 复制完整配置，修改单字段后提交；下帧重新读取结果。
    if ( m_showDivisorPopup ) {
        // 在 Toolbar 窗口左侧显示悬浮窗
        // 工具栏存在是本函数正常绘制的前置条件，由主窗口同帧建立。
        ImVec2 toolbarPos = ImGui::FindWindowByName(" ###Toolbar")->Pos;

        // 所有边界计算限定在主视口，避免多视口模式下弹层跨屏漂移。
        ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        float          viewportTop  = mainViewport->Pos.y;
        float viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
        float viewportLeft   = mainViewport->Pos.x;

        // 横向位置 = 工具栏左边缘往左 4px
        // 纵向位置 = 按钮的顶部对齐
        float targetX = toolbarPos.x - std::floor(4.0f * dpiScale);
        float targetY = m_lastBtnY;

        // 灵活微调 Y 和 X 的起始坐标，确保弹出菜单不会溢出视口边界而被截断
        // 首帧使用保守估计，之后复用上一帧真实窗口尺寸。
        float popupW =
            m_popupWidth > 0.0f ? m_popupWidth : std::floor(160.0f * dpiScale);
        float popupH  = m_popupHeight > 0.0f ? m_popupHeight
                                             : std::floor(120.0f * dpiScale);
        float padding = std::floor(8.0f * dpiScale);

        // 限制 X 以免溢出左侧边界
        targetX = std::max(targetX, viewportLeft + popupW + padding);
        // 限制 Y 以免溢出底部与顶部边界
        targetY = std::min(targetY, viewportBottom - popupH - padding);
        targetY = std::max(targetY, viewportTop + padding);

        ImVec2 popupPos = ImVec2(targetX, targetY);

        // 枢轴点 (1.0, 0.0) 代表将弹窗的右上角对齐到 popupPos
        ImGui::SetNextWindowViewport(mainViewport->ID);
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));

        // 弹层位置完全由锚点控制，不写入 imgui.ini。
        ImGuiWindowFlags popupFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;

        // 主题尺寸在使用前按 DPI 取整，保持像素边界清晰。
        auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        float winPadding    = std::floor(aesthetics.windowPadding * dpiScale);
        float winRounding   = std::floor(aesthetics.windowRounding * dpiScale);
        float frameRounding = std::floor(aesthetics.frameRounding * dpiScale);
        float itemSpacing   = std::floor(aesthetics.itemSpacing * dpiScale);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, winRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(winPadding, winPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(itemSpacing, itemSpacing));

        if ( ImGui::Begin("##BeatDivisorPopup", nullptr, popupFlags) ) {
            // 每帧重取配置，保证设置页或快捷键修改能同步反映。
            auto editorCfg      = currentEditorConfig();
            int  currentDivisor = editorCfg.settings.beatDivisor;

            ImGui::TextUnformatted(TR("ui.toolbar.beat_divisor").data());
            ImGui::Separator();

            ImGui::SetNextItemWidth(std::floor(120.0f * dpiScale));
            if ( ::MMM::UI::FeedbackSliderInt(
                     "##DivisorSlider", &currentDivisor, 1, 64) ) {
                // 滑条变化通过统一配置入口立即应用。
                auto newConfig                 = editorCfg;
                newConfig.settings.beatDivisor = currentDivisor;
                updateEditorConfig(newConfig);
            }
            if ( ImGui::IsItemHovered() ) {
                // 分拍算法说明较长，固定向右展开以避开工具栏。
                Utils::renderTooltip(
                    TR("ui.settings.editor.beat_divisor_tooltip").data(),
                    Utils::TooltipDir::Right);
            }

            // 可以加一些常用的快速设置按钮
            // 预设来自皮肤配置，允许主题定制常用分拍集合。
            const auto& commonDivisors =
                Config::SkinManager::instance().getCommonDivisors();
            float presetTextWidth = 0.0f;
            for ( const int divisor : commonDivisors ) {
                // 先测量最长文本，保证同一网格中按钮宽度一致。
                char previewBuf[32];
                snprintf(previewBuf, sizeof(previewBuf), "1/%d", divisor);
                presetTextWidth = std::max(presetTextWidth,
                                           ImGui::CalcTextSize(previewBuf).x);
            }
            const ImGuiStyle& popupStyle = ImGui::GetStyle();
            // 限制横向内边距，避免四列预设在小 DPI 窗口中溢出。
            const float compactPaddingX = std::min(popupStyle.FramePadding.x,
                                                   std::floor(4.0f * dpiScale));
            const float presetButtonWidth =
                std::ceil(std::max(std::floor(40.0f * dpiScale),
                                   presetTextWidth + compactPaddingX * 2.0f +
                                       std::floor(2.0f * dpiScale)));
            const float presetButtonHeight = std::floor(24.0f * dpiScale);
            ImGui::PushStyleVar(
                ImGuiStyleVar_FramePadding,
                ImVec2(compactPaddingX, popupStyle.FramePadding.y));
            for ( size_t i = 0; i < commonDivisors.size(); ++i ) {
                // 每四项换行，索引加入 ID 以允许重复显示文本。
                if ( i > 0 && i % 4 != 0 ) ImGui::SameLine();
                char buf[64];
                snprintf(buf,
                         sizeof(buf),
                         "1/%d##ToolbarDivisorPreset%zu",
                         commonDivisors[i],
                         i);
                if ( ::MMM::UI::FeedbackButton(
                         buf, ImVec2(presetButtonWidth, presetButtonHeight)) ) {
                    // 预设和滑条使用完全相同的配置更新路径。
                    auto newConfig                 = editorCfg;
                    newConfig.settings.beatDivisor = commonDivisors[i];
                    updateEditorConfig(newConfig);
                }
            }
            ImGui::PopStyleVar();

            // 实时获取并记录当前帧计算出的真实尺寸，供下一帧定位计算参考，防止视口越界截断
            ImVec2 sz     = ImGui::GetWindowSize();
            m_popupWidth  = sz.x;
            m_popupHeight = sz.y;
        }
        ImGui::End();

        // 恢复弹层专用外观。
        ImGui::PopStyleVar(4);
    }

    // --- 绘制主音轨倍速设置悬浮窗 ---
    //
    // 倍速是音频运行时状态，而非此处直接维护的局部字段。弹层读取当前值，
    // 再通过 CmdSetPlaybackSpeed 请求逻辑线程更新。活动谱面消失时关闭弹层，
    // 防止对已释放播放上下文继续发送控制命令。
    //
    // 倍速范围固定为 0.25..2.0，与底部按钮滚轮预设边界一致。滑条允许范围
    // 内任意小数，快捷按钮提供常用减速值。applyPopupSpeed 再次夹取所有输入，
    // 使将来增加其他入口时也无法向音频层提交越界倍速。
    //
    // 会话锁覆盖当前弹层绘制是既有同步约束；控件回调只投递命令，不等待
    // 音频线程完成。显示值每帧从 AudioManager 读取，不假设命令已同步执行。
    if ( m_showSpeedPopup ) {
        // 倍速弹层沿用分拍弹层的锚定与视口夹取策略。
        ImVec2 toolbarPos = ImGui::FindWindowByName(" ###Toolbar")->Pos;

        ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        float          viewportTop  = mainViewport->Pos.y;
        float viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
        float viewportLeft   = mainViewport->Pos.x;

        float targetX = toolbarPos.x - std::floor(4.0f * dpiScale);
        float targetY = m_lastSpeedBtnY;

        // 独立缓存尺寸，避免不同弹层内容宽高互相干扰。
        float popupW = m_speedPopupWidth > 0.0f ? m_speedPopupWidth
                                                : std::floor(160.0f * dpiScale);
        float popupH = m_speedPopupHeight > 0.0f
                           ? m_speedPopupHeight
                           : std::floor(120.0f * dpiScale);
        float padding = std::floor(8.0f * dpiScale);

        targetX = std::max(targetX, viewportLeft + popupW + padding);
        targetY = std::min(targetY, viewportBottom - popupH - padding);
        targetY = std::max(targetY, viewportTop + padding);

        ImVec2 popupPos = ImVec2(targetX, targetY);

        ImGui::SetNextWindowViewport(mainViewport->ID);
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));

        ImGuiWindowFlags popupFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;

        auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        float winPadding    = std::floor(aesthetics.windowPadding * dpiScale);
        float winRounding   = std::floor(aesthetics.windowRounding * dpiScale);
        float frameRounding = std::floor(aesthetics.frameRounding * dpiScale);
        float itemSpacing   = std::floor(aesthetics.itemSpacing * dpiScale);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, winRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(winPadding, winPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(itemSpacing, itemSpacing));

        if ( ImGui::Begin("##PlaybackSpeedPopup", nullptr, popupFlags) ) {
            // 谱面指针只在持锁期间访问；命令提交不会直接递归修改对象。
            auto& engine = Logic::EditorEngine::instance();
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();

            if ( session && session->getContext().currentBeatmap ) {
                // 所有入口统一夹取合法倍速，防止预设与滑条范围漂移。
                /// @brief 夹取弹层输入并投递统一倍速命令。
                auto applyPopupSpeed = [&engine](double speed) {
                    engine.pushCommand(Logic::CmdSetPlaybackSpeed{
                        std::clamp(speed, 0.25, 2.0) });
                };

                // 音频运行时值作为权威显示来源。
                float currentSpeed = static_cast<float>(std::clamp(
                    Audio::AudioManager::instance().getPlaybackSpeed(),
                    0.25,
                    2.0));

                ImGui::TextUnformatted(TR("ui.toolbar.playback_speed").data());
                ImGui::Separator();

                ImGui::SetNextItemWidth(std::floor(140.0f * dpiScale));
                if ( ::MMM::UI::FeedbackSliderFloat(
                         "##PlaybackSpeedSlider",
                         &currentSpeed,
                         0.25f,
                         2.0f,
                         "%.4fx",
                         ImGuiSliderFlags_AlwaysClamp) ) {
                    // 滑条拖动立即进入命令队列，提供连续听感反馈。
                    applyPopupSpeed(static_cast<double>(currentSpeed));
                }

                // 常用减速档以二列布局呈现，正常速度作为最后一项。
                constexpr std::array<double, 4> presets = {
                    0.25, 0.5, 0.75, 1.0
                };
                const float presetButtonH = std::floor(26.0f * dpiScale);
                const float presetButtonW =
                    std::max(std::floor(64.0f * dpiScale),
                             ImGui::CalcTextSize("0.75x").x +
                                 ImGui::GetStyle().FramePadding.x * 2.0f);
                for ( size_t i = 0; i < presets.size(); ++i ) {
                    // 奇数索引与前一项同行，形成稳定的两列网格。
                    if ( i > 0 && i % 2 != 0 ) ImGui::SameLine();
                    char buf[64];
                    snprintf(buf,
                             sizeof(buf),
                             "%.2gx##ToolbarSpeedPreset%zu",
                             presets[i],
                             i);
                    if ( ::MMM::UI::FeedbackButton(
                             buf, ImVec2(presetButtonW, presetButtonH)) ) {
                        applyPopupSpeed(presets[i]);
                    }
                }
            } else {
                // 谱面在弹层打开期间关闭时立即收起，避免空控制器残留。
                m_showSpeedPopup = false;
            }

            ImVec2 sz          = ImGui::GetWindowSize();
            m_speedPopupWidth  = sz.x;
            m_speedPopupHeight = sz.y;
        }
        ImGui::End();

        ImGui::PopStyleVar(4);
    }

    // --- 绘制Key数量设置悬浮窗 ---
    //
    // 轨道数属于谱面元数据，修改时复制完整 BaseMapMetadata 并投递更新命令。
    // 弹层不保留元数据引用，活动谱面在帧间关闭时会自动收起。滑条覆盖全部
    // 合法范围，快捷按钮只提供常见键数，不限制其他值。
    //
    // 轨道数变化可能触发布局和谱面数据适配，全部交给 CmdUpdateBeatmapMetadata
    // 的逻辑处理；UI 不直接遍历或迁移音符。预设按钮与滑条使用相同元数据
    // 副本，其他字段始终原样保留。
    //
    // 固定按钮样式只包围常用键数行，并在所有分支结束前恢复。弹层尺寸缓存
    // 独立于分拍与倍速，避免三种内容高度在切换时互相污染锚点计算。
    if ( m_showKeyPopup ) {
        // 轨道数弹层仅在活动谱面存在时有可编辑目标。
        ImVec2 toolbarPos = ImGui::FindWindowByName(" ###Toolbar")->Pos;

        ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        float          viewportTop  = mainViewport->Pos.y;
        float viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
        float viewportLeft   = mainViewport->Pos.x;

        // 横向位置 = 工具栏左边缘往左 4px
        // 纵向位置 = 按钮的顶部对齐
        float targetX = toolbarPos.x - std::floor(4.0f * dpiScale);
        float targetY = m_lastKeyBtnY;

        // 使用独立上帧尺寸完成视口边缘修正。
        float popupW  = m_keyPopupWidth > 0.0f ? m_keyPopupWidth
                                               : std::floor(160.0f * dpiScale);
        float popupH  = m_keyPopupHeight > 0.0f ? m_keyPopupHeight
                                                : std::floor(120.0f * dpiScale);
        float padding = std::floor(8.0f * dpiScale);

        targetX = std::max(targetX, viewportLeft + popupW + padding);
        targetY = std::min(targetY, viewportBottom - popupH - padding);
        targetY = std::max(targetY, viewportTop + padding);

        ImVec2 popupPos = ImVec2(targetX, targetY);

        ImGui::SetNextWindowViewport(mainViewport->ID);
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));

        ImGuiWindowFlags popupFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;

        auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        float winPadding    = std::floor(aesthetics.windowPadding * dpiScale);
        float winRounding   = std::floor(aesthetics.windowRounding * dpiScale);
        float frameRounding = std::floor(aesthetics.frameRounding * dpiScale);
        float itemSpacing   = std::floor(aesthetics.itemSpacing * dpiScale);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, winRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(winPadding, winPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(itemSpacing, itemSpacing));

        if ( ImGui::Begin("##KeyCountPopup", nullptr, popupFlags) ) {
            // 元数据副本在控件确认后通过命令替换，UI 不直接改谱面实体。
            auto& engine = Logic::EditorEngine::instance();
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();

            if ( session && session->getContext().currentBeatmap ) {
                // 保留除 track_count 之外的全部谱面基础元数据。
                auto meta =
                    session->getContext().currentBeatmap->m_baseMapMetadata;
                int currentTracks = meta.track_count;

                ImGui::TextUnformatted(TR("ui.settings.beatmap.tracks").data());
                ImGui::Separator();

                ImGui::SetNextItemWidth(std::floor(120.0f * dpiScale));
                if ( ::MMM::UI::FeedbackSliderInt(
                         "##TracksSlider", &currentTracks, 1, 32) ) {
                    // 滑条与工具栏滚轮共享 1..32 边界。
                    meta.track_count = currentTracks;
                    engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ meta });
                }

                // 常用 Key 数快速设置按钮
                const std::vector<int> commonKeys = { 4, 5, 6, 7, 8 };
                // 固定尺寸快捷按钮不继承主题内容内边距，避免 K
                // 标签被挤压或裁切。
                Utils::pushFixedButtonStyleVars();
                for ( size_t i = 0; i < commonKeys.size(); ++i ) {
                    // 预设横向排列，索引后缀保证各按钮 ID 唯一。
                    if ( i > 0 ) ImGui::SameLine();
                    char buf[64];
                    snprintf(buf,
                             sizeof(buf),
                             "%dK##ToolbarKeyPreset%zu",
                             commonKeys[i],
                             i);
                    if ( ::MMM::UI::FeedbackButton(
                             buf,
                             ImVec2(std::floor(28.0f * dpiScale),
                                    std::floor(24.0f * dpiScale))) ) {
                        // 预设只覆盖轨道数字段，保留标题、作者和 BGM 轨道信息。
                        meta.track_count = commonKeys[i];
                        engine.pushCommand(
                            Logic::CmdUpdateBeatmapMetadata{ meta });
                    }
                }
                Utils::popFixedButtonStyleVars();
            } else {
                // 谱面生命周期变化后主动关闭失效弹层。
                m_showKeyPopup = false;
            }

            // 自动尺寸结果供下一帧锚定夹取使用。
            ImVec2 sz        = ImGui::GetWindowSize();
            m_keyPopupWidth  = sz.x;
            m_keyPopupHeight = sz.y;
        }
        ImGui::End();

        ImGui::PopStyleVar(4);
    }
}

/// @brief 绘制锚定在工具栏按钮旁的音效逐轨与分类控制弹层。
/// @param dpiScale 当前 DPI 缩放。
///
/// 弹层把音效控制分成四个层级：全部击打音、绑定与未绑定击打音、
/// 玩家/草稿/BGM 区域总开关，以及各区域内的逐轨增益。全局增益直接
/// 交给 AudioManager，编辑器语义配置通过 EditorApplicationService 更新，
/// 逐轨运行时状态则投递编辑命令，避免 UI 持有音频资源的生命周期。
///
/// 行模型预先把标题、总控和轨道映射为连续索引，只遍历当前滚动视口
/// 覆盖的行。即使谱面包含大量草稿或 BGM 轨道，每帧创建的 ImGui 控件数
/// 仍由弹层高度限制。占位行保留区域结构，使无谱面和零轨道状态不会导致
/// 后续区域索引变化。
///
/// 增益滑条区分即时试听和持久化：拖动时把值应用到运行时，控件失活后
/// 才写配置。这样既能连续试听，也避免每个鼠标采样点触发磁盘保存。
/// @warning UI 热路径：弹层打开时每帧执行，仅绘制滚动区可见控制行。
void ToolbarView::renderSoundEffectTool(float dpiScale)
{
    // 显示标志是最便宜的快速路径，关闭时不查询会话或音频状态。
    if ( !m_showSoundEffectTool ) return;

    // 弹层必须锚定到同帧工具栏；工具栏未建立时等待下一帧重试。
    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    if ( !toolbarWindow ) return;

    // 轨道数只决定弹层行布局；逻辑线程持有会话锁更新时沿用上次布局，
    // 不能让每帧 UI 等待完整的 BeatmapSession::update。
    auto& engine = Logic::EditorEngine::instance();
    // 活动索引是原子快照；先处理标签切换，避免抢锁失败时继续显示
    // 上一张谱面的轨道控制行。
    const int activeSessionIndex = engine.getActiveSessionIndex();
    if ( activeSessionIndex != m_soundEffectTrackSessionIndex ) {
        // 新标签不能短暂显示前一个谱面的混音轨道。
        m_soundEffectTrackSessionIndex = activeSessionIndex;
        m_soundEffectHasBeatmap        = false;
        m_soundEffectPlayerTrackCount  = 0;
        m_soundEffectDraftTrackCount   = 0;
        m_soundEffectBgmTrackCount     = 0;
    }
    {
        // try_lock 失败仅延后一帧刷新，不阻塞主线程；成功后借用 SessionEntry，
        // 避免每帧复制 BeatmapSession 的共享所有权。
        // 高 UPS 时也不重试抢锁，否则会变相等待整个会话更新。
        std::unique_lock<std::recursive_mutex> sessionLock(
            engine.getSessionMutex(), std::try_to_lock);
        if ( sessionLock.owns_lock() ) {
            // 活动标签可能恰好在 try_lock 前切换；锁内复核并清空旧布局。
            const int lockedActiveIndex = engine.getActiveSessionIndex();
            if ( lockedActiveIndex != m_soundEffectTrackSessionIndex ) {
                m_soundEffectTrackSessionIndex = lockedActiveIndex;
                m_soundEffectHasBeatmap        = false;
                m_soundEffectPlayerTrackCount  = 0;
                m_soundEffectDraftTrackCount   = 0;
                m_soundEffectBgmTrackCount     = 0;
            }
            // getSessionEntry 的内部递归加锁在当前线程立即成功；外层锁
            // 继续保护返回指针和 SessionContext 的读取，避免标签关闭时悬空。
            const auto* entry = engine.getSessionEntry(lockedActiveIndex);
            m_soundEffectHasBeatmap =
                entry && entry->session &&
                entry->session->getContext().currentBeatmap;
            if ( m_soundEffectHasBeatmap ) {
                // 元数据轨道数和草稿轨道数来源不同，读取后规范为非负值。
                const auto& context = entry->session->getContext();
                const auto& metadata =
                    context.currentBeatmap->m_baseMapMetadata;
                m_soundEffectPlayerTrackCount =
                    std::max(0, metadata.track_count);
                m_soundEffectDraftTrackCount =
                    std::max(0, context.draftTrackCount);
                m_soundEffectBgmTrackCount =
                    std::max(0, metadata.bgm_track_count);
            } else {
                m_soundEffectPlayerTrackCount = 0;
                m_soundEffectDraftTrackCount  = 0;
                m_soundEffectBgmTrackCount    = 0;
            }
        }
    }
    // 离开锁后只消费缓存标量，不借用会话或谱面对象。
    // 轨道数变化在下一次成功读取时更新，不影响本帧控件交互。
    const bool hasBeatmap       = m_soundEffectHasBeatmap;
    const int  playerTrackCount = m_soundEffectPlayerTrackCount;
    const int  draftTrackCount  = m_soundEffectDraftTrackCount;
    const int  bgmTrackCount    = m_soundEffectBgmTrackCount;

    // 弹层外观沿用编辑器主题，几何尺寸统一按 DPI 取整。
    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    const float windowPadding = std::floor(aesthetics.windowPadding * dpiScale);
    const float windowRounding =
        std::floor(aesthetics.windowRounding * dpiScale);
    const float frameRounding = std::floor(aesthetics.frameRounding * dpiScale);
    const float rowHeight     = ImGui::GetFrameHeightWithSpacing();
    // 布局 helper 保证三个区域至少拥有一个可见占位行。
    const auto trackLayout = calculateSoundEffectToolTrackLayout(
        playerTrackCount, draftTrackCount, bgmTrackCount);
    // 分解命名字段，后续索引公式保持可读且不重复访问布局对象。
    const int playerRows = trackLayout.playerRows;
    const int draftRows  = trackLayout.draftRows;
    const int bgmRows    = trackLayout.bgmRows;
    // totalRows 已包含各区域标题、总控和零轨道占位。
    const int totalRows = trackLayout.totalRows;

    // 音效工具不跨视口，定位和高度上限都以主视口为边界。
    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    const float    viewportTop    = mainViewport->Pos.y;
    const float    viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    const float    viewportLeft   = mainViewport->Pos.x;
    const float    edgePadding    = std::floor(8.0F * dpiScale);
    const float    popupWidth     = std::floor(420.0F * dpiScale);
    // 标题高度计入分隔后的标准项目间距。
    const float titleHeight =
        ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    // 理想高度覆盖全部逻辑行，实际高度在最小可用值和视口上限间夹取。
    const float desiredHeight = windowPadding * 2.0F + titleHeight +
                                static_cast<float>(totalRows) * rowHeight;
    // 可用高度至少保留设计最小值，小视口由窗口裁剪承担最终限制。
    const float availableHeight =
        std::max(std::floor(180.0F * dpiScale),
                 viewportBottom - viewportTop - edgePadding * 2.0F);
    const float maximumHeight =
        std::min(availableHeight, std::floor(480.0F * dpiScale));
    const float minimumHeight =
        std::min(maximumHeight, std::floor(180.0F * dpiScale));
    const float popupHeight =
        std::clamp(desiredHeight, minimumHeight, maximumHeight);

    // 以弹层右上角对齐工具栏左侧，避免遮挡纵向工具按钮。
    float targetX = toolbarWindow->Pos.x - std::floor(4.0F * dpiScale);
    float targetY = m_lastSoundEffectToolBtnY;
    // 左边界要计入完整弹层宽度，因为后续使用右上角枢轴定位。
    targetX = std::max(targetX, viewportLeft + popupWidth + edgePadding);
    targetY = std::clamp(targetY,
                         viewportTop + edgePadding,
                         std::max(viewportTop + edgePadding,
                                  viewportBottom - popupHeight - edgePadding));

    // 强制位置和尺寸，用户不能拖动此工具型弹层。
    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        { targetX, targetY }, ImGuiCond_Always, { 1.0F, 0.0F });
    ImGui::SetNextWindowSize({ popupWidth, popupHeight }, ImGuiCond_Always);

    // 外层禁用滚动，唯一滚动源是下方子窗口，标题始终保持可见。
    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(windowPadding, windowPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRounding);

    if ( !ImGui::Begin("##SoundEffectToolPopup", nullptr, popupFlags) ) {
        // Begin 返回 false 时也必须成对结束窗口和样式栈。
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }

    ImGui::TextUnformatted(TR("ui.key_sound_tool.title").data());
    ImGui::Separator();

    // 音频管理器提供当前混音快照；配置未变化时沿用上一帧的值副本，
    // 避免每帧在配置锁内复制完整视觉设置、配色表及近期项目列表。
    auto& audio = Audio::AudioManager::instance();
    if ( m_editorApplicationService ) {
        // 修订号未变时只做一次 acquire 读取；配置变更由 EditorEngine
        // 统一发布，滑条提交后的新值会在下一帧自动进入缓存。
        static_cast<void>(engine.refreshEditorConfigSnapshot(
            m_soundEffectEditorConfigCache, m_soundEffectEditorConfigRevision));
    } else {
        // 未注入服务的测试与初始化路径继续使用应用配置。
        m_soundEffectEditorConfigCache =
            Config::AppConfig::instance().getEditorConfig();
    }
    const auto& editorConfig = m_soundEffectEditorConfigCache;
    if ( !m_soundEffectGainDraftInitialized || !ImGui::IsAnyItemActive() ) {
        // 没有活动控件时允许外部设置覆盖草稿；拖动期间保持本地连续值，
        // 防止异步运行时回读把滑块拉回上一采样点。
        m_unboundHitSoundGainDraft =
            editorConfig.settings.sfxConfig.unboundHitSfxGain;
        m_boundHitSoundGainDraft =
            editorConfig.settings.sfxConfig.boundHitSfxGain;
        m_editorMetronomeGainDraft =
            editorConfig.settings.sfxConfig.editorMetronomeGain;
        m_soundEffectGainDraftInitialized = true;
    }
    const float muteButtonSize  = std::floor(ImGui::GetFrameHeight());
    const float gainSliderWidth = std::floor(170.0F * dpiScale);
    const float controlSpacing  = ImGui::GetStyle().ItemSpacing.x;
    // 子窗口占用标题下方全部剩余空间，始终提供纵向滚动条。
    const float trackViewportHeight =
        std::max(1.0F, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("##SoundEffectControlScroller",
                      { 0.0F, trackViewportHeight },
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    /// @brief 根据静音态和增益选择图标，并绘制统一大小的状态按钮。
    ///
    /// applyChange 接收目标静音态，调用者决定写入全局音频状态、配置开关
    /// 还是逐轨命令。该 helper 不缓存状态，确保每帧反映外部变更。
    const auto drawMuteStateButton = [&](bool        muted,
                                         float       gain,
                                         const auto& applyChange) {
        // 静音优先级最高；未静音时用增益区间选择音量层级图标。
        const char* icon = ICON_MMM_VOLUME_MUTE;
        if ( !muted ) {
            if ( gain <= 0.0F )
                icon = ICON_MMM_VOLUME_OFF;
            else if ( gain < 1.0F )
                icon = ICON_MMM_VOLUME_LOW;
            else
                icon = ICON_MMM_VOLUME_HIGH;
        }

        // 每一行都处于独立 ID 栈，固定后缀不会造成控件冲突。
        char buttonLabel[64];
        std::snprintf(buttonLabel, sizeof(buttonLabel), "%s##MuteState", icon);
        if ( muted ) {
            // 危险色仅提示当前处于静音，不改变按钮背景和主题反馈。
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  Utils::UIThemeUtils::getDangerColor());
        }
        // 固定尺寸按钮移除主题过大内边距，保证图标不被裁切。
        Utils::pushFixedButtonStyleVars();
        const bool clicked = ::MMM::UI::FeedbackButton(
            buttonLabel, ImVec2(muteButtonSize, muteButtonSize));
        Utils::popFixedButtonStyleVars();
        if ( muted ) ImGui::PopStyleColor();

        if ( ImGui::IsItemHovered() ) {
            // 提示描述点击后的动作，而不是重复当前状态。
            Utils::renderTooltip(muted ? TR("ui.audio_manager.unmute").data()
                                       : TR("ui.audio_manager.mute").data());
        }
        // 点击只在反馈控件确认后翻转一次状态。
        if ( clicked ) applyChange(!muted);
    };

    /// @brief 绘制仅含区域名称与总静音按钮的紧凑行。
    ///
    /// 控件右对齐，长翻译标签只会占用左侧剩余区域；ID 由区域名隔离。
    const auto drawMuteOnlyRow = [&](const char* label,
                                     const char* id,
                                     bool        muted,
                                     const auto& applyMute) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        // 若标签已经超过目标列则保持当前位置，避免游标向左回退并重叠。
        ImGui::SetCursorPosX(
            std::max(ImGui::GetCursorPosX(),
                     ImGui::GetWindowContentRegionMax().x - muteButtonSize));
        drawMuteStateButton(muted, 1.0F, applyMute);
        ImGui::PopID();
    };

    /// @brief 绘制带静音按钮和百分比增益滑条的完整混音行。
    ///
    /// applyGain 用于拖动期间即时试听，persistGain 只在编辑结束边沿调用。
    /// maximumGain 按音效类别限制范围，全局总增益为 100%，分类和逐轨可到
    /// 200%，为较弱采样保留补偿空间。
    const auto drawMixRow = [&](const char* label,
                                const char* id,
                                bool        muted,
                                float       gain,
                                float       maximumGain,
                                const auto& applyMute,
                                const auto& applyGain,
                                const auto& persistGain) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        // 控件组作为整体右对齐，标签长度不会改变滑条列位置。
        const float controlWidth =
            muteButtonSize + controlSpacing + gainSliderWidth;
        ImGui::SetCursorPosX(
            std::max(ImGui::GetCursorPosX(),
                     ImGui::GetWindowContentRegionMax().x - controlWidth));
        drawMuteStateButton(muted, gain, applyMute);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(gainSliderWidth);
        // UI 使用百分比显示，音频接口继续接收线性 0..maximumGain 值。
        const float maximumGainPercent = maximumGain * 100.0F;
        float gainPercent = std::clamp(gain * 100.0F, 0.0F, maximumGainPercent);
        if ( ::MMM::UI::FeedbackSliderFloat(
                 "##Gain", &gainPercent, 0.0F, maximumGainPercent, "%.0f%%") ) {
            // 拖动每帧只更新内存/命令队列，不触发配置文件写入。
            applyGain(gainPercent * 0.01F);
        }
        if ( ImGui::IsItemDeactivatedAfterEdit() ) {
            // 鼠标释放或键盘编辑结束时提交最终持久值。
            persistGain(gainPercent * 0.01F);
        }
        if ( ImGui::IsItemHovered() ) {
            Utils::renderTooltip(TR("ui.key_sound_tool.gain").data());
        }
        ImGui::PopID();
    };

    // 行索引由前方区域长度递推，保持虚拟化循环中的分支互斥。
    // 固定的四行击打音控制后依次排列玩家、草稿和 BGM 区域。
    // 击打音区域固定占四行：标题、全部总线、未绑定组和绑定组。
    // 全部总线属于 AudioManager 全局运行时，两个分类组属于 EditorConfig。
    // 分类滑条允许提升到 200%，以补偿单个采样响度；总线限制为 100%，避免
    // 在分类补偿基础上再次无界放大。分类开关使用 enable 字段，静音 UI 语义
    // 需取反。拖动草稿保存在成员中，异步命令回读不会打断当前手势。
    const int hitSoundHeaderRow  = 0;
    const int allHitSoundRow     = 1;
    const int unboundHitSoundRow = 2;
    const int boundHitSoundRow   = 3;
    // 节拍器有独立标题和混音行，不属于打击音或逐轨键声分组。
    const int metronomeHeaderRow = 4;
    const int editorMetronomeRow = 5;
    // 玩家区域从第七行开始：标题、区域总开关，然后是每个玩家轨道。
    // 该总开关沿用 enableHitSfx 持久配置，逐轨静音和增益则是运行时命令。
    // 即使没有谱面，布局 helper 也保留一条占位轨道行，向用户解释不可用原因，
    // 后方草稿和 BGM 区域的索引仍保持连续且可预测。
    const int playerHeaderRow  = 6;
    const int playerMasterRow  = 7;
    const int playerTrackBegin = 8;
    // 草稿区域轨道数来自 SessionContext，而不是谱面持久元数据。草稿轨道可能
    // 随临时编辑状态变化，因此每帧在短锁内重新读取并重建逻辑行数。区域总控
    // 和逐轨值都是 EditorEngine 命令，不写入常规击打音 enable 配置。
    const int draftHeaderRow  = playerTrackBegin + playerRows;
    const int draftMasterRow  = draftHeaderRow + 1;
    const int draftTrackBegin = draftMasterRow + 1;
    // BGM 键声音轨数量来自 base metadata 的 bgm_track_count，和主播放音频总线
    // 不是同一概念。这里的增益只影响这些轨道触发的键声，不能替代 BGM 主音量。
    // 零轨道时总控禁用且保留占位行，使用户仍能看到区域结构。
    const int bgmHeaderRow  = draftTrackBegin + draftRows;
    const int bgmMasterRow  = bgmHeaderRow + 1;
    const int bgmTrackBegin = bgmMasterRow + 1;
    // contentStart 是逻辑行零点，所有虚拟行都用绝对子窗口游标定位。
    const ImVec2 contentStart = ImGui::GetCursorPos();
    const float  scrollY      = ImGui::GetScrollY();
    // 可见高度扣除子窗口内边距，至少保留一个像素防止退化除法语义。
    const float visibleHeight = std::max(
        1.0F,
        ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y * 2.0F);
    const float visibleStart = std::max(0.0F, scrollY - contentStart.y);
    const float visibleEnd =
        std::max(visibleStart, scrollY + visibleHeight - contentStart.y);
    // 额外绘制末端后一行，允许部分可见控件完整接收交互。
    const int firstVisibleRow = std::clamp(
        static_cast<int>(std::floor(visibleStart / rowHeight)), 0, totalRows);
    const int lastVisibleRow =
        std::clamp(static_cast<int>(std::ceil(visibleEnd / rowHeight)) + 1,
                   firstVisibleRow,
                   totalRows);

    // 仅实例化可见行；逻辑总高度在循环后用 Dummy 补齐。
    for ( int row = firstVisibleRow; row < lastVisibleRow; ++row ) {
        ImGui::SetCursorPos(
            { contentStart.x,
              contentStart.y + static_cast<float>(row) * rowHeight });

        // 以下分支按递增边界覆盖每个逻辑行且每次 continue，避免一行绘制多控件。
        if ( row == hitSoundHeaderRow ) {
            // 分隔标题占用标准行高，便于后续索引直接相加。
            ImGui::SeparatorText(TR("ui.key_sound_tool.hit_sound_area").data());
            continue;
        }
        if ( row == allHitSoundRow ) {
            // 全部击打音直接操作 AudioManager 的 SFX 总线。
            drawMixRow(
                TR("ui.key_sound_tool.all_hit_sounds").data(),
                "AllHitSounds",
                audio.isSFXGainMuted(),
                audio.getSFXGain(),
                1.0F,
                // 总线静音直接进入音频管理器，不需要编辑器命令中转。
                [&audio](bool muted) { audio.setSFXGainMute(muted); },
                // false 表示即时试听更新，不在每个拖动采样点持久化。
                [&audio](float gain) { audio.setSFXGain(gain, false); },
                // 控件失活后使用默认持久语义写入最终总线增益。
                [&audio](float gain) { audio.setSFXGain(gain); });
            continue;
        }
        if ( row == unboundHitSoundRow ) {
            // 未绑定音效开关持久化到配置，增益拖动通过组命令即时试听。
            drawMixRow(
                TR("ui.key_sound_tool.unbound_hit_sound").data(),
                "UnboundHitSound",
                !editorConfig.settings.sfxConfig.enableUnboundHitSfx,
                m_unboundHitSoundGainDraft,
                2.0F,
                [this, &engine](bool muted) {
                    // 配置字段表达“启用”，因此与 muted 语义取反。
                    auto config = currentEditorConfig();
                    config.settings.sfxConfig.enableUnboundHitSfx = !muted;
                    updateEditorConfig(config);
                },
                [this, &engine](float gain) {
                    // 草稿先更新，运行时命令可在逻辑线程异步消费。
                    m_unboundHitSoundGainDraft = gain;
                    engine.pushCommand(Logic::CmdSetKeySoundEffectGroupGain{
                        .group = Logic::KeySoundEffectGroup::Unbound,
                        .gain  = gain,
                    });
                },
                [this, &engine](float gain) {
                    // 最终值经配置入口保存，供下次启动恢复。
                    auto config = currentEditorConfig();
                    config.settings.sfxConfig.unboundHitSfxGain = gain;
                    updateEditorConfig(config);
                });
            continue;
        }
        if ( row == boundHitSoundRow ) {
            // 绑定音效与未绑定音效采用相同的即时/持久化分层。
            drawMixRow(
                TR("ui.key_sound_tool.bound_hit_sound").data(),
                "BoundHitSound",
                !editorConfig.settings.sfxConfig.enableBoundHitSfx,
                m_boundHitSoundGainDraft,
                2.0F,
                [this, &engine](bool muted) {
                    // enableBoundHitSfx 与静音目标取反后经应用服务保存。
                    auto config = currentEditorConfig();
                    config.settings.sfxConfig.enableBoundHitSfx = !muted;
                    updateEditorConfig(config);
                },
                [this, &engine](float gain) {
                    // 成员草稿先更新，保证下一帧滑条连续显示新值。
                    m_boundHitSoundGainDraft = gain;
                    engine.pushCommand(Logic::CmdSetKeySoundEffectGroupGain{
                        .group = Logic::KeySoundEffectGroup::Bound,
                        .gain  = gain,
                    });
                },
                [this, &engine](float gain) {
                    // 最终值写回持久配置，供重新启动时恢复。
                    auto config = currentEditorConfig();
                    config.settings.sfxConfig.boundHitSfxGain = gain;
                    updateEditorConfig(config);
                });
            continue;
        }
        // 节拍器标题占独立逻辑行，滚动虚拟化不会把它并入打击音区域。
        // 固定行高也保证窄窗口下能滚动到后续轨道分组。
        if ( row == metronomeHeaderRow ) {
            ImGui::SeparatorText(TR("ui.key_sound_tool.metronome_area").data());
            continue;
        }
        // 编辑器节拍器的开关和增益与普通击打音类别独立保存。
        // BPM 测量工具继续使用自己的音效池和调度时钟。
        // 分类滑条允许 200% 增益，以补偿较轻的皮肤采样。
        if ( row == editorMetronomeRow ) {
            // 控制编辑器自己的两种节拍音；BPM 测量工具保留独立路由。
            drawMixRow(
                TR("ui.key_sound_tool.editor_metronome").data(),
                "EditorMetronome",
                !editorConfig.settings.sfxConfig.enableEditorMetronome,
                m_editorMetronomeGainDraft,
                2.0F,
                // 开关切换会同步到编辑器配置，播放源在下一轮接纳新状态。
                // 关闭时只停止编辑器专属节拍声，不取消打击音预约。
                [this](bool muted) {
                    auto config = currentEditorConfig();
                    config.settings.sfxConfig.enableEditorMetronome = !muted;
                    updateEditorConfig(config);
                    // 已载入的短 PCM 可复用，重复启用不会再次解码。
                    // 资源加载可能等待文件读取，只允许在这次点击边沿进行。
                    // 点击启用后由低频 UI 事件准备资源，播放循环不做同步解码。
                    if ( !muted ) {
                        static_cast<void>(
                            Logic::PlaybackController::preloadMetronomeSounds(
                                config.settings.sfxConfig.editorMetronomeGain));
                    }
                },
                [this, &audio](float gain) {
                    // 草稿先写入成员，下一帧回读配置时不会覆盖当前手势。
                    // 重拍和普通拍同步改变音量，节拍层次只由采样自身决定。
                    // 拖动期间即时更新已载入的两种节拍音，不写配置文件。
                    m_editorMetronomeGainDraft = gain;
                    audio.setSFXPoolVolume("editor.metronome.beat_low", gain);
                    audio.setSFXPoolVolume("editor.metronome.downbeat_high",
                                           gain);
                },
                // 控件失活后再保存最终增益，避免每帧鼠标移动写文件。
                // 此前的即时试听已经修改运行时池，不必重新播放节拍。
                [this](float gain) {
                    auto config = currentEditorConfig();
                    config.settings.sfxConfig.editorMetronomeGain = gain;
                    updateEditorConfig(config);
                });
            continue;
        }
        if ( row == playerHeaderRow ) {
            // 玩家区域始终出现，即使当前没有活动谱面。
            ImGui::SeparatorText(TR("ui.key_sound_tool.player_area").data());
            continue;
        }
        if ( row == playerMasterRow ) {
            // 玩家总开关沿用历史 enableHitSfx 配置语义。
            drawMuteOnlyRow(TR("ui.key_sound_tool.area_master").data(),
                            "PlayerArea",
                            !editorConfig.settings.sfxConfig.enableHitSfx,
                            [this, &engine](bool muted) {
                                auto config = currentEditorConfig();
                                config.settings.sfxConfig.enableHitSfx = !muted;
                                updateEditorConfig(config);
                            });
            continue;
        }
        if ( row < draftHeaderRow ) {
            // playerTrackBegin 到 draftHeaderRow 之间只属于玩家轨道或占位行。
            const int track = row - playerTrackBegin;
            if ( track >= playerTrackCount ) {
                // 零轨道布局仍保留一行，显示无谱面或无轨道原因。
                ImGui::TextDisabled(
                    "%s",
                    TR(hasBeatmap ? "ui.key_sound_tool.no_player_tracks"
                                  : "ui.key_sound_tool.no_beatmap")
                        .data());
                continue;
            }

            // 区域名与轨道索引组成稳定 ID，显示标签可随语言变化。
            ImGui::PushID("PlayerTrack");
            ImGui::PushID(track);
            // 玩家轨道标签把零基索引转换为用户可读的一基编号。
            char label[64];
            std::snprintf(label,
                          sizeof(label),
                          TR("ui.key_sound_tool.player_track").data(),
                          track + 1);
            // 经过非负范围判断后再转换为音频接口使用的无符号索引。
            const auto trackIndex = static_cast<std::uint32_t>(track);
            drawMixRow(
                label,
                "MixState",
                audio.isPlayerKeySoundTrackMuted(trackIndex),
                audio.getPlayerKeySoundTrackGain(trackIndex),
                2.0F,
                [&engine, trackIndex](bool muted) {
                    // 区域枚举与索引共同标识唯一玩家键声音轨。
                    engine.pushCommand(Logic::CmdSetKeySoundTrackMute{
                        .area       = Logic::KeySoundTrackArea::Player,
                        .trackIndex = trackIndex,
                        .muted      = muted,
                    });
                },
                [&engine, trackIndex](float gain) {
                    // 增益命令只更新运行时玩家轨道状态。
                    engine.pushCommand(Logic::CmdSetKeySoundTrackGain{
                        .area       = Logic::KeySoundTrackArea::Player,
                        .trackIndex = trackIndex,
                        .gain       = gain,
                    });
                },
                // 逐轨增益由命令维护运行时状态，不额外写入全局配置。
                [](float) {});
            ImGui::PopID();
            ImGui::PopID();
            continue;
        }
        if ( row == draftHeaderRow ) {
            // 草稿区域与玩家区域使用独立音频总线。
            ImGui::SeparatorText(TR("ui.key_sound_tool.draft_area").data());
            continue;
        }
        if ( row == draftMasterRow ) {
            // 没有实际草稿轨道时保留总控外观但禁止交互。
            if ( !hasBeatmap || draftTrackCount == 0 ) ImGui::BeginDisabled();
            drawMuteOnlyRow(
                TR("ui.key_sound_tool.area_master").data(),
                "DraftArea",
                audio.isDraftKeySoundAreaMuted(),
                [&engine](bool muted) {
                    engine.pushCommand(
                        Logic::CmdSetDraftKeySoundAreaMute{ .muted = muted });
                });
            if ( !hasBeatmap || draftTrackCount == 0 ) ImGui::EndDisabled();
            continue;
        }
        if ( row < bgmHeaderRow ) {
            // 当前范围只映射草稿轨道；至少一行用于空状态提示。
            const int track = row - draftTrackBegin;
            if ( track >= draftTrackCount ) {
                ImGui::TextDisabled(
                    "%s",
                    TR(hasBeatmap ? "ui.key_sound_tool.no_draft_tracks"
                                  : "ui.key_sound_tool.no_beatmap")
                        .data());
                continue;
            }

            // 区域 ID 防止相同轨道号与玩家控件冲突。
            // Draft 区域 ID 隔离同号玩家轨道控件。
            ImGui::PushID("DraftTrack");
            ImGui::PushID(track);
            // 草稿轨道沿用一基用户编号和零基命令索引。
            char label[64];
            std::snprintf(label,
                          sizeof(label),
                          TR("ui.key_sound_tool.draft_track").data(),
                          track + 1);
            const auto trackIndex = static_cast<std::uint32_t>(track);
            drawMixRow(
                label,
                "MixState",
                audio.isDraftKeySoundTrackMuted(trackIndex),
                audio.getDraftKeySoundTrackGain(trackIndex),
                2.0F,
                [&engine, trackIndex](bool muted) {
                    engine.pushCommand(Logic::CmdSetKeySoundTrackMute{
                        // 区域字段确保命令不误命中玩家轨道。
                        .area       = Logic::KeySoundTrackArea::Draft,
                        .trackIndex = trackIndex,
                        .muted      = muted,
                    });
                },
                [&engine, trackIndex](float gain) {
                    engine.pushCommand(Logic::CmdSetKeySoundTrackGain{
                        // 实时增益只作用于指定草稿轨道。
                        .area       = Logic::KeySoundTrackArea::Draft,
                        .trackIndex = trackIndex,
                        .gain       = gain,
                    });
                },
                [](float) {});
            ImGui::PopID();
            ImGui::PopID();
            continue;
        }
        if ( row == bgmHeaderRow ) {
            // BGM 音效轨道是最后一个区域，标题后仍有总控行。
            ImGui::SeparatorText(TR("ui.key_sound_tool.bgm_area").data());
            continue;
        }
        if ( row == bgmMasterRow ) {
            // 总控只在活动谱面声明了 BGM 轨道时可用。
            if ( !hasBeatmap || bgmTrackCount == 0 ) ImGui::BeginDisabled();
            drawMuteOnlyRow(
                TR("ui.key_sound_tool.area_master").data(),
                "BgmArea",
                audio.isBgmKeySoundAreaMuted(),
                [this, &engine](bool muted) {
                    engine.pushCommand(
                        Logic::CmdSetBgmKeySoundAreaMute{ .muted = muted });
                });
            if ( !hasBeatmap || bgmTrackCount == 0 ) ImGui::EndDisabled();
            continue;
        }
        if ( row >= bgmTrackBegin ) {
            // 最后范围不存在后续标题边界，因此显式检查实际轨道数。
            const int track = row - bgmTrackBegin;
            if ( track >= bgmTrackCount ) {
                ImGui::TextDisabled(
                    "%s",
                    TR(hasBeatmap ? "ui.key_sound_tool.no_bgm_tracks"
                                  : "ui.key_sound_tool.no_beatmap")
                        .data());
                continue;
            }

            // BGM 区域使用独立 ID 前缀和音频命令枚举。
            // BGM 区域使用独立 ID 前缀和命令区域枚举。
            ImGui::PushID("BgmTrack");
            ImGui::PushID(track);
            // BGM 显示编号不改变元数据中的轨道顺序。
            char label[64];
            std::snprintf(label,
                          sizeof(label),
                          TR("ui.key_sound_tool.bgm_track").data(),
                          track + 1);
            const auto trackIndex = static_cast<std::uint32_t>(track);
            drawMixRow(
                label,
                "MixState",
                audio.isBgmKeySoundTrackMuted(trackIndex),
                audio.getBgmKeySoundTrackGain(trackIndex),
                2.0F,
                [&engine, trackIndex](bool muted) {
                    engine.pushCommand(Logic::CmdSetKeySoundTrackMute{
                        // Bgm 枚举防止与主音频或其他键声区域混淆。
                        .area       = Logic::KeySoundTrackArea::Bgm,
                        .trackIndex = trackIndex,
                        .muted      = muted,
                    });
                },
                [&engine, trackIndex](float gain) {
                    engine.pushCommand(Logic::CmdSetKeySoundTrackGain{
                        // 逐轨增益只作用于键声混音层。
                        .area       = Logic::KeySoundTrackArea::Bgm,
                        .trackIndex = trackIndex,
                        .gain       = gain,
                    });
                },
                [](float) {});
            ImGui::PopID();
            ImGui::PopID();
            continue;
        }
    }

    // 把游标推进到所有逻辑行之后，向 ImGui 声明完整滚动内容高度。
    ImGui::SetCursorPos(
        { contentStart.x,
          contentStart.y + static_cast<float>(totalRows) * rowHeight });
    // 一个像素 Dummy 让最后一行底边可滚入视口且不创建额外交互项。
    ImGui::Dummy({ 1.0F, 1.0F });
    ImGui::EndChild();
    ImGui::End();
    // 恢复窗口圆角、内边距和控件圆角。
    ImGui::PopStyleVar(3);
}

/// @brief 首次加载皮肤默认调色盘并标记运行时缓存可用。
///
/// 初始化集中调用完整加载路径，确保画笔和分拍线渲染配置同步更新。
/// 调用方无需预先清空颜色数组，加载路径会完整覆盖所有固定槽位。
/// 初始化标志只表示内存状态已经建立，不代表项目偏好已解析；后者由
/// applyProjectPalettePreference 在同一 update 中根据项目身份单独完成。
/// 此拆分允许皮肤切换只刷新颜色来源，而不伪造项目偏好变化。
void ToolbarView::initializeColorPalette()
{
    // 先完成所有颜色与下游状态推送，再设置初始化标记。
    loadSkinDefaultPalette();
    m_colorPaletteInitialized = true;
}

/// @brief 皮肤变化后刷新依赖皮肤颜色的当前调色盘。
///
/// 自定义方案包含完整颜色值，不随皮肤切换重载；皮肤默认和软件继承需要刷新。
/// 未初始化时由首次 update 负责加载，避免重复向逻辑层推送。
///
/// 选择种类而非活动索引决定是否刷新，因为继承态可能解析到某个自定义方案，
/// 但它仍需在皮肤变化时重新求值：软件默认可能已经切换回皮肤来源。
/// 自定义态保留用户保存的绝对 RGBA 数值，避免换肤意外改写方案外观。
/// 函数不保存配置，皮肤版本管理者负责自身持久化和刷新通知。
void ToolbarView::refreshPaletteAfterSkinChange()
{
    // 没有运行时调色盘时无需处理皮肤版本变化。
    if ( !m_colorPaletteInitialized ) return;

    if ( m_activePaletteSelection == PaletteSelectionKind::SkinDefault ) {
        // 皮肤默认直接重新读取新皮肤全部颜色。
        loadSkinDefaultPalette();
    } else if ( m_activePaletteSelection ==
                PaletteSelectionKind::InheritSoftwareDefault ) {
        // 软件默认可能本身指向皮肤默认，因此重新解析继承链。
        loadSoftwareDefaultPalette();
    }
}

/// @brief 将当前皮肤笔记与分拍线颜色设为活动调色盘。
///
/// 该选择关闭分拍线覆盖，让渲染器继续使用皮肤语义；方案索引恢复为无自定义项。
/// 加载完成后同步画笔和渲染配置，并清除方案名校验错误。
///
/// 内存数组仍保存一份当前皮肤颜色，用于色块预览和后续从内置方案新建
/// 自定义方案。分拍线覆盖关闭后，数组不是渲染器权威来源，但仍必须同步，
/// 以便用户打开调色盘时看到与画面一致的初值。
/// 该操作不修改软件默认或项目偏好字段，只改变当前解析结果。
void ToolbarView::loadSkinDefaultPalette()
{
    // 两套数组分别从皮肤键读取，并包含旧皮肤兼容回退。
    fillPaletteWithSkinDefaults(m_paletteColors);
    fillBeatLinePaletteWithSkinDefaults(m_beatLinePaletteColors);
    // 皮肤默认无需向视觉配置声明自定义分拍线覆盖。
    m_overrideBeatLinePalette  = false;
    m_activePaletteSchemeIndex = -1;
    m_activePaletteSelection   = PaletteSelectionKind::SkinDefault;
    m_paletteSchemeErrorKey.clear();
    // 名称输入框展示本地化内置名称，但它不能作为自定义方案保存。
    setPaletteSchemeNameBuffer(defaultPaletteSchemeName());
    pushPaletteToBrush();
    pushBeatLinePaletteToRenderer();
}

/// @brief 解析并加载软件级默认调色盘作为项目继承结果。
///
/// 软件设置可指向自定义方案或皮肤默认；缺失、空值与无效名称均安全回退皮肤。
/// 运行时选择种类保持 InheritSoftwareDefault，使项目偏好仍表达“跟随软件”。
/// 继承到自定义方案时开启分拍线覆盖，继承皮肤时关闭覆盖。
///
/// 继承语义与解析结果刻意分离：即便当前软件默认恰好指向自定义方案，UI
/// 仍显示“继承软件默认”，也禁止覆盖、重命名或删除该方案。用户需要先显式
/// 选择自定义项，才能执行管理动作。这能防止项目继承状态被一次编辑意外固化。
/// 未找到名称被视为可恢复配置漂移，不记录错误或写回设置。
void ToolbarView::loadSoftwareDefaultPalette()
{
    // 默认方案名从全局编辑器设置读取，不修改项目自身偏好字段。
    const auto& settings   = Config::AppConfig::instance().getEditorSettings();
    const auto& schemeName = settings.defaultColorPaletteSchemeName;
    bool        loaded     = false;

    // 空名和固定皮肤默认 ID 都直接进入皮肤回退分支。
    if ( !schemeName.empty() &&
         schemeName != Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ) {
        const auto& paletteConfig = settings.colorPalettes;
        // 自定义方案按稳定名称定位，不依赖可能因增删变化的保存索引。
        auto it = std::find_if(paletteConfig.schemes.begin(),
                               paletteConfig.schemes.end(),
                               [&](const Config::ColorPaletteScheme& scheme) {
                                   return scheme.name == schemeName;
                               });
        if ( it != paletteConfig.schemes.end() ) {
            // 找到完整方案后覆盖笔记与分拍线全部槽位。
            applyStoredPaletteScheme(
                m_paletteColors, m_beatLinePaletteColors, *it);
            m_overrideBeatLinePalette = true;
            loaded                    = true;
        }
    }

    if ( !loaded ) {
        // 无效软件偏好不阻断工具栏，回退当前皮肤颜色。
        fillPaletteWithSkinDefaults(m_paletteColors);
        fillBeatLinePaletteWithSkinDefaults(m_beatLinePaletteColors);
        m_overrideBeatLinePalette = false;
    }
    // 继承态不允许直接覆盖或删除实际自定义方案。
    m_activePaletteSchemeIndex = -1;
    m_activePaletteSelection   = PaletteSelectionKind::InheritSoftwareDefault;
    m_paletteSchemeErrorKey.clear();
    // 输入框展示继承语义，而非被继承方案的内部名称。
    setPaletteSchemeNameBuffer(inheritedPaletteSchemeName());
    pushPaletteToBrush();
    pushBeatLinePaletteToRenderer();
}

/// @brief 按持久化名称加载皮肤默认或自定义调色盘。
/// @param schemeName 固定皮肤默认 ID 或自定义方案名。
/// @return 成功解析并加载时返回 true。
///
/// 空名称兼容旧项目并等价于皮肤默认；未知自定义名称返回 false 供调用方回退。
///
/// 此函数只解释“显式名称”语义，不处理软件继承；调用者若需要继承必须直接
/// 调用 loadSoftwareDefaultPalette，以保留 PaletteSelectionKind。自定义名称
/// 使用精确、区分大小写比较，与保存阶段的冲突规则一致。
/// 成功路径会完整同步画笔与分拍线，调用者无需再次推送。
bool ToolbarView::loadPaletteSchemeByName(const std::string& schemeName)
{
    if ( schemeName.empty() ||
         schemeName == Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ) {
        // 旧空值与稳定内置 ID 共用皮肤默认加载路径。
        loadSkinDefaultPalette();
        return true;
    }

    auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().colorPalettes;
    // 名称是项目偏好跨配置变更保持稳定的关联键。
    auto it = std::find_if(paletteConfig.schemes.begin(),
                           paletteConfig.schemes.end(),
                           [&](const Config::ColorPaletteScheme& scheme) {
                               return scheme.name == schemeName;
                           });
    // 不在此处自行回退，让调用方区分加载失败并决定语义。
    if ( it == paletteConfig.schemes.end() ) return false;

    loadPaletteScheme(static_cast<std::size_t>(
        std::distance(paletteConfig.schemes.begin(), it)));
    return true;
}

/// @brief 按当前项目、项目偏好和软件默认解析应使用的活动调色盘。
///
/// 无项目时使用皮肤默认；项目无显式方案时继承软件默认；显式名称优先。
/// 组合项目根路径、偏好来源和方案名形成应用键，未变化时跳过重复下游推送。
/// 项目引用的自定义方案缺失时回退皮肤默认，但不静默改写原项目字段。
///
/// 应用键同时包含“来源”是必要的：项目显式选择和软件继承可能暂时解析为
/// 同一个方案名，但二者在软件默认变化后的行为不同。项目根路径用于隔离
/// 两个恰好选择同名方案的项目，换项目时必须重新同步画笔状态。
///
/// 无效项目引用保留在项目文件中，便于用户重新安装或重新导入同名方案后
/// 自动恢复；运行时只做安全回退。函数不保存项目，因此每帧调用不会产生 I/O。
/// @warning UI 热路径：每帧调用，应用键未变化时必须常数时间返回。
void ToolbarView::applyProjectPalettePreference()
{
    auto&       engine   = Logic::EditorEngine::instance();
    const auto* project  = engine.getCurrentProject();
    const auto& settings = Config::AppConfig::instance().getEditorSettings();

    // 默认无项目状态明确选择皮肤来源和稳定内置方案 ID。
    std::string projectKey;
    std::string preferenceSource = "skin";
    std::string schemeName       = Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
    if ( project ) {
        // 项目身份使用根路径，避免切换项目后复用相同方案名的旧应用状态。
        projectKey       = Config::pathToUtf8(project->m_projectRoot);
        preferenceSource = "inherit";
        schemeName       = settings.defaultColorPaletteSchemeName;
        if ( !project->m_settings.m_colorPaletteSchemeName.empty() ) {
            // 非空项目字段覆盖全局默认并标记来源为项目显式选择。
            preferenceSource = "project";
            schemeName       = project->m_settings.m_colorPaletteSchemeName;
        }
    }
    if ( schemeName.empty() ) {
        // 软件默认的旧空值统一规范化为皮肤默认 ID。
        schemeName = Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
    }

    // 使用不可出现在路径或普通名称拼接歧义位置的换行分隔各字段。
    std::string applyKey = projectKey;
    applyKey.push_back('\n');
    applyKey += preferenceSource;
    applyKey.push_back('\n');
    applyKey += schemeName;
    // 完整解析输入未变化时不重复写画笔命令和编辑器配置。
    if ( applyKey == m_lastAppliedProjectPaletteKey ) return;

    m_lastAppliedProjectPaletteKey = applyKey;
    if ( preferenceSource == "inherit" ) {
        // 继承态需要保留专用选择种类，不能只按最终方案名加载。
        loadSoftwareDefaultPalette();
        return;
    }
    if ( !loadPaletteSchemeByName(schemeName) ) {
        // 已删除或损坏的方案引用安全回退当前皮肤。
        loadSkinDefaultPalette();
    }
}

/// @brief 将当前笔记调色盘通过逻辑命令同步到画笔。
///
/// 定长颜色数组按值进入命令，后续本地编辑不会改变已排队快照。
/// 命令只更新未来绘制使用的颜色，不遍历现有音符，也不改变当前选择。
/// 调色盘拖动可每帧调用此入口，因此实现必须保持无阻塞并避免磁盘访问。
/// 生命周期由命令值语义保证，ToolbarView 销毁后队列仍可安全消费。
void ToolbarView::pushPaletteToBrush()
{
    Logic::EditorEngine::instance().pushCommand(
        Logic::CmdSetBrushNotePalette{ m_paletteColors });
}

/// @brief 将当前笔记调色盘应用到画布已有选择。
///
/// 这是显式用户动作，不会在加载方案时自动改写既有音符颜色。
/// 命令携带所有槽位，使逻辑层能按各音符所属槽位一次处理当前选择集合。
/// 颜色选择器只在编辑结束时调用，避免拖动期间生成大量选择修改命令。
/// 该入口不检查选择是否为空，逻辑层把空选择作为正常无操作处理。
void ToolbarView::pushPaletteToSelection()
{
    Logic::EditorEngine::instance().pushCommand(
        Logic::CmdApplyNotePaletteToSelection{ m_paletteColors });
}

/// @brief 把当前分拍线与玩家音符默认配色写入编辑器运行时视觉配置。
///
/// 两组槽位转换为固定存储数组后一次提交，保证渲染器观察一致快照。
/// 皮肤默认选择会关闭覆盖，自定义或软件自定义继承会开启覆盖。
///
/// 画笔命令只影响新建物件；画布已有物件的默认色由 VisualConfig 提供。
/// 分拍线及音符配色共享同一快照，且不会改写物件的自定义颜色。
/// 函数使用应用服务入口，使配置变更通知与设置页修改具有相同传播路径。
/// 若应用服务未绑定，currentEditorConfig 仍提供安全副本，但提交会无操作。
void ToolbarView::pushBeatLinePaletteToRenderer()
{
    // 同一次快照提交分拍线与音符默认配色，避免界面方案和已有物件分帧错位。
    auto config                          = currentEditorConfig();
    config.visual.overrideBeatLineColors = m_overrideBeatLinePalette;
    config.visual.overrideNoteColors     = m_overrideBeatLinePalette;
    for ( std::size_t i = 0; i < m_paletteColors.size(); ++i ) {
        config.visual.noteColors[i] = toStoredColor(m_paletteColors[i]);
    }
    // 运行时 glm 表示逐槽转换回可序列化数组。
    for ( std::size_t i = 0; i < m_beatLinePaletteColors.size(); ++i ) {
        config.visual.beatLineColors[i] =
            toStoredColor(m_beatLinePaletteColors[i]);
    }
    updateEditorConfig(config);
}

/// @brief 按配置数组索引激活一个自定义调色盘方案。
/// @param schemeIndex colorPalettes.schemes 中的索引。
///
/// 越界输入无副作用；合法方案完整覆盖两套颜色并成为可管理的活动自定义项。
/// 加载只更新全局活动索引，不在此处保存 AppConfig，调用方决定持久化时机。
///
/// 方案对象在配置容器中的引用只用于同步复制，函数结束前不会跨越可能改变
/// 容器的操作。activeSchemeIndex 是软件级最近选择，不等价于项目偏好；项目
/// 显式关联始终使用稳定名称。加载不会把方案颜色应用到已有音符选择。
/// 分拍线覆盖对所有自定义方案强制开启，因为方案文件携带完整分拍线数组。
void ToolbarView::loadPaletteScheme(std::size_t schemeIndex)
{
    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().colorPalettes;
    // 防御菜单索引在方案删除或导入后失效。
    if ( schemeIndex >= paletteConfig.schemes.size() ) return;

    const auto& scheme = paletteConfig.schemes[schemeIndex];
    applyStoredPaletteScheme(m_paletteColors, m_beatLinePaletteColors, scheme);
    // 自定义方案始终携带完整分拍线颜色，因此启用覆盖。
    m_overrideBeatLinePalette = true;

    m_activePaletteSchemeIndex = static_cast<int>(schemeIndex);
    m_activePaletteSelection   = PaletteSelectionKind::Custom;
    // 保存全局最近活动索引，项目偏好仍由独立名称字段管理。
    paletteConfig.activeSchemeIndex = schemeIndex;
    m_paletteSchemeErrorKey.clear();
    setPaletteSchemeNameBuffer(scheme.name);
    pushPaletteToBrush();
    pushBeatLinePaletteToRenderer();
}

/// @brief 判断当前活动选择是否对应仍存在的可编辑自定义方案。
/// @return 选择种类、非负索引和配置范围均有效时返回 true。
///
/// 内置皮肤默认和项目继承选项不能被覆盖、重命名或删除。
/// 此函数每次读取当前配置容器大小，防止索引在导入、删除或外部配置刷新后
/// 仍被误认为有效。它不验证方案名称，因为旧配置中的空名也应先由保存校验
/// 给出可修复提示，而不是让管理按钮越界。
bool ToolbarView::canManageActivePaletteScheme() const
{
    const auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().colorPalettes;
    return m_activePaletteSelection == PaletteSelectionKind::Custom &&
           m_activePaletteSchemeIndex >= 0 &&
           static_cast<std::size_t>(m_activePaletteSchemeIndex) <
               paletteConfig.schemes.size();
}

/// @brief 检查自定义方案名是否与其他现有方案重复。
/// @param name 待保存名称。
/// @param ignoredIndex 重命名或覆盖时忽略的当前方案索引。
/// @return 任一未忽略方案使用完全相同名称时返回 true。
///
/// 比较区分大小写并不做空白规范化，保留用户输入的精确方案身份。
/// ignoredIndex 只用于覆盖或重命名当前方案；新建和导入必须传空值。
/// 即使旧配置意外存在多个同名项，循环也会跳过至多一个索引并检测其余项。
/// 函数不缓存结果，避免方案列表变化后使用过期冲突状态。
bool ToolbarView::hasPaletteSchemeNameConflict(
    const std::string& name, std::optional<std::size_t> ignoredIndex) const
{
    const auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().colorPalettes;
    for ( std::size_t i = 0; i < paletteConfig.schemes.size(); ++i ) {
        // 覆盖当前方案时，它自己的原名称不构成冲突。
        if ( ignoredIndex && *ignoredIndex == i ) continue;
        if ( paletteConfig.schemes[i].name == name ) return true;
    }
    return false;
}

/// @brief 校验方案名并设置供弹窗展示的错误翻译键。
/// @param name 待保存名称。
/// @param ignoredIndex 允许保持原名的当前方案索引。
/// @return 名称既非保留名也不重复时返回 true。
///
/// 失败只记录稳定翻译键，实际本地化留到绘制错误提示时执行。
/// 校验顺序先处理空名和内置名称，再检查用户方案冲突，因此错误提示稳定且
/// 更具可操作性。成功会清除旧错误，调用者可直接继续保存而无需额外重置。
/// 此函数不裁剪或规范化输入，保存到磁盘的名称与用户确认文本完全一致。
bool ToolbarView::validatePaletteSchemeNameForSave(
    const std::string& name, std::optional<std::size_t> ignoredIndex)
{
    if ( isReservedPaletteSchemeName(name) ) {
        // 空名和内置显示名共用保留名错误。
        m_paletteSchemeErrorKey = "ui.toolbar.note_palette.name_reserved_error";
        return false;
    }
    if ( hasPaletteSchemeNameConflict(name, ignoredIndex) ) {
        // 重复错误与保留名分开，便于用户采取不同修正。
        m_paletteSchemeErrorKey = "ui.toolbar.note_palette.name_conflict_error";
        return false;
    }

    // 成功校验清除上一次失败残留。
    m_paletteSchemeErrorKey.clear();
    return true;
}

/// @brief 新建或覆盖保存当前颜色为自定义调色盘方案。
/// @param createNew true 追加新方案，false 覆盖当前可管理方案。
///
/// 保存前构造完整方案并校验名称；覆盖时忽略当前索引自身以允许原名不变。
/// 成功后更新活动索引、选择种类和分拍线覆盖，再保存全局应用配置。
/// 笔记画笔颜色已在编辑过程中实时同步，此处只需刷新分拍线配置和持久化。
///
/// createNew 决定容器结构是否变化。覆盖保持索引稳定，便于尚未刷新列表的
/// UI 状态继续引用目标；新建追加到末尾并激活。两条路径都先构造值快照，
/// 避免校验或容器扩容期间引用 ToolbarView 的可变颜色数组。
///
/// 保存方案本身不改变项目的显式方案名称。项目是否跟随新方案由项目设置
/// 单独决定，避免在软件级方案编辑中产生隐式项目文件修改。
void ToolbarView::savePaletteScheme(bool createNew)
{
    // 直接操作 AppConfig 持有的方案容器，最终由 app.save 一次落盘。
    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().colorPalettes;

    // 从名称缓冲和两套运行时颜色构造值类型快照。
    Config::ColorPaletteScheme scheme          = buildCurrentPaletteScheme();
    bool                       hasActiveScheme = canManageActivePaletteScheme();
    // 覆盖操作要求活动项确实指向自定义方案，内置项无副作用退出。
    if ( !createNew && !hasActiveScheme ) return;

    std::optional<std::size_t> ignoredIndex;
    if ( !createNew && hasActiveScheme ) {
        // 覆盖校验忽略目标自身，其余方案仍必须保持唯一名称。
        ignoredIndex = static_cast<std::size_t>(m_activePaletteSchemeIndex);
    }
    if ( !validatePaletteSchemeNameForSave(scheme.name, ignoredIndex) ) {
        // 错误键已由校验函数设置，弹窗保留用户输入供修正。
        return;
    }

    if ( createNew || !hasActiveScheme ) {
        // 新方案追加到末尾并立即成为活动索引。
        paletteConfig.schemes.push_back(std::move(scheme));
        m_activePaletteSchemeIndex =
            static_cast<int>(paletteConfig.schemes.size() - 1);
    } else {
        // 覆盖保持容器位置和其他方案索引稳定。
        paletteConfig
            .schemes[static_cast<std::size_t>(m_activePaletteSchemeIndex)] =
            std::move(scheme);
    }

    // 配置级最近活动索引与工具栏当前索引保持一致。
    paletteConfig.activeSchemeIndex =
        static_cast<std::size_t>(m_activePaletteSchemeIndex);
    m_activePaletteSelection  = PaletteSelectionKind::Custom;
    m_overrideBeatLinePalette = true;
    // 从已存入容器的最终名称回填输入缓冲，确保移动后仍读取有效对象。
    setPaletteSchemeNameBuffer(
        paletteConfig.schemes[paletteConfig.activeSchemeIndex].name);
    pushBeatLinePaletteToRenderer();
    // 所有运行时与配置字段同步后一次写入应用设置。
    app.save();
}

/// @brief 按用户配置打开原生或内置调色盘导出文件选择器。
///
/// 默认目录沿用上次文件选择位置，建议文件名由当前方案名清理后生成。
/// 原生选择器同步返回并直接执行导出；内置选择器只打开弹窗，后续帧处理结果。
/// 取消选择不显示失败状态，NFD 错误或异常空成功路径才记录失败。
///
/// 两种选择器最终都向 exportCurrentPaletteToPath 传递 UTF-8 路径，扩展名
/// 规范化和实际写入不在此重复实现。原生库返回的路径所有权在处理完成后
/// 立即释放；内置选择器状态由后续 renderPaletteExportFileDialog 关闭。
/// 上一次状态提示在新操作开始时清除，避免用户误把旧结果当成本次结果。
/// @warning 低频用户操作路径：原生对话框会阻塞，仅在点击导出时调用。
void ToolbarView::openPaletteExportFilePicker()
{
    // 空上次路径回退当前目录，避免把空指针语义交给不同选择器实现。
    auto&             app         = Config::AppConfig::instance();
    auto&             settings    = app.getEditorSettings();
    const std::string defaultPath = settings.lastFilePickerPath.empty()
                                        ? std::string(".")
                                        : settings.lastFilePickerPath;
    // 建议名称只包含安全文件名主体和统一扩展名。
    const std::string defaultFileName =
        sanitizePaletteExportFileName(currentPaletteSchemeName());
    // 新操作开始前清除上次成功或失败提示。
    m_paletteExportStatusKey.clear();

    if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 原生对话框打开前播放一次统一弹窗反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t* outPath = nullptr;
        // 过滤器只展示项目调色盘扩展名。
        nfdu8filteritem_t filters[1] = { { "MusicMapMaker Color Palette",
                                           "mmpalette" } };
        const nfdresult_t result     = NativeFileDialog::saveFile(
            &outPath, filters, 1, defaultPath.c_str(), defaultFileName.c_str());
        if ( result == NFD_OKAY && outPath != nullptr ) {
            // 导出函数会规范化扩展名并更新状态与最近目录。
            exportCurrentPaletteToPath(outPath);
            // NFD 分配的 UTF-8 路径在使用后由调用方释放。
            NFD_FreePathU8(outPath);
        } else if ( result == NFD_OKAY ) {
            // 成功状态却没有路径视为异常失败，不能尝试写空路径。
            m_paletteExportSucceeded = false;
            m_paletteExportStatusKey = "ui.toolbar.note_palette.export_failed";
        } else if ( result == NFD_ERROR ) {
            // 记录库错误详情，界面只展示稳定本地化失败提示。
            const char* error = NFD_GetError();
            XERROR("Failed to open color palette export dialog: {}",
                   error ? error : "Unknown NFD error");
            m_paletteExportSucceeded = false;
            m_paletteExportStatusKey = "ui.toolbar.note_palette.export_failed";
        }
        // 原生对话框已完成整个选择流程，不再打开内置弹窗。
        return;
    }

    // 内置对话框保存状态跨帧存在，配置只负责首次打开参数。
    IGFD::FileDialogConfig dialogConfig;
    dialogConfig.path              = defaultPath;
    dialogConfig.countSelectionMax = 1;
    dialogConfig.fileName          = defaultFileName;
    dialogConfig.flags =
        ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
    // 记录旧打开状态，使弹窗音效只在关闭到打开边沿播放一次。
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened("NotePaletteExportPicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "NotePaletteExportPicker",
        TR("ui.toolbar.note_palette.export_dialog_title").data(),
        ".mmpalette",
        dialogConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened("NotePaletteExportPicker") ) {
        // 重复点击已开弹窗不会重复播放反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 按用户配置打开原生或内置调色盘导入文件选择器。
///
/// 成功选取路径后只读取并准备待确认方案，不立即写入自定义方案列表。
/// 原生和内置路径最终汇入 preparePaletteImportFromPath，保持校验与状态一致。
/// 新操作会清除上次导入错误和状态，取消选择则保持无提示。
///
/// 文件格式过滤只帮助用户选择，不替代配置层内容校验。原生返回路径在读取
/// 完成后释放；内置选择器则跨帧保存路径直到 Display 返回确认结果。
/// 导入分成“读取文件”和“确认名称”两阶段，确保冲突方案不会在用户看到
/// 提示之前进入配置容器。
/// @warning 低频用户操作路径：原生对话框会阻塞，仅在点击导入时调用。
void ToolbarView::openPaletteImportFilePicker()
{
    auto&             app         = Config::AppConfig::instance();
    auto&             settings    = app.getEditorSettings();
    const std::string defaultPath = settings.lastFilePickerPath.empty()
                                        ? std::string(".")
                                        : settings.lastFilePickerPath;
    // 新一轮选择先清空名称校验错误和导入结果提示。
    m_paletteImportErrorKey.clear();
    m_paletteImportStatusKey.clear();

    if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 原生弹窗使用专用扩展过滤器并同步返回结果。
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      outPath    = nullptr;
        nfdu8filteritem_t filters[1] = { { "MusicMapMaker Color Palette",
                                           "mmpalette" } };
        const nfdresult_t result     = NativeFileDialog::openFile(
            &outPath, filters, 1, defaultPath.c_str());
        if ( result == NFD_OKAY && outPath != nullptr ) {
            // 文件解析成功后仍需用户在调色盘弹窗中确认名称。
            preparePaletteImportFromPath(outPath);
            // 路径解析结束后释放 NFD 所有权。
            NFD_FreePathU8(outPath);
        } else if ( result == NFD_OKAY ) {
            // 异常空路径不进入文件解析。
            m_paletteImportSucceeded = false;
            m_paletteImportStatusKey = "ui.toolbar.note_palette.import_failed";
        } else if ( result == NFD_ERROR ) {
            // 详细错误写日志，用户界面保持简洁稳定提示。
            const char* error = NFD_GetError();
            XERROR("Failed to open color palette import dialog: {}",
                   error ? error : "Unknown NFD error");
            m_paletteImportSucceeded = false;
            m_paletteImportStatusKey = "ui.toolbar.note_palette.import_failed";
        }
        return;
    }

    // 内置导入对话框只允许单选并隐藏无关类型列。
    IGFD::FileDialogConfig dialogConfig;
    dialogConfig.path              = defaultPath;
    dialogConfig.countSelectionMax = 1;
    dialogConfig.flags =
        ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
    // 音效只对应首次打开边沿，不随每帧 OpenDialog 调用重复。
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened("ColorPaletteImportPicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "ColorPaletteImportPicker",
        TR("ui.toolbar.note_palette.import_dialog_title").data(),
        ".mmpalette",
        dialogConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened("ColorPaletteImportPicker") ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 推进内置调色盘导出对话框并处理确认结果。
/// @param dpiScale 当前窗口内容缩放。
///
/// 原生文件选择器路径不会进入本函数；内置实例打开时使用居中固定尺寸弹窗。
/// 用户确认后导出选定路径，无论确认或取消都关闭对话框实例。
///
/// CenteredModalPopupScope 负责临时样式，Display 的返回值表示本次交互已经
/// 结束而非一定确认。只有 IsOk 为真才执行文件写入；取消和关闭按钮都只
/// 释放对话框状态。尺寸使用逻辑像素，由居中 helper 结合 dpiScale 处理。
/// 本函数每帧可调用，但未打开对应实例时只经过轻量状态检查。
void ToolbarView::renderPaletteExportFileDialog(float dpiScale)
{
    // 居中样式作用域在函数结束时恢复弹窗相关 ImGui 样式。
    Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
    if ( ImGuiFileDialog::Instance()->IsOpened("NotePaletteExportPicker") ) {
        // 仅已打开状态才排队窗口居中，避免影响其他弹窗。
        Utils::prepareCenteredModalWindow({ 600, 400 });
    }
    if ( ImGuiFileDialog::Instance()->Display(
             "NotePaletteExportPicker",
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings,
             { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            // GetFilePathName 返回 UTF-8 完整路径，导出入口会修正扩展名。
            exportCurrentPaletteToPath(
                ImGuiFileDialog::Instance()->GetFilePathName());
        }
        // Display 返回结束状态后统一关闭并释放本次选择器状态。
        ImGuiFileDialog::Instance()->Close();
    }
}

/// @brief 推进内置调色盘导入对话框并读取确认文件。
/// @param dpiScale 当前窗口内容缩放。
///
/// 选择成功只准备待导入方案和名称缓冲，最终写入需在调色盘弹窗再次确认。
///
/// 与导出对话框共用居中和关闭协议，但确认路径进入解析准备阶段。解析错误
/// 只更新导入状态提示，不关闭调色盘主弹层，也不影响当前活动方案。
/// Display 结束后必须调用 Close，否则 ImGuiFileDialog 会保留旧选择并在
/// 后续打开时错误复用结果。
void ToolbarView::renderPaletteImportFileDialog(float dpiScale)
{
    // 导入和导出共用居中尺寸与无保存窗口状态约束。
    Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
    if ( ImGuiFileDialog::Instance()->IsOpened("ColorPaletteImportPicker") ) {
        Utils::prepareCenteredModalWindow({ 600, 400 });
    }
    if ( ImGuiFileDialog::Instance()->Display(
             "ColorPaletteImportPicker",
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings,
             { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            // 文件读取与错误状态由统一准备函数负责。
            preparePaletteImportFromPath(
                ImGuiFileDialog::Instance()->GetFilePathName());
        }
        ImGuiFileDialog::Instance()->Close();
    }
}

/// @brief 将当前调色盘方案序列化到指定 UTF-8 路径。
/// @param path 文件选择器返回的路径。
///
/// 路径扩展名先规范化，再调用配置层原子导出接口；结果转换为稳定状态翻译键。
/// 成功后保存父目录为下次文件选择器起点，失败不改写最近目录。
///
/// 传入路径可以来自原生或内置选择器，也可能缺少扩展名。规范化 helper 只
/// 调整文件名后缀，不创建目录。ColorPaletteScheme 在调用前按值构造，写入
/// 过程不会观察到用户继续编辑颜色造成的中间状态。
/// 状态键只用于 UI 本地化，详细文件错误由配置层日志承担。
/// @warning 低频文件操作路径：仅由用户确认导出后调用。
void ToolbarView::exportCurrentPaletteToPath(const std::string& path)
{
    const std::string normalizedPath = normalizePaletteExportPath(path);
    // 空规范化路径短路，不向配置层传递无效目标。
    const bool succeeded =
        !normalizedPath.empty() &&
        Config::exportColorPaletteFile(Config::utf8ToPath(normalizedPath),
                                       buildCurrentPaletteScheme());
    // 布尔值供提示颜色选择，翻译键供本地化文本选择。
    m_paletteExportSucceeded = succeeded;
    m_paletteExportStatusKey = succeeded
                                   ? "ui.toolbar.note_palette.export_success"
                                   : "ui.toolbar.note_palette.export_failed";
    if ( !succeeded ) {
        // 写入失败时保留用户原最近目录，便于重新选择。
        return;
    }

    auto& app      = Config::AppConfig::instance();
    auto& settings = app.getEditorSettings();
    // 从最终规范化路径提取真实父目录，而非用户原始输入后缀。
    const std::filesystem::path parent =
        Config::utf8ToPath(normalizedPath).parent_path();
    if ( !parent.empty() ) {
        // 相对当前目录且没有父路径时无需把空值写入设置。
        settings.lastFilePickerPath = Config::pathToUtf8(parent);
        app.save();
    }
}

/// @brief 从 UTF-8 路径读取调色盘并准备名称确认状态。
/// @param path 用户选择的调色盘文件路径。
///
/// 配置层负责格式解析和内容校验；成功结果暂存在 optional
/// 中，不立即修改方案列表。
/// 导入名称复制到固定输入缓冲并保证末尾空字符，供用户解决冲突或保留名问题。
/// 成功后保存文件父目录为下次选择起点，解析失败不改变现有活动调色盘。
///
/// pending optional 持有完整值对象，使原文件可在确认弹层出现前移动、删除或
/// 卸载。文件中的名称仅作为初始建议，用户确认前可以修改。UTF-8 缓冲按
/// 字节截断以满足 ImGui InputText 固定容量约束，尾部清零保证 C 字符串安全。
/// 解析失败不会触碰当前活动调色盘和运行时画笔状态。
/// @warning 低频文件操作路径：仅由用户确认文件选择后调用。
void ToolbarView::preparePaletteImportFromPath(const std::string& path)
{
    Config::ColorPaletteScheme importedScheme;
    // 空路径短路；非空路径由配置层读入完整值类型方案。
    const bool succeeded =
        !path.empty() && Config::importColorPaletteFile(
                             Config::utf8ToPath(path), importedScheme);
    // 文件解析阶段只产生总体导入状态，名称错误在确认阶段单独设置。
    m_paletteImportSucceeded = succeeded;
    m_paletteImportStatusKey =
        succeeded ? std::string{} : "ui.toolbar.note_palette.import_failed";
    if ( !succeeded ) {
        // 失败时不覆盖已有 pending，弹窗通过状态键展示错误。
        return;
    }

    // 解析成功后持有待确认方案值，不依赖文件或局部对象生命周期。
    m_pendingImportedPaletteScheme = std::move(importedScheme);
    m_importPaletteSchemeNameBuffer.fill('\0');
    const std::string& importedName = m_pendingImportedPaletteScheme->name;
    // 为结尾空字符保留一个字节，过长名称安全截断。
    const std::size_t copyCount = std::min(
        importedName.size(), m_importPaletteSchemeNameBuffer.size() - 1);
    // 目标缓冲已清零，复制后天然具备终止空字符。
    std::copy_n(importedName.begin(),
                copyCount,
                m_importPaletteSchemeNameBuffer.begin());
    // 新文件成功读取后清除上一次名称校验错误。
    m_paletteImportErrorKey.clear();

    auto& app      = Config::AppConfig::instance();
    auto& settings = app.getEditorSettings();
    // 最近路径使用被读取文件的真实父目录。
    const std::filesystem::path parent = Config::utf8ToPath(path).parent_path();
    if ( !parent.empty() ) {
        settings.lastFilePickerPath = Config::pathToUtf8(parent);
        app.save();
    }
}

/// @brief 校验待导入名称并把方案追加到自定义列表。
///
/// 保留名和重复名分别设置导入错误键，失败时保留 pending 供用户继续修改。
/// 成功后移动完整方案、激活新索引、刷新运行时颜色并保存 AppConfig。
///
/// 导入始终创建新方案，不允许覆盖同名项，以免外部文件在一次确认中静默
/// 替换本地配置。方案移动到容器后立即清空 pending，防止模态弹层重复确认。
/// loadPaletteScheme 负责统一设置活动种类、索引、画笔和分拍线；本函数只在
/// 全部运行时状态同步后保存软件配置。
void ToolbarView::confirmPaletteImport()
{
    // 没有成功解析的待导入方案时确认无副作用。
    if ( !m_pendingImportedPaletteScheme ) return;

    // 固定输入缓冲保证以空字符结束，可安全构造最终名称。
    const std::string name(m_importPaletteSchemeNameBuffer.data());
    if ( isReservedPaletteSchemeName(name) ) {
        // 导入错误使用独立状态，不覆盖普通方案编辑错误。
        m_paletteImportErrorKey = "ui.toolbar.note_palette.name_reserved_error";
        return;
    }
    if ( hasPaletteSchemeNameConflict(name, std::nullopt) ) {
        // 导入总是创建新方案，因此不忽略任何已有索引。
        m_paletteImportErrorKey = "ui.toolbar.note_palette.name_conflict_error";
        return;
    }

    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().colorPalettes;
    // 用户确认名称覆盖文件内原名，再把值移动进配置容器。
    m_pendingImportedPaletteScheme->name = name;
    paletteConfig.schemes.push_back(std::move(*m_pendingImportedPaletteScheme));
    // 新追加元素位于末尾，可直接计算其活动索引。
    const std::size_t importedIndex = paletteConfig.schemes.size() - 1;
    m_pendingImportedPaletteScheme.reset();
    m_paletteImportErrorKey.clear();
    m_paletteImportSucceeded = true;
    m_paletteImportStatusKey = "ui.toolbar.note_palette.import_success";
    // 加载新方案会同步画笔和分拍线渲染配置。
    loadPaletteScheme(importedIndex);
    app.save();
}

/// @brief 将名称输入与两套运行时颜色构造为可序列化方案。
/// @return 包含完整笔记和分拍线槽位的值类型方案。
///
/// 名称空白时 currentPaletteSchemeName
/// 会回退皮肤默认显示名，后续保存校验将拒绝它。
///
/// 返回对象不携带当前选择种类或覆盖标志，文件格式仅描述可复用的颜色方案。
/// 固定数组逐槽转换确保导出不会依赖 glm 的内存布局或序列化实现。
/// 函数无副作用，可供保存和导出在各自操作开始时取得一致快照。
Config::ColorPaletteScheme ToolbarView::buildCurrentPaletteScheme() const
{
    // 先填名称，再逐项转换颜色表示。
    Config::ColorPaletteScheme scheme;
    scheme.name = currentPaletteSchemeName();
    // 定长数组保证方案不会遗漏任何笔记槽位。
    for ( std::size_t i = 0; i < m_paletteColors.size(); ++i ) {
        scheme.noteColors[i] = toStoredColor(m_paletteColors[i]);
    }
    // 分拍线数组独立转换并保持分母槽位顺序。
    for ( std::size_t i = 0; i < m_beatLinePaletteColors.size(); ++i ) {
        scheme.beatLineColors[i] = toStoredColor(m_beatLinePaletteColors[i]);
    }
    return scheme;
}

/// @brief 将当前名称缓冲应用到活动自定义方案。
///
/// 内置或失效活动项不能重命名；名称校验忽略方案自身但拒绝其他冲突。
/// 成功只修改名称与活动索引，不改变方案颜色，并立即保存 AppConfig。
///
/// 项目偏好按名称关联，因此此处不会自动改写任何已打开项目的显式引用；
/// 这与删除方案时清理悬空引用不同。重命名后引用旧名的项目将在下次解析时
/// 安全回退，用户可显式选择新名称。函数保持容器顺序不变，活动索引稳定。
void ToolbarView::renamePaletteScheme()
{
    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().colorPalettes;
    // 先验证选择种类和索引总体有效性。
    if ( !canManageActivePaletteScheme() ) return;

    std::size_t index = static_cast<std::size_t>(m_activePaletteSchemeIndex);
    // 再次检查局部索引，防御配置容器在前一步后发生变化。
    if ( index >= paletteConfig.schemes.size() ) return;

    std::string name = currentPaletteSchemeName();
    // 保持原名合法，其他已有方案仍参与冲突检查。
    if ( !validatePaletteSchemeNameForSave(name, index) ) return;

    paletteConfig.schemes[index].name = name;
    paletteConfig.activeSchemeIndex   = index;
    // 回填缓冲确保截断与配置内最终名称保持一致。
    setPaletteSchemeNameBuffer(name);
    app.save();
}

/// @brief 删除指定自定义方案并修复全局和项目中的失效引用。
/// @param schemeIndex colorPalettes.schemes 中待删除索引。
///
/// 删除后若同名方案已不存在，软件默认引用回退皮肤，当前项目显式引用改为继承。
/// 活动索引夹到剩余范围，运行时先加载皮肤默认并清除应用键以便下一帧重新解析。
/// 应用配置始终保存；只有项目引用被修改时才额外保存项目。
///
/// 删除确认保存的是索引，但执行时仍重新检查范围，防御确认弹层打开期间方案
/// 容器发生变化。旧配置可能含重名项，因此只有最后一个同名方案被删除时才
/// 修复名称引用。运行时不直接加载新的 activeSchemeIndex，项目偏好解析在
/// 下一帧统一决定最终来源，避免软件默认与项目显式选择优先级分叉。
///
/// 项目保存是低频且有条件的：只有当前打开项目确实引用被移除名称时执行。
/// 其他项目文件不在此批量扫描，打开时会通过同一缺失方案回退逻辑处理。
void ToolbarView::deletePaletteScheme(std::size_t schemeIndex)
{
    auto& app           = Config::AppConfig::instance();
    auto& settings      = app.getEditorSettings();
    auto& paletteConfig = settings.colorPalettes;
    // 过期确认索引不修改任何配置。
    if ( schemeIndex >= paletteConfig.schemes.size() ) return;

    // 删除前保存名称，用于检查所有名称关联偏好。
    const std::string deletedName = paletteConfig.schemes[schemeIndex].name;
    paletteConfig.schemes.erase(paletteConfig.schemes.begin() +
                                static_cast<std::ptrdiff_t>(schemeIndex));

    // 虽然正常保存禁止重名，仍兼容旧配置可能包含重复名称。
    const bool deletedNameStillExists =
        std::any_of(paletteConfig.schemes.begin(),
                    paletteConfig.schemes.end(),
                    [&](const Config::ColorPaletteScheme& scheme) {
                        return scheme.name == deletedName;
                    });
    if ( !deletedNameStillExists &&
         settings.defaultColorPaletteSchemeName == deletedName ) {
        // 软件默认不能继续引用不存在名称，改为稳定皮肤默认 ID。
        settings.defaultColorPaletteSchemeName =
            Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
    }

    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    // 当前项目显式方案缺失时清空字段，语义恢复为继承软件默认。
    bool projectChanged = false;
    if ( !deletedNameStillExists && project &&
         project->m_settings.m_colorPaletteSchemeName == deletedName ) {
        project->m_settings.m_colorPaletteSchemeName.clear();
        projectChanged = true;
    }

    if ( paletteConfig.schemes.empty() ) {
        // 空容器没有合法索引，配置约定使用零占位。
        paletteConfig.activeSchemeIndex = 0;
    } else {
        // 优先选择删除位置的后继，末项删除时夹到新末项。
        paletteConfig.activeSchemeIndex =
            std::min(schemeIndex, paletteConfig.schemes.size() - 1);
    }

    // 确认请求、应用缓存和名称错误都与旧方案集合绑定，统一清除。
    m_pendingDeletePaletteSchemeIndex.reset();
    m_lastAppliedProjectPaletteKey.clear();
    m_paletteSchemeErrorKey.clear();
    // 临时回到安全皮肤默认，下一帧按修复后的项目偏好重新应用。
    loadSkinDefaultPalette();
    app.save();
    if ( projectChanged ) {
        // 未修改项目字段时避免不必要的项目保存。
        engine.saveProject();
    }
}

/// @brief 安全复制方案名到固定 ImGui 输入缓冲。
/// @param name 待显示名称。
///
/// 过长 UTF-8 名称按字节截断；缓冲预先清零以保证终止空字符。
///
/// 该缓冲服务于 ImGui 的可变字符接口，容量由类成员固定。按字节截断可能
/// 截在多字节字符内部，但不会越界；后续输入组件和名称校验仍以缓冲内容为准。
/// 函数不改变当前方案或错误状态，调用方负责决定回填时机。
void ToolbarView::setPaletteSchemeNameBuffer(const std::string& name)
{
    m_paletteSchemeNameBuffer.fill('\0');
    // 为结尾空字符保留一个字节。
    std::size_t count =
        std::min(name.size(), m_paletteSchemeNameBuffer.size() - 1);
    std::copy_n(name.begin(), count, m_paletteSchemeNameBuffer.begin());
}

/// @brief 用指定槽位颜色刷新十六进制编辑缓冲和缓存身份。
/// @param tab 笔记或分拍线标签页。
/// @param slotIndex 标签页内槽位索引。
/// @param color 当前槽位颜色。
///
/// tab 与索引用于避免每帧覆盖用户尚未提交的文本输入。
///
/// 缓冲始终写入规范化的八位 RGBA 十六进制形式，因此合法短格式输入在提交后
/// 会展开成统一显示。缓存身份与文本一起更新，标签页或槽位切换时下一帧即可
/// 判断内容是否对应当前颜色。函数只格式化内存，不推送任何颜色命令。
void ToolbarView::setColorHexBuffer(PaletteTab tab, std::size_t slotIndex,
                                    glm::vec4 color)
{
    m_colorHexBuffer.fill('\0');
    // 格式始终为带 Alpha 的固定大写十六进制文本。
    std::string text  = colorToHexString(color);
    std::size_t count = std::min(text.size(), m_colorHexBuffer.size() - 1);
    std::copy_n(text.begin(), count, m_colorHexBuffer.begin());
    // 内容复制完成后再更新身份，避免异常中间状态被视为命中。
    m_colorHexBufferTab  = tab;
    m_colorHexBufferSlot = slotIndex;
}

/// @brief 读取方案名输入缓冲并提供空值显示回退。
/// @return 用户输入名称；空缓冲时返回皮肤默认本地化名称。
///
/// 保存入口仍会将回退名称识别为保留名，因此空输入不能创建方案。
/// 回退值也让直接导出皮肤默认颜色时获得可读文件名建议，而不是空文件名。
/// 返回值按值构造，不把固定缓冲暴露给可能跨帧持有的调用方。
/// 本函数不裁剪首尾空白；名称精确性与冲突判断保持一致。
std::string ToolbarView::currentPaletteSchemeName() const
{
    std::string name(m_paletteSchemeNameBuffer.data());
    // 空名称只用于界面展示友好文本。
    if ( name.empty() ) return defaultPaletteSchemeName();
    return name;
}

/// @brief 同步单个颜色槽到画笔，并可同时应用到当前选择。
/// @param slot 目标笔记颜色槽。
/// @param color 自定义颜色；空值表示恢复自动或默认语义。
/// @param applyToSelection 是否额外修改已选择音符。
///
/// 两个动作分别进入逻辑命令队列，画笔状态始终先更新。
/// optional 的空状态表示移除对象级颜色覆盖，不等价于透明黑色。
/// applyToSelection 为 false 时只影响后续绘制，不遍历或修改当前选择。
/// 为 true 时逻辑层可把选择修改纳入统一撤销语义；UI 不直接接触实体集合。
/// 两条命令均携带颜色值副本，调用后槽位切换不会改变已排队内容。
void ToolbarView::pushColorCommands(Logic::NoteColorSlot     slot,
                                    std::optional<glm::vec4> color,
                                    bool                     applyToSelection)
{
    auto& engine = Logic::EditorEngine::instance();
    // 画笔命令影响后续新建音符。
    engine.pushCommand(Logic::CmdSetBrushNoteColor{ slot, color });
    if ( applyToSelection ) {
        // 选择命令是显式可选副作用，保持撤销入口一致。
        engine.pushCommand(Logic::CmdApplyNoteColorToSelection{ slot, color });
    }
}

/// @brief 绘制调色盘方案管理、导入导出与颜色编辑浮层。
/// @param dpiScale 当前窗口内容缩放。
///
/// 浮层锚定工具栏颜色按钮左侧并夹在主视口内，使用无标题自动尺寸窗口。
/// 上半区管理继承、皮肤默认和自定义方案，下半区按标签页编辑笔记或分拍线槽位。
/// 方案删除和导入名称均通过二次弹窗确认，文件选择器结果由独立函数推进。
/// 颜色编辑同时提供选择器与十六进制文本，只有有效值才向画笔或渲染配置推送。
///
/// 活动选择分为三类：
/// - InheritSoftwareDefault 表示项目未指定方案，运行时跟随软件默认；
/// - SkinDefault 表示显式使用当前皮肤颜色；
/// - Custom 表示活动索引指向软件配置中的用户方案。
/// 选择类别与最终颜色内容分开保存，才能在换肤、修改软件默认或切换项目时
/// 正确重新解析，而不会把一次解析结果误当成固定项目选择。
///
/// 笔记色与分拍线色共享方案生命周期，但拥有独立槽位数组和下游目标。
/// 笔记颜色即时同步画笔，只有明确动作或编辑结束才修改当前选择；分拍线颜色
/// 通过完整 VisualConfig 同步渲染器，并用 overrideBeatLineColors 区分皮肤
/// 语义与显式数组。
///
/// 名称缓冲、十六进制缓冲和导入 pending 都是跨帧 UI 状态。控件活动时不会
/// 用外部值覆盖未完成输入；槽位或标签页变化时则立即刷新规范文本。所有文件
/// 操作都由按钮动作进入专用函数，普通每帧绘制不访问文件系统。
/// @warning UI 热路径：浮层打开时每帧执行；文件 I/O 只能由明确按钮动作触发。
void ToolbarView::renderColorPalettePopup(float dpiScale)
{
    // 关闭状态立即返回，不查询工具栏窗口或构造任何弹窗控件。
    if ( !m_showColorPopup ) return;

    // 通过稳定内部窗口名定位工具栏，以其位置作为浮层锚点。
    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    // 工具栏尚未创建或被隐藏时无法定位浮层。
    if ( !toolbarWindow ) return;

    ImVec2 toolbarPos = toolbarWindow->Pos;

    // 浮层固定在主视口，避免跨多视口时出现在错误显示器。
    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    float          viewportTop    = mainViewport->Pos.y;
    float          viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    float          viewportLeft   = mainViewport->Pos.x;

    // 水平锚点位于工具栏左缘附近，窗口 Pivot 随后使用右上角。
    float targetX = toolbarPos.x - std::floor(4.0f * dpiScale);
    float targetY = m_lastColorBtnY;

    // 首帧使用默认尺寸估计，后续使用上帧测得实际浮层宽高进行夹取。
    float popupW  = m_colorPopupWidth > 0.0f ? m_colorPopupWidth
                                             : std::floor(360.0f * dpiScale);
    float popupH  = m_colorPopupHeight > 0.0f ? m_colorPopupHeight
                                              : std::floor(360.0f * dpiScale);
    float padding = std::floor(8.0f * dpiScale);

    // 右上角 Pivot 要求目标 X 至少容纳完整窗口和左侧安全留白。
    targetX = std::max(targetX, viewportLeft + popupW + padding);
    // Y 轴同时限制在视口上下边界内。
    targetY = std::min(targetY, viewportBottom - popupH - padding);
    targetY = std::max(targetY, viewportTop + padding);

    // 每帧显式设置视口和位置，浮层不参与持久化窗口布局。
    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(targetX, targetY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    // 浮层尺寸由内容自动决定，用户不能单独移动、缩放或保存其位置。
    ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;

    // 项目美学设置转换为 DPI 后 ImGui 样式，仅局部覆盖本浮层。
    auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    float winPadding    = std::floor(aesthetics.windowPadding * dpiScale);
    float winRounding   = std::floor(aesthetics.windowRounding * dpiScale);
    float frameRounding = std::floor(aesthetics.frameRounding * dpiScale);
    float itemSpacing   = std::floor(aesthetics.itemSpacing * dpiScale);

    // 四项样式在函数尾部统一弹出，Begin 返回值不影响栈平衡。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, winRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(winPadding, winPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(itemSpacing, itemSpacing));

    if ( ImGui::Begin("##ColorPalettePopup", nullptr, popupFlags) ) {
        // 标题作为内容绘制，窗口本身不显示系统标题栏。
        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.title").data());
        ImGui::Separator();

        // 方案列表直接读取应用配置，增删改动作负责显式保存。
        auto& paletteConfig =
            Config::AppConfig::instance().getEditorSettings().colorPalettes;
        // 组合框预览文本按选择语义生成，不直接复用名称输入缓冲。名称缓冲可能
        // 包含用户尚未保存的草稿，而预览必须始终描述当前已经应用的方案。
        // 自定义索引在读取前重新检查范围，配置容器变化不会造成越界。
        std::string previewName;
        if ( m_activePaletteSelection ==
             PaletteSelectionKind::InheritSoftwareDefault ) {
            // 继承态显示语义名称，而不是当前被继承方案名称。
            previewName = inheritedPaletteSchemeName();
        } else if ( m_activePaletteSelection ==
                    PaletteSelectionKind::SkinDefault ) {
            // 皮肤默认同样使用本地化内置显示名。
            previewName = defaultPaletteSchemeName();
        } else {
            // 自定义态只有合法索引才能读取配置容器名称。
            std::size_t activeIndex =
                m_activePaletteSchemeIndex >= 0
                    ? static_cast<std::size_t>(m_activePaletteSchemeIndex)
                    : paletteConfig.schemes.size();
            // 索引过期时回退皮肤默认文本，避免越界访问。
            previewName = activeIndex < paletteConfig.schemes.size()
                              ? paletteConfig.schemes[activeIndex].name
                              : defaultPaletteSchemeName();
        }

        // 方案选择区展示来源语义，管理区只对 Custom 选择开放。
        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.scheme").data());
        ImGui::SameLine();
        // 方案组合框使用固定 DPI 宽度，保证弹窗布局稳定。
        ImGui::SetNextItemWidth(std::floor(210.0f * dpiScale));
        if ( ::MMM::UI::FeedbackBeginCombo("##ColorPaletteScheme",
                                           previewName.c_str()) ) {
            // 继承与皮肤默认作为两个内置项，始终位于自定义列表之前。
            const bool inheritSelected =
                m_activePaletteSelection ==
                PaletteSelectionKind::InheritSoftwareDefault;
            if ( ::MMM::UI::FeedbackSelectable(
                     inheritedPaletteSchemeName().c_str(), inheritSelected) ) {
                // 选择后立即解析软件默认并同步下游颜色。
                loadSoftwareDefaultPalette();
            }
            if ( inheritSelected ) ImGui::SetItemDefaultFocus();

            // 皮肤默认项按选择种类显示勾选。
            const bool skinSelected =
                m_activePaletteSelection == PaletteSelectionKind::SkinDefault;
            if ( ::MMM::UI::FeedbackSelectable(
                     defaultPaletteSchemeName().c_str(), skinSelected) ) {
                // 内置选择不需要保存自定义方案活动索引。
                loadSkinDefaultPalette();
            }
            if ( skinSelected ) ImGui::SetItemDefaultFocus();

            if ( !paletteConfig.schemes.empty() ) {
                // 分隔内置选项和用户方案，提高列表层次。
                ImGui::Separator();
            }
            // 自定义方案按配置顺序展示，选择后保存最近活动索引。
            for ( std::size_t i = 0; i < paletteConfig.schemes.size(); ++i ) {
                bool selected =
                    m_activePaletteSelection == PaletteSelectionKind::Custom &&
                    m_activePaletteSchemeIndex == static_cast<int>(i);
                if ( ::MMM::UI::FeedbackSelectable(
                         paletteConfig.schemes[i].name.c_str(), selected) ) {
                    // 加载先同步运行时颜色，再保存应用配置索引。
                    loadPaletteScheme(i);
                    Config::AppConfig::instance().save();
                }
                if ( selected ) ImGui::SetItemDefaultFocus();
            }
            ::MMM::UI::FeedbackEndCombo();
        }

        // 名称区是后续保存、新建、重命名和导出的共同输入来源。
        ImGui::TextUnformatted(
            TR("ui.toolbar.note_palette.scheme_name").data());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(210.0f * dpiScale));
        // 名称编辑本身不重命名方案，需点击保存、新建或重命名提交。
        if ( ImGui::InputText("##ColorPaletteSchemeName",
                              m_paletteSchemeNameBuffer.data(),
                              m_paletteSchemeNameBuffer.size()) ) {
            // 用户修改后清除旧校验错误，等待下一次提交重新验证。
            m_paletteSchemeErrorKey.clear();
        }

        // 方案动作按钮使用统一尺寸，管理动作按当前选择有效性禁用。
        // 固定按钮尺寸让管理动作在翻译变化时仍形成稳定网格。
        const float schemeButtonH   = std::floor(24.0f * dpiScale);
        const float schemeButtonW   = std::floor(78.0f * dpiScale);
        const bool  canManageScheme = canManageActivePaletteScheme();
        // 覆盖保存只适用于现有自定义方案。
        ImGui::BeginDisabled(!canManageScheme);
        // 新建始终可用，但名称仍需通过保留名和冲突校验。
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.save_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            savePaletteScheme(false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.new_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            // 新建忽略当前选择种类，总是追加通过校验的新方案。
            savePaletteScheme(true);
        }
        ImGui::SameLine();
        // 重命名只修改当前自定义方案名称。
        ImGui::BeginDisabled(!canManageScheme);
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.rename_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            // 重命名保持颜色和容器位置不变。
            renamePaletteScheme();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        // 删除需要合法活动索引并进入确认弹窗。
        ImGui::BeginDisabled(!canManageScheme);
        if ( ::MMM::UI::FeedbackButton(TR("ui.common.delete").data(),
                                       ImVec2(schemeButtonW, schemeButtonH)) ) {
            // 暂存稳定于当前配置快照的索引，确认后再次执行范围检查。
            m_pendingDeletePaletteSchemeIndex =
                static_cast<std::size_t>(m_activePaletteSchemeIndex);
            ::MMM::UI::FeedbackOpenPopup("DeleteColorPaletteConfirm");
        }
        ImGui::EndDisabled();
        // 导入与导出操作独立于是否当前为自定义方案。
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.import_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            // 文件读取完成后仍需在模态弹层确认最终名称。
            openPaletteImportFilePicker();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.export_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            // 任意活动来源都可导出为完整独立方案文件。
            openPaletteExportFilePicker();
        }

        // 删除弹窗阻止误触，正文明确该操作会移除方案及相关引用。
        // 删除确认弹层保存待删索引而不是方案引用。确认期间容器若变化，删除
        // 函数会再次检查索引边界；取消则清空 optional，下一帧不会重复打开。
        // 弹层不是模态窗口，但入口位于主调色盘内，其他方案动作仍受 ImGui
        // 弹窗输入捕获规则约束。
        if ( ImGui::BeginPopup("DeleteColorPaletteConfirm") ) {
            ImGui::TextWrapped(
                "%s",
                TR("ui.toolbar.note_palette.delete_scheme_confirm").data());
            // 确认和取消保持相同宽度，降低误触风险。
            const float confirmButtonW = std::floor(92.0f * dpiScale);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.common.confirm").data(),
                     ImVec2(confirmButtonW, schemeButtonH)) ) {
                // 先复制 optional 再关闭弹窗，随后删除可能重建方案状态。
                auto pendingIndex = m_pendingDeletePaletteSchemeIndex;
                ImGui::CloseCurrentPopup();
                if ( pendingIndex ) {
                    deletePaletteScheme(*pendingIndex);
                }
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.common.cancel").data(),
                     ImVec2(confirmButtonW, schemeButtonH)) ) {
                // 取消只清除待删索引，不修改配置。
                m_pendingDeletePaletteSchemeIndex.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        // 成功读取文件后自动打开名称确认弹窗，已有弹窗不重复 OpenPopup。
        // pending 只在文件解析成功后存在，并驱动名称确认弹窗打开。
        if ( m_pendingImportedPaletteScheme &&
             !ImGui::IsPopupOpen("ImportColorPaletteRename") ) {
            ::MMM::UI::FeedbackOpenPopup("ImportColorPaletteRename");
        }
        // 导入名称弹窗保持模态，直到用户确认合法名称或取消 pending。
        if ( ImGui::BeginPopupModal("ImportColorPaletteRename",
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoSavedSettings) ) {
            // 用户可覆盖文件内方案名以解决本地冲突。
            ImGui::TextUnformatted(
                TR("ui.toolbar.note_palette.import_rename_prompt").data());
            ImGui::SetNextItemWidth(std::floor(240.0f * dpiScale));
            if ( ImGui::InputText("##ImportedColorPaletteName",
                                  m_importPaletteSchemeNameBuffer.data(),
                                  m_importPaletteSchemeNameBuffer.size()) ) {
                // 名称变化清除旧错误，确认时重新完整校验。
                m_paletteImportErrorKey.clear();
            }
            if ( !m_paletteImportErrorKey.empty() ) {
                // 名称错误使用危险色，并按当前语言即时翻译稳定键。
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      Utils::UIThemeUtils::getDangerColor());
                ImGui::TextWrapped("%s",
                                   TR(m_paletteImportErrorKey.c_str()).data());
                ImGui::PopStyleColor();
            }

            // 确认失败时弹层保持打开，允许直接修正名称再提交。
            const float importButtonW = std::floor(92.0f * dpiScale);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.toolbar.note_palette.import_scheme").data(),
                     ImVec2(importButtonW, schemeButtonH)) ) {
                // 校验失败保留 pending 和弹窗，成功后 optional 被清空。
                confirmPaletteImport();
                if ( !m_pendingImportedPaletteScheme ) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.common.cancel").data(),
                     ImVec2(importButtonW, schemeButtonH)) ) {
                // 取消放弃已读取方案和名称错误，不影响当前活动方案。
                m_pendingImportedPaletteScheme.reset();
                m_paletteImportErrorKey.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        // 普通方案保存或重命名错误显示在主浮层中。
        // 三类状态分别占用独立成员，避免一次操作覆盖另一操作的反馈。
        if ( !m_paletteSchemeErrorKey.empty() ) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  Utils::UIThemeUtils::getDangerColor());
            ImGui::TextWrapped("%s",
                               TR(m_paletteSchemeErrorKey.c_str()).data());
            ImGui::PopStyleColor();
        }
        // 导出成功使用普通文本色，失败临时切换危险色。
        // 导出与导入提示都保存稳定翻译键和成功布尔值。语言切换时文本即时
        // 重新翻译，成功态使用普通主题色，失败态使用危险色。提示只反映最近
        // 一次对应操作，不改变方案选择或颜色编辑状态。
        if ( !m_paletteExportStatusKey.empty() ) {
            if ( !m_paletteExportSucceeded ) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      Utils::UIThemeUtils::getDangerColor());
            }
            ImGui::TextWrapped("%s",
                               TR(m_paletteExportStatusKey.c_str()).data());
            if ( !m_paletteExportSucceeded ) {
                ImGui::PopStyleColor();
            }
        }
        // 导入文件阶段或最终确认结果使用独立状态提示。
        if ( !m_paletteImportStatusKey.empty() ) {
            if ( !m_paletteImportSucceeded ) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      Utils::UIThemeUtils::getDangerColor());
            }
            ImGui::TextWrapped("%s",
                               TR(m_paletteImportStatusKey.c_str()).data());
            if ( !m_paletteImportSucceeded ) {
                ImGui::PopStyleColor();
            }
        }

        ImGui::Separator();

        // 标签页只切换当前编辑槽位集合，不修改任何颜色。
        // 标签页只改变编辑焦点，不影响活动方案和方案名。
        // TabBar 的稳定 ID 与翻译标签分离。标签切换只更新 PaletteTab，具体
        // 活动槽位分别保存在 m_activeColorSlot 和 m_activeBeatLineColorSlot，
        // 因此来回切换能恢复各页最后一次编辑位置。
        if ( ImGui::BeginTabBar("##ColorPaletteTabs") ) {
            if ( ImGui::BeginTabItem(
                     TR("ui.toolbar.note_palette.note_tab").data()) ) {
                // 活动标签页成员供十六进制缓冲身份和后续编辑分支使用。
                m_activePaletteTab = PaletteTab::Note;
                ImGui::EndTabItem();
            }
            if ( ImGui::BeginTabItem(
                     TR("ui.toolbar.note_palette.beat_line_tab").data()) ) {
                m_activePaletteTab = PaletteTab::BeatLine;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        // 所有色块使用统一 DPI 后方形尺寸。
        const float swatchSize = std::floor(24.0f * dpiScale);
        if ( m_activePaletteTab == PaletteTab::Note ) {
            // 笔记标签页按固定槽位枚举顺序展示色块和本地化名称。
            for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
                // 数组索引与 NoteColorSlot 连续枚举保持一一对应。
                auto slot   = static_cast<Logic::NoteColorSlot>(i);
                bool active = slot == m_activeColorSlot;
                ImGui::PushID(static_cast<int>(i));
                if ( ::MMM::UI::FeedbackColorButton(
                         "##SlotColor",
                         toImVec4(m_paletteColors[i]),
                         ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoPicker |
                             ImGuiColorEditFlags_AlphaPreviewHalf,
                         ImVec2(swatchSize, swatchSize)) ) {
                    // 点击色块或同行文本都会切换当前编辑槽位。
                    m_activeColorSlot = slot;
                }
                ImGui::SameLine();
                if ( ::MMM::UI::FeedbackSelectable(
                         TR(colorSlotLabelKey(slot)).data(),
                         active,
                         0,
                         ImVec2(std::floor(150.0f * dpiScale), swatchSize)) ) {
                    m_activeColorSlot = slot;
                }
                ImGui::PopID();
            }
        } else {
            // 分拍线标签页使用数组索引映射具体拍分母名称。
            for ( std::size_t i = 0; i < m_beatLinePaletteColors.size(); ++i ) {
                // 分拍线槽位使用索引，因为其标签由分母映射 helper 提供。
                const bool active = i == m_activeBeatLineColorSlot;
                ImGui::PushID(static_cast<int>(i));
                if ( ::MMM::UI::FeedbackColorButton(
                         "##BeatLineSlotColor",
                         toImVec4(m_beatLinePaletteColors[i]),
                         ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoPicker |
                             ImGuiColorEditFlags_AlphaPreviewHalf,
                         ImVec2(swatchSize, swatchSize)) ) {
                    m_activeBeatLineColorSlot = i;
                }
                ImGui::SameLine();
                if ( ::MMM::UI::FeedbackSelectable(
                         TR(beatLineColorSlotLabelKey(i)).data(),
                         active,
                         0,
                         ImVec2(std::floor(150.0f * dpiScale), swatchSize)) ) {
                    m_activeBeatLineColorSlot = i;
                }
                ImGui::PopID();
            }
        }

        ImGui::Separator();

        // 当前标签页决定活动数组、槽位索引和颜色变化的下游目标。
        // 以下输入控件通过活动引用复用两类颜色的编辑流程。
        const bool        editingNote = m_activePaletteTab == PaletteTab::Note;
        const std::size_t activeSlotIndex =
            editingNote ? colorSlotIndex(m_activeColorSlot)
                        : m_activeBeatLineColorSlot;
        // 两套数组都返回可写引用，后续编辑路径可共享控件逻辑。
        // 引用仅在本帧使用，数组和槽位均为 ToolbarView 稳定成员。
        glm::vec4& activeColor = editingNote
                                     ? m_paletteColors[activeSlotIndex]
                                     : m_beatLinePaletteColors[activeSlotIndex];
        // 切换槽位或输入框未激活时从实际颜色刷新十六进制缓存。
        if ( m_colorHexBufferTab != m_activePaletteTab ||
             m_colorHexBufferSlot != activeSlotIndex ||
             !m_colorHexInputActive ) {
            setColorHexBuffer(m_activePaletteTab, activeSlotIndex, activeColor);
        }

        // 选择器显示模式属于 UI 偏好，不写入方案文件。
        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.color_mode").data());
        ImGui::SameLine();
        // RGB 与 HSV 只改变选择器显示模式，不转换或修改当前颜色。
        if ( ::MMM::UI::FeedbackRadioButton("RGB##ColorPaletteMode",
                                            !m_colorPickerUseHsv) ) {
            m_colorPickerUseHsv = false;
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackRadioButton("HSV##ColorPaletteMode",
                                            m_colorPickerUseHsv) ) {
            m_colorPickerUseHsv = true;
        }

        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.hex").data());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(148.0f * dpiScale));
        // 文本框拒绝空白字符，解析器仍保留首尾空白兼容供其他调用使用。
        // InputText 的即时返回值允许在完整合法文本出现时立即预览。
        bool hexChanged = ImGui::InputText("##PaletteColorHex",
                                           m_colorHexBuffer.data(),
                                           m_colorHexBuffer.size(),
                                           ImGuiInputTextFlags_CharsNoBlank);
        // 活动标志阻止每帧颜色回写覆盖用户尚未完成的输入。
        m_colorHexInputActive = ImGui::IsItemActive();
        // 即时解析仅在完整合法格式出现时改变颜色。用户输入中间态如单个井号
        // 或不足六位字符保持原色，且不显示错误打断编辑；失活时再恢复规范文本。
        if ( hexChanged ) {
            glm::vec4 parsedColor;
            if ( parseHexColor(m_colorHexBuffer.data(), parsedColor) ) {
                // 输入达到合法格式后即时预览，并同步对应运行时目标。
                activeColor = parsedColor;
                if ( editingNote ) {
                    // 笔记颜色拖动期间只更新画笔，不反复修改已有选择。
                    pushPaletteToBrush();
                } else {
                    // 分拍线颜色属于视觉配置，需要启用覆盖并立即提交。
                    m_overrideBeatLinePalette = true;
                    pushBeatLinePaletteToRenderer();
                }
            }
        }
        // 文本编辑结束时规范化显示，并对笔记选择提交最终颜色。
        if ( ImGui::IsItemDeactivatedAfterEdit() ) {
            glm::vec4 parsedColor;
            if ( parseHexColor(m_colorHexBuffer.data(), parsedColor) ) {
                activeColor = parsedColor;
                // 合法输入统一格式化为大写且包含 Alpha 的文本。
                setColorHexBuffer(
                    m_activePaletteTab, activeSlotIndex, activeColor);
                if ( editingNote ) {
                    // 仅在编辑结束时把最终笔记颜色应用到当前选择。
                    pushPaletteToSelection();
                } else {
                    m_overrideBeatLinePalette = true;
                    pushBeatLinePaletteToRenderer();
                }
            } else {
                // 非法输入不改变实际颜色，并恢复对应合法文本。
                setColorHexBuffer(
                    m_activePaletteTab, activeSlotIndex, activeColor);
            }
            m_colorHexInputActive = false;
        }

        // 颜色选择器始终提供 Alpha，并按用户模式显示 RGB 或 HSV 数值。
        // Alpha 始终可编辑，Display 标志只决定通道呈现方式。
        ImGuiColorEditFlags pickerFlags =
            ImGuiColorEditFlags_AlphaBar |
            ImGuiColorEditFlags_AlphaPreviewHalf |
            (m_colorPickerUseHsv ? ImGuiColorEditFlags_DisplayHSV
                                 : ImGuiColorEditFlags_DisplayRGB);

        // 取色器直接绑定活动数组元素，拖动自然保留本地连续状态。下游更新策略
        // 仍按标签页区分：画笔命令轻量即时，选择修改延迟到手势结束；分拍线
        // 需要即时刷新完整视觉配置以同步主画布和预览区。
        if ( ImGui::ColorPicker4(
                 "##PaletteColorPicker", &activeColor.r, pickerFlags) ) {
            // 连续选择器变化即时更新画笔或分拍线渲染预览。
            if ( editingNote ) {
                pushPaletteToBrush();
            } else {
                m_overrideBeatLinePalette = true;
                pushBeatLinePaletteToRenderer();
            }
        }
        if ( editingNote && ImGui::IsItemDeactivatedAfterEdit() ) {
            // 笔记选择只接收选择器手势结束时的最终颜色。
            pushPaletteToSelection();
        }

        ImGui::Separator();

        // 默认色快捷按钮与动作按钮使用统一 DPI 后高度。
        const float buttonH = std::floor(26.0f * dpiScale);
        if ( editingNote ) {
            // 笔记页附加皮肤默认槽位和选择应用动作。
            ImGui::TextUnformatted(
                TR("ui.toolbar.note_palette.skin_defaults").data());

            // 一排色块展示当前皮肤所有笔记槽位默认值。
            for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
                if ( i > 0 ) ImGui::SameLine();
                auto slot = static_cast<Logic::NoteColorSlot>(i);
                // 每帧读取当前皮肤值，换肤后无需重建弹层状态。
                auto defaultColor = toVec4(skinColorForSlot(slot));
                ImGui::PushID(static_cast<int>(i + 100));
                if ( ::MMM::UI::FeedbackColorButton(
                         "##SkinDefaultColor",
                         toImVec4(defaultColor),
                         ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoPicker |
                             ImGuiColorEditFlags_AlphaPreviewHalf,
                         ImVec2(swatchSize, swatchSize)) ) {
                    // 点击默认色同时切换槽位、恢复颜色并应用到画笔与选择。
                    m_activeColorSlot  = slot;
                    m_paletteColors[i] = defaultColor;
                    pushPaletteToBrush();
                    pushPaletteToSelection();
                }
                if ( ImGui::IsItemHovered() ) {
                    // 紧凑色块通过悬浮提示说明对应槽位。
                    drawTooltip(TR(colorSlotLabelKey(slot)).data());
                }
                ImGui::PopID();
            }

            // 显式按钮把完整当前调色盘应用到所有选择音符。
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.toolbar.note_palette.apply_selected").data(),
                     ImVec2(std::floor(140.0f * dpiScale), buttonH)) ) {
                pushPaletteToSelection();
            }
            ImGui::SameLine();
            // 清除单槽自定义使用空 optional 命令恢复对象默认语义。
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.toolbar.note_palette.clear_custom").data(),
                     ImVec2(std::floor(140.0f * dpiScale), buttonH)) ) {
                // 清除后本地预览恢复皮肤色，逻辑命令使用空 optional 表达继承。
                auto slot   = m_activeColorSlot;
                activeColor = toVec4(skinColorForSlot(slot));
                pushColorCommands(slot, std::nullopt, true);
            }

            // 全谱清理只在明确确认后入队，单次动作可撤销且不修改当前画笔。
            // 此入口只出现在物件页，不影响分拍线颜色或方案配置。
            // 保持按钮与上方两项动作同宽，防止调色浮层横向扩张。
            // 确认后清理已有对象；当前槽位选择及未来画笔颜色不变。
            // 只有用户点击才构造命令，浮层每帧绘制不访问谱面数据。
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.toolbar.note_palette.clear_all_notes").data(),
                     ImVec2(std::floor(288.0f * dpiScale), buttonH)) ) {
                ::MMM::UI::FeedbackOpenPopup("ClearAllNoteColorsConfirm");
            }
            if ( ImGui::BeginPopup("ClearAllNoteColorsConfirm") ) {
                // 当前谱面所有正式物件都会被修改，因此确认文案明确给出范围。
                // 弹窗借用本浮层的输入捕获，取消时不触发任何逻辑命令。
                ImGui::TextWrapped(
                    "%s",
                    TR("ui.toolbar.note_palette.clear_all_notes_confirm")
                        .data());
                if ( ::MMM::UI::FeedbackButton(
                         TR("ui.common.confirm").data()) ) {
                    // UI 只发送命令；逻辑层在消费时解析活动会话和注册表。
                    // 不在界面线程扫描 Note，避免大谱面下阻塞浮层绘制。
                    Logic::EditorEngine::instance().pushCommand(
                        Logic::CmdClearAllNoteColorOverrides{});
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                // 取消只关闭确认弹窗，调色盘及当前颜色编辑状态保持不变。
                if ( ::MMM::UI::FeedbackButton(
                         TR("ui.common.cancel").data()) ) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        } else {
            // 分拍线页附加每槽皮肤默认和整体恢复动作。
            ImGui::TextUnformatted(
                TR("ui.toolbar.note_palette.beat_line_skin_defaults").data());
            // 分拍线默认色块同样按槽位顺序横向排列。
            for ( std::size_t i = 0; i < m_beatLinePaletteColors.size(); ++i ) {
                if ( i > 0 ) ImGui::SameLine();
                // 皮肤 helper 包含旧键名兼容和槽位回退。
                const glm::vec4 defaultColor = toVec4(skinBeatLineColor(i));
                ImGui::PushID(static_cast<int>(i + 200));
                if ( ::MMM::UI::FeedbackColorButton(
                         "##SkinDefaultBeatLineColor",
                         toImVec4(defaultColor),
                         ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoPicker |
                             ImGuiColorEditFlags_AlphaPreviewHalf,
                         ImVec2(swatchSize, swatchSize)) ) {
                    // 单槽恢复仍启用覆盖，以保留其他自定义分拍线颜色。
                    m_activeBeatLineColorSlot  = i;
                    m_beatLinePaletteColors[i] = defaultColor;
                    m_overrideBeatLinePalette  = true;
                    pushBeatLinePaletteToRenderer();
                }
                if ( ImGui::IsItemHovered() ) {
                    drawTooltip(TR(beatLineColorSlotLabelKey(i)).data());
                }
                ImGui::PopID();
            }

            // 整体恢复皮肤分拍线会覆盖所有槽，但仍显式写入视觉配置。
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.toolbar.note_palette.use_skin_beat_lines").data(),
                     ImVec2(std::floor(180.0f * dpiScale), buttonH)) ) {
                fillBeatLinePaletteWithSkinDefaults(m_beatLinePaletteColors);
                m_overrideBeatLinePalette = true;
                pushBeatLinePaletteToRenderer();
            }
        }

        // 保存本帧实际尺寸，下一帧定位时用于视口边界夹取。
        ImVec2 sz          = ImGui::GetWindowSize();
        m_colorPopupWidth  = sz.x;
        m_colorPopupHeight = sz.y;
    }
    // Begin 无论是否返回可绘制内容都必须对应 End。
    ImGui::End();

    // 恢复窗口圆角、内边距、框圆角和项目间距四项局部样式。
    ImGui::PopStyleVar(4);
}

/// @brief 绘制带自定义图标和可选短标签的工具栏按钮。
/// @param icon 图标字体文本。
/// @param id 隐藏的稳定 ImGui 按钮 ID。
/// @param shortLabel 图标下方可选短标签。
/// @param width 按钮屏幕宽度。
/// @param height 按钮屏幕高度。
/// @param showLabel 是否绘制短标签。
/// @return 本帧按钮被点击时返回 true。
///
/// FeedbackButton 只创建背景和交互，图标与文字按实际按钮矩形手工居中绘制。
///
/// 图标和短标签不参与 ImGui 布局，因此按钮命中区始终覆盖完整矩形，字体
/// 基线或翻译文本变化不会改变相邻控件位置。两层文本分别使用 pure_icons
/// 与 menu 语义字体；任一字体缺失时只跳过对应装饰，不影响按钮交互。
///
/// 调用方负责在进入前压入活动/非活动背景色，并在返回后绘制 tooltip。
/// 此函数只读取按钮最终矩形和主题文本色，不修改字体栈或颜色栈。
/// @warning UI 热路径：每个工具栏按钮每帧调用，只执行字体测量与绘制提交。
bool ToolbarView::drawIconButton(const char* icon, const char* id,
                                 const char* shortLabel, float width,
                                 float height, bool showLabel) const
{
    // 先提交完整按钮命中区，再使用其最终矩形叠加内容。
    const bool clicked = ::MMM::UI::FeedbackButton(id, ImVec2(width, height));

    // 图标和短标签使用皮肤语义字体，缺失时对应层跳过绘制。
    Config::SkinManager& skinCfg   = Config::SkinManager::instance();
    ImFont*              iconFont  = skinCfg.getFont("pure_icons");
    ImFont*              labelFont = skinCfg.getFont("menu");
    ImDrawList*          drawList  = ImGui::GetWindowDrawList();
    const ImVec2         minPos    = ImGui::GetItemRectMin();
    const ImVec2         maxPos    = ImGui::GetItemRectMax();
    const ImU32          textCol   = ImGui::GetColorU32(ImGuiCol_Text);
    const float          dpiScale =
        Config::AppConfig::instance().getWindowContentScale();

    // 图标层和标签层都裁剪在按钮矩形所属窗口绘制列表内。这里不额外
    // PushClipRect， 因为 ImGui
    // 窗口已有内容裁剪；尺寸计算保证正常字形位于命中区内。
    if ( icon && iconFont ) {
        // 显示标签时缩小图标并上移，为底部文本留出空间。
        const float iconFontSize =
            showLabel ? std::floor(width * 0.58f) : ImGui::GetFontSize();
        // 不换行测量完整图标字形。
        const ImVec2 iconSize = iconFont->CalcTextSizeA(
            iconFontSize, std::numeric_limits<float>::max(), 0.0f, icon);
        const float  iconTopPadding = showLabel ? std::floor(4.0f * dpiScale)
                                                : (height - iconSize.y) * 0.5f;
        const ImVec2 iconPos        = {
            minPos.x + (width - iconSize.x) * 0.5f,
            minPos.y + iconTopPadding,
        };
        // 直接绘制不会改变后续工具栏 ImGui 布局游标。
        drawList->AddText(iconFont, iconFontSize, iconPos, textCol, icon);
    }

    // 短标签只在所有前置条件满足时绘制。空翻译结果不会占据虚假文本高度，
    // 缺失菜单字体也不会回退图标字体，避免把普通文字解释为图标码点。
    if ( showLabel && shortLabel && shortLabel[0] != '\0' && labelFont ) {
        // 仅同时满足显示开关、非空文本和可用字体时绘制标签。
        const float labelFontSize =
            std::floor(std::min(width * 0.38f, ImGui::GetFontSize() * 0.72f));
        const ImVec2 labelSize = labelFont->CalcTextSizeA(
            labelFontSize, std::numeric_limits<float>::max(), 0.0f, shortLabel);
        // 标签水平居中并贴近按钮底部内边距。
        const ImVec2 labelPos = {
            minPos.x + (width - labelSize.x) * 0.5f,
            maxPos.y - labelSize.y - std::floor(3.0f * dpiScale),
        };
        drawList->AddText(
            labelFont, labelFontSize, labelPos, textCol, shortLabel);
    }

    return clicked;
}

/// @brief 绘制普通编辑工具按钮并处理工具切换与画布焦点恢复。
/// @param icon 工具图标。
/// @param tool 对应逻辑编辑工具。
/// @param tooltip 本地化悬浮说明。
/// @param width 按钮宽度。
/// @param height 按钮高度。
/// @param shortLabel 可选短标签。
/// @param showLabel 是否显示短标签。
/// @param sourceManager UI 管理器，用于恢复时间轴焦点。
///
/// 活动工具使用 ButtonActive 固定色，非活动工具使用透明主题样式。
/// 切换 ColorBrush 时先同步当前调色盘，再通过菜单命令分发统一工具变更。
///
/// m_currentTool 是本帧显示快照，权威状态由 update 开头从应用服务同步。
/// 点击非活动按钮先更新本地快照以立即反馈，再向逻辑层投递 CmdChangeTool。
/// 重复点击普通工具不会退出工具；只有 Layout 工具有显式再次点击恢复语义。
///
/// 焦点恢复只针对上一帧已经聚焦的 TimelineWindow，避免点击工具栏后编辑器
/// 键盘输入永久离开画布，同时不抢占文件管理器、设置页等其他窗口焦点。
/// @warning UI 热路径：每个普通工具按钮每帧调用，不得查询文件或阻塞。
void ToolbarView::drawToolButton(const char* icon, Logic::EditTool tool,
                                 const char* tooltip, float width, float height,
                                 const char* shortLabel, bool showLabel,
                                 UIManager* sourceManager)
{
    // 活动态来自引擎同步后的当前工具成员。
    bool isActive = (m_currentTool == tool);

    if ( isActive ) {
        // 三种按钮状态固定同色，鼠标经过不会掩盖当前工具语义。
        ImVec4 activeCol = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
    } else {
        // 非活动按钮仅在悬浮或按下时通过公共样式显示背景。
        Utils::UIThemeUtils::pushTransparentButtonStyles();
    }

    // 工具枚举加入 ID 栈，所有按钮可复用相同隐藏局部 ID。
    ImGui::PushID(static_cast<int>(tool));
    // 点击检测与装饰绘制由 drawIconButton 统一完成；工具切换逻辑只消费返回值。
    // PushID 中的枚举保证不同工具即使共享图标或翻译文本也拥有独立状态。
    const bool clicked = drawIconButton(
        icon, "##ToolbarToolButton", shortLabel, width, height, showLabel);
    if ( sourceManager ) {
        // 所有可见普通工具共享一个语义目标，Spotlight 会合并为完整选择区。
        // 只有实际点击 Draw 才完成；Draw 已激活时也可点击自身或按“知道了”。
        auto& spotlight = sourceManager->walkthroughSpotlight();
        spotlight.reportLastItem("compose.toolbar.tool-selection");
        if ( clicked && tool == Logic::EditTool::Draw )
            spotlight.completeTarget("compose.toolbar.tool-selection");
        // 后续编辑先切到拖拽工具，续写时再返回绘制工具；每个按钮只上报
        // 自己对应的目标，避免整个工具组都被误认成正确选择。
        // 通用绘制工具选择步骤仍沿用旧目标，维护旧教程阶段的完成条件。
        // 新目标只在当前按钮绘制时报告，聚光灯不会高亮相邻的工具按钮。
        // 用户若已选中目标工具，可由步骤的确认入口继续而不伪造点击。
        if ( tool == Logic::EditTool::Move ) {
            spotlight.reportLastItem("compose.toolbar.select-move-tool");
            if ( clicked )
                spotlight.completeTarget("compose.toolbar.select-move-tool");
        }
        if ( tool == Logic::EditTool::Draw ) {
            spotlight.reportLastItem("compose.toolbar.select-draw-tool-edit");
            if ( clicked )
                spotlight.completeTarget(
                    "compose.toolbar.select-draw-tool-edit");
        }
    }
    if ( clicked ) {
        if ( m_currentTool != tool ) {
            // 重复点击活动普通工具无副作用，不产生重复逻辑命令。
            m_currentTool = tool;
            if ( tool == Logic::EditTool::ColorBrush ) {
                // 颜色画笔激活前保证逻辑层拥有最新完整调色盘。
                pushPaletteToBrush();
            }
            // 使用菜单公共分发入口保持快捷键和工具栏行为一致。
            MenuUtil::dispatchCommand(Logic::CmdChangeTool{ tool });
            // UIManager 仅用于可选焦点恢复，不参与工具命令所有权。
            if ( sourceManager ) {
                auto* timeline = sourceManager->getCanvasView("TimelineWindow");
                if ( timeline && timeline->wasFocusedLastFrame() ) {
                    // 仅原本聚焦时间轴时请求恢复，避免抢走其他窗口焦点。
                    timeline->requestFocus();
                }
            }
        }
    }
    ImGui::PopID();

    // 工具提示在基础说明后追加当前配置对应的快捷键文本。
    // 快捷键提示每帧从当前设置格式化，用户修改绑定后无需重建 ToolbarView。
    std::string tooltipText = tooltip ? tooltip : "";
    const auto& settings    = Config::AppConfig::instance().getEditorSettings();
    std::string shortcutText = ShortcutUtils::formatShortcut(
        ShortcutUtils::getToolShortcut(settings, tool));
    if ( !shortcutText.empty() ) {
        // 空绑定不显示空括号。
        tooltipText += " (";
        tooltipText += shortcutText;
        tooltipText += ")";
    }
    drawTooltip(tooltipText.c_str());

    // 两种样式分支都保证压入三项颜色，统一恢复。
    ImGui::PopStyleColor(3);
}

/// @brief 绘制布局工具按钮并实现再次点击返回前一工具的切换语义。
/// @param width 按钮宽度。
/// @param height 按钮高度。
/// @param showLabel 是否显示短标签。
///
/// 首次进入记录前一工具并打开布局弹窗；活动时再次点击恢复前一工具并关闭弹窗。
/// 布局按钮还记录屏幕 Y 坐标，供设置浮层在同一行锚定。
///
/// 若历史工具本身是 Layout，则恢复目标规范为 Move，防止退出动作仍停留在
/// 布局模式。进入布局时关闭音效工具，因为两个宽弹层共享工具栏左侧锚定区。
/// 其他小弹层已由 update 中的互斥规则管理，不在此重复清理。
/// 此函数与普通工具按钮使用相同三态样式栈和平面图标绘制入口。
/// @param sourceManager 提供实际可见按钮的引导登记入口。
/// @warning UI 热路径：Layout 按钮可见时每帧执行，只提交轻量绘制与点击命令。
void ToolbarView::drawLayoutButton(float width, float height, bool showLabel,
                                   UIManager* sourceManager)
{
    // 活动态样式与普通工具按钮保持一致。
    const bool isActive = m_currentTool == Logic::EditTool::Layout;
    if ( isActive ) {
        const ImVec4 activeCol =
            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
    } else {
        Utils::UIThemeUtils::pushTransparentButtonStyles();
    }

    ImGui::PushID("LayoutTool");
    // Layout 使用专用 ID 和短标签，图标按钮本身不理解进入/退出双态语义。
    const bool clicked = drawIconButton(ICON_MMM_TRACK_LAYOUT,
                                        "##ToolbarLayoutButton",
                                        TR("ui.toolbar.short.layout").data(),
                                        width,
                                        height,
                                        showLabel);
    if ( clicked ) {
        // 默认点击进入 Layout，活动态点击则计算恢复目标。
        // nextTool 先以进入目标初始化，活动态分支再替换为恢复目标。
        Logic::EditTool nextTool = Logic::EditTool::Layout;
        if ( isActive ) {
            nextTool          = m_toolBeforeLayout == Logic::EditTool::Layout
                                    ? Logic::EditTool::Move
                                    : m_toolBeforeLayout;
            m_showLayoutPopup = false;
        } else {
            // 保存进入前工具，并关闭与布局浮层互斥的音效工具。
            m_toolBeforeLayout    = m_currentTool;
            m_showLayoutPopup     = true;
            m_showSoundEffectTool = false;
        }
        m_currentTool = nextTool;
        // 视图成员和逻辑引擎通过同一命令保持同步。
        MenuUtil::dispatchCommand(Logic::CmdChangeTool{ nextTool });
    }
    if ( sourceManager ) {
        // 个性化路线直接突出布局工具；已有活动工具时允许确认跳过点击。
        // 仅从其它工具进入布局模式的点击才能自动完成此入口步骤。
        auto& spotlight = sourceManager->walkthroughSpotlight();
        spotlight.reportLastItem("personalization.editor.layout-tool");
        if ( clicked && !isActive )
            spotlight.completeTarget("personalization.editor.layout-tool",
                                     true);
    }
    // 在弹窗渲染前缓存本帧按钮顶部屏幕坐标。
    m_lastLayoutBtnY = ImGui::GetItemRectMin().y;
    ImGui::PopID();
    drawTooltip(TR("ui.toolbar.layout").data());
    ImGui::PopStyleColor(3);
}

/// @brief 绘制画布滚动和物件放置吸附设置浮层。
/// @param dpiScale 当前窗口内容缩放。
///
/// 浮层锚定磁吸按钮并夹在主视口内，提供滚动吸附、放置吸附、模式和常用分拍。
/// 每项配置修改通过应用服务立即生效并保存，避免关闭弹窗时丢失离散开关变化。
///
/// 滚动吸附与物件放置吸附互相独立。放置吸附启用后可选择只使用当前分拍
/// 或使用 commonBeatDivisorMask 中的多个常用分拍。位掩码的编码和范围由
/// Config helper 维护，UI 不假设具体位偏移。
///
/// 此弹层只有离散交互，没有连续拖动，所以每次变化可以立即持久化，不需要
/// 脏标记。局部 editorConfig 在一次绘制中连续更新，后续控件可观察前一项
/// 刚提交的新状态，而不会从 AppConfig 旧快照覆盖它。
///
/// 四列表格覆盖 Config 声明的完整合法分母闭区间。复选框 ID 同时包含可见
/// 分数和数值后缀，因此翻译不参与身份。关闭放置吸附只隐藏详细选项，不清空
/// 模式和掩码，用户重新启用时可以恢复此前组合。
///
/// 弹层尺寸随详细选项显隐变化，每帧记录自动尺寸，下一帧重新夹取主视口。
/// 工具栏缺失时不清除显示标志，允许窗口恢复后继续显示原设置状态。
/// @warning UI 热路径：仅浮层打开时执行，不得引入谱面遍历或阻塞操作。
void ToolbarView::renderMagnetPopup(float dpiScale)
{
    // 关闭状态不定位工具栏也不读取配置。
    if ( !m_showMagnetPopup ) return;

    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    // 工具栏不可见时缺少稳定锚点，本帧跳过浮层。
    if ( !toolbarWindow ) return;

    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    const float    viewportTop    = mainViewport->Pos.y;
    const float    viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    const float    viewportLeft   = mainViewport->Pos.x;
    const float    padding        = std::floor(8.0f * dpiScale);
    // 用上帧实际尺寸夹取位置，首帧采用安全估计值。
    const float popupW  = m_magnetPopupWidth > 0.0f
                              ? m_magnetPopupWidth
                              : std::floor(280.0f * dpiScale);
    const float popupH  = m_magnetPopupHeight > 0.0f
                              ? m_magnetPopupHeight
                              : std::floor(360.0f * dpiScale);
    float       targetX = toolbarWindow->Pos.x - std::floor(4.0f * dpiScale);
    float       targetY = m_lastMagnetBtnY;
    // 右上角 Pivot 要求目标 X 足以容纳整个窗口左侧范围。
    targetX                = std::max(targetX, viewportLeft + popupW + padding);
    const float minTargetY = viewportTop + padding;
    const float maxTargetY =
        std::max(minTargetY, viewportBottom - popupH - padding);
    // 高度超过视口时 maxTargetY 退化到顶部安全边界。
    targetY = std::clamp(targetY, minTargetY, maxTargetY);

    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(targetX, targetY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    // 无标题自动尺寸浮层不写入 ImGui 布局配置。
    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;
    // AppConfig 同时提供美学设置和待修改编辑器快照；外观引用在本帧只读。
    auto&       appConfig  = Config::AppConfig::instance();
    const auto& aesthetics = appConfig.getEditorSettings().aesthetics;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        std::floor(aesthetics.windowRounding * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(aesthetics.windowPadding * dpiScale),
               std::floor(aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                        std::floor(aesthetics.frameRounding * dpiScale));

    if ( ImGui::Begin("##MagnetToolPopup", nullptr, popupFlags) ) {
        // 所有控件读取同一份编辑器配置副本，修改后整份提交。
        ImGui::TextUnformatted(TR("ui.magnet_tool.title").data());
        ImGui::Separator();

        auto editorConfig = appConfig.getEditorConfig();
        /// @brief 应用当前局部配置快照并立即保存软件设置。
        ///
        /// 局部对象在每次修改后保留新值，使同帧后续控件在一致快照上继续编辑。
        auto persistConfig = [&]() {
            updateEditorConfig(editorConfig);
            appConfig.save();
        };

        // 滚动画布吸附与物件放置吸附为独立开关。
        // 先复制到 bool 以适配 ImGui 可写指针接口。
        bool scrollSnap = editorConfig.settings.scrollSnap;
        if ( ::MMM::UI::FeedbackCheckbox(
                 TR("ui.magnet_tool.scroll_canvas_snap").data(),
                 &scrollSnap) ) {
            // 写回局部完整配置，再通过统一闭包提交。
            editorConfig.settings.scrollSnap = scrollSnap;
            persistConfig();
        }

        // 放置吸附关闭时隐藏其模式和分母细节，但保留已存配置。
        // 放置吸附是下方模式控件的显示前置条件。
        bool objectPlacementSnap = editorConfig.settings.objectPlacementSnap;
        if ( ::MMM::UI::FeedbackCheckbox(
                 TR("ui.magnet_tool.object_placement_snap").data(),
                 &objectPlacementSnap) ) {
            editorConfig.settings.objectPlacementSnap = objectPlacementSnap;
            persistConfig();
        }

        if ( objectPlacementSnap ) {
            // 模式单选仅在总开关启用时可操作。
            ImGui::Separator();
            // 局部 mode 随单选变化更新，控制下方常用分拍表格显隐。
            auto mode = editorConfig.settings.objectPlacementSnapMode;
            if ( ::MMM::UI::FeedbackRadioButton(
                     TR("ui.magnet_tool.current_beat_lines").data(),
                     mode == Config::ObjectPlacementSnapMode::
                                 CurrentBeatDivisor) ) {
                // 当前分拍模式不清空常用分拍掩码，切回时恢复用户选择。
                mode = Config::ObjectPlacementSnapMode::CurrentBeatDivisor;
                editorConfig.settings.objectPlacementSnapMode = mode;
                persistConfig();
            }
            if ( ::MMM::UI::FeedbackRadioButton(
                     TR("ui.magnet_tool.common_beat_lines").data(),
                     mode == Config::ObjectPlacementSnapMode::
                                 CommonBeatDivisors) ) {
                // 常用分拍模式激活已有掩码，不自动增删任何分母。
                mode = Config::ObjectPlacementSnapMode::CommonBeatDivisors;
                editorConfig.settings.objectPlacementSnapMode = mode;
                persistConfig();
            }

            if ( mode == Config::ObjectPlacementSnapMode::CommonBeatDivisors ) {
                // 常用分拍模式允许在固定范围内逐分母启用或关闭。
                ImGui::Spacing();
                ImGui::TextDisabled(
                    "%s", TR("ui.magnet_tool.common_beat_lines_hint").data());
                if ( ImGui::BeginTable("##CommonBeatDivisorSelection",
                                       4,
                                       ImGuiTableFlags_SizingFixedFit |
                                           ImGuiTableFlags_NoSavedSettings) ) {
                    // 四列表格紧凑展示完整合法分母范围。
                    for ( int divisor = Config::COMMON_BEAT_DIVISOR_MIN;
                          divisor <= Config::COMMON_BEAT_DIVISOR_MAX;
                          ++divisor ) {
                        // TableNextColumn 自动按四列循环换行，无需手工
                        // SameLine。
                        ImGui::TableNextColumn();
                        // 位掩码访问统一交给 Config
                        // helper，避免手工位偏移错误。
                        bool selected = Config::isCommonBeatDivisorEnabled(
                            editorConfig.settings.commonBeatDivisorMask,
                            divisor);
                        char label[32];
                        std::snprintf(label,
                                      sizeof(label),
                                      "1/%d##CommonBeatDivisor%d",
                                      divisor,
                                      divisor);
                        if ( ::MMM::UI::FeedbackCheckbox(label, &selected) ) {
                            // 修改单一位后立即提交完整配置快照。
                            Config::setCommonBeatDivisorEnabled(
                                editorConfig.settings.commonBeatDivisorMask,
                                divisor,
                                selected);
                            persistConfig();
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }

        // 缓存实际尺寸供下一帧视口边界夹取。
        // 自动尺寸包含当前展开模式，切换后下一帧会用新高度重新夹取。
        const ImVec2 size   = ImGui::GetWindowSize();
        m_magnetPopupWidth  = size.x;
        m_magnetPopupHeight = size.y;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

/// @brief 绘制分拍线显示模式与光标邻域参数浮层。
/// @param dpiScale 当前窗口内容缩放。
///
/// 三种显示模式立即保存；NearCursor
/// 的连续范围滑条即时应用、编辑结束后合并保存。
/// 浮层关闭时若仍有未提交连续值，执行一次兜底保存并清除脏标记。
///
/// BeatLineDisplayModeHistory 保存最近一次非隐藏模式，使工具栏快捷键能在
/// Hidden 和用户偏好的可见模式间切换。弹层中的显式单选会更新该历史；
/// 邻域百分比只在 NearCursor 模式显示，但切换模式不会丢弃其配置值。
///
/// 可见范围和淡出范围以 0..1 存储、以百分比展示。拖动时立即更新渲染配置
/// 以获得视觉反馈，磁盘写入推迟到控件失活或弹层关闭。两个滑条共享脏标记，
/// 任何一个结束都保存当前完整 AppConfig 快照。
///
/// Always 和 Hidden 模式不销毁 NearCursor 参数，仅折叠对应控件。再次切回时
/// 使用上次保存比例。模式按钮属于离散操作，会先同步历史、立即保存并清除
/// 连续脏标志；此时 AppConfig 已包含此前滑条通过应用服务更新的最新值。
///
/// 关闭边沿的兜底保存处理窗口因互斥弹层、工具隐藏或其他状态变化直接消失的
/// 情况，不能只依赖 ImGui 控件失活事件。无脏值时快速返回不产生文件 I/O。
/// @warning UI 热路径：只在浮层打开时更新控件，不得引入谱面遍历。
void ToolbarView::renderBeatLinePopup(float dpiScale)
{
    // 关闭边沿负责提交可能因异常关闭未收到 Deactivated 的滑条变化。
    if ( !m_showBeatLinePopup ) {
        if ( m_beatLinePopupConfigDirty ) {
            // 只在确有连续编辑脏值时落盘。
            Config::AppConfig::instance().save();
            m_beatLinePopupConfigDirty = false;
        }
        return;
    }

    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    // 工具栏缺失时保留脏状态，下一帧关闭或恢复后再保存。
    if ( !toolbarWindow ) return;

    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    const float    viewportTop    = mainViewport->Pos.y;
    const float    viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    const float    viewportLeft   = mainViewport->Pos.x;
    const float    padding        = std::floor(8.0f * dpiScale);
    // 首帧以估计尺寸定位，之后使用实际缓存尺寸避免越界。
    const float popupW     = m_beatLinePopupWidth > 0.0f
                                 ? m_beatLinePopupWidth
                                 : std::floor(260.0f * dpiScale);
    const float popupH     = m_beatLinePopupHeight > 0.0f
                                 ? m_beatLinePopupHeight
                                 : std::floor(220.0f * dpiScale);
    float       targetX    = toolbarWindow->Pos.x - std::floor(4.0f * dpiScale);
    float       targetY    = m_lastBeatLineBtnY;
    targetX                = std::max(targetX, viewportLeft + popupW + padding);
    const float minTargetY = viewportTop + padding;
    const float maxTargetY =
        std::max(minTargetY, viewportBottom - popupH - padding);
    // 垂直位置夹在主视口安全留白范围。
    targetY = std::clamp(targetY, minTargetY, maxTargetY);

    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(targetX, targetY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    // 自动尺寸会随 NearCursor 额外控件变化，位置使用上帧尺寸稳定修正。
    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;
    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        std::floor(aesthetics.windowRounding * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(aesthetics.windowPadding * dpiScale),
               std::floor(aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                        std::floor(aesthetics.frameRounding * dpiScale));

    if ( ImGui::Begin("##BeatLineDisplayModePopup", nullptr, popupFlags) ) {
        // 当前模式读取一次，单选闭包成功后同步更新局部值。
        ImGui::TextUnformatted(TR("ui.toolbar.beat_lines.title").data());
        ImGui::Separator();

        auto& appConfig = Config::AppConfig::instance();
        auto  mode      = appConfig.getVisualConfig().beatLineDisplayMode;
        /// @brief 绘制显示模式单选并提交离散配置变化。
        auto selectMode = [&](Config::BeatLineDisplayMode candidate,
                              const char*                 labelKey) {
            // candidate 与翻译键由调用点成对传入，闭包只处理统一状态转换。
            if ( !::MMM::UI::FeedbackRadioButton(TR(labelKey).data(),
                                                 mode == candidate) ) {
                // 未点击候选项不复制或保存配置。
                return;
            }
            // 只修改视觉模式字段后通过应用服务提交完整快照。
            auto updatedConfig = appConfig.getEditorConfig();
            updatedConfig.visual.beatLineDisplayMode = candidate;
            updateEditorConfig(updatedConfig);
            // 记录显式用户选择，供模式历史恢复逻辑使用。
            // 历史对象忽略 Hidden 或按自身规则记录最近可恢复模式。
            m_beatLineDisplayModeHistory.observe(candidate);
            appConfig.save();
            // 离散模式已经立即保存，不属于连续滑条脏状态。
            m_beatLinePopupConfigDirty = false;
            mode                       = candidate;
        };

        // 单选顺序与用户理解的可见程度一致：始终、邻近、隐藏。
        selectMode(Config::BeatLineDisplayMode::Always,
                   "ui.toolbar.beat_lines.always");
        selectMode(Config::BeatLineDisplayMode::NearCursor,
                   "ui.toolbar.beat_lines.near_cursor");
        selectMode(Config::BeatLineDisplayMode::Hidden,
                   "ui.toolbar.beat_lines.hidden");

        if ( mode == Config::BeatLineDisplayMode::NearCursor ) {
            // 只有邻近光标模式需要展示可见和淡出范围比例。
            ImGui::Separator();
            // 提示在固定内容宽度内换行，避免撑宽自动尺寸窗口。
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                   std::floor(230.0f * dpiScale));
            ImGui::TextDisabled("%s",
                                TR("ui.toolbar.beat_lines.auto_hint").data());
            ImGui::PopTextWrapPos();

            // 存储使用 0 至 1 比例，UI 滑条转换为更直观百分数。
            // 每帧从 AppConfig 最新快照读取，外部设置变化会立即反映。
            float visiblePercent =
                appConfig.getVisualConfig().beatLineCursorVisibleRatio * 100.0f;
            ImGui::TextUnformatted(
                TR("ui.toolbar.beat_lines.visible_range").data());
            ImGui::SetNextItemWidth(std::floor(230.0f * dpiScale));
            // 5% 下限保证光标附近始终有明确实线区域，50% 上限避免覆盖全屏。
            if ( ::MMM::UI::FeedbackSliderFloat("##BeatLineVisibleRange",
                                                &visiblePercent,
                                                5.0f,
                                                50.0f,
                                                "%.0f%%") ) {
                // 拖动期间即时提交到运行时，但暂不每帧写配置文件。
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual.beatLineCursorVisibleRatio =
                    visiblePercent * 0.01f;
                updateEditorConfig(updatedConfig);
                // 脏标记等待滑条结束或弹窗关闭时合并保存。
                m_beatLinePopupConfigDirty = true;
            }
            if ( ImGui::IsItemDeactivatedAfterEdit() &&
                 m_beatLinePopupConfigDirty ) {
                // 滑条手势完成后只保存一次最终值。
                appConfig.save();
                m_beatLinePopupConfigDirty = false;
            }

            // 淡出范围沿用相同百分比转换和延迟保存策略。
            // 淡出区从可见区边缘向外过渡，配置范围独立于可见范围。
            float fadePercent =
                appConfig.getVisualConfig().beatLineCursorFadeRatio * 100.0f;
            ImGui::TextUnformatted(
                TR("ui.toolbar.beat_lines.fade_range").data());
            ImGui::SetNextItemWidth(std::floor(230.0f * dpiScale));
            // 淡出范围允许更小起点，最大 40% 为边缘保留完全透明区域。
            if ( ::MMM::UI::FeedbackSliderFloat("##BeatLineFadeRange",
                                                &fadePercent,
                                                2.0f,
                                                40.0f,
                                                "%.0f%%") ) {
                // 从最新配置构造副本，保留上方滑条同帧已经应用的值。
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual.beatLineCursorFadeRatio =
                    fadePercent * 0.01f;
                updateEditorConfig(updatedConfig);
                m_beatLinePopupConfigDirty = true;
            }
            if ( ImGui::IsItemDeactivatedAfterEdit() &&
                 m_beatLinePopupConfigDirty ) {
                appConfig.save();
                m_beatLinePopupConfigDirty = false;
            }
        }

        // 实际尺寸缓存用于下一帧准确夹取锚定位置。
        // 缓存当前模式对应高度，下一帧在视口底部重新夹取。
        const ImVec2 size     = ImGui::GetWindowSize();
        m_beatLinePopupWidth  = size.x;
        m_beatLinePopupHeight = size.y;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

/// @brief 绘制布局工具关联的拖动、笔记、背景、频谱和轨道视觉设置。
/// @param dpiScale 当前窗口内容缩放。
///
/// 浮层仅在 Layout 工具活动时存在；离开工具或关闭浮层会提交所有连续编辑脏值。
/// 离散开关和组合框立即保存，滑条及颜色选择器即时应用并在手势结束后合并保存。
/// 各视觉分组提供局部恢复默认按钮，只重置对应配置职责而不覆盖其他设置。
/// 轨道数量从活动谱面读取，并在持有会话锁的短作用域内完成。
///
/// 配置同步约定如下：
/// - 离散复选框、组合框和恢复按钮在变化时立即应用并保存；
/// - 普通视觉滑条在拖动期间只应用，控件失活后保存；
/// - 组件和频谱颜色在取色器打开期间连续应用，关闭边沿兜底保存；
/// - 弹层或 Layout 工具关闭时提交仍处于脏状态的最终值；
/// - 每次应用都从 AppConfig 最新快照构造完整 EditorConfig，避免旧快照
///   覆盖同一帧中其他设置入口刚刚修改的字段。
///
/// 组件布局按轨道数保存独立配置。活动谱面缺失时使用零轨道键，已有谱面
/// 则把非法或过时的非正轨道数规范为一。此函数只读取谱面元数据，不持有
/// Beatmap、Session 或音频对象的长期引用。
///
/// 颜色持久化存在两层兜底：ColorPicker 控件正常失活时立即保存，所有取色器
/// 从打开变为关闭时再检查一次，整个 Layout 弹层关闭时最后检查成员脏标志。
/// 每层保存后都清除标志，因此同一最终值不会重复写入。
///
/// 恢复默认操作直接调用 EditorConfig 的职责化 reset 函数或组件布局 reset，
/// 不在 UI 中复制默认常量。频谱组件额外恢复专用尺寸字段，是因为这些字段不
/// 属于通用 placement。所有恢复动作立即应用并保存，覆盖未完成连续编辑状态。
/// 个性化引导借用每个显隐 Checkbox 的真实 Item 边界作为备用目标。
/// 画布组件已显示时由画布目标优先；显隐选项仍通过整个弹层的独立亮区可读。
/// 两处几何不能在本函数合并，否则中间窗口也会被照亮并误导拖拽位置。
/// 弹层的屏幕位置和大小每帧重算，因为工具栏可移动、DPI 和展开组会改变。
/// 当前目标若被弹层滚动裁掉，只请求下一帧滚到该项，不使用裁剪外旧坐标。
/// @warning UI 热路径：浮层打开时每帧执行；会话锁只允许短时读取元数据。
/// @param sourceManager 提供组件开关的引导备用目标。
void ToolbarView::renderLayoutPopup(float dpiScale, UIManager* sourceManager)
{
    // 浮层关闭或工具切走时提交未完成连续编辑，并清理颜色选择器状态。
    if ( !m_showLayoutPopup || m_currentTool != Logic::EditTool::Layout ) {
        if ( m_layoutComponentColorDirty ) {
            // 组件颜色编辑可能因浮层关闭未触发控件失活，需兜底保存。
            Config::AppConfig::instance().save();
            m_layoutComponentColorDirty = false;
        }
        if ( m_layoutSpectrumConfigDirty ) {
            // 频谱连续参数使用独立脏标记，避免无变化保存。
            Config::AppConfig::instance().save();
            m_layoutSpectrumConfigDirty = false;
        }
        if ( m_layoutVisualConfigDirty ) {
            // 普通视觉滑条的最终值在关闭边沿统一持久化。
            Config::AppConfig::instance().save();
            m_layoutVisualConfigDirty = false;
        }
        // 关闭后丢弃颜色选择器打开态，重新进入时由控件状态重建。
        m_layoutComponentColorPickerOpen = false;
        return;
    }

    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    // 找不到工具栏锚点时保持脏标记，不提前丢失待保存状态。
    if ( !toolbarWindow ) return;

    // Layout 设置只在主视口内出现：组件拖拽也发生在主编辑画布，跨视口弹出会
    // 破坏按钮与弹层的空间关联。安全留白同时用于四条边；右上角 Pivot 意味着
    // X 下限必须包含整个弹层宽度，Y 则在顶部和“底部减高度”之间夹取。
    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    const float    viewportTop    = mainViewport->Pos.y;
    const float    viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    const float    viewportLeft   = mainViewport->Pos.x;
    const float    padding        = std::floor(8.0f * dpiScale);
    // 使用上帧实际尺寸或首帧估计值计算主视口内位置。
    const float popupW     = m_layoutPopupWidth > 0.0f
                                 ? m_layoutPopupWidth
                                 : std::floor(260.0f * dpiScale);
    const float popupH     = m_layoutPopupHeight > 0.0f
                                 ? m_layoutPopupHeight
                                 : std::floor(100.0f * dpiScale);
    float       targetX    = toolbarWindow->Pos.x - std::floor(4.0f * dpiScale);
    float       targetY    = m_lastLayoutBtnY;
    targetX                = std::max(targetX, viewportLeft + popupW + padding);
    const float minTargetY = viewportTop + padding;
    const float maxTargetY =
        std::max(minTargetY, viewportBottom - popupH - padding);
    // 垂直锚点与按钮对齐，并夹取到视口安全范围。
    targetY = std::clamp(targetY, minTargetY, maxTargetY);

    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(targetX, targetY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    // 自动尺寸由展开的折叠分组决定。窗口不可移动或缩放，也不写 imgui.ini，
    // 因为它的生命周期绑定 Layout 工具而非独立工作区面板。上一帧尺寸仅用于
    // 边界估计，本帧结束会更新为实际值。
    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;
    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        std::floor(aesthetics.windowRounding * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(aesthetics.windowPadding * dpiScale),
               std::floor(aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                        std::floor(aesthetics.frameRounding * dpiScale));

    if ( ImGui::Begin("##LayoutComponentsPopup", nullptr, popupFlags) ) {
        // 所有设置分组共享同一个 AppConfig 实例和应用服务提交入口。
        ImGui::TextUnformatted(TR("ui.toolbar.layout_settings").data());
        ImGui::Separator();

        auto& appConfig = Config::AppConfig::instance();
        // 默认无谱面时轨道数为零，相关控件可据此禁用或隐藏。
        std::int32_t keyCount = 0;
        {
            // 会话互斥只覆盖指针获取和 track_count 读取，不包围 UI 绘制。
            auto& engine = Logic::EditorEngine::instance();
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            const auto session = engine.getActiveSession();
            if ( session && session->getContext().currentBeatmap ) {
                // 合法谱面轨道数至少按一处理，避免下游范围退化。
                keyCount = std::max(
                    1,
                    session->getContext()
                        .currentBeatmap->m_baseMapMetadata.track_count);
            }
        }
        {
            // 禁止垂直拖动属于编辑器设置，不是视觉配置。
            auto& dragSettings = appConfig.getEditorSettings();
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.toolbar.layout_disable_vertical_object_drag")
                         .data(),
                     &dragSettings.disableVerticalObjectDrag) ) {
                // 引用字段已直接修改 AppConfig，随后广播完整配置并保存。
                updateEditorConfig(appConfig.getEditorConfig());
                appConfig.save();
            }
            if ( ImGui::IsItemHovered() ) {
                // 详细行为通过悬浮提示说明，主标签保持简洁。
                drawTooltip(
                    TR("ui.toolbar.layout_disable_vertical_object_drag_hint")
                        .data());
            }
        }
        ImGui::Separator();

        // 公共尺寸供各折叠分组中的颜色和滑条控件复用。
        // 颜色按钮、控制宽度和恢复按钮列共同定义各分组的视觉网格。恢复按钮
        // 宽度按当前翻译文本测量，颜色列相对它定位，因此语言切换后仍对齐。
        // 视觉滑条采用统一宽度，自动尺寸弹层不会随当前数值文本抖动。
        const float colorButtonSize    = std::floor(22.0f * dpiScale);
        const float visualControlWidth = std::floor(230.0f * dpiScale);
        const auto  resetButtonLabel = TR("ui.toolbar.layout_component_reset");
        const float resetButtonWidth =
            ImGui::CalcTextSize(resetButtonLabel.data()).x +
            ImGui::GetStyle().FramePadding.x * 2.0f;
        // 聚合颜色选择器打开态，用于决定连续颜色保存边沿。
        bool anyColorPickerOpen = false;
        /// @brief 把局部 VisualConfig 快照合并回完整编辑器配置并应用。
        const auto applyVisualConfig = [&](const Config::VisualConfig& visual) {
            // 每次从最新 EditorConfig 复制后只替换 visual，保留音效、快捷键和
            // 其他设置在同一帧可能产生的变化。应用服务负责广播新快照。
            auto updatedConfig   = appConfig.getEditorConfig();
            updatedConfig.visual = visual;
            updateEditorConfig(updatedConfig);
        };
        /// @brief 在当前 ImGui 控件结束编辑后保存普通视觉脏值。
        const auto saveVisualAfterEdit = [&]() {
            // helper 必须紧跟产生当前 ImGui item 的滑条调用；插入其他控件后
            // IsItemDeactivatedAfterEdit 会查询错误对象并遗漏保存边沿。
            if ( ImGui::IsItemDeactivatedAfterEdit() &&
                 m_layoutVisualConfigDirty ) {
                // 连续拖动期间只应用内存配置，失活边沿一次落盘。
                appConfig.save();
                m_layoutVisualConfigDirty = false;
            }
        };
        /// @brief 绘制分组右对齐恢复按钮并执行对应配置重置函数。
        const auto drawRenderingResetButton = [&](const char* id,
                                                  auto&&      resetConfig) {
            // id
            // 区分音符与背景分组，虽然两个按钮显示相同本地化文本。resetConfig
            // 接收完整 EditorConfig，使配置类型自身的默认重置函数维护关联字段。
            ImGui::PushID(id);
            // 按钮在可用空间足够时靠右，窄窗口保持当前起点避免负偏移。
            const float remainingWidth = ImGui::GetContentRegionAvail().x;
            if ( remainingWidth > resetButtonWidth ) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remainingWidth -
                                     resetButtonWidth);
            }
            if ( ::MMM::UI::FeedbackSmallButton(resetButtonLabel.data()) ) {
                // 重置闭包只改对应字段，随后同步 AppConfig 和应用服务。
                auto updatedConfig = appConfig.getEditorConfig();
                resetConfig(updatedConfig);
                appConfig.getEditorConfig() = updatedConfig;
                updateEditorConfig(updatedConfig);
                appConfig.save();
                // 重置已立即保存，清除可能来自此前滑条的脏标记。
                // 完整重置已经保存，旧滑条手势留下的脏标记不能再次写过时值。
                m_layoutVisualConfigDirty = false;
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(TR("ui.toolbar.layout_render_reset_hint").data());
            }
            ImGui::PopID();
        };

        // 音符渲染分组只修改与音符外观直接相关的 VisualConfig 字段。
        //
        // 分组字段职责：
        // - noteScaleX 与 noteScaleY 分别控制宽高，不强制保持比例；
        // - nonHoldHitEffectDuration 只影响非长按音符的命中特效寿命；
        // - showBoundSampleLabels 控制绑定采样的辅助文字；
        // - noteFillMode 决定纹理如何适配音符几何；
        // - defaultColorPaletteSchemeName 是软件新项目继承来源。
        //
        // 前三项连续值使用“即时应用、失活保存”，离散项立即保存。默认调色盘
        // 是软件设置引用，不会改变当前项目已经显式选择的方案，也不会把当前
        // 画笔颜色应用到已有选择。恢复按钮只调用 note rendering 默认重置入口。
        if ( ::MMM::UI::FeedbackCollapsingHeader(
                 TR("ui.settings.visual.note").data()) ) {
            // 恢复默认保留背景、画布组件和其他编辑器设置。
            drawRenderingResetButton("NoteRenderingReset",
                                     [](Config::EditorConfig& config) {
                                         config.resetNoteRenderingToDefaults();
                                     });
            // 每个连续控件前重取快照，防止前一控件应用后被旧值覆盖。
            auto visual = appConfig.getVisualConfig();
            // 横向缩放影响音符宽度，范围限制为默认尺寸的 0.5..3 倍。
            ImGui::TextUnformatted(
                TR("ui.settings.visual.note_scale_x").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat("##LayoutNoteScaleX",
                                                &visual.noteScaleX,
                                                0.5f,
                                                3.0f,
                                                "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();

            // 纵向缩放独立控制音符高度，不隐式联动横向比例。
            visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(
                TR("ui.settings.visual.note_scale_y").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat("##LayoutNoteScaleY",
                                                &visual.noteScaleY,
                                                0.5f,
                                                3.0f,
                                                "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();

            // 非长按击打特效时长使用配置声明的边界，避免 UI 常量漂移。
            visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(
                TR("ui.settings.visual.non_hold_hit_effect_duration").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##LayoutNonHoldHitEffectDuration",
                     &visual.nonHoldHitEffectDuration,
                     Config::VisualConfig::MIN_NON_HOLD_HIT_EFFECT_DURATION,
                     Config::VisualConfig::MAX_NON_HOLD_HIT_EFFECT_DURATION,
                     "%.3f s") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            // 在调用保存 helper 前缓存悬浮态，因为后续 ImGui 项会改变查询对象。
            const bool hitEffectDurationHovered = ImGui::IsItemHovered();
            saveVisualAfterEdit();
            if ( hitEffectDurationHovered ) {
                drawTooltip(TR("ui.settings.visual.non_hold_hit_effect_"
                               "duration_tooltip")
                                .data());
            }

            // 绑定采样标签属于离散显示开关，切换后立即保存。
            visual = appConfig.getVisualConfig();
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.settings.visual.note_bound_sample_labels").data(),
                     &visual.showBoundSampleLabels) ) {
                applyVisualConfig(visual);
                appConfig.save();
                m_layoutVisualConfigDirty = false;
            }

            // 填充模式通过整数适配 ImGui Combo，再显式转换回强类型枚举。
            visual           = appConfig.getVisualConfig();
            int noteFillMode = static_cast<int>(visual.noteFillMode);
            // 顺序必须与 BackgroundFillMode 枚举序值一致。
            const char* fillModes[] = {
                TR("ui.settings.visual.fill_mode.stretch").data(),
                TR("ui.settings.visual.fill_mode.aspect_fit").data(),
                TR("ui.settings.visual.fill_mode.aspect_fill").data(),
                TR("ui.settings.visual.fill_mode.center").data(),
            };
            ImGui::TextUnformatted(
                TR("ui.settings.visual.note_fill_mode").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackCombo("##LayoutNoteFillMode",
                                          &noteFillMode,
                                          fillModes,
                                          IM_ARRAYSIZE(fillModes)) ) {
                // 组合框只产生数组范围内索引，可安全映射到枚举。
                visual.noteFillMode =
                    static_cast<Config::BackgroundFillMode>(noteFillMode);
                applyVisualConfig(visual);
                appConfig.save();
                m_layoutVisualConfigDirty = false;
            }

            // 默认调色方案属于软件设置，不随当前项目活动方案自动变化。
            auto&       settings      = appConfig.getEditorSettings();
            auto&       defaultScheme = settings.defaultColorPaletteSchemeName;
            const auto& paletteConfig = settings.colorPalettes;
            // 内部皮肤默认标识在 UI 中替换为本地化友好名称。
            const std::string previewName =
                defaultScheme == Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID
                    ? std::string(
                          TR("ui.toolbar.note_palette.skin_default_scheme")
                              .data())
                    : defaultScheme;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.note_palette_default").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackBeginCombo("##LayoutDefaultNotePalette",
                                               previewName.c_str()) ) {
                // 皮肤默认项始终位于自定义方案列表之前。
                const bool skinSelected =
                    defaultScheme ==
                    Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                if ( ::MMM::UI::FeedbackSelectable(
                         TR("ui.toolbar.note_palette.skin_default_scheme")
                             .data(),
                         skinSelected) ) {
                    defaultScheme =
                        Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                    appConfig.save();
                }
                if ( skinSelected ) ImGui::SetItemDefaultFocus();
                // 方案名同时作为持久化引用；选择后无需复制颜色数组。
                for ( const auto& scheme : paletteConfig.schemes ) {
                    const bool selected = defaultScheme == scheme.name;
                    if ( ::MMM::UI::FeedbackSelectable(scheme.name.c_str(),
                                                       selected) ) {
                        defaultScheme = scheme.name;
                        appConfig.save();
                    }
                    if ( selected ) ImGui::SetItemDefaultFocus();
                }
                ::MMM::UI::FeedbackEndCombo();
            }
            // 提示说明 Layout 模式中的直接拖拽能力，不承担交互。
            ImGui::TextDisabled(
                "%s", TR("ui.toolbar.layout_note_resize_hint").data());
        }

        // 背景分组聚合图片填充、明暗处理和辅助线视觉强度。
        //
        // 分组字段职责：
        // - background.fillMode 决定图片裁剪与留白策略；
        // - opaque_ratio 控制原背景贡献；
        // - darken_ratio 控制编辑时附加暗化；
        // - beatLineAlpha 只影响分拍线最终透明度；
        // - hoverSubdivisionLineExtensionRatio 控制悬浮辅助线延伸。
        //
        // 所有比例字段保持 0..1，并以四位小数显示，便于细调和配置往返。
        // 填充模式是离散枚举，改变后立即保存；比例滑条共享视觉脏标记，
        // 避免拖动每个采样点写配置文件。恢复按钮只重置背景渲染职责。
        if ( ::MMM::UI::FeedbackCollapsingHeader(
                 TR("ui.settings.visual.background").data()) ) {
            // 背景恢复默认不触碰音符渲染和组件布局。
            drawRenderingResetButton(
                "BackgroundRenderingReset", [](Config::EditorConfig& config) {
                    config.resetBackgroundRenderingToDefaults();
                });
            // 背景填充枚举与音符填充共用同一模式集合。
            auto visual     = appConfig.getVisualConfig();
            int  bgFillMode = static_cast<int>(visual.background.fillMode);
            // 显示顺序与 BackgroundFillMode 枚举保持一致。
            const char* fillModes[] = {
                TR("ui.settings.visual.fill_mode.stretch").data(),
                TR("ui.settings.visual.fill_mode.aspect_fit").data(),
                TR("ui.settings.visual.fill_mode.aspect_fill").data(),
                TR("ui.settings.visual.fill_mode.center").data(),
            };
            ImGui::TextUnformatted(
                TR("ui.settings.visual.bg_fill_mode").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackCombo("##LayoutBackgroundFillMode",
                                          &bgFillMode,
                                          fillModes,
                                          IM_ARRAYSIZE(fillModes)) ) {
                visual.background.fillMode =
                    static_cast<Config::BackgroundFillMode>(bgFillMode);
                applyVisualConfig(visual);
                appConfig.save();
                m_layoutVisualConfigDirty = false;
            }

            // opaque_ratio 控制原图混合比例，合法范围为零到一。
            visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(TR("ui.settings.visual.bg_opaque").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat("##LayoutBackgroundOpaque",
                                                &visual.background.opaque_ratio,
                                                0.0f,
                                                1.0f,
                                                "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();

            // darken_ratio 在背景图之上叠加暗化，不改变资源本身。
            visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(TR("ui.settings.visual.bg_darken").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat("##LayoutBackgroundDarken",
                                                &visual.background.darken_ratio,
                                                0.0f,
                                                1.0f,
                                                "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();

            // 分拍线透明度只改变渲染可见度，不等价于隐藏分拍线模式。
            visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(
                TR("ui.settings.visual.beat_line_alpha").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat("##LayoutBeatLineAlpha",
                                                &visual.beatLineAlpha,
                                                0.0f,
                                                1.0f,
                                                "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();

            // 悬浮细分线延伸比例控制选中反馈超出画布的长度。
            visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(
                TR("ui.settings.visual.hover_subdivision_line_extension")
                    .data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##LayoutHoverSubdivisionLineExtension",
                     &visual.hoverSubdivisionLineExtensionRatio,
                     0.0f,
                     1.0f,
                     "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();
        }

        // 下方组件列表编辑按轨道数分组保存的可见性、颜色和位置默认值。
        //
        // 每个 CanvasComponentPlacement 至少包含可见性、颜色和布局位置。位置由
        // Layout 工具在画布上直接拖动，此弹层提供可见性、颜色与恢复默认入口。
        // 配置按 keyCount 隔离，因为不同轨道数的画布宽度和合理组件位置不同。
        //
        // 通用控件不假设组件的渲染实现。BackgroundSpectrum 是唯一兼容特例：
        // 它既有组件 placement，又保留 background.spectrum.enabled 和专用
        // 宽高、基线、左右颜色参数。通用可见性和恢复动作必须同步这些字段，
        // 防止新旧配置读取路径产生不同结果。
        ImGui::TextUnformatted(TR("ui.toolbar.layout_components").data());
        ImGui::Separator();

        /// @brief 绘制一个画布组件的可见性、颜色和局部恢复入口。
        ///
        /// type 同时作为 ImGui ID 和配置索引。showColor 为 false 的组件仍可
        /// 控制可见性与位置恢复，但颜色由更细粒度的专用设置组管理。
        /// 可见性变化会同步 BackgroundSpectrum 的兼容 enabled 字段，保证
        /// 新旧渲染读取路径观察到一致状态。
        const auto drawComponentControl = [&](Config::CanvasComponentType type,
                                              std::string_view            label,
                                              bool showColor = true) {
            ImGui::PushID(static_cast<int>(type));
            // 从当前轨道数对应布局读取值，避免跨键数修改其他布局。
            bool visible = appConfig.getVisualConfig()
                               .canvasComponentsForKeyCount(keyCount)
                               .placement(type)
                               .visible;
            if ( ::MMM::UI::FeedbackCheckbox(label.data(), &visible) ) {
                // 基于最新完整配置修改单一 placement 字段。
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual
                    .editableCanvasComponentsForKeyCount(keyCount)
                    .placement(type)
                    .visible = visible;
                if ( type == Config::CanvasComponentType::BackgroundSpectrum ) {
                    // 兼容字段与组件可见性必须原子地写入同一配置快照。
                    updatedConfig.visual.background.spectrum.enabled = visible;
                }
                updateEditorConfig(updatedConfig);
                appConfig.save();
            }
            // 未启用或暂时离屏时，精确指向该组件的显隐开关；画布实体
            // 一旦可见便覆盖这个备用锚点，避免跨面板合并成大片亮区。
            // 这里必须紧跟 Checkbox：后续颜色和复位按钮会覆盖 LastItem
            // 的矩形，届时再报目标就会把用户引向错误按钮。
            // 固定 ID 与翻译后的 label 解耦，语言切换不会丢失教程目标。
            if ( sourceManager ) {
                std::string_view walkthroughTarget;
                // 绘制顺序来自布局面板，目标名称来自引导数据；显式映射
                // 避免误以枚举数值拼接 ID，也防止本地化改名破坏定位。
                // KPS 虽然在频谱细项之后绘制，仍和其他组件走同一入口。
                switch ( type ) {
                case Config::CanvasComponentType::JudgmentLineTime:
                    walkthroughTarget =
                        "personalization.editor.component.judgment-time";
                    break;
                case Config::CanvasComponentType::BeatNumber:
                    walkthroughTarget =
                        "personalization.editor.component.beat-number";
                    break;
                case Config::CanvasComponentType::BeatLineTime:
                    walkthroughTarget =
                        "personalization.editor.component.beat-line-time";
                    break;
                case Config::CanvasComponentType::BackgroundSpectrum:
                    walkthroughTarget =
                        "personalization.editor.component.spectrum";
                    break;
                case Config::CanvasComponentType::Kps:
                    walkthroughTarget = "personalization.editor.component.kps";
                    break;
                default: break;
                }
                auto& spotlight = sourceManager->walkthroughSpotlight();
                if ( spotlight.awaitingTarget(walkthroughTarget) ) {
                    if ( ImGui::IsItemVisible() ) {
                        // 可见行只提供备用入口；画布若报出真实组件矩形，
                        // Spotlight 会按主目标优先规则替换它。
                        spotlight.reportTarget(walkthroughTarget,
                                               ImGui::GetItemRectMin(),
                                               ImGui::GetItemRectMax(),
                                               ImGui::GetWindowViewport(),
                                               true,
                                               true);
                    } else {
                        // 较短的弹层会滚动；让被引导的开关进入视野后再高亮。
                        // 不用视口外的 ItemRect，否则亮区会被夹在窗口边缘。
                        // 下帧 ImGui 完成滚动后再取新的实际控件坐标。
                        ImGui::SetScrollHereY();
                    }
                }
            }

            if ( showColor ) {
                // 频谱使用左右声道专用颜色，因此不显示通用组件色按钮。
                ImGui::SameLine();
                // 颜色按钮固定在复位列左侧，避免组件名称长度改变颜色列位置。
                const float contentRight =
                    ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                const float colorColumnX = contentRight - resetButtonWidth -
                                           ImGui::GetStyle().ItemSpacing.x -
                                           colorButtonSize;
                if ( ImGui::GetCursorPosX() < colorColumnX ) {
                    ImGui::SetCursorPosX(colorColumnX);
                }
                // 存储数组先转换为命名通道结构，供 ImGui RGBA 接口编辑。
                const auto componentColor =
                    fromStoredColor(appConfig.getVisualConfig()
                                        .canvasComponentsForKeyCount(keyCount)
                                        .placement(type)
                                        .color);
                // 按钮仅打开自定义取色器，不使用 ImGui 默认大型预览弹窗。
                if ( ::MMM::UI::FeedbackColorButton(
                         "##Color",
                         toImVec4(componentColor),
                         ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoPicker |
                             ImGuiColorEditFlags_AlphaPreviewHalf,
                         ImVec2(colorButtonSize, colorButtonSize)) ) {
                    ::MMM::UI::FeedbackOpenPopup("ColorPicker");
                }
                if ( ImGui::IsItemHovered() ) {
                    drawTooltip(TR("ui.toolbar.layout_component_color").data());
                }
            }

            ImGui::SameLine();
            // 将所有复位按钮锚定到弹窗内容区右侧，避免标签长度造成错位。
            const float remainingWidth = ImGui::GetContentRegionAvail().x;
            if ( remainingWidth > resetButtonWidth ) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remainingWidth -
                                     resetButtonWidth);
            }
            if ( ::MMM::UI::FeedbackSmallButton(resetButtonLabel.data()) ) {
                // 恢复当前位置和颜色到该轨道数布局的默认值。
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual
                    .editableCanvasComponentsForKeyCount(keyCount)
                    .resetPlacementToDefault(type);
                if ( type == Config::CanvasComponentType::BackgroundSpectrum ) {
                    // 频谱尺寸属于独立配置结构，组件 placement 重置不会覆盖它。
                    // 因此额外恢复宽、高和基线，形成用户理解中的完整组件复位。
                    const Config::BackgroundSpectrumConfig defaults;
                    updatedConfig.visual.background.spectrum.widthRatio =
                        defaults.widthRatio;
                    updatedConfig.visual.background.spectrum.heightRatio =
                        defaults.heightRatio;
                    updatedConfig.visual.background.spectrum.baselineRatio =
                        defaults.baselineRatio;
                }
                updateEditorConfig(updatedConfig);
                appConfig.save();
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(
                    TR("ui.toolbar.layout_component_reset_hint").data());
            }

            if ( showColor && ImGui::BeginPopup("ColorPicker") ) {
                // 汇总打开态供弹层末尾检测“所有取色器已关闭”的保存边沿。
                anyColorPickerOpen = true;
                // 每帧从配置重建编辑色；取色器活动期间应用服务同步值会成为
                // 下一帧起点，而成员脏标记负责把最后一次值落盘。
                auto editableColor =
                    fromStoredColor(appConfig.getVisualConfig()
                                        .canvasComponentsForKeyCount(keyCount)
                                        .placement(type)
                                        .color);
                if ( ImGui::ColorPicker4(
                         "##Value",
                         &editableColor.r,
                         ImGuiColorEditFlags_AlphaBar |
                             ImGuiColorEditFlags_AlphaPreviewHalf |
                             ImGuiColorEditFlags_DisplayRGB) ) {
                    // 拖动中只应用新颜色并设置脏标志，不逐帧写磁盘。
                    auto updatedConfig = appConfig.getEditorConfig();
                    updatedConfig.visual
                        .editableCanvasComponentsForKeyCount(keyCount)
                        .placement(type)
                        .color = toStoredColor(editableColor);
                    updateEditorConfig(updatedConfig);
                    m_layoutComponentColorDirty = true;
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() &&
                     m_layoutComponentColorDirty ) {
                    // 正常失活路径立即提交；关闭弹层路径由函数入口兜底。
                    appConfig.save();
                    m_layoutComponentColorDirty = false;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        };

        // 判定时间、拍号和分拍时间复用通用颜色与布局控制。
        drawComponentControl(
            Config::CanvasComponentType::JudgmentLineTime,
            TR("ui.toolbar.layout_current_judgment_time").view());
        drawComponentControl(Config::CanvasComponentType::BeatNumber,
                             TR("ui.toolbar.layout_beat_number").view());
        drawComponentControl(Config::CanvasComponentType::BeatLineTime,
                             TR("ui.toolbar.layout_beat_line_time").view());
        // 背景频谱颜色由下方左右声道控件管理，关闭通用颜色入口。
        drawComponentControl(Config::CanvasComponentType::BackgroundSpectrum,
                             TR("ui.toolbar.layout_background_spectrum").view(),
                             false);
        // 频谱专用设置只在用户展开时创建控件，折叠不影响运行时状态。
        //
        // 频谱配置分为四类：
        // - bandCount 控制频率离散精度并受编译期上下限约束；
        // - widthRatio 与 heightRatio 控制相对画布包围尺寸；
        // - leftBarColor 与 rightBarColor 区分双声道柱颜色；
        // - opacity 和 includeHitEffects 控制最终混合与分析输入。
        //
        // 连续数值及颜色共用 m_layoutSpectrumConfigDirty。滑条失活、颜色取色器
        // 关闭、Layout 弹层关闭三条路径都能保存最终值，且任一路径保存后清除
        // 标志，避免重复磁盘写入。离散 includeHitEffects
        // 和匹配画布按钮立即保存。
        if ( ::MMM::UI::FeedbackCollapsingHeader(
                 TR("ui.toolbar.layout_background_spectrum_settings")
                     .data()) ) {
            // 所有连续控件统一宽度，保持标签下方滑条对齐。
            const float spectrumControlWidth = std::floor(230.0F * dpiScale);
            /// @brief 合并频谱快照并维持组件可见性作为 enabled 权威值。
            const auto applySpectrumConfig =
                [&](const Config::BackgroundSpectrumConfig& spectrum) {
                    // 频谱局部副本不拥有 enabled 权威值；它始终跟随当前键数布局
                    // 中 BackgroundSpectrum placement 的 visible 字段。
                    auto updatedConfig = appConfig.getEditorConfig();
                    updatedConfig.visual.background.spectrum = spectrum;
                    // enabled 不从可能过时的局部 spectrum 副本读取。
                    updatedConfig.visual.background.spectrum.enabled =
                        updatedConfig.visual
                            .canvasComponentsForKeyCount(keyCount)
                            .backgroundSpectrum.visible;
                    updateEditorConfig(updatedConfig);
                };
            /// @brief 在连续频谱控件结束编辑时一次性保存最终值。
            const auto saveSpectrumAfterEdit = [&]() {
                // 与普通视觉 helper 相同，此函数必须在目标控件之后立即调用。
                if ( ImGui::IsItemDeactivatedAfterEdit() &&
                     m_layoutSpectrumConfigDirty ) {
                    appConfig.save();
                    m_layoutSpectrumConfigDirty = false;
                }
            };
            /// @brief 绘制一个由成员指针指定的频谱颜色取色器。
            ///
            /// 成员指针让左右声道共享完全一致的即时应用与保存逻辑，同时
            /// 保留各自独立的 ImGui ID。颜色弹层沿用外层统一关闭检测。
            const auto drawSpectrumColorControl =
                [&](std::string_view label,
                    const char*      id,
                    std::array<float, 4>
                        Config::BackgroundSpectrumConfig::* colorMember) {
                    // 每帧从最新配置重新构造编辑值，避免左右颜色互相覆盖。
                    auto spectrum =
                        appConfig.getVisualConfig().background.spectrum;
                    auto editableColor = fromStoredColor(spectrum.*colorMember);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(label.data());
                    ImGui::SameLine();
                    // 色块右对齐，翻译文本长度只影响左侧标签区域。
                    const float remainingWidth =
                        ImGui::GetContentRegionAvail().x;
                    if ( remainingWidth > colorButtonSize ) {
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                             remainingWidth - colorButtonSize);
                    }
                    // 左右声道传入不同 ID，内部相同 ##Color/ColorPicker
                    // 不冲突。
                    ImGui::PushID(id);
                    if ( ::MMM::UI::FeedbackColorButton(
                             "##Color",
                             toImVec4(editableColor),
                             ImGuiColorEditFlags_NoTooltip |
                                 ImGuiColorEditFlags_NoPicker |
                                 ImGuiColorEditFlags_AlphaPreviewHalf,
                             ImVec2(colorButtonSize, colorButtonSize)) ) {
                        ::MMM::UI::FeedbackOpenPopup("ColorPicker");
                    }
                    if ( ImGui::BeginPopup("ColorPicker") ) {
                        // 标记任一专用取色器打开，用于跨控件统一持久化。
                        anyColorPickerOpen = true;
                        if ( ImGui::ColorPicker4(
                                 "##Value",
                                 &editableColor.r,
                                 ImGuiColorEditFlags_AlphaBar |
                                     ImGuiColorEditFlags_AlphaPreviewHalf |
                                     ImGuiColorEditFlags_DisplayRGB) ) {
                            // 重新获取最新结构后只替换目标颜色成员。
                            spectrum =
                                appConfig.getVisualConfig().background.spectrum;
                            spectrum.*colorMember =
                                toStoredColor(editableColor);
                            applySpectrumConfig(spectrum);
                            m_layoutSpectrumConfigDirty = true;
                        }
                        // 取色器失活时保存；直接关闭弹层仍由外层边沿检测兜底。
                        saveSpectrumAfterEdit();
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                };

            // 频带数受渲染器声明的上下限约束，避免过量顶点或空频谱。
            auto spectrum = appConfig.getVisualConfig().background.spectrum;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.background_spectrum.band_count").data());
            ImGui::SetNextItemWidth(spectrumControlWidth);
            if ( ::MMM::UI::FeedbackSliderInt(
                     "##LayoutBackgroundSpectrumBands",
                     &spectrum.bandCount,
                     Config::BACKGROUND_SPECTRUM_MIN_BANDS,
                     Config::BACKGROUND_SPECTRUM_MAX_BANDS) ) {
                applySpectrumConfig(spectrum);
                m_layoutSpectrumConfigDirty = true;
            }
            saveSpectrumAfterEdit();

            // 宽度比例相对画布宽度计算，保留至少 10% 可见范围。
            spectrum = appConfig.getVisualConfig().background.spectrum;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.background_spectrum.width_ratio")
                    .data());
            ImGui::SetNextItemWidth(spectrumControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##LayoutBackgroundSpectrumWidth",
                     &spectrum.widthRatio,
                     0.10F,
                     1.0F,
                     "%.2f") ) {
                applySpectrumConfig(spectrum);
                m_layoutSpectrumConfigDirty = true;
            }
            saveSpectrumAfterEdit();

            // 高度比例相对画布高度计算，最小值避免频谱完全退化。
            spectrum = appConfig.getVisualConfig().background.spectrum;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.background_spectrum.height_ratio")
                    .data());
            ImGui::SetNextItemWidth(spectrumControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##LayoutBackgroundSpectrumHeight",
                     &spectrum.heightRatio,
                     0.05F,
                     1.0F,
                     "%.2f") ) {
                applySpectrumConfig(spectrum);
                m_layoutSpectrumConfigDirty = true;
            }
            saveSpectrumAfterEdit();

            // 一键匹配画布只重置宽高，不改变基线、颜色和透明度。
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.settings.visual.background_spectrum."
                        "match_canvas_aspect")
                         .data(),
                     ImVec2(spectrumControlWidth, 0.0F)) ) {
                // 离散按钮结果立即应用并保存，无需设置连续编辑脏标志。
                spectrum.widthRatio  = 1.0F;
                spectrum.heightRatio = 1.0F;
                applySpectrumConfig(spectrum);
                appConfig.save();
                m_layoutSpectrumConfigDirty = false;
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(TR("ui.settings.visual.background_spectrum."
                               "match_canvas_aspect_hint")
                                .data());
            }

            // 左右声道颜色分别由成员指针映射到同一绘制 helper。
            drawSpectrumColorControl(
                TR("ui.settings.visual.background_spectrum.left_bar_color")
                    .view(),
                "BackgroundLevelLeft",
                &Config::BackgroundSpectrumConfig::leftBarColor);
            drawSpectrumColorControl(
                TR("ui.settings.visual.background_spectrum.right_bar_color")
                    .view(),
                "BackgroundLevelRight",
                &Config::BackgroundSpectrumConfig::rightBarColor);

            // 总透明度作用于两侧颜色的最终混合，不改写颜色 Alpha 通道。
            spectrum = appConfig.getVisualConfig().background.spectrum;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.background_spectrum.opacity").data());
            ImGui::SetNextItemWidth(spectrumControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##LayoutBackgroundSpectrumOpacity",
                     &spectrum.opacity,
                     0.0F,
                     1.0F,
                     "%.2f") ) {
                applySpectrumConfig(spectrum);
                m_layoutSpectrumConfigDirty = true;
            }
            saveSpectrumAfterEdit();

            // 击打特效是否注入频谱属于离散分析源开关，立即持久化。
            spectrum = appConfig.getVisualConfig().background.spectrum;
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.settings.visual.background_spectrum."
                        "include_hit_effects")
                         .data(),
                     &spectrum.includeHitEffects) ) {
                applySpectrumConfig(spectrum);
                appConfig.save();
                m_layoutSpectrumConfigDirty = false;
            }
        }
        // KPS 组件本身仍使用通用可见性、颜色和复位入口。
        drawComponentControl(Config::CanvasComponentType::Kps,
                             TR("ui.toolbar.layout_kps").view());
        // KPS 同步分组控制不同轨道数布局之间的尺寸和相对位置传播策略。
        //
        // 三个选项解决不同层级的同步需求：
        // - syncKpsTrackSizes 让各轨道 KPS 面板使用一致尺寸；
        // - syncKpsTrackRelativePositions 同步轨道内的相对位置；
        // - syncAllKpsComponentPositions 把同步扩展到所有 KPS 组件。
        //
        // 后两项通过配置对象 setter 修改，因为它们可能维护关联字段或默认值。
        // UI 只提交 setter 产出的完整配置，不能直接推导同步传播规则。三个开关
        // 都是离散设置，改变后立即保存，不参与颜色或连续参数脏标记。
        if ( ::MMM::UI::FeedbackCollapsingHeader(
                 TR("ui.toolbar.layout_kps_sync_settings").data(),
                 ImGuiTreeNodeFlags_DefaultOpen) ) {
            // 尺寸同步只影响轨道级 KPS 组件，不隐式开启位置同步。
            bool syncKpsTrackSizes = appConfig.getVisualConfig()
                                         .canvasComponentsForKeyCount(keyCount)
                                         .syncKpsTrackSizes;
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.toolbar.layout_kps_sync_track_sizes").data(),
                     &syncKpsTrackSizes) ) {
                // 配置字段按当前键数布局修改并立即保存。
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual
                    .editableCanvasComponentsForKeyCount(keyCount)
                    .syncKpsTrackSizes = syncKpsTrackSizes;
                updateEditorConfig(updatedConfig);
                appConfig.save();
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(
                    TR("ui.toolbar.layout_kps_sync_track_sizes_hint").data());
            }

            // 相对位置同步通过配置 helper 更新相关内部派生状态。
            bool syncKpsTrackRelativePositions =
                appConfig.getVisualConfig()
                    .canvasComponentsForKeyCount(keyCount)
                    .syncKpsTrackRelativePositions;
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.toolbar.layout_kps_sync_track_positions").data(),
                     &syncKpsTrackRelativePositions) ) {
                // set 方法维护此选项关联的不变量，禁止直接写裸字段。
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual
                    .editableCanvasComponentsForKeyCount(keyCount)
                    .setSyncKpsTrackRelativePositions(
                        syncKpsTrackRelativePositions);
                updateEditorConfig(updatedConfig);
                appConfig.save();
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(
                    TR("ui.toolbar.layout_kps_sync_track_positions_hint")
                        .data());
            }

            // 全组件同步是更高层策略，与单轨相对位置同步分开控制。
            bool syncAllKpsComponentPositions =
                appConfig.getVisualConfig()
                    .canvasComponentsForKeyCount(keyCount)
                    .syncAllKpsComponentPositions;
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.toolbar.layout_kps_sync_all_positions").data(),
                     &syncAllKpsComponentPositions) ) {
                // 使用专用 setter 统一传播同步状态。
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual
                    .editableCanvasComponentsForKeyCount(keyCount)
                    .setSyncAllKpsComponentPositions(
                        syncAllKpsComponentPositions);
                updateEditorConfig(updatedConfig);
                appConfig.save();
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(
                    TR("ui.toolbar.layout_kps_sync_all_positions_hint").data());
            }
        }

        // 上一帧有取色器而本帧全部关闭时，提交可能未收到失活事件的最终值。
        if ( m_layoutComponentColorPickerOpen && !anyColorPickerOpen ) {
            if ( m_layoutComponentColorDirty || m_layoutSpectrumConfigDirty ||
                 m_layoutVisualConfigDirty ) {
                // 多种脏状态合并为一次配置文件保存。
                // 同一关闭边沿最多执行一次保存，并清除三类连续编辑标志。
                appConfig.save();
                m_layoutComponentColorDirty = false;
                m_layoutSpectrumConfigDirty = false;
                m_layoutVisualConfigDirty   = false;
            }
        }
        // 保存本帧聚合状态，供下一帧检测关闭边沿。
        m_layoutComponentColorPickerOpen = anyColorPickerOpen;

        ImGui::TextDisabled("%s",
                            TR("ui.toolbar.layout_component_drag_hint").data());

        // 编辑器个性化始终保持右侧布局设置可见；用户需要一边看画布组件，
        // 一边检查显隐、同步与尺寸选项。设置区与画布目标分别挖孔，不能
        // 合并成跨过时间线的大矩形，也不能让未勾选的开关被遮罩盖住。
        // 判定线、轨宽和 Note 步骤也保留此面板：它们同样有配置入口。
        // 仅当前个性化目标可请求额外亮区，其他演练继续只照亮自身目标。
        // 弹层必须先绘制完所有可选项，才可读取完整的当帧窗口尺寸。
        // 以实际 WindowPos/Size 而非上帧 m_layoutPopupWidth/Height 上报，
        // 否则展开“物件渲染”或“KPS 同步设置”后亮区会漏掉新行。
        // 弹层未打开时不报告，Spotlight 下一帧会清掉上一帧的面板亮区。
        if ( sourceManager ) {
            auto& spotlight = sourceManager->walkthroughSpotlight();
            constexpr std::array layoutTargets{
                "personalization.editor.component.judgment-time",
                "personalization.editor.component.beat-number",
                "personalization.editor.component.beat-line-time",
                "personalization.editor.component.spectrum",
                "personalization.editor.component.kps",
                "personalization.editor.judgment-line",
                "personalization.editor.player-lanes",
                "personalization.editor.draft-lanes",
                "personalization.editor.bgm-lanes",
                "personalization.editor.note-size"
            };
            for ( std::string_view target : layoutTargets ) {
                if ( !spotlight.awaitingTarget(target) ) continue;
                const ImVec2 position = ImGui::GetWindowPos();
                const ImVec2 size     = ImGui::GetWindowSize();
                spotlight.reportCompanionRegion(
                    target,
                    position,
                    { position.x + size.x, position.y + size.y },
                    ImGui::GetWindowViewport());
                break;
            }
        }

        // 缓存真实自动布局尺寸，下一帧可在视口边缘稳定定位。
        const ImVec2 size   = ImGui::GetWindowSize();
        m_layoutPopupWidth  = size.x;
        m_layoutPopupHeight = size.y;
    }
    ImGui::End();
    // 恢复仅属于 Layout 弹层的窗口样式。
    ImGui::PopStyleVar(3);
}

/// @brief 在工具栏项目左侧绘制统一悬浮提示。
/// @param text 已完成本地化和快捷键拼接的提示文本。
///
/// 工具栏靠近视口侧边，因此固定向左展开，避免覆盖窄按钮和后续项目。
/// @warning UI 热路径：仅在项目悬浮时由工具栏各控件调用。
void ToolbarView::drawTooltip(const char* text)
{
    Utils::renderTooltip(text, Utils::TooltipDir::Left);
}

}  // namespace MMM::UI
