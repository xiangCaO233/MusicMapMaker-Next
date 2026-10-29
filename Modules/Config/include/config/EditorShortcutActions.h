#pragma once

#include "config/ShortcutConfig.h"
#include <array>
#include <string_view>

namespace MMM::Config
{
/// @brief 可配置快捷键动作身份，None 专用于录制空状态。
enum class EditorShortcutAction {
    /// @brief 没有正在录制的动作。
    None,
    /// @brief 移动工具保留当前选择，后续指针操作由活动方案解释。
    ToolMove,
    /// @brief 框选与移动工具分开，区域选择保留独立键位。
    ToolMarquee,
    /// @brief 绘制工具建立画笔事务，不在按键触发时直接创建物件。
    ToolDraw,
    /// @brief 配色笔刷仍受专业模式门禁约束，重绑定不能绕过能力。
    ToolColorBrush,
    /// @brief 配色擦除只改变颜色属性，不等同于删除物件。
    ToolColorEraser,
    /// @brief 镜像作用于当前选择，保持与镜像粘贴独立。
    Mirror,
    /// @brief 镜像粘贴拥有 Shift 组合优先级，不能再触发普通粘贴。
    MirrorPaste,
    /// @brief 音量批量编辑保留原有选择与可用性检查，默认无键位。
    EditSelectedVolume,
    /// @brief 批注仍要求合法单个目标，旧冲突组合继续迁移。
    AddSelectedAnnotation,
    /// @brief 主画布删除使用用户绑定，时间线兼容退格入口另行保留。
    DeleteSelected,
    /// @brief 播放切换保留活动画笔和拖拽期间的既有焦点策略。
    TogglePlayback,
    /// @brief BPM 段落插入只属于测量工具焦点，不能落到谱面。
    AddBpmSegmentAtViewCenter,
    /// @brief 滚动反向属于持续配置，与单次滚动命令分开。
    ToggleReverseScroll,
    /// @brief 滚动吸附与放置吸附职责不同，不合并两项配置。
    ToggleScrollSnap,
    /// @brief 吸附向下取整单独保存，不隐式改变分拍除数。
    ToggleSnapFloor,
    /// @brief 视觉时间映射切换沿用逻辑配置更新通道。
    ToggleScrollTimingMapping,
    /// @brief 分拍线显示切换不改变谱面中的时间点。
    ToggleBeatLines,
    /// @brief 滚动停播开关是应用偏好，不等同于立即暂停。
    ToggleStopPlaybackOnScroll,
    /// @brief 打击音效开关不改变物件音量或全局静音。
    ToggleHitSfx,
    /// @brief 打击特效独立于音效，保留各自可选绑定。
    ToggleHitEffects,
    /// @brief 同主音轨同步仅切换会话同步策略，不重选资源。
    ToggleSyncSameMainAudio,
    /// @brief 撤销默认排除 Shift，避免与备用重做组合同时触发。
    Undo,
    /// @brief Ctrl+Y 的旧默认允许 Shift，用户重绑定后使用严格修饰匹配。
    Redo,
    /// @brief 备用重做保留 Ctrl+Shift+Z，与主重做共享业务动作。
    RedoAlternative,
    /// @brief 复制保留旧默认的额外修饰键兼容，按下不允许自动重复。
    Copy,
    /// @brief 剪切复用同一逻辑命令与撤销事务，不由 UI 直接删除对象。
    Cut,
    /// @brief 普通粘贴将 Shift 默认组合留给镜像粘贴动作。
    Paste,
    /// @brief 当前轨道区全选保留鼠标区域语义，不能扩大到全部物件。
    SelectAll,
    /// @brief 跨轨道全选明确使用独立动作，不依赖当前鼠标命中区。
    SelectAllObjects,
    /// @brief 新建工程与新建谱面默认按 Shift 区分，保留原向导。
    NewProject,
    /// @brief 新建谱面通过激活来源保留菜单与快捷键的业务标记。
    NewBeatmap,
    /// @brief 打开工程继续经过原文件选择与脏状态确认。
    OpenProject,
    /// @brief 普通保存不会绕过外部文件修改和覆盖确认流程。
    Save,
    /// @brief 另存为独立于普通保存，沿用导出格式与兼容提醒。
    SaveAs,
    /// @brief 音频导入沿用完整资源校验，不把主键改绑当作资源加载成功。
    ImportAudio,
    /// @brief 文件菜单导航只请求弹窗切换，不执行文件业务。
    MenuFile,
    /// @brief 编辑菜单导航保留旧 Alt 组合的额外修饰键语义。
    MenuEdit,
    /// @brief 工具菜单只改变导航归属，操作仍由对应动作校验。
    MenuTools,
    /// @brief 查看菜单导航与普通粘贴共享字母但使用不同修饰键。
    MenuView,
    /// @brief 帮助菜单导航不直接启动浏览器或打开系统目录。
    MenuHelp,
    /// @brief 对齐仍由逻辑命令完成，不改变选中物件的权限与历史。
    AlignSelected,
    /// @brief 取消只结束当前表面的临时状态，标准文本控件保持自身行为。
    CancelInteraction,
    /// @brief 重叠层导航不移动物件，只改变当前命中候选。
    PreviousLayerUp,
    /// @brief 备用层导航保留方向键对等语义，不吞掉文本光标。
    PreviousLayerLeft,
    /// @brief 下一层导航循环当前可见命中集合，不扫描整份谱面。
    NextLayerDown,
    /// @brief 备用下一层键位与主键位各自可重绑定。
    NextLayerRight,
    /// @brief 相机平移模式不等同于谱面物件移动工具。
    CameraMove,
    /// @brief 相机旋转只改变布局参数，不修改谱面内容。
    CameraRotate,
    /// @brief 轴约束保留同一拖拽起点，不重新创建编辑事务。
    CameraAxisX,
    /// @brief 纵向轴身份与重做动作按表面及焦点区分。
    CameraAxisY,
    /// @brief 深度轴不应因为撤销快捷键而触发谱面历史。
    CameraAxisZ,
    /// @brief 备用退格只在原本支持它的时间线入口生效。
    DeleteAlternative,
    /// @brief 工具焦点优先于全局播放，不得把输入转交到后台谱面。
    BpmTogglePlayback,
    /// @brief 音频结果导航只改变当前结果焦点，不移动谱面物件。
    AudioResultNext,
    /// @brief 音频搜索保留按住重复导航，选择提交仍由原控件处理。
    AudioResultPrevious,
};
/// @brief 一个动作的持久身份、标签与设置存储位置。
struct EditorShortcutDescriptor {
    /// @brief 设置页录制状态不保存配置指针。
    /// @details 录制页只存此值，不保存 AppConfig 成员的地址到下一帧。
    /// None 不在目录内产生可执行动作，避免空录制目标被误派发。
    /// 枚举顺序仅服务当前界面，配置文件始终保存稳定文本字段。
    EditorShortcutAction m_action;
    /// @brief 稳定配置字段名，方案按此区分同键不同操作。
    /// @details 身份由编译期字面量提供，生命周期覆盖整个程序。
    /// 界面把 data() 传给 ImGui ID，因此目录项必须使用以零结尾的静态文本。
    /// 增加动作可增加字段，已有字段不能随显示名称或翻译改名。
    /// 未知身份没有猜测回退，避免未来配置误触发另一个编辑动作。
    std::string_view m_id;
    /// @brief 标签使用当前翻译资源，不把控件文本作为操作身份。
    /// @details 翻译资源只决定用户看到的名称，不影响动作身份与保存字段。
    /// 设置页、搜索和教程可以引用此标签，不能反过来从文本推导操作。
    /// 运行期翻译切换仍使用相同目录，不重建键位配置或方案状态。
    const char* m_labelKey;
    /// @brief 指向快捷键配置的成员，避免每帧复制绑定字符串。
    /// @details 成员指针只描述存储位置，不能借用某次配置副本的地址。
    /// 录制页用可写 ShortcutConfig 取得引用，派发器只读同一个成员。
    /// 这使目录、序列化和录制共用一个真实字段，避免平行名单发生遗漏。
    /// 调用者不得把绑定复制成跨帧悬空字符串或修改临时对象后丢弃。
    ShortcutBinding ShortcutConfig::* m_binding;
    /// @brief 旧硬编码默认组合允许的额外修饰键掩码，用户重绑定使用严格匹配。
    /// @details 位值依次为 Control=1、Shift=2、Alt=4、Super=8。
    /// 这些位只允许默认绑定中未要求的额外修饰键，不能取消必需修饰键。
    /// 用户改变主键或修饰键后采用严格匹配，不继承旧硬编码入口的歧义。
    /// 普通可配置动作使用零，只有历史入口需要显式兼容的位才设为一。
    unsigned m_legacyExtraModifiers{};
};
/// @brief 唯一的快捷键动作目录，所有设置行和派发器使用同一名单。
/// @warning 返回编译期定长数组，热路径不得复制整个 ShortcutConfig。
/// @return 按界面顺序排列的只读描述值；调用者不能据此改变业务命令权限。
/// @details 目录没有实例、事件总线或文件系统依赖，可在配置与 UI 两层复用。
/// 未绑定动作仍在表内，清除键位不会使其从设置页永久消失。
/// 默认组合兼容按触发入口保留，不能给全目录统一放开修饰键。
/// 例如普通粘贴必须拒绝 Shift，否则镜像粘贴会同时执行两次业务。
/// 控制器存在更严格的局部约束时，可在查询时提供额外掩码覆盖。
/// 同一动作的多个入口共用存储，避免菜单与画布展示不同的用户配置。
/// 工具切换与业务菜单身份分别定义：这里描述输入绑定，而非菜单对象。
/// 枚举用于设置页的值状态，文本身份用于跨方案接口与配置文件。
/// 两者必须随成员指针一起增加，禁止只登记显示行而缺少可保存字段。
/// 成员指针指向 ShortcutConfig 本身，不涉及 EditorSettings 的对象布局。
/// 新字段采用默认成员初始化，旧配置缺少字段时无需强制迁移整份文件。
/// 未绑定状态由 ShortcutBinding::enabled 表达，不从空标签推测。
/// 主键大小写与显示格式仍由现有键名解析器处理，目录不做第二套解析。
/// 菜单助记键也可重绑定，但仍受菜单导航器自己的弹窗与焦点门禁约束。
/// 相机轴约束与时间线删除保持独立身份，避免全局重绑定牵连另一表面。
/// 音频结果导航保留按住重复；是否重复由查询入口而非目录全局决定。
/// 后续新增手势可直接发业务动作，无须为每个手势虚构一条键盘绑定。
/// 表内不持有方案或处理器对象，因此应用切换方案不需要重建目录。
/// 翻译键缺失只影响显示回退，不影响持久字段或输入匹配。
inline constexpr auto editorShortcutActions()
{
    return std::array{
        // 移动工具保留当前选择，后续指针操作由活动方案解释。
        EditorShortcutDescriptor{ EditorShortcutAction::ToolMove,
                                  "toolMove",
                                  "ui.settings.shortcut.tool_move",
                                  &ShortcutConfig::toolMove,
                                  0 },
        // 框选与移动工具分开，区域选择保留独立键位。
        EditorShortcutDescriptor{ EditorShortcutAction::ToolMarquee,
                                  "toolMarquee",
                                  "ui.settings.shortcut.tool_marquee",
                                  &ShortcutConfig::toolMarquee,
                                  0 },
        // 绘制工具建立画笔事务，不在按键触发时直接创建物件。
        EditorShortcutDescriptor{ EditorShortcutAction::ToolDraw,
                                  "toolDraw",
                                  "ui.settings.shortcut.tool_draw",
                                  &ShortcutConfig::toolDraw,
                                  0 },
        // 配色笔刷仍受专业模式门禁约束，重绑定不能绕过能力。
        EditorShortcutDescriptor{ EditorShortcutAction::ToolColorBrush,
                                  "toolColorBrush",
                                  "ui.settings.shortcut.tool_color_brush",
                                  &ShortcutConfig::toolColorBrush,
                                  0 },
        // 配色擦除只改变颜色属性，不等同于删除物件。
        EditorShortcutDescriptor{ EditorShortcutAction::ToolColorEraser,
                                  "toolColorEraser",
                                  "ui.settings.shortcut.tool_color_eraser",
                                  &ShortcutConfig::toolColorEraser,
                                  0 },
        // 镜像作用于当前选择，保持与镜像粘贴独立。
        EditorShortcutDescriptor{ EditorShortcutAction::Mirror,
                                  "mirror",
                                  "ui.settings.shortcut.mirror",
                                  &ShortcutConfig::mirror,
                                  0 },
        // 镜像作用于当前选择，保持与镜像粘贴独立。
        EditorShortcutDescriptor{ EditorShortcutAction::MirrorPaste,
                                  "mirrorPaste",
                                  "ui.settings.shortcut.mirror_paste",
                                  &ShortcutConfig::mirrorPaste,
                                  0 },
        // 音量批量编辑保留原有选择与可用性检查，默认无键位。
        EditorShortcutDescriptor{ EditorShortcutAction::EditSelectedVolume,
                                  "editSelectedVolume",
                                  "ui.settings.shortcut.edit_selected_volume",
                                  &ShortcutConfig::editSelectedVolume,
                                  0 },
        // 批注仍要求合法单个目标，旧冲突组合继续迁移。
        EditorShortcutDescriptor{
            EditorShortcutAction::AddSelectedAnnotation,
            "addSelectedAnnotation",
            "ui.settings.shortcut.add_selected_annotation",
            &ShortcutConfig::addSelectedAnnotation,
            0 },
        // 主画布删除使用用户绑定，时间线兼容退格入口另行保留。
        EditorShortcutDescriptor{ EditorShortcutAction::DeleteSelected,
                                  "deleteSelected",
                                  "ui.settings.shortcut.delete_selected",
                                  &ShortcutConfig::deleteSelected,
                                  0 },
        // 播放切换保留活动画笔和拖拽期间的既有焦点策略。
        EditorShortcutDescriptor{ EditorShortcutAction::TogglePlayback,
                                  "togglePlayback",
                                  "ui.settings.shortcut.toggle_playback",
                                  &ShortcutConfig::togglePlayback,
                                  0 },
        // BPM 段落插入只属于测量工具焦点，不能落到谱面。
        EditorShortcutDescriptor{
            EditorShortcutAction::AddBpmSegmentAtViewCenter,
            "addBpmSegmentAtViewCenter",
            "ui.settings.shortcut.add_bpm_segment_at_view_center",
            &ShortcutConfig::addBpmSegmentAtViewCenter,
            0 },
        // 滚动反向属于持续配置，与单次滚动命令分开。
        EditorShortcutDescriptor{ EditorShortcutAction::ToggleReverseScroll,
                                  "toggleReverseScroll",
                                  "ui.settings.shortcut.toggle_reverse_scroll",
                                  &ShortcutConfig::toggleReverseScroll,
                                  0 },
        // 滚动吸附与放置吸附职责不同，不合并两项配置。
        EditorShortcutDescriptor{ EditorShortcutAction::ToggleScrollSnap,
                                  "toggleScrollSnap",
                                  "ui.settings.shortcut.toggle_scroll_snap",
                                  &ShortcutConfig::toggleScrollSnap,
                                  0 },
        // 吸附向下取整单独保存，不隐式改变分拍除数。
        EditorShortcutDescriptor{ EditorShortcutAction::ToggleSnapFloor,
                                  "toggleSnapFloor",
                                  "ui.settings.shortcut.toggle_snap_floor",
                                  &ShortcutConfig::toggleSnapFloor,
                                  0 },
        // 视觉时间映射切换沿用逻辑配置更新通道。
        EditorShortcutDescriptor{
            EditorShortcutAction::ToggleScrollTimingMapping,
            "toggleScrollTimingMapping",
            "ui.settings.shortcut.toggle_scroll_timing_mapping",
            &ShortcutConfig::toggleScrollTimingMapping,
            0 },
        // 分拍线显示切换不改变谱面中的时间点。
        EditorShortcutDescriptor{ EditorShortcutAction::ToggleBeatLines,
                                  "toggleBeatLines",
                                  "ui.settings.shortcut.toggle_beat_lines",
                                  &ShortcutConfig::toggleBeatLines,
                                  0 },
        // 滚动停播开关是应用偏好，不等同于立即暂停。
        EditorShortcutDescriptor{
            EditorShortcutAction::ToggleStopPlaybackOnScroll,
            "toggleStopPlaybackOnScroll",
            "ui.settings.shortcut.toggle_stop_playback_on_scroll",
            &ShortcutConfig::toggleStopPlaybackOnScroll,
            0 },
        // 打击音效开关不改变物件音量或全局静音。
        EditorShortcutDescriptor{ EditorShortcutAction::ToggleHitSfx,
                                  "toggleHitSfx",
                                  "ui.settings.shortcut.toggle_hit_sfx",
                                  &ShortcutConfig::toggleHitSfx,
                                  0 },
        // 打击特效独立于音效，保留各自可选绑定。
        EditorShortcutDescriptor{ EditorShortcutAction::ToggleHitEffects,
                                  "toggleHitEffects",
                                  "ui.settings.shortcut.toggle_hit_effects",
                                  &ShortcutConfig::toggleHitEffects,
                                  0 },
        // 同主音轨同步仅切换会话同步策略，不重选资源。
        EditorShortcutDescriptor{
            EditorShortcutAction::ToggleSyncSameMainAudio,
            "toggleSyncSameMainAudio",
            "ui.settings.shortcut.toggle_sync_same_main_audio",
            &ShortcutConfig::toggleSyncSameMainAudio,
            0 },
        // 撤销默认排除 Shift，避免与备用重做组合同时触发。
        EditorShortcutDescriptor{ EditorShortcutAction::Undo,
                                  "undo",
                                  "ui.settings.shortcut.undo",
                                  &ShortcutConfig::m_undo,
                                  0 },
        // Ctrl+Y 的旧默认允许 Shift，用户重绑定后使用严格修饰匹配。
        EditorShortcutDescriptor{ EditorShortcutAction::Redo,
                                  "redo",
                                  "ui.settings.shortcut.redo",
                                  &ShortcutConfig::m_redo,
                                  2 },
        // Ctrl+Y 的旧默认允许 Shift，用户重绑定后使用严格修饰匹配。
        EditorShortcutDescriptor{ EditorShortcutAction::RedoAlternative,
                                  "redoAlternative",
                                  "ui.settings.shortcut.redo_alternative",
                                  &ShortcutConfig::m_redoAlternative,
                                  0 },
        // 复制保留旧默认的额外修饰键兼容，按下不允许自动重复。
        EditorShortcutDescriptor{ EditorShortcutAction::Copy,
                                  "copy",
                                  "ui.settings.shortcut.copy",
                                  &ShortcutConfig::m_copy,
                                  14 },
        // 剪切复用同一逻辑命令与撤销事务，不由 UI 直接删除对象。
        EditorShortcutDescriptor{ EditorShortcutAction::Cut,
                                  "cut",
                                  "ui.settings.shortcut.cut",
                                  &ShortcutConfig::m_cut,
                                  14 },
        // 普通粘贴将 Shift 默认组合留给镜像粘贴动作。
        EditorShortcutDescriptor{ EditorShortcutAction::Paste,
                                  "paste",
                                  "ui.settings.shortcut.paste",
                                  &ShortcutConfig::m_paste,
                                  12 },
        // 当前轨道区全选保留鼠标区域语义，不能扩大到全部物件。
        EditorShortcutDescriptor{ EditorShortcutAction::SelectAll,
                                  "selectAll",
                                  "ui.settings.shortcut.select_all",
                                  &ShortcutConfig::m_selectAll,
                                  12 },
        // 当前轨道区全选保留鼠标区域语义，不能扩大到全部物件。
        EditorShortcutDescriptor{ EditorShortcutAction::SelectAllObjects,
                                  "selectAllObjects",
                                  "ui.settings.shortcut.select_all_objects",
                                  &ShortcutConfig::m_selectAllObjects,
                                  12 },
        // 新建工程与新建谱面默认按 Shift 区分，保留原向导。
        EditorShortcutDescriptor{ EditorShortcutAction::NewProject,
                                  "newProject",
                                  "ui.settings.shortcut.new_project",
                                  &ShortcutConfig::m_newProject,
                                  12 },
        // 新建谱面通过激活来源保留菜单与快捷键的业务标记。
        EditorShortcutDescriptor{ EditorShortcutAction::NewBeatmap,
                                  "newBeatmap",
                                  "ui.settings.shortcut.new_beatmap",
                                  &ShortcutConfig::m_newBeatmap,
                                  12 },
        // 打开工程继续经过原文件选择与脏状态确认。
        EditorShortcutDescriptor{ EditorShortcutAction::OpenProject,
                                  "openProject",
                                  "ui.settings.shortcut.open_project",
                                  &ShortcutConfig::m_openProject,
                                  12 },
        // 普通保存不会绕过外部文件修改和覆盖确认流程。
        EditorShortcutDescriptor{ EditorShortcutAction::Save,
                                  "save",
                                  "ui.settings.shortcut.save",
                                  &ShortcutConfig::m_save,
                                  12 },
        // 普通保存不会绕过外部文件修改和覆盖确认流程。
        EditorShortcutDescriptor{ EditorShortcutAction::SaveAs,
                                  "saveAs",
                                  "ui.settings.shortcut.save_as",
                                  &ShortcutConfig::m_saveAs,
                                  12 },
        // 音频导入沿用完整资源校验，不把主键改绑当作资源加载成功。
        EditorShortcutDescriptor{ EditorShortcutAction::ImportAudio,
                                  "importAudio",
                                  "ui.settings.shortcut.import_audio",
                                  &ShortcutConfig::m_importAudio,
                                  0 },
        // 文件菜单导航只请求弹窗切换，不执行文件业务。
        EditorShortcutDescriptor{ EditorShortcutAction::MenuFile,
                                  "menuFile",
                                  "ui.settings.shortcut.menu_file",
                                  &ShortcutConfig::m_menuFile,
                                  11 },
        // 编辑菜单导航保留旧 Alt 组合的额外修饰键语义。
        EditorShortcutDescriptor{ EditorShortcutAction::MenuEdit,
                                  "menuEdit",
                                  "ui.settings.shortcut.menu_edit",
                                  &ShortcutConfig::m_menuEdit,
                                  11 },
        // 工具菜单只改变导航归属，操作仍由对应动作校验。
        EditorShortcutDescriptor{ EditorShortcutAction::MenuTools,
                                  "menuTools",
                                  "ui.settings.shortcut.menu_tools",
                                  &ShortcutConfig::m_menuTools,
                                  11 },
        // 查看菜单导航与普通粘贴共享字母但使用不同修饰键。
        EditorShortcutDescriptor{ EditorShortcutAction::MenuView,
                                  "menuView",
                                  "ui.settings.shortcut.menu_view",
                                  &ShortcutConfig::m_menuView,
                                  11 },
        // 帮助菜单导航不直接启动浏览器或打开系统目录。
        EditorShortcutDescriptor{ EditorShortcutAction::MenuHelp,
                                  "menuHelp",
                                  "ui.settings.shortcut.menu_help",
                                  &ShortcutConfig::m_menuHelp,
                                  11 },
        // 对齐仍由逻辑命令完成，不改变选中物件的权限与历史。
        EditorShortcutDescriptor{ EditorShortcutAction::AlignSelected,
                                  "alignSelected",
                                  "ui.settings.shortcut.align_selected",
                                  &ShortcutConfig::m_alignSelected,
                                  14 },
        // 取消只结束当前表面的临时状态，标准文本控件保持自身行为。
        EditorShortcutDescriptor{ EditorShortcutAction::CancelInteraction,
                                  "cancelInteraction",
                                  "ui.settings.shortcut.cancel_interaction",
                                  &ShortcutConfig::m_cancelInteraction,
                                  15 },
        // 重叠层导航不移动物件，只改变当前命中候选。
        EditorShortcutDescriptor{ EditorShortcutAction::PreviousLayerUp,
                                  "previousLayerUp",
                                  "ui.settings.shortcut.previous_layer_up",
                                  &ShortcutConfig::m_previousLayerUp,
                                  15 },
        // 备用层导航保留方向键对等语义，不吞掉文本光标。
        EditorShortcutDescriptor{ EditorShortcutAction::PreviousLayerLeft,
                                  "previousLayerLeft",
                                  "ui.settings.shortcut.previous_layer_left",
                                  &ShortcutConfig::m_previousLayerLeft,
                                  15 },
        // 下一层导航循环当前可见命中集合，不扫描整份谱面。
        EditorShortcutDescriptor{ EditorShortcutAction::NextLayerDown,
                                  "nextLayerDown",
                                  "ui.settings.shortcut.next_layer_down",
                                  &ShortcutConfig::m_nextLayerDown,
                                  15 },
        // 备用下一层键位与主键位各自可重绑定。
        EditorShortcutDescriptor{ EditorShortcutAction::NextLayerRight,
                                  "nextLayerRight",
                                  "ui.settings.shortcut.next_layer_right",
                                  &ShortcutConfig::m_nextLayerRight,
                                  15 },
        // 相机平移模式不等同于谱面物件移动工具。
        EditorShortcutDescriptor{ EditorShortcutAction::CameraMove,
                                  "cameraMove",
                                  "ui.settings.shortcut.camera_move",
                                  &ShortcutConfig::m_cameraMove,
                                  15 },
        // 相机旋转只改变布局参数，不修改谱面内容。
        EditorShortcutDescriptor{ EditorShortcutAction::CameraRotate,
                                  "cameraRotate",
                                  "ui.settings.shortcut.camera_rotate",
                                  &ShortcutConfig::m_cameraRotate,
                                  15 },
        // 轴约束保留同一拖拽起点，不重新创建编辑事务。
        EditorShortcutDescriptor{ EditorShortcutAction::CameraAxisX,
                                  "cameraAxisX",
                                  "ui.settings.shortcut.camera_axis_x",
                                  &ShortcutConfig::m_cameraAxisX,
                                  15 },
        // 纵向轴身份与重做动作按表面及焦点区分。
        EditorShortcutDescriptor{ EditorShortcutAction::CameraAxisY,
                                  "cameraAxisY",
                                  "ui.settings.shortcut.camera_axis_y",
                                  &ShortcutConfig::m_cameraAxisY,
                                  15 },
        // 深度轴不应因为撤销快捷键而触发谱面历史。
        EditorShortcutDescriptor{ EditorShortcutAction::CameraAxisZ,
                                  "cameraAxisZ",
                                  "ui.settings.shortcut.camera_axis_z",
                                  &ShortcutConfig::m_cameraAxisZ,
                                  15 },
        // 备用退格只在原本支持它的时间线入口生效。
        EditorShortcutDescriptor{ EditorShortcutAction::DeleteAlternative,
                                  "deleteAlternative",
                                  "ui.settings.shortcut.delete_alternative",
                                  &ShortcutConfig::m_deleteAlternative,
                                  15 },
        // 工具焦点优先于全局播放，不得把输入转交到后台谱面。
        EditorShortcutDescriptor{ EditorShortcutAction::BpmTogglePlayback,
                                  "bpmTogglePlayback",
                                  "ui.settings.shortcut.bpm_toggle_playback",
                                  &ShortcutConfig::m_bpmTogglePlayback,
                                  15 },
        // 音频结果导航只改变当前结果焦点，不移动谱面物件。
        EditorShortcutDescriptor{ EditorShortcutAction::AudioResultNext,
                                  "audioResultNext",
                                  "ui.settings.shortcut.audio_result_next",
                                  &ShortcutConfig::m_audioResultNext,
                                  15 },
        // 音频搜索保留按住重复导航，选择提交仍由原控件处理。
        EditorShortcutDescriptor{ EditorShortcutAction::AudioResultPrevious,
                                  "audioResultPrevious",
                                  "ui.settings.shortcut.audio_result_previous",
                                  &ShortcutConfig::m_audioResultPrevious,
                                  15 },
    };
}
/// @brief 根据稳定动作名查询描述，不使用显示文本或 ImGui 键值作为身份。
/// @warning 仅扫描固定小表，返回指针指向静态只读数据。
/// @param id 精确匹配的稳定字段名，大小写和拼写不能由显示文本推导。
/// @return 找不到返回空，不使用首项或键位相似项作为隐式回退。
/// @details 返回存储在函数静态 constexpr 数组内，不能释放或修改描述符。
inline const EditorShortcutDescriptor* findEditorShortcut(std::string_view id)
{
    static constexpr auto entries = editorShortcutActions();
    for ( const auto& entry : entries )
        if ( entry.m_id == id ) return &entry;
    return nullptr;
}
}  // namespace MMM::Config
