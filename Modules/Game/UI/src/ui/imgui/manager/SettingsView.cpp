#include "ui/imgui/manager/SettingsView.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "imgui.h"
#include "mmm/SafeParse.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 设置窗口布局测量辅助函数集合。
///
/// 这些函数把当前 UI 状态压缩为可比较的快照，再根据当前标签页计算稳定的最小
/// 尺寸。它们不拥有字体或翻译文本，所有指针生命周期由 SkinManager 与 ImGui
/// 字体图集保证。
///
/// 测量标签列表与各标签页绘制函数存在显式维护关系。新增、删除或改名设置项时，
/// 必须同步检查标签列列表和不可换行控件列表，防止窗口最小宽度回归。
///
/// 缓存失效由翻译版本、字体偏好、缩放、间距和皮肤布局值共同驱动。不得只比较
/// DPI 或当前标签页，否则运行中切换语言、字体或皮肤后会沿用旧尺寸。
///
/// 标签宽度与控件宽度分开计算：前者覆盖左列可见说明，后者覆盖右列不可换行的
/// 交互内容。两者相加后再计入 section 和 Child 装饰空间。
///
/// 侧栏短标签使用菜单字体，设置项标签使用内容字体。字体缺失时二者都回退到帧
/// 快照中的当前 ImGui 字体，避免缓存生成零尺寸窗口。

/// @brief 获取设置分类短标签。
/// @param tab 设置页枚举。
/// @return 当前语言下用于侧栏的短标签；未知值返回空字符串。
///
/// 短标签只用于侧栏展示和宽度测量，不作为 ImGui ID。窗口内部状态使用稳定枚举
/// 值构造，切换语言不会改变分类按钮身份。
const char* getCategoryShortLabel(Event::SettingsTab tab)
{
    // 每个枚举值显式映射翻译键，避免依赖枚举顺序拼接资源名称。
    switch ( tab ) {
    case Event::SettingsTab::Software:
        return TR_CACHE("ui.settings.software.short").data();
    case Event::SettingsTab::Collaboration:
        return TR_CACHE("ui.settings.collaboration.short").data();
    case Event::SettingsTab::Visual:
        return TR_CACHE("ui.settings.visual.short").data();
    case Event::SettingsTab::Project:
        return TR_CACHE("ui.settings.project.short").data();
    case Event::SettingsTab::Beatmap:
        return TR_CACHE("ui.settings.beatmap.short").data();
    case Event::SettingsTab::Editor:
        return TR_CACHE("ui.settings.editor.short").data();
    case Event::SettingsTab::Shortcut:
        return TR_CACHE("ui.settings.shortcut.short").data();
    case Event::SettingsTab::Debug:
        return TR_CACHE("ui.settings.debug.short").data();
    }
    // 为未来未知枚举保留安全回退，测量结果自然为零宽。
    return "";
}

/// @brief 使用指定字体测量单行文本宽度。
/// @param text UTF-8 文本，可为空指针。
/// @param font 用于测量的 ImGui 字体，可为空指针。
/// @param fontSize 目标字体像素高度。
/// @return 不换行文本宽度；缺少文本或字体时返回零。
///
/// 该 helper 不读取当前 ImGui 字体栈，保证并行准备使用快照字体得到稳定结果。
float measureSettingsText(const char* text, ImFont* font, float fontSize)
{
    // 空输入无法安全交给字体 API，统一视作不占宽度。
    if ( !text ) return 0.0f;
    if ( !font ) return 0.0f;

    // FLT_MAX 禁止自动换行，设置列宽必须覆盖完整单行标签。
    return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, nullptr).x;
}

/// @brief 无异常解析皮肤布局中的浮点值。
/// @param value 皮肤布局配置中的字符串表示。
/// @param fallback 解析失败、无数字或非有限值时使用的回退值。
/// @return 可用于布局计算的有限浮点数。
///
/// 解析只接受数值前缀，具体语法由 SafeParse 统一实现；本函数不会抛出异常。
float parseLayoutFloat(const std::string& value, float fallback)
{
    // parseFloatingPrefix 同时返回值、错误码和实际消费长度。
    const auto  result = Internal::parseFloatingPrefix(value);
    const float parsed = static_cast<float>(result.value);
    if ( result.error != std::errc{} || result.parsedLength == 0 ||
         !std::isfinite(parsed) ) {
        // 无穷、NaN 和空前缀都不能进入尺寸计算。
        return fallback;
    }
    return parsed;
}

/// @brief 测量一组设置文本中的最大单行宽度。
/// @tparam N 固定标签数组长度。
/// @param labels 待测量的 UTF-8 标签数组。
/// @param font 用于测量的字体。
/// @param fontSize 目标字体像素高度。
/// @return 数组中最大的单行文本宽度。
///
/// 固定数组让调用点显式维护标签集合；空指针元素由单项 helper 按零处理。
template<size_t N>
float measureSettingsTextList(const std::array<const char*, N>& labels,
                              ImFont* font, float fontSize)
{
    // 从零开始累积最大值，空数组自然返回零。
    float maxWidth = 0.0f;
    for ( const char* label : labels ) {
        // 每项独立测量，不把标签拼接后引入额外字符宽度。
        maxWidth =
            std::max(maxWidth, measureSettingsText(label, font, fontSize));
    }
    return maxWidth;
}

