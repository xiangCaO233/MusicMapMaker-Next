#pragma once

/**
 * @file Icons.h
 * @brief Font icon definitions for the MusicMapMaker UI.
 *
 * This file contains UTF-8 encoded strings for FontAwesome 6 icons used in the
 * application. Using these constants instead of hardcoded hex strings ensures
 * consistency and easier maintenance.
 */

namespace MMM::UI
{

// --- General UI Icons ---
constexpr const char* ICON_MMM_DESKTOP = "\xef\x84\x88";  ///< \uf108 desktop
constexpr const char* ICON_MMM_EYE     = "\xef\x81\xae";  ///< \uf06e eye
constexpr const char* ICON_MMM_FOLDER  = "\xef\x81\xbb";  ///< \uf07b folder
constexpr const char* ICON_MMM_FOLDER_OPEN =
    "\xef\x81\xbc";  ///< \uf07c folder-open
constexpr const char* ICON_MMM_PEN =
    "\xef\x81\x80";  ///< \uf040 pencil (Draw Tool) - NerdFont Safe
constexpr const char* ICON_MMM_FILE   = "\xef\x85\x9b";  ///< \uf15b file
constexpr const char* ICON_MMM_MUSIC  = "\xef\x80\x81";  ///< \uf001 music
constexpr const char* ICON_MMM_COG    = "\xef\x80\x93";  ///< \uf013 cog
constexpr const char* ICON_MMM_SEARCH = "\xef\x80\x82";  ///< \uf002 search
constexpr const char* ICON_MMM_SAVE   = "\xef\x83\x87";  ///< \uf0c7 floppy-disk
constexpr const char* ICON_MMM_PACK   = "\xef\x86\x87";  ///< \uf187 box-archive
constexpr const char* ICON_MMM_BOOK   = "\xef\x80\xad";  ///< \uf02d book
constexpr const char* ICON_MMM_PLUS   = "\xef\x81\xa7";  ///< \uf067 plus
constexpr const char* ICON_MMM_FILE_ADD =
    "\xef\x8c\x99";  ///< \uf319 file-circle-plus

// --- Playback Icons ---
constexpr const char* ICON_MMM_PLAY  = "\xef\x81\x8b";  ///< \uf04b play
constexpr const char* ICON_MMM_PAUSE = "\xef\x81\x8c";  ///< \uf04c pause
constexpr const char* ICON_MMM_STOP  = "\xef\x81\x8d";  ///< \uf04d stop

// --- Edit Tool Icons ---
constexpr const char* ICON_MMM_MOUSE = "\xef\xa3\x8c";  ///< \uf8cc mouse
constexpr const char* ICON_MMM_MOUSE_POINTER =
    "\xef\x89\x85";  ///< \uf245 mouse-pointer (Move Tool)
constexpr const char* ICON_MMM_MOVE_ARROWS =
    "\xef\x81\x87";  ///< \uf047 arrows-alt
constexpr const char* ICON_MMM_HAND =
    "\xef\x89\x96";  ///< \uf256 hand-back-fist (Move tool)
constexpr const char* ICON_MMM_SQUARE_SELECT =
    "\xef\x83\x88";  ///< \uf0c8 square (Marquee Tool)
constexpr const char* ICON_MMM_SCISSORS =
    "\xef\x83\x84";  ///< \uf0c4 scissors (Cut Tool)
constexpr const char* ICON_MMM_UNDO = "\xef\x8b\xaa";  ///< \uf2ea rotate-left
constexpr const char* ICON_MMM_REDO = "\xef\x8b\xb9";  ///< \uf2f9 rotate-right
constexpr const char* ICON_MMM_COPY = "\xef\x83\x85";  ///< \uf0c5 copy
constexpr const char* ICON_MMM_PASTE =
    "\xef\x8c\xa8";  ///< \uf328 clipboard / paste

constexpr const char* ICON_MMM_ARROWS_UP_DOWN =
    "\xef\x81\xbd";  ///< \uf07d arrows-up-down

constexpr const char* ICON_MMM_MAGNET = "\xef\x81\xb6";  ///< \uf076 magnet
constexpr const char* ICON_MMM_ARROW_DOWN = "\xef\x81\xa3";  ///< \uf063 arrow-down
constexpr const char* ICON_MMM_BARS =
    "\xef\x83\x89";  ///< \uf0c9 bars (for beat divisor)

// --- Audio Icons ---
constexpr const char* ICON_MMM_VOLUME_HIGH =
    "\xef\x80\xa8";  ///< \uf028 volume-high
constexpr const char* ICON_MMM_VOLUME_LOW =
    "\xef\x80\xa7";  ///< \uf027 volume-low
constexpr const char* ICON_MMM_VOLUME_OFF =
    "\xef\x80\xa6";  ///< \uf026 volume-off
constexpr const char* ICON_MMM_VOLUME_MUTE =
    "\xef\x80\xa6";  ///< \uf026 volume-off (Mute Fallback)

// --- Window Control Icons ---
constexpr const char* ICON_MMM_MINIMIZE =
    "\xef\x8b\x91";  ///< \uf2d1 window-minimize
constexpr const char* ICON_MMM_MAXIMIZE =
    "\xef\x8b\x90";  ///< \uf2d0 window-maximize
constexpr const char* ICON_MMM_RESTORE =
    "\xef\x8b\x92";  ///< \uf2d2 window-restore
constexpr const char* ICON_MMM_CLOSE =
    "\xef\x80\x8d";  ///< \uf00d xmark / close

}  // namespace MMM::UI
