#pragma once

/**
 * @file Icons.h
 * @brief MusicMapMaker UI 使用的字体图标定义。
 *
 * 本文件保存 FontAwesome 6 图标的 UTF-8 字符串常量，避免在业务代码中散落
 * 硬编码十六进制字符串。
 * 常量均为编译期字符串指针，不拥有字体资源；实际字形是否可见取决于当前
 * 皮肤合并的图标字体覆盖范围。
 */

namespace MMM::UI
{

/// @name 通用 UI 图标
/// @brief 文件、导航、保存和协作入口共用的基础图标。
/// @details 常量内容是 Nerd Font/FontAwesome UTF-8 编码，调用方直接与标签拼接。
/// @{
constexpr const char* ICON_MMM_DESKTOP = "\xef\x84\x88";  ///< \uf108 desktop
constexpr const char* ICON_MMM_EYE     = "\xef\x81\xae";  ///< \uf06e eye
constexpr const char* ICON_MMM_FOLDER  = "\xef\x81\xbb";  ///< \uf07b folder
constexpr const char* ICON_MMM_FOLDER_OPEN =
    "\xef\x81\xbc";  ///< \uf07c folder-open
constexpr const char* ICON_MMM_PEN =
    "\xef\x81\x80";  ///< \uf040 pencil (Draw Tool) - NerdFont Safe
constexpr const char* ICON_MMM_FILE     = "\xef\x85\x9b";  ///< \uf15b file
constexpr const char* ICON_MMM_MUSIC    = "\xef\x80\x81";  ///< \uf001 music
constexpr const char* ICON_MMM_COG      = "\xef\x80\x93";  ///< \uf013 cog
constexpr const char* ICON_MMM_KEYBOARD = "\xef\x84\x9c";  ///< \uf11c keyboard
constexpr const char* ICON_MMM_SEARCH   = "\xef\x80\x82";  ///< \uf002 search
constexpr const char* ICON_MMM_SAVE  = "\xef\x83\x87";  ///< \uf0c7 floppy-disk
constexpr const char* ICON_MMM_PACK  = "\xef\x86\x87";  ///< \uf187 box-archive
constexpr const char* ICON_MMM_BOOK  = "\xef\x80\xad";  ///< \uf02d book
constexpr const char* ICON_MMM_CHECK = "\xef\x80\x8c";  ///< 已了解或已完成。
constexpr const char* ICON_MMM_ARROW_LEFT = "\xef\x81\xa0";  ///< 返回上一页。
constexpr const char* ICON_MMM_PLUS       = "\xef\x81\xa7";  ///< \uf067 plus
constexpr const char* ICON_MMM_BUG        = "\xef\x86\x88";  ///< \uf188 bug
constexpr const char* ICON_MMM_FILE_ADD =
    "\xef\x8c\x99";  ///< \uf319 file-circle-plus
constexpr const char* ICON_MMM_LINK  = "\xef\x83\x81";  ///< \uf0c1 link
constexpr const char* ICON_MMM_USERS = "\xef\x83\x80";  ///< \uf0c0 users
/// @}

/// @name 播放控制图标
/// @brief 播放、暂停和停止按钮使用的传输控制符号。
/// @{
constexpr const char* ICON_MMM_PLAY  = "\xef\x81\x8b";  ///< \uf04b play
constexpr const char* ICON_MMM_PAUSE = "\xef\x81\x8c";  ///< \uf04c pause
constexpr const char* ICON_MMM_STOP  = "\xef\x81\x8d";  ///< \uf04d stop
/// @}

/// @name 编辑工具图标
/// @brief 画布工具、剪贴板命令及编辑模式入口使用的符号。
/// @details 相同字形可在不同语义按钮复用，但常量名保留业务含义。
/// @{
constexpr const char* ICON_MMM_MOUSE = "\xef\xa3\x8c";  ///< \uf8cc mouse
constexpr const char* ICON_MMM_MOUSE_POINTER =
    "\xef\x89\x85";  ///< \uf245 mouse-pointer (Move Tool)
constexpr const char* ICON_MMM_MOVE_ARROWS =
    "\xef\x81\x87";  ///< \uf047 arrows-alt
constexpr const char* ICON_MMM_TRACK_LAYOUT =
    "\xef\x81\x87";  ///< \uf047 arrows-alt (Track Layout Tool)
constexpr const char* ICON_MMM_HAND =
    "\xef\x89\x96";  ///< \uf256 hand-back-fist (Move tool)
constexpr const char* ICON_MMM_SQUARE_SELECT =
    "\xef\x83\x88";  ///< \uf0c8 square (Marquee Tool)
constexpr const char* ICON_MMM_PAINT_BRUSH =
    "\xef\x87\xbc";  ///< \uf1fc paint-brush (Color Brush Tool)
constexpr const char* ICON_MMM_ERASER =
    "\xef\x84\xad";  ///< \uf12d eraser (Color Eraser Tool)
constexpr const char* ICON_MMM_SCISSORS =
    "\xef\x83\x84";  ///< \uf0c4 scissors (Cut Tool)
constexpr const char* ICON_MMM_UNDO  = "\xef\x8b\xaa";  ///< \uf2ea rotate-left
constexpr const char* ICON_MMM_REDO  = "\xef\x8b\xb9";  ///< \uf2f9 rotate-right
constexpr const char* ICON_MMM_COPY  = "\xef\x83\x85";  ///< \uf0c5 copy
constexpr const char* ICON_MMM_PASTE = "\xef\x83\xaa";  ///< \uf0ea paste
constexpr const char* ICON_MMM_MIRROR = "\xef\x81\xbe";  ///< \uf07e arrows-h
constexpr const char* ICON_MMM_SELECT_ALL =
    "\xef\x89\x87";  ///< \uf247 object-group
constexpr const char* ICON_MMM_COMMENT = "\xef\x81\xb5";  ///< \uf075 comment
/// @brief 专业模式调节滑杆图标（Nerd Font fa-sliders，U+F1DE）。
constexpr const char* ICON_MMM_SLIDERS = "\xef\x87\x9e";
/// @brief 折线编辑图标（Nerd Font fa-line_chart，U+F201）。
constexpr const char* ICON_MMM_POLYLINE = "\xef\x88\x81";

constexpr const char* ICON_MMM_ARROWS_UP_DOWN =
    "\xef\x81\xbd";  ///< \uf07d arrows-up-down

constexpr const char* ICON_MMM_MAGNET = "\xef\x81\xb6";  ///< \uf076 magnet
constexpr const char* ICON_MMM_ARROW_DOWN =
    "\xef\x81\xa3";  ///< \uf063 arrow-down
constexpr const char* ICON_MMM_BARS =
    "\xef\x83\x89";  ///< \uf0c9 bars (for beat divisor)
/// @}

/// @name 音频图标
/// @brief 音量等级、静音、音效与视觉效果入口使用的符号。
/// @note MUTE 当前回退到 volume-off 字形，业务代码仍使用独立语义名称。
/// @{
constexpr const char* ICON_MMM_VOLUME_HIGH =
    "\xef\x80\xa8";  ///< \uf028 volume-high
constexpr const char* ICON_MMM_VOLUME_LOW =
    "\xef\x80\xa7";  ///< \uf027 volume-low
constexpr const char* ICON_MMM_VOLUME_OFF =
    "\xef\x80\xa6";  ///< \uf026 volume-off
constexpr const char* ICON_MMM_VOLUME_MUTE =
    "\xef\x80\xa6";  ///< \uf026 volume-off (Mute Fallback)

constexpr const char* ICON_MMM_HIT_SFX = "\xef\x80\x81";  ///< \uf001 music
constexpr const char* ICON_MMM_VISUAL_EFFECTS =
    "\xef\x83\xa7";  ///< \uf0e7 bolt
/// @}

/// @name 窗口控制图标
/// @brief 自定义标题栏最小化、最大化、还原和关闭操作的符号。
/// @{
constexpr const char* ICON_MMM_MINIMIZE =
    "\xef\x8b\x91";  ///< \uf2d1 window-minimize
constexpr const char* ICON_MMM_MAXIMIZE =
    "\xef\x8b\x90";  ///< \uf2d0 window-maximize
constexpr const char* ICON_MMM_RESTORE =
    "\xef\x8b\x92";  ///< \uf2d2 window-restore
constexpr const char* ICON_MMM_CLOSE =
    "\xef\x80\x8d";  ///< \uf00d xmark / close
/// @}

/// @name 帮助与更新图标
/// @brief 下载更新和信息提示入口使用的符号。
/// @{
constexpr const char* ICON_MMM_DOWNLOAD =
    "\xef\x8C\x81";  ///< \uf381 cloud-arrow-down
constexpr const char* ICON_MMM_INFO_CIRCLE =
    "\xef\x81\x9a";  ///< \uf05a circle-info
/// @}

}  // namespace MMM::UI