/// @brief 估算当前设置页标签列中的最大宽度。
/// @param tab 需要测量的设置页。
/// @param snapshot 当前帧字体、字号和翻译环境快照。
/// @return 当前页所有已知标签的最大单行宽度。
///
/// 列表必须覆盖对应 `draw*Settings`
/// 中所有左侧标签。新增设置项时应同步加入这里，
/// 否则窄窗口的标签列可能低估并挤压文本。
///
/// 测量只访问快照字体和翻译缓存，不触碰 ImGui 当前窗口状态，便于准备阶段复用。
float measureSettingsTabLabelWidth(Event::SettingsTab     tab,
                                   const UiFrameSnapshot& snapshot)
{
    // 内容字体缺失时使用帧快照中的后备字体，避免返回全零布局。
    ImFont* font =
        snapshot.contentFont ? snapshot.contentFont : snapshot.fallbackFont;
    switch ( tab ) {
    case Event::SettingsTab::Software: {
        // 软件页标签最多，覆盖外观、保存、自动任务、时间与同步配置。
        // 数组顺序按页面分组排列，便于与 SoftwareTab 的控件清单人工核对。
        const std::array<const char*, 46> labels{
            TR_CACHE("ui.settings.software.language").data(),
            TR_CACHE("ui.settings.software.default_creator").data(),
            TR_CACHE("ui.settings.software.framelimit").data(),
            TR_CACHE("ui.settings.software.auto_upload_pgo_profiles").data(),
            TR_CACHE("ui.settings.software.skin").data(),
            TR_CACHE("ui.settings.software.theme").data(),
            TR_CACHE("ui.settings.software.font.ascii").data(),
            TR_CACHE("ui.settings.software.font.cjk").data(),
            TR_CACHE("ui.settings.software.ui_scale.multiplier").data(),
            TR_CACHE("ui.settings.software.font.multiplier").data(),
            TR_CACHE("ui.settings.editor.cursor_style").data(),
            TR_CACHE("ui.settings.software.cursor_size").data(),
            TR_CACHE("ui.settings.software.trail_size").data(),
            TR_CACHE("ui.settings.software.trail_life").data(),
            TR_CACHE("ui.settings.software.smoke_size").data(),
            TR_CACHE("ui.settings.software.cursor_bpm_sync").data(),
            TR_CACHE("ui.settings.software.smoke_life").data(),
            TR_CACHE("ui.settings.software.aesthetics.window_rounding").data(),
            TR_CACHE("ui.settings.software.aesthetics.frame_rounding").data(),
            TR_CACHE("ui.settings.software.aesthetics.window_gap").data(),
            TR_CACHE("ui.settings.software.aesthetics.item_spacing").data(),
            TR_CACHE("ui.settings.software.aesthetics.window_padding").data(),
            TR_CACHE("ui.settings.software.aesthetics.animation_transition")
                .data(),
            TR_CACHE("ui.settings.software.picker_style").data(),
            TR_CACHE("ui.settings.software.save_format").data(),
            TR_CACHE("ui.settings.software.auto_save.mode").data(),
            TR_CACHE("ui.settings.software.auto_save.interval_unit").data(),
            TR_CACHE("ui.settings.software.auto_save.interval").data(),
            TR_CACHE("ui.settings.software.auto_save.on_object_modified")
                .data(),
            TR_CACHE("ui.settings.software.auto_save.on_beatmap_switch").data(),
            TR_CACHE("ui.settings.software.auto_save.on_imgui_focus_lost")
                .data(),
            TR_CACHE("ui.settings.software.auto_save.on_native_focus_lost")
                .data(),
            TR_CACHE("ui.settings.software.auto_backup.mode").data(),
            TR_CACHE("ui.settings.software.auto_backup.interval_unit").data(),
            TR_CACHE("ui.settings.software.auto_backup.interval").data(),
            TR_CACHE("ui.settings.software.auto_backup.max_count").data(),
            TR_CACHE("ui.settings.software.auto_backup.on_object_modified")
                .data(),
            TR_CACHE("ui.settings.software.auto_backup.on_beatmap_switch")
                .data(),
            TR_CACHE("ui.settings.software.auto_backup.on_imgui_focus_lost")
                .data(),
            TR_CACHE("ui.settings.software.auto_backup.on_native_focus_lost")
                .data(),
            TR_CACHE("ui.settings.software.time_format").data(),
            TR_CACHE("ui.settings.software.recent_limit").data(),
            TR_CACHE("ui.settings.software.sync_mode").data(),
            TR_CACHE("ui.settings.software.sync_factor").data(),
            TR_CACHE("ui.settings.software.sync_buffer").data(),
            TR_CACHE("ui.settings.software.sync_interval").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Collaboration: {
        // 协作页测量服务器连接四项固定字段。
        // 应用按钮属于右侧控件，不计入左侧标签列。
        const std::array<const char*, 4> labels{
            TR_CACHE("ui.collaboration.server_address").data(),
            TR_CACHE("ui.collaboration.signaling_port").data(),
            TR_CACHE("ui.collaboration.use_tls").data(),
            TR_CACHE("ui.collaboration.directory.status").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Visual: {
        // 视觉页覆盖偏移、预览、画布交互、特效和频谱细节标签。
        // 组合框选项宽度由 widget 测量函数另行覆盖。
        // 内部滑键开关的“不含首尾”属于完整标签，测量时不能截断限定语。
        // 即使分组折叠也保留这些宽度，避免展开时值列位置跳动。
        const std::array<const char*, 19> labels{
            TR_CACHE("ui.settings.visual.flick_hit_effect_mode").data(),
            TR_CACHE("ui.settings.visual.polyline_internal_flick_effects")
                .data(),
            TR_CACHE("ui.settings.visual.beat_line_before_first_timing").data(),
            TR_CACHE("ui.settings.visual.preview_ratio").data(),
            TR_CACHE("ui.settings.visual.preview_edge_scroll_sensitivity")
                .data(),
            TR_CACHE("ui.settings.visual.preview_margin_left").data(),
            TR_CACHE("ui.settings.visual.preview_margin_top").data(),
            TR_CACHE("ui.settings.visual.preview_margin_right").data(),
            TR_CACHE("ui.settings.visual.preview_margin_bottom").data(),
            TR_CACHE("ui.settings.visual.preview_draw_beat_lines").data(),
            TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
            TR_CACHE("ui.settings.visual.timeline_zoom").data(),
            TR_CACHE("ui.settings.visual.scroll_animation_duration").data(),
            TR_CACHE("ui.settings.visual.linear_scroll").data(),
            TR_CACHE("ui.settings.visual.snap_threshold").data(),
            TR_CACHE("ui.settings.visual.spectrum_detail").data(),
            TR_CACHE("ui.settings.visual.visual_offset").data(),
            TR_CACHE("ui.settings.visual.waveform_visual_offset").data(),
            TR_CACHE("ui.settings.visual.spectrum_visual_offset").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Project: {
        // 项目页同时纳入继承提示相关的自动备份字段。
        // 折叠标题本身不占设置行标签列，但保留相关字段确保展开后宽度稳定。
        const std::array<const char*, 13> labels{
            TR_CACHE("ui.settings.project.info").data(),
            TR_CACHE("ui.settings.project.path").data(),
            TR_CACHE("ui.settings.project.no_project").data(),
            TR_CACHE("ui.settings.project.note_palette").data(),
            TR_CACHE("ui.settings.project.auto_backup.override").data(),
            TR_CACHE("ui.settings.software.auto_backup.mode").data(),
            TR_CACHE("ui.settings.software.auto_backup.interval_unit").data(),
            TR_CACHE("ui.settings.software.auto_backup.interval").data(),
            TR_CACHE("ui.settings.software.auto_backup.max_count").data(),
            TR_CACHE("ui.settings.software.auto_backup.on_object_modified")
                .data(),
            TR_CACHE("ui.settings.software.auto_backup.on_beatmap_switch")
                .data(),
            TR_CACHE("ui.settings.software.auto_backup.on_imgui_focus_lost")
                .data(),
            TR_CACHE("ui.settings.software.auto_backup.on_native_focus_lost")
                .data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Beatmap: {
        // 谱面页覆盖基本信息、封面、偏好和资源选择标签。
        // 动态视频起始行也必须纳入，不应因当前封面类型隐藏而缩小窗口。
        const std::array<const char*, 19> labels{
            TR_CACHE("ui.settings.beatmap.name").data(),
            TR_CACHE("ui.settings.beatmap.title").data(),
            TR_CACHE("ui.settings.beatmap.title_unicode").data(),
            TR_CACHE("ui.settings.beatmap.artist").data(),
            TR_CACHE("ui.settings.beatmap.artist_unicode").data(),
            TR_CACHE("ui.settings.beatmap.mapper").data(),
            TR_CACHE("ui.settings.beatmap.version").data(),
            TR_CACHE("ui.settings.beatmap.path").data(),
            TR_CACHE("ui.settings.beatmap.cover_type").data(),
            TR_CACHE("ui.settings.beatmap.video_start").data(),
            TR_CACHE("ui.settings.beatmap.bg_offset").data(),
            TR_CACHE("ui.settings.beatmap.bpm").data(),
            TR_CACHE("ui.settings.beatmap.tracks").data(),
            TR_CACHE("ui.settings.beatmap.draft_tracks").data(),
            TR_CACHE("ui.settings.beatmap.bgm_tracks").data(),
            TR_CACHE("ui.settings.beatmap.length").data(),
            TR_CACHE("ui.settings.beatmap.audio").data(),
            TR_CACHE("ui.settings.beatmap.cover").data(),
            TR_CACHE("ui.settings.beatmap.background").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Editor: {
        // 编辑器页覆盖行为、选择和命中音效三组字段。
        // 动态滑键倍率行始终参与测量，开关切换时窗口尺寸不会跳变。
        const std::array<const char*, 23> labels{
            TR_CACHE("ui.settings.editor.default_palette").data(),
            TR_CACHE("ui.settings.editor.reverse_scroll").data(),
            TR_CACHE("ui.settings.editor.snap_floor").data(),
            TR_CACHE("ui.settings.editor.stop_playback_on_scroll").data(),
            TR_CACHE("ui.settings.editor.toolbar_value_wheel_adjustment")
                .data(),
            TR_CACHE("ui.settings.editor.hit_effects").data(),
            TR_CACHE("ui.settings.editor.sync_same_main_audio").data(),
            TR_CACHE("ui.settings.editor.scroll_snap").data(),
            TR_CACHE("ui.settings.editor.disable_scroll_accel_while_drawing")
                .data(),
            TR_CACHE("ui.settings.editor.remove_objects_on_polyline_path")
                .data(),
            TR_CACHE("ui.settings.editor.select_pasted_objects").data(),
            TR_CACHE("ui.settings.editor.copy_paste_time_basis").data(),
            TR_CACHE("ui.settings.editor.timeline_selection_includes_bpm")
                .data(),
            TR_CACHE("ui.settings.editor.scroll_multiplier").data(),
            TR_CACHE("ui.settings.editor.beat_divisor").data(),
            TR_CACHE("ui.settings.editor.selection").data(),
            TR_CACHE("ui.settings.editor.selection.thickness").data(),
            TR_CACHE("ui.settings.editor.selection.rounding").data(),
            TR_CACHE("ui.settings.editor.polyline_internal_flick_sfx").data(),
            TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
            TR_CACHE("ui.settings.editor.sfx_flick_mul").data(),
            TR_CACHE("ui.settings.editor.sfx_stereo_hit_effects").data(),
            TR_CACHE("ui.settings.editor.sfx_sync_speed").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Shortcut: {
        // 快捷键页每个可录制动作都参与标签列宽度估算。
        // 快捷键字符串和操作按钮的宽度在 widget 测量函数中单独计算。
        const std::array<const char*, 20> labels{
            TR_CACHE("ui.settings.shortcut.tool_move").data(),
            TR_CACHE("ui.settings.shortcut.tool_marquee").data(),
            TR_CACHE("ui.settings.shortcut.tool_draw").data(),
            TR_CACHE("ui.settings.shortcut.tool_color_brush").data(),
            TR_CACHE("ui.settings.shortcut.tool_color_eraser").data(),
            TR_CACHE("ui.settings.shortcut.mirror").data(),
            TR_CACHE("ui.settings.shortcut.mirror_paste").data(),
            TR_CACHE("ui.settings.shortcut.edit_selected_volume").data(),
            TR_CACHE("ui.settings.shortcut.add_selected_annotation").data(),
            TR_CACHE("ui.settings.shortcut.delete_selected").data(),
            TR_CACHE("ui.settings.shortcut.toggle_playback").data(),
            TR_CACHE("ui.settings.shortcut.toggle_reverse_scroll").data(),
            TR_CACHE("ui.settings.shortcut.toggle_scroll_snap").data(),
            TR_CACHE("ui.settings.shortcut.toggle_snap_floor").data(),
            TR_CACHE("ui.settings.shortcut.toggle_scroll_timing_mapping")
                .data(),
            TR_CACHE("ui.settings.shortcut.toggle_beat_lines").data(),
            TR_CACHE("ui.settings.shortcut.toggle_stop_playback_on_scroll")
                .data(),
            TR_CACHE("ui.settings.shortcut.toggle_hit_sfx").data(),
            TR_CACHE("ui.settings.shortcut.toggle_hit_effects").data(),
            TR_CACHE("ui.settings.shortcut.toggle_sync_same_main_audio").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    case Event::SettingsTab::Debug: {
        // 调试页仅测量公开给用户的诊断开关和命中框缩放。
        // 调试日志运行态输出不属于设置标签，不在此列表中出现。
        const std::array<const char*, 5> labels{
            TR_CACHE("ui.settings.debug.draw_hitboxes").data(),
            TR_CACHE("ui.settings.debug.hitbox_scale_x").data(),
            TR_CACHE("ui.settings.debug.hitbox_scale_y").data(),
            TR_CACHE("ui.settings.debug.render_profile_logging").data(),
            TR_CACHE("ui.settings.debug.rtc_diagnostic_logging").data()
        };
        return measureSettingsTextList(labels, font, snapshot.fontSize);
    }
    }
    // 未知设置页没有可测量标签，返回零宽交由外层最小值保护。
    return 0.0f;
}

/// @brief 估算当前设置页右侧控件中不可再换行内容的宽度。
/// @param tab 需要估算的设置页。
/// @param snapshot 当前帧字体、字号、DPI 与样式快照。
/// @return 向上取整后的最小控件列宽度。
///
/// 估算关注组合框选项、快捷键录制行等不可换行内容。普通数值控件使用统一基础
/// 宽度；页面仍可在更宽窗口中让控件随 Clay 列宽增长。
float measureSettingsTabWidgetWidth(Event::SettingsTab     tab,
                                    const UiFrameSnapshot& snapshot)
{
    // DPI 至少按一倍计算，避免异常小缩放令交互控件退化。
    const float scale = std::max(1.0f, snapshot.dpiScale);
    // 组合框除了文字还需容纳左右 padding 和下拉箭头区域。
    const float framePad   = snapshot.framePadding.x * 2.0f;
    const float comboArrow = snapshot.frameHeight;
    ImFont*     font =
        snapshot.contentFont ? snapshot.contentFont : snapshot.fallbackFont;
    // 基础宽度覆盖常见四位小数文本和额外拖动空间。
    float minWidth = measureSettingsText("0.0000", font, snapshot.fontSize) +
                     framePad + std::floor(48.0f * scale);

    /// 将一组组合框选项的最大宽度纳入当前控件列下限。
    auto addOptions = [&](auto&& labels) {
        minWidth =
            std::max(minWidth,
                     measureSettingsTextList(labels, font, snapshot.fontSize) +
                         framePad + comboArrow);
    };

    switch ( tab ) {
    case Event::SettingsTab::Software: {
        // 软件页同时考虑固定语言名称和最长帧率/字体选项。
        // 语言名称是产品固定文本，翻译表切换不会改变其自描述写法。
        addOptions(std::array<const char*, 2>{ "简体中文 (zh_cn)",
                                               "English (en_us)" });
        addOptions(std::array<const char*, 6>{
            TR_CACHE("ui.settings.software.framelimit.vsync").data(),
            TR_CACHE("ui.settings.software.framelimit.2x").data(),
            TR_CACHE("ui.settings.software.framelimit.4x").data(),
            TR_CACHE("ui.settings.software.framelimit.8x").data(),
            TR_CACHE("ui.settings.software.framelimit.unlimited").data(),
            TR_CACHE("ui.settings.software.font.default").data() });
        break;
    }
    case Event::SettingsTab::Collaboration: {
        // 服务器地址输入使用代表性主机名给出可用编辑宽度。
        // 额外缩放空间允许用户看清并编辑常见完整域名。
        minWidth =
            std::max(minWidth,
                     measureSettingsText(
                         "collaboration.example.com", font, snapshot.fontSize) +
                         framePad + std::floor(48.0f * scale));
        break;
    }
    case Event::SettingsTab::Visual: {
        // 填充模式与滑键播放模式都参与离散控件的最小宽度计算。
        // 连续数值滑块继续使用基础数值宽度，无需逐项测量。
        // 英文“头到尾”文本较长，不能只依据当前中文选项或选中项估算。
        // 翻译版本变化时布局缓存重建，选项宽度随当前语言统一更新。
        // 仅测量文本，不在宽度计算阶段访问配置写入口或绘制控件。
        addOptions(std::array<const char*, 3>{
            TR_CACHE("ui.settings.visual.flick_hit_effect_mode.head_only")
                .data(),
            TR_CACHE("ui.settings.visual.flick_hit_effect_mode.tail_only")
                .data(),
            TR_CACHE("ui.settings.visual.flick_hit_effect_mode.head_to_tail")
                .data() });
        addOptions(std::array<const char*, 4>{
            TR_CACHE("ui.settings.visual.fill_mode.stretch").data(),
            TR_CACHE("ui.settings.visual.fill_mode.aspect_fit").data(),
            TR_CACHE("ui.settings.visual.fill_mode.aspect_fill").data(),
            TR_CACHE("ui.settings.visual.fill_mode.center").data() });
        break;
    }
    case Event::SettingsTab::Beatmap: {
        // 谱面页确保图片/视频封面类型选项完整显示。
        // 资源路径组合框可横向裁剪，但须给并排导入按钮留出固定宽度。
        // 使用翻译后的最长按钮文案，语言切换会由布局缓存重新测量。
        // 下拉箭头也占用一帧高的宽度，不能让按钮把它挤成零宽。
        // 额外逻辑像素留给路径预览，不随实际工程路径长度增长。
        addOptions(std::array<const char*, 2>{
            TR_CACHE("ui.settings.beatmap.cover_type.image").data(),
            TR_CACHE("ui.settings.beatmap.cover_type.video").data() });
        const float importLabelWidth =
            std::max(measureSettingsText(
                         TR_CACHE("ui.settings.beatmap.import_audio").data(),
                         font,
                         snapshot.fontSize),
                     measureSettingsText(
                         TR_CACHE("ui.settings.beatmap.import_image").data(),
                         font,
                         snapshot.fontSize));
        minWidth = std::max(minWidth,
                            importLabelWidth + framePad + comboArrow +
                                std::floor(110.0f * scale));
        break;
    }
    case Event::SettingsTab::Editor: {
        // 编辑器页用选择模式的两项可见文本建立下限。
        // 其他布尔控件本体只需复选框宽度，标签由左列承担。
        addOptions(std::array<const char*, 2>{
            TR_CACHE("ui.settings.editor.selection.strict").data(),
            TR_CACHE("ui.settings.editor.selection.intersection").data() });
        break;
    }
    case Event::SettingsTab::Shortcut: {
        // 快捷键行需要同时容纳键串、录制按钮、清除按钮及三组 padding。
        // 代表性组合覆盖修饰键和长按键名，避免录制状态下按钮被挤出。
        const float shortcutWidth =
            measureSettingsText(
                "Ctrl+Shift+RightArrow", font, snapshot.fontSize) +
            measureSettingsText(TR_CACHE("ui.settings.shortcut.record").data(),
                                font,
                                snapshot.fontSize) +
            measureSettingsText(TR_CACHE("ui.settings.shortcut.clear").data(),
                                font,
                                snapshot.fontSize) +
            framePad * 3.0f + std::floor(32.0f * scale);
        minWidth = std::max(minWidth, shortcutWidth);
        break;
    }
    case Event::SettingsTab::Project: break;
    case Event::SettingsTab::Debug: break;
    }

    // 没有专用离散选项的页面沿用基础数值控件宽度。
    // 像素宽度向上取整，避免小数截断导致末尾字符被裁剪。
    return std::ceil(minWidth);
}

/// @brief 捕获设置窗口同步测量所需的当前帧快照。
/// @param dpiScale 当前窗口内容缩放。
/// @return 设置窗口布局测量快照。
///
/// 快照集中收集字体指针、ImGui 样式和影响布局的持久化配置。后续缓存比较与尺寸
/// 构建只读取该值对象，避免在计算过程中多次查询变化中的全局状态。
/// @warning UI
/// 热路径：每次获取布局缓存时调用；只复制轻量字段，不得加入文件访问。
UiFrameSnapshot captureSettingsUiFrameSnapshot(float dpiScale)
{
    // 设置与外观值来自同一个 AppConfig 实例，保证快照内部版本一致。
    auto&       appConfig  = Config::AppConfig::instance();
    const auto& settings   = appConfig.getEditorSettings();
    const auto& aesthetics = settings.aesthetics;
    // SkinManager 提供当前字体、布局字符串和翻译版本。
    auto& skinCfg = Config::SkinManager::instance();
    // ImGui 样式只在 UI 线程读取并复制进快照。
    const auto& style = ImGui::GetStyle();

    // 所有影响测量命中的字段必须在这里和 layoutMetricsMatch 中成对维护。
    UiFrameSnapshot snapshot;
    // DPI 下限与实际尺寸计算保持一致。
    snapshot.dpiScale               = std::max(1.0f, dpiScale);
    snapshot.framePadding           = style.FramePadding;
    snapshot.frameHeight            = ImGui::GetFrameHeight();
    snapshot.frameHeightWithSpacing = ImGui::GetFrameHeightWithSpacing();
    // 字体可能缺失，调用方会退回当前 ImGui 字体。
    snapshot.contentFont  = skinCfg.getFont("content");
    snapshot.menuFont     = skinCfg.getFont("menu");
    snapshot.fallbackFont = ImGui::GetFont();
    snapshot.fontSize     = ImGui::GetFontSize();
    // 翻译版本变化会使所有可见标签宽度缓存失效。
    snapshot.translationVersion = skinCfg.getTranslator().getVersion();
    snapshot.language           = settings.language;
    snapshot.preferredAsciiFont = settings.preferredAsciiFont;
    snapshot.preferredCjkFont   = settings.preferredCjkFont;
    snapshot.fontSizeMultiplier = settings.fontSizeMultiplier;
    snapshot.uiScaleMultiplier  = settings.uiScaleMultiplier;
    snapshot.windowPadding      = aesthetics.windowPadding;
    snapshot.itemSpacing        = aesthetics.itemSpacing;
    // 皮肤侧栏宽度保留原始字符串，以便缓存比较和无异常解析。
    snapshot.sidebarWidthConfig = skinCfg.getLayoutConfig("side_bar.width");
    return snapshot;
}
}  // namespace

/// @brief 构造设置面板视图并订阅设置页切换事件。
/// @param viewName 视图名称。
///
/// 事件订阅捕获 `this`，因此析构前必须使用保存的订阅 ID 取消注册。
SettingsView::SettingsView(const std::string& viewName) : IUIView(viewName)
{
    // 外部菜单或快捷入口发布事件后，统一通过 open() 初始化窗口状态。
    // 订阅 ID 保存为成员，生命周期与 SettingsView 实例严格一致。
    m_tabSubId =
        Event::EventBus::instance().subscribe<Event::UISettingsTabEvent>(
            [this](const Event::UISettingsTabEvent& e) { open(e.tab); });
}

/// @brief 析构设置面板视图并取消设置页切换事件订阅。
SettingsView::~SettingsView()
{
    // 零值表示没有有效订阅，避免向 EventBus 提交无效 ID。
    if ( m_tabSubId != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::UISettingsTabEvent>(
            m_tabSubId);
        // 析构后回调不再持有已失效的 this 指针。
    }
}

/// @brief 获取或创建指定索引的设置项行布局。
/// @param index 行布局缓存索引。
/// @return 已清空并可复用的横向行布局。
///
/// 缓存只增长不缩减，避免设置页每帧为相同数量的行重复分配。返回引用在当前
/// SettingsView 生命周期内稳定，但调用方只应把它用于本帧布局树。
CLayHBox& SettingsView::getRow(size_t index)
{
    // 各页面按顺序请求索引，因此一次追加即可覆盖当前索引。
    if ( index >= m_settingRows.size() ) {
        m_settingRows.emplace_back();
    }
    // 清除旧父子关系和绘制回调，不销毁行对象本身。
    m_settingRows[index].clear();
    return m_settingRows[index];
}

/// @brief 获取或创建指定索引的设置段落布局。
/// @param index 段落布局缓存索引。
/// @return 已清空并可复用的纵向段落布局。
///
/// section 与行缓存独立，因为折叠状态会改变本帧实际创建的段落数量。
CLayVBox& SettingsView::getSection(size_t index)
{
    // 只有展开分组请求 section，按需扩展缓存。
    if ( index >= m_sectionBoxes.size() ) {
        m_sectionBoxes.emplace_back();
    }
    // 清空上一帧内容后交给当前标签页重新设置装饰与间距。
    m_sectionBoxes[index].clear();
    return m_sectionBoxes[index];
}

/// @brief 判断布局测量缓存是否匹配当前帧状态。
/// @param cache 需要检查的布局缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param tab 当前设置页。
/// @return 完全匹配时返回 true。
///
/// 缓存键覆盖设置页、DPI、语言、字体偏好、缩放、间距、皮肤侧栏宽度与翻译版本。
/// 任一字段变化都可能改变文字或装饰尺寸，因此必须重新测量。
bool SettingsView::layoutMetricsMatch(const LayoutMetricsCache& cache,
                                      const UiFrameSnapshot&    snapshot,
                                      Event::SettingsTab        tab)
{
    /// 以小容差比较布局浮点键，过滤无意义的表示噪声。
    auto floatEqual = [](float lhs, float rhs) {
        return std::abs(lhs - rhs) <= 0.0001f;
    };

    // valid 防止默认构造缓存被误判为命中。
    // 字体指针本身不作键比较；字体偏好与翻译版本负责表示资源变化。
    return cache.valid && cache.tab == tab &&
           floatEqual(cache.dpiScale, snapshot.dpiScale) &&
           cache.language == snapshot.language &&
           cache.preferredAsciiFont == snapshot.preferredAsciiFont &&
           cache.preferredCjkFont == snapshot.preferredCjkFont &&
           floatEqual(cache.fontSizeMultiplier, snapshot.fontSizeMultiplier) &&
           floatEqual(cache.uiScaleMultiplier, snapshot.uiScaleMultiplier) &&
           floatEqual(cache.windowPadding, snapshot.windowPadding) &&
           floatEqual(cache.itemSpacing, snapshot.itemSpacing) &&
           cache.sidebarWidthConfig == snapshot.sidebarWidthConfig &&
           cache.translationVersion == snapshot.translationVersion;
}

/// @brief 构造设置窗口布局测量缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param tab 需要测量的设置页。
/// @return 设置窗口布局测量结果。
///
/// 结果同时包含侧栏宽度、当前页标签列宽、控件列宽和整窗最小尺寸。计算只使用
/// `UiFrameSnapshot` 与翻译缓存，不访问当前窗口布局状态。
///
/// 横向最小值按“窗口 padding + 分类栏 + 分隔线 + 内容列”组成；内容列继续拆成
/// 标签宽度、控件宽度和 section 装饰。纵向最小值确保八个分类按钮完整显示。
///
/// 中间尺寸使用 `floor` 对齐设备像素，最终窗口尺寸使用 `ceil` 防止累积小数造成
/// 裁剪。新增实际布局装饰时必须同步调整这里的 contentDecorations。
SettingsView::LayoutMetricsCache SettingsView::buildLayoutMetrics(
    const UiFrameSnapshot& snapshot, Event::SettingsTab tab)
{
    // 先复制完整缓存键，后续 layoutMetricsMatch 可逐项验证来源状态。
    LayoutMetricsCache cache;
    cache.valid              = true;
    cache.tab                = tab;
    cache.dpiScale           = snapshot.dpiScale;
    cache.language           = snapshot.language;
    cache.preferredAsciiFont = snapshot.preferredAsciiFont;
    cache.preferredCjkFont   = snapshot.preferredCjkFont;
    cache.fontSizeMultiplier = snapshot.fontSizeMultiplier;
    cache.uiScaleMultiplier  = snapshot.uiScaleMultiplier;
    cache.windowPadding      = snapshot.windowPadding;
    cache.itemSpacing        = snapshot.itemSpacing;
    cache.sidebarWidthConfig = snapshot.sidebarWidthConfig;
    cache.translationVersion = snapshot.translationVersion;
    // 缓存值与键在同一函数生成，不存在部分有效的中间状态。

    // 所有像素尺寸按不小于一倍的 DPI 缩放计算。
    const float scale = std::max(1.0f, snapshot.dpiScale);
    // 菜单字体缺失时退回帧快照字体，保证侧栏仍可测量。
    ImFont* menuFont =
        snapshot.menuFont ? snapshot.menuFont : snapshot.fallbackFont;
    // 皮肤字符串解析失败时使用 40 像素基础图标区域。
    const float sidebarBaseW =
        parseLayoutFloat(cache.sidebarWidthConfig, 40.0f);
    const float btnSize = std::floor(sidebarBaseW * scale);
    // 像素尺寸向下取整，让按钮边界在高 DPI 下保持清晰。

    // 全部八个分类共同决定侧栏短标签的最大宽度。
    const std::array<const char*, 8> labels{
        getCategoryShortLabel(Event::SettingsTab::Software),
        getCategoryShortLabel(Event::SettingsTab::Collaboration),
        getCategoryShortLabel(Event::SettingsTab::Visual),
        getCategoryShortLabel(Event::SettingsTab::Project),
        getCategoryShortLabel(Event::SettingsTab::Beatmap),
        getCategoryShortLabel(Event::SettingsTab::Editor),
        getCategoryShortLabel(Event::SettingsTab::Shortcut),
        getCategoryShortLabel(Event::SettingsTab::Debug)
    };
    // 侧栏总宽度由图标、分隔区、最长标签和两侧 padding 组成。
    const float maxLabelWidth =
        measureSettingsTextList(labels, menuFont, snapshot.fontSize);
    const float sepAreaW       = std::floor(12.0f * scale);
    const float labelPadding   = std::floor(12.0f * scale);
    const float vboxPadding    = std::floor(12.0f * scale);
    cache.categorySidebarWidth = std::floor(btnSize + sepAreaW + maxLabelWidth +
                                            labelPadding + vboxPadding);
    // 侧栏宽度与实际 drawContent 组成项使用相同的逻辑常量。

    // 最小窗口横向需要同时容纳左右窗口 padding。
    const float windowPad = std::floor(snapshot.windowPadding * scale) * 2.0f;
    // 高度至少容纳八个分类按钮、七个间距和上下侧栏 padding。
    const float categorySize    = std::floor(sidebarBaseW * scale);
    const float categorySpacing = std::floor(snapshot.itemSpacing * scale);
    const float categoryHeight  = std::floor(8.0f * scale) * 2.0f +
                                  categorySize * 8.0f + categorySpacing * 7.0f;

    // 标签列额外留出间隔，使文字与右侧控件不贴合。
    cache.tabLabelWidth =
        measureSettingsTabLabelWidth(tab, snapshot) + std::floor(16.0f * scale);
    cache.tabWidgetWidth = measureSettingsTabWidgetWidth(tab, snapshot);
    // 内容装饰包含 Child padding、section padding 和标签/控件间距。
    const float contentDecorations = std::floor(15.0f * scale) * 2.0f +
                                     std::floor(8.0f * scale) * 4.0f +
                                     std::floor(8.0f * scale);
    const float contentWidth =
        cache.tabLabelWidth + cache.tabWidgetWidth + contentDecorations;
    // 内容宽度只针对当前 tab，切换页会通过缓存键重新计算。
    // 垂直最小值还需覆盖窗口标题/内容起始的一行高度。
    const float titleHeight = snapshot.frameHeightWithSpacing;

    // 横向取当前页真实需求，纵向取完整分类侧栏需求并向上取整。
    cache.minWindowSize = ImVec2(
        std::ceil(windowPad + cache.categorySidebarWidth + 1.0f + contentWidth),
        std::ceil(windowPad + titleHeight + categoryHeight));
    // 返回完整值对象，调用方可安全移动到准备槽或活动缓存。
    return cache;
}

/// @brief 获取当前设置页布局测量缓存。
/// @param dpiScale 当前窗口内容缩放。
/// @return 与当前语言、字体、缩放和设置页匹配的布局测量结果。
/// @warning UI 热路径：仅在缓存未命中时同步测量；通常由并行准备提前填充。
const SettingsView::LayoutMetricsCache& SettingsView::getLayoutMetrics(
    float dpiScale) const
{
    // 快照捕获当前可能影响尺寸的所有键值。
    UiFrameSnapshot snapshot = captureSettingsUiFrameSnapshot(dpiScale);
    if ( !layoutMetricsMatch(m_layoutMetricsCache, snapshot, m_currentTab) ) {
        // 同步回退只在准备缓存缺失或状态刚变化时发生。
        m_layoutMetricsCache = buildLayoutMetrics(snapshot, m_currentTab);
    }
    // 引用指向成员缓存，在下次失效重建前保持有效。
    return m_layoutMetricsCache;
}

/// @brief 判断当前帧设置窗口是否需要准备布局数据。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要刷新布局缓存时返回 true。
/// @warning 每帧准备阶段调用；只比较轻量缓存键，不得执行文字测量或分配。
bool SettingsView::needsParallelUiPrepare(const UiFrameSnapshot& snapshot) const
{
    // 窗口关闭时不预计算，首次重新打开由正常准备或同步回退填充。
    return m_isOpen &&
           !layoutMetricsMatch(m_layoutMetricsCache, snapshot, m_currentTab);
}

/// @brief 在 UI 主线程准备设置窗口布局数据。
/// @param snapshot 当前帧 UI 快照。
///
/// 结果先写入 prepared 槽位，直到统一交换阶段才替换正在使用的缓存。
void SettingsView::prepareUiFrameData(const UiFrameSnapshot& snapshot)
{
    // 使用调用方捕获的同一帧快照，避免准备中再次读取全局状态。
    m_preparedLayoutMetricsCache = buildLayoutMetrics(snapshot, m_currentTab);
    // 标志发布完整结果；交换函数不会读取未准备的数据。
    m_hasPreparedLayoutMetrics = true;
}

/// @brief 将准备好的布局数据切换给主线程使用。
/// @warning 帧边界调用；只移动值对象，不得在这里重新测量字体。
void SettingsView::swapPreparedUiFrameData()
{
    if ( !m_hasPreparedLayoutMetrics ) {
        // 无新结果时保留当前有效缓存。
        return;
    }

    // 移动 prepared 值并清除标志，使同一结果不会重复交换。
    m_layoutMetricsCache       = std::move(m_preparedLayoutMetricsCache);
    m_hasPreparedLayoutMetrics = false;
}

/// @brief 获取当前设置页标签列宽度。
/// @param dpiScale 当前窗口内容缩放。
/// @return 当前设置页标签列宽度。
/// @warning 设置页布局热路径；通常只读取命中缓存。
float SettingsView::getCurrentTabLabelWidth(float dpiScale) const
{
    return getLayoutMetrics(dpiScale).tabLabelWidth;
}

/// @brief 计算设置分类侧栏中不可再换行文本所需的宽度。
/// @param dpiScale 当前窗口内容缩放。
/// @return 图标、分隔线和最长短标签所需的侧栏宽度。
/// @warning 设置窗口热路径；通常只读取命中缓存。
float SettingsView::getCategorySidebarWidth(float dpiScale) const
{
    return getLayoutMetrics(dpiScale).categorySidebarWidth;
}

/// @brief 计算设置窗口当前内容所需的最小整窗尺寸。
/// @param dpiScale 当前窗口内容缩放。
/// @return 当前页内容与全部分类按钮都可用的最小尺寸。
/// @warning 每帧用于窗口尺寸约束；通常只读取命中缓存。
ImVec2 SettingsView::getMinWindowSize(float dpiScale) const
{
    return getLayoutMetrics(dpiScale).minWindowSize;
}

/// @brief 从持久化设置刷新默认 Creator 输入缓冲区。
///
/// 输入缓冲只在打开窗口等低频入口刷新，编辑过程中不会每帧覆盖用户尚未提交的
/// 文本。normalizeCreatorIdentity 保证写入内容符合身份字段长度约束。
void SettingsView::refreshDefaultCreatorInputBuffer()
{
    // 先清零整个固定缓冲，确保短字符串后仍有终止符和无旧尾部数据。
    m_defaultCreatorInputBuffer.fill('\0');
    const auto creator = Config::normalizeCreatorIdentity(
        Config::AppConfig::instance().getEditorSettings().defaultCreator);
    // 规范化函数保证结果能够容纳在目标数组中。
    std::copy(
        creator.begin(), creator.end(), m_defaultCreatorInputBuffer.begin());
}

/// @brief 从持久化设置刷新协作服务器输入缓冲区。
///
/// 地址、端口和 TLS 三项作为一个待应用草稿刷新；同时清除上一次连接应用状态，
/// 避免新一轮编辑显示旧成功或失败结果。
void SettingsView::refreshCollaborationServerInputBuffer()
{
    // 服务器配置由 AppConfig 持有，本函数只复制到 UI 固定缓冲。
    const auto& server =
        Config::AppConfig::instance().getEditorSettings().collaborationServer;
    // 清零保证即使来源达到容量上限也保留 NUL 终止符。
    m_collaborationServerAddressInputBuffer.fill('\0');
    // 最多复制容量减一字节，最后一个字节继续保持零值。
    const auto copyLength =
        std::min(server.address.size(),
                 m_collaborationServerAddressInputBuffer.size() - 1U);
    std::copy_n(server.address.begin(),
                copyLength,
                m_collaborationServerAddressInputBuffer.begin());
    // 标量草稿与地址来自同一份服务器配置快照。
    m_collaborationSignalingPortInput = server.signalingPort;
    m_collaborationUseTlsInput        = server.useTls;
    m_collaborationServerApplyState   = CollaborationServerApplyState::None;
}

/// @brief 打开设置窗口并切换到指定设置页。
/// @param tab 需要激活的设置页。
///
/// 打开动作同时请求连续若干帧焦点、下一帧中心停靠和皮肤目录刷新。输入缓冲在此
/// 低频入口同步，避免窗口已打开时覆盖用户的编辑草稿。
///
/// 若目标不是快捷键页，任何进行中的录制都会取消。协作服务器缓冲只在直接打开
/// 协作页时刷新；从侧栏切换时由协作页保持当前待应用草稿。
void SettingsView::open(Event::SettingsTab tab)
{
    // 先设置目标页和可见状态，后续布局测量会以新 tab 为缓存键。
    m_currentTab                    = tab;
    m_isOpen                        = true;
    m_focusRequestFramesRemaining   = FOCUS_REQUEST_FRAME_COUNT;
    m_dockToCenterNextFrame         = true;
    m_availableSkinDirectoriesDirty = true;
    // 默认 Creator 在所有标签页都可能被软件页访问，统一刷新。
    refreshDefaultCreatorInputBuffer();
    if ( tab == Event::SettingsTab::Collaboration ) {
        // 协作草稿只在直接打开该页时读取持久化服务器配置。
        refreshCollaborationServerInputBuffer();
    }
    if ( tab != Event::SettingsTab::Shortcut ) {
        // 离开快捷键页必须结束录制，防止按键继续被后台捕获。
        m_recordingShortcutTarget = ShortcutRecordTarget::None;
        ShortcutUtils::setShortcutRecordingActive(false);
    }
}

/// @brief 请求下一帧将设置窗口停靠到主编辑区中心标签页。
///
/// 只设置一次性标志，实际 Dock ID 必须在窗口 Begin 前交给 LayoutContext。
void SettingsView::requestDockToCenter()
{
    // 如果中心 Dock 尚不可用，标志会保留到后续帧继续尝试。
    m_dockToCenterNextFrame = true;
}

/// @brief 请求下一帧聚焦设置窗口。
///
/// 使用帧计数而非单帧脉冲，覆盖窗口刚创建、停靠节点尚未稳定的阶段。
void SettingsView::requestFocus()
{
    // update() 每次提交 SetNextWindowFocus 后递减一次。
    // 重复请求会重置完整计数，保证最近调用获得同等聚焦机会。
    m_focusRequestFramesRemaining = FOCUS_REQUEST_FRAME_COUNT;
}

/// @brief 更新并渲染独立设置窗口。
/// @param sourceManager 当前 UI 管理器。
///
/// 该函数负责稳定窗口 ID、中心停靠、焦点请求、最小尺寸约束和关闭后的快捷键录制
/// 清理。具体标签页内容由 `drawContent()` 分派。
///
/// `sourceManager` 作为非拥有观察指针保存，生命周期由外层 UI 管理器保证。窗口名
/// 的可见部分允许随翻译变化，`###SettingsWindow` 后缀必须保持稳定。
///
/// 停靠请求只有在中心节点返回非零 ID 时才消费；否则后续帧继续尝试。焦点请求
/// 独立按固定帧数递减，覆盖窗口创建与停靠节点稳定的时序差异。
/// @warning UI 热路径：窗口注册期间每帧调用；不得加入阻塞等待或文件系统遍历。
void SettingsView::update(UIManager* sourceManager)
{
    // 保存非拥有 UIManager 指针，供各设置页执行需要的界面操作。
    m_sourceManager = sourceManager;

    // `###SettingsWindow` 固定内部 ID，翻译标题变化不会破坏停靠状态。
    std::string windowName =
        TR("title.settings_manager").toString() + "###SettingsWindow";

    // 零 Dock ID 表示本帧不强制停靠，保留用户当前布局。
    ImGuiID dockId = 0;
    if ( m_dockToCenterNextFrame ) {
        // 中心节点可能尚未创建，只有非零结果才视为请求完成。
        dockId = MainDockSpaceUI::getCenterDockId();
    }
    if ( m_focusRequestFramesRemaining > 0 ) {
        // 连续数帧请求焦点，确保新建或刚停靠窗口最终成为活动标签。
        ImGui::SetNextWindowFocus();
        --m_focusRequestFramesRemaining;
    }
    // 当前窗口内容缩放参与布局缓存和最小尺寸约束。
    const float dpiScale =
        MMM::Config::AppConfig::instance().getWindowContentScale();
    // 上限保持无限，仅防止用户缩小到标签和控件无法使用。
    ImGui::SetNextWindowSizeConstraints(getMinWindowSize(dpiScale),
                                        ImVec2(FLT_MAX, FLT_MAX));

    // LayoutContext 管理 ImGui Begin/End，并把 m_isOpen 连接到关闭按钮。
    LayoutContext layoutContext(m_layoutCtx,
                                windowName,
                                false,
                                ImGuiWindowFlags_None,
                                &m_isOpen,
                                dockId,
                                ImGuiCond_Always);
    (void)layoutContext;
    if ( dockId != 0 ) {
        // 只有成功解析中心节点后才消费一次性停靠请求。
        m_dockToCenterNextFrame = false;
    }

    // 窗口上下文有效期间绘制侧栏和当前标签页。
    drawContent();
    // 谱面页回调已结束并释放会话锁；原生文件选择器只能在此之后打开。
    // 内置选择器同样由本层跨帧驱动，避免与 Clay 行回调生命周期耦合。
    // 此时普通设置窗口作用域仍有效，可作为文件模态框的 ImGui 父上下文。
    renderBeatmapResourcePicker(dpiScale);
    if ( !m_isOpen ) {
        // 关闭窗口立即结束快捷键录制，避免全局按键被隐藏页面截获。
        m_recordingShortcutTarget = ShortcutRecordTarget::None;
        ShortcutUtils::setShortcutRecordingActive(false);
    }
}

/// @brief 绘制设置视图内容。
///
/// 页面由三列组成：左侧分类栏、中间一像素分隔线、右侧当前设置内容。分类栏使用
/// Clay 纵向布局分配按钮矩形，具体交互和文字图标仍由 ImGui 绘制。
///
/// 所有字体、样式和 Child 作用域必须在本函数内严格配对恢复。标签页函数只能在
/// `SettingsContent` Child 和内容字体作用域中执行。
///
/// 分类按钮本体没有可见 ImGui 文本，图标、分隔线和短标签通过 DrawList 叠加。
/// FeedbackButton 仍提供统一的悬浮色、点击反馈与音效，稳定 ID 只取决于枚举。
///
/// 当前标签页在侧栏点击后即可于同帧绘制。切换离开快捷键页会立即终止录制，防止
/// 右侧控件消失后仍捕获全局输入。
///
/// 字体缺失采用分层降级：图标先回退内部字体，再回退当前 ImGui 字体；菜单字体
/// 缺失时省略短标签，但按钮 Tooltip 仍提供完整分类名称。
/// @warning UI
/// 热路径：设置窗口可见时每帧调用；不得复制共享所有权或执行阻塞操作。
void SettingsView::drawContent()
{
    // SkinManager 提供布局宽度和分类栏所需字体。
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    // 所有固定像素尺寸按当前内容缩放转换。
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    // 皮肤侧栏基础宽度解析失败时使用 40 像素回退。
    float sidebarBaseW =
        parseLayoutFloat(skinCfg.getLayoutConfig("side_bar.width"), 40.0f);
    // 按钮图标区与 buildLayoutMetrics 使用相同基础宽度和取整规则。
    float btnSize = std::floor(sidebarBaseW * dpiScale);

    // 完整侧栏宽度来自缓存，包含图标、分隔区和最长短标签。
    // 宽度不会随当前活动分类变化，切换标签页时侧栏保持稳定。
    float sidebarWidth = getCategorySidebarWidth(dpiScale);
    // 滚动条样式作用域随函数退出自动恢复。
    Utils::VerticalScrollbarStyleScope verticalScrollbarStyle(dpiScale);

    // 第一列：左侧分类栏。Child 自身去除 padding 和圆角，由内部 VBox 控制留白。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::BeginChild("SettingsCategories", { sidebarWidth, 0 }, false);
    {
        // 局部作用域用于成对管理该 Child 内的样式栈。
        auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        // 分类按钮圆角与软件外观配置、DPI 同步。
        float rounding = std::floor(aesthetics.frameRounding * dpiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        // 分类项之间的间距由 Clay VBox 控制，因此 ImGui ItemSpacing 置零。
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        Utils::pushFixedButtonStyleVars();
        // 固定按钮样式保证反馈区域与 Clay 矩形完全一致。

        /// 在 Clay 分配矩形中绘制一个分类按钮、图标、分隔线和短标签。
        /// @param tab 点击后激活的设置页。
        /// @param iconStr 图标字体中的字形字符串。
        /// @param tooltip 分类完整名称提示。
        /// @param rect Clay 分配的绝对屏幕矩形。
        auto DrawCategoryButton = [&](Event::SettingsTab tab,
                                      const char*        iconStr,
                                      const char*        tooltip,
                                      Clay_BoundingBox   rect) {
            // ImGui 控件从 Clay 绝对矩形起点绘制。
            ImGui::SetCursorScreenPos({ rect.x, rect.y });

            // 活动页使用主题 Active 色，其他按钮采用透明样式。
            bool isActive = (m_currentTab == tab);
            if ( isActive ) {
                ImVec4 activeCol =
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
            } else {
                // 非活动按钮隐藏常驻背景，只在 FeedbackButton 交互时呈现反馈。
                Utils::UIThemeUtils::pushTransparentButtonStyles();
            }

            // 非活动图标与标签降低透明度，当前页保持正常强调。
            ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if ( !isActive ) iconVec4.w *= 0.7f;
            ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

            // 内部 ID 只使用稳定枚举值，不依赖翻译后的可见文本。
            std::string btnId   = "##setting_tab_" + std::to_string((int)tab);
            const bool  clicked = ::MMM::UI::FeedbackButton(
                btnId.c_str(), { rect.width, rect.height });
            if ( tab == Event::SettingsTab::Shortcut && m_sourceManager ) {
                // 分类按钮的真实 Item 覆盖图标与文字，无需依赖翻译测量。
                // 点击后同帧切到快捷键内容，下一步可以直接定位绑定行。
                // 未点击时仍持续上报，供从演练页面返回时重放高亮。
                // 目标 ID 固定，不受分类短标签或当前语言变化影响。
                auto& spotlight = m_sourceManager->walkthroughSpotlight();
                spotlight.reportLastItem(
                    "personalization.settings.shortcut-tab");
                if ( clicked )
                    spotlight.completeTarget(
                        "personalization.settings.shortcut-tab", true);
            }
            if ( clicked ) {
                // 点击仅切换枚举；右侧内容在同一帧后续 switch 中立即更新。
                m_currentTab = tab;
                if ( tab != Event::SettingsTab::Shortcut ) {
                    // 切换离开快捷键页立即结束录制状态。
                    m_recordingShortcutTarget = ShortcutRecordTarget::None;
                    ShortcutUtils::setShortcutRecordingActive(false);
                }
            }

            // 左侧固定图标区后绘制竖向分隔线，右侧留给短标签。
            float iconAreaW = btnSize;
            float sepX      = rect.x + iconAreaW;

            // 优先使用纯图标字体，缺失时退回设置内部字体。
            ImFont* iconFont = skinCfg.getFont("pure_icons");
            if ( !iconFont ) {
                iconFont = skinCfg.getFont("setting_internal");
            }
            // 只有有效字体才压栈；绘制指针始终提供当前字体回退。
            if ( iconFont ) ImGui::PushFont(iconFont, iconFont->LegacySize);
            ImFont* drawIconFont = iconFont ? iconFont : ImGui::GetFont();
            ImVec2  iconSize     = ImGui::CalcTextSize(iconStr);
            // 图标在固定区域中水平、垂直居中。
            ImVec2 iconPos = { rect.x + (iconAreaW - iconSize.x) * 0.5f,
                               rect.y + (rect.height - iconSize.y) * 0.5f };
            ImGui::GetWindowDrawList()->AddText(
                drawIconFont,
                ImGui::GetFontSize(),
                iconPos,
                ImGui::GetColorU32(ImGuiCol_Text),
                iconStr);
            if ( iconFont ) ImGui::PopFont();
            // 图标字体作用域到此结束，分隔线与标签使用普通主题状态。

            // 分隔线沿用文字颜色但降低透明度，线宽随 DPI 取整。
            ImVec4 sepCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            sepCol.w *= 0.3f;
            ImGui::GetWindowDrawList()->AddLine(
                { sepX, rect.y + rect.height * 0.25f },
                { sepX, rect.y + rect.height * 0.75f },
                ImGui::GetColorU32(sepCol),
                std::floor(1.0f * dpiScale));

            // 短标签使用菜单字体；缺失菜单字体时保留只有图标的可用按钮。
            std::string label    = getCategoryShortLabel(tab);
            ImFont*     menuFont = skinCfg.getFont("menu");
            if ( menuFont ) {
                // 菜单字体有效时才绘制文字，按钮和 Tooltip 在字体缺失时仍可用。
                ImGui::PushFont(menuFont, menuFont->LegacySize);
                // 标签在分隔线右侧留出固定缩放 padding，并垂直居中。
                ImVec2 labelSize       = ImGui::CalcTextSize(label.c_str());
                float  textLeftPadding = std::floor(8.0f * dpiScale);
                ImVec2 labelPos = { sepX + textLeftPadding,
                                    rect.y +
                                        (rect.height - labelSize.y) * 0.5f };
                ImGui::GetWindowDrawList()->AddText(
                    menuFont,
                    ImGui::GetFontSize(),
                    labelPos,
                    ImGui::GetColorU32(ImGuiCol_Text),
                    label.c_str());
                ImGui::PopFont();
            }

            // 完整分类名称在按钮悬浮时向右显示，不占常驻侧栏宽度。
            Utils::renderTooltip(tooltip, Utils::TooltipDir::Right);

            // 恢复文字色及活动/透明按钮样式，保持栈严格平衡。
            ImGui::PopStyleColor(1);
            if ( isActive )
                // 活动按钮恢复 Button、Hovered、Active 三项颜色。
                ImGui::PopStyleColor(3);
            else
                // 非活动按钮恢复透明样式 helper 压入的完整状态。
                Utils::UIThemeUtils::popTransparentButtonStyles();
        };

        // VBox 按固定高度纵向排列全部八个分类按钮。
        CLayVBox vbox;
        // 上下 padding 与按钮间距按 DPI 和外观配置缩放。
        vbox.setPadding(std::floor(6.0f * dpiScale),
                        std::floor(6.0f * dpiScale),
                        std::floor(8.0f * dpiScale),
                        std::floor(8.0f * dpiScale))
            .setSpacing(std::floor(aesthetics.itemSpacing * dpiScale));

        // 每个元素使用稳定英文 ID，回调只转发对应枚举、图标和翻译提示。
        vbox.addElement("SoftwareTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            // 软件页入口使用桌面图标，管理全局程序配置。
                            DrawCategoryButton(
                                Event::SettingsTab::Software,
                                ICON_MMM_DESKTOP,
                                TR_CACHE("ui.settings.software").data(),
                                rect);
                        });

        vbox.addElement("CollaborationTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            // 协作页入口管理服务器与联机连接设置。
                            DrawCategoryButton(
                                Event::SettingsTab::Collaboration,
                                ICON_MMM_USERS,
                                TR_CACHE("ui.settings.collaboration").data(),
                                rect);
                        });

        vbox.addElement("VisualTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            // 视觉页入口管理画布和频谱呈现参数。
                            DrawCategoryButton(
                                Event::SettingsTab::Visual,
                                ICON_MMM_EYE,
                                TR_CACHE("ui.settings.visual").data(),
                                rect);
                        });

        vbox.addElement("ProjectTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            // 项目页入口只在有工程时展示可编辑项目配置。
                            DrawCategoryButton(
                                Event::SettingsTab::Project,
                                ICON_MMM_FOLDER,
                                TR_CACHE("ui.settings.project").data(),
                                rect);
                        });

        vbox.addElement("BeatmapTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            // 谱面页入口管理当前活动谱面的元数据。
                            DrawCategoryButton(
                                Event::SettingsTab::Beatmap,
                                ICON_MMM_FILE,
                                TR_CACHE("ui.settings.beatmap").data(),
                                rect);
                        });

        vbox.addElement("EditorTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            // 编辑器页入口管理工具行为、选择与命中音效。
                            DrawCategoryButton(
                                Event::SettingsTab::Editor,
                                ICON_MMM_PEN,
                                TR_CACHE("ui.settings.editor").data(),
                                rect);
                        });

        vbox.addElement("ShortcutTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            // 快捷键页入口可进入全局按键录制状态。
                            DrawCategoryButton(
                                Event::SettingsTab::Shortcut,
                                ICON_MMM_KEYBOARD,
                                TR_CACHE("ui.settings.shortcut").data(),
                                rect);
                        });

        vbox.addElement("DebugTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            // 调试页入口展示开发和诊断开关。
                            DrawCategoryButton(
                                Event::SettingsTab::Debug,
                                ICON_MMM_BUG,
                                TR_CACHE("ui.settings.debug").data(),
                                rect);
                        });

        // 所有分类登记完毕后，以 Child 可用高度统一执行 Clay 布局。
        ImVec2 startPos = ImGui::GetCursorScreenPos();
        vbox.renderInCurrent(
            startPos, { sidebarWidth, ImGui::GetContentRegionAvail().y });
        // 侧栏高度填满 Child，按钮按固定高度从顶部排列。

        // 恢复固定按钮样式与本 Child 内压入的两项样式变量。
        Utils::popFixedButtonStyleVars();
        ImGui::PopStyleVar(2);
    }
    ImGui::EndChild();
    // 恢复 SettingsCategories Child 外层的 WindowPadding 与 ChildRounding。
    ImGui::PopStyleVar(2);

    ImGui::SameLine(0, 0);
    // 零间距让分隔线紧贴分类栏右边缘。

    // 第二列：一像素中间分隔线，高度覆盖侧栏之后的全部可用区域。
    {
        // 使用当前游标和剩余高度绘制，再用 Dummy 占据实际布局宽度。
        ImVec2 p = ImGui::GetCursorScreenPos();
        float  h = ImGui::GetContentRegionAvail().y;
        ImGui::GetWindowDrawList()->AddLine(
            { p.x, p.y }, { p.x, p.y + h }, IM_COL32(80, 80, 80, 255));
        ImGui::Dummy({ 1.0f, h });
    }

    ImGui::SameLine(0, 0);
    // 右侧内容区紧贴分隔线，由自身 WindowPadding 提供内部留白。

    // 第三列：标准 ImGui 内容 Child，具体标签页内部可继续使用 Clay。
    {
        // 内容区采用更宽的窗口 padding 和纵向 item spacing。
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 25.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 15.0f));

        if ( ImGui::BeginChild("SettingsContent", { 0, 0 }, false) ) {
            // 内容字体仅在有效时压栈，否则沿用当前 ImGui 字体。
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) {
                ImGui::PushFont(contentFont, contentFont->LegacySize);
            }

            // 每帧只调用当前枚举对应的设置页绘制函数。
            switch ( m_currentTab ) {
            case Event::SettingsTab::Software: drawSoftwareSettings(); break;
            case Event::SettingsTab::Collaboration:
                drawCollaborationSettings();
                break;
            case Event::SettingsTab::Visual: drawVisualSettings(); break;
            case Event::SettingsTab::Project: drawProjectSettings(); break;
            case Event::SettingsTab::Beatmap: drawBeatmapSettings(); break;
            case Event::SettingsTab::Editor: drawEditorSettings(); break;
            case Event::SettingsTab::Shortcut: drawShortcutSettings(); break;
            case Event::SettingsTab::Debug: drawDebugSettings(); break;
            }
            // 所有枚举分支都只绘制一个标签页，避免隐藏页产生副作用。

            // 底部留白防止最后一项紧贴 Child 边界或滚动条末端。
            ImGui::Dummy(ImVec2(0, 50));
            // 只在本分支确实压入内容字体时恢复。
            if ( contentFont ) ImGui::PopFont();
        }
        // BeginChild 无论返回值如何都必须与 EndChild 配对。
        ImGui::EndChild();
        // 恢复右侧内容区的 WindowPadding 与 ItemSpacing。
        ImGui::PopStyleVar(2);
    }
}

}  // namespace MMM::UI
