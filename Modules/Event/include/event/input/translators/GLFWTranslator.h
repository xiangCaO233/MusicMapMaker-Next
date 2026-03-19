#pragma once

#include "event/input/MMMInput.h"
#include "graphic/glfw/GLFWHeader.h"

namespace MMM::Event::Translator::GLFW
{
inline Input::Action GetAction(int action)
{
    if ( action == GLFW_PRESS ) return Input::Action::Press;
    if ( action == GLFW_RELEASE ) return Input::Action::Release;
    if ( action == GLFW_REPEAT ) return Input::Action::Repeat;
    return Input::Action::Release;
}

inline Input::Modifiers GetMods(int mods)
{
    Input::Modifiers m;
    m.shift    = (mods & GLFW_MOD_SHIFT) != 0;
    m.ctrl     = (mods & GLFW_MOD_CONTROL) != 0;
    m.alt      = (mods & GLFW_MOD_ALT) != 0;
    m.super    = (mods & GLFW_MOD_SUPER) != 0;
    m.capsLock = (mods & GLFW_MOD_CAPS_LOCK) != 0;
    m.numLock  = (mods & GLFW_MOD_NUM_LOCK) != 0;
    return m;
}

inline Input::MouseButton GetMouseButton(int button)
{
    switch ( button ) {
    case GLFW_MOUSE_BUTTON_LEFT: return Input::MouseButton::Left;
    case GLFW_MOUSE_BUTTON_RIGHT: return Input::MouseButton::Right;
    case GLFW_MOUSE_BUTTON_MIDDLE: return Input::MouseButton::Middle;
    case GLFW_MOUSE_BUTTON_4: return Input::MouseButton::Button4;
    case GLFW_MOUSE_BUTTON_5: return Input::MouseButton::Button5;
    case GLFW_MOUSE_BUTTON_6: return Input::MouseButton::Button6;
    case GLFW_MOUSE_BUTTON_7: return Input::MouseButton::Button7;
    case GLFW_MOUSE_BUTTON_8: return Input::MouseButton::Button8;
    default: return Input::MouseButton::Left;
    }
}

inline Input::Key GetKey(int key)
{
    switch ( key ) {
    case GLFW_KEY_A: return Input::Key::A;
    case GLFW_KEY_B: return Input::Key::B;
    case GLFW_KEY_C: return Input::Key::C;
    case GLFW_KEY_D: return Input::Key::D;
    case GLFW_KEY_E: return Input::Key::E;
    case GLFW_KEY_F: return Input::Key::F;
    case GLFW_KEY_G: return Input::Key::G;
    case GLFW_KEY_H: return Input::Key::H;
    case GLFW_KEY_I: return Input::Key::I;
    case GLFW_KEY_J: return Input::Key::J;
    case GLFW_KEY_K: return Input::Key::K;
    case GLFW_KEY_L: return Input::Key::L;
    case GLFW_KEY_M: return Input::Key::M;
    case GLFW_KEY_N: return Input::Key::N;
    case GLFW_KEY_O: return Input::Key::O;
    case GLFW_KEY_P: return Input::Key::P;
    case GLFW_KEY_Q: return Input::Key::Q;
    case GLFW_KEY_R: return Input::Key::R;
    case GLFW_KEY_S: return Input::Key::S;
    case GLFW_KEY_T: return Input::Key::T;
    case GLFW_KEY_U: return Input::Key::U;
    case GLFW_KEY_V: return Input::Key::V;
    case GLFW_KEY_W: return Input::Key::W;
    case GLFW_KEY_X: return Input::Key::X;
    case GLFW_KEY_Y: return Input::Key::Y;
    case GLFW_KEY_Z: return Input::Key::Z;
    case GLFW_KEY_0: return Input::Key::D0;
    case GLFW_KEY_1: return Input::Key::D1;
    case GLFW_KEY_2: return Input::Key::D2;
    case GLFW_KEY_3: return Input::Key::D3;
    case GLFW_KEY_4: return Input::Key::D4;
    case GLFW_KEY_5: return Input::Key::D5;
    case GLFW_KEY_6: return Input::Key::D6;
    case GLFW_KEY_7: return Input::Key::D7;
    case GLFW_KEY_8: return Input::Key::D8;
    case GLFW_KEY_9: return Input::Key::D9;
    case GLFW_KEY_F1: return Input::Key::F1;
    case GLFW_KEY_F2: return Input::Key::F2;
    case GLFW_KEY_F3: return Input::Key::F3;
    case GLFW_KEY_F4: return Input::Key::F4;
    case GLFW_KEY_F5: return Input::Key::F5;
    case GLFW_KEY_F6: return Input::Key::F6;
    case GLFW_KEY_F7: return Input::Key::F7;
    case GLFW_KEY_F8: return Input::Key::F8;
    case GLFW_KEY_F9: return Input::Key::F9;
    case GLFW_KEY_F10: return Input::Key::F10;
    case GLFW_KEY_F11: return Input::Key::F11;
    case GLFW_KEY_F12: return Input::Key::F12;
    case GLFW_KEY_F13: return Input::Key::F13;
    case GLFW_KEY_F14: return Input::Key::F14;
    case GLFW_KEY_F15: return Input::Key::F15;
    case GLFW_KEY_ESCAPE: return Input::Key::Escape;
    case GLFW_KEY_ENTER: return Input::Key::Enter;
    case GLFW_KEY_TAB: return Input::Key::Tab;
    case GLFW_KEY_BACKSPACE: return Input::Key::Backspace;
    case GLFW_KEY_INSERT: return Input::Key::Insert;
    case GLFW_KEY_DELETE: return Input::Key::Delete;
    case GLFW_KEY_RIGHT: return Input::Key::Right;
    case GLFW_KEY_LEFT: return Input::Key::Left;
    case GLFW_KEY_DOWN: return Input::Key::Down;
    case GLFW_KEY_UP: return Input::Key::Up;
    case GLFW_KEY_PAGE_UP: return Input::Key::PageUp;
    case GLFW_KEY_PAGE_DOWN: return Input::Key::PageDown;
    case GLFW_KEY_HOME: return Input::Key::Home;
    case GLFW_KEY_END: return Input::Key::End;
    case GLFW_KEY_CAPS_LOCK: return Input::Key::CapsLock;
    case GLFW_KEY_SCROLL_LOCK: return Input::Key::ScrollLock;
    case GLFW_KEY_NUM_LOCK: return Input::Key::NumLock;
    case GLFW_KEY_PRINT_SCREEN: return Input::Key::PrintScreen;
    case GLFW_KEY_PAUSE: return Input::Key::Pause;
    case GLFW_KEY_LEFT_SHIFT: return Input::Key::LeftShift;
    case GLFW_KEY_LEFT_CONTROL: return Input::Key::LeftControl;
    case GLFW_KEY_LEFT_ALT: return Input::Key::LeftAlt;
    case GLFW_KEY_LEFT_SUPER: return Input::Key::LeftSuper;
    case GLFW_KEY_RIGHT_SHIFT: return Input::Key::RightShift;
    case GLFW_KEY_RIGHT_CONTROL: return Input::Key::RightControl;
    case GLFW_KEY_RIGHT_ALT: return Input::Key::RightAlt;
    case GLFW_KEY_RIGHT_SUPER: return Input::Key::RightSuper;
    case GLFW_KEY_MENU: return Input::Key::Menu;
    case GLFW_KEY_SPACE: return Input::Key::Space;
    case GLFW_KEY_APOSTROPHE: return Input::Key::Apostrophe;
    case GLFW_KEY_COMMA: return Input::Key::Comma;
    case GLFW_KEY_MINUS: return Input::Key::Minus;
    case GLFW_KEY_PERIOD: return Input::Key::Period;
    case GLFW_KEY_SLASH: return Input::Key::Slash;
    case GLFW_KEY_SEMICOLON: return Input::Key::Semicolon;
    case GLFW_KEY_EQUAL: return Input::Key::Equal;
    case GLFW_KEY_LEFT_BRACKET: return Input::Key::LeftBracket;
    case GLFW_KEY_BACKSLASH: return Input::Key::Backslash;
    case GLFW_KEY_RIGHT_BRACKET: return Input::Key::RightBracket;
    case GLFW_KEY_GRAVE_ACCENT: return Input::Key::GraveAccent;
    case GLFW_KEY_KP_0: return Input::Key::KP_0;
    case GLFW_KEY_KP_1: return Input::Key::KP_1;
    case GLFW_KEY_KP_2: return Input::Key::KP_2;
    case GLFW_KEY_KP_3: return Input::Key::KP_3;
    case GLFW_KEY_KP_4: return Input::Key::KP_4;
    case GLFW_KEY_KP_5: return Input::Key::KP_5;
    case GLFW_KEY_KP_6: return Input::Key::KP_6;
    case GLFW_KEY_KP_7: return Input::Key::KP_7;
    case GLFW_KEY_KP_8: return Input::Key::KP_8;
    case GLFW_KEY_KP_9: return Input::Key::KP_9;
    case GLFW_KEY_KP_DECIMAL: return Input::Key::KP_Decimal;
    case GLFW_KEY_KP_DIVIDE: return Input::Key::KP_Divide;
    case GLFW_KEY_KP_MULTIPLY: return Input::Key::KP_Multiply;
    case GLFW_KEY_KP_SUBTRACT: return Input::Key::KP_Subtract;
    case GLFW_KEY_KP_ADD: return Input::Key::KP_Add;
    case GLFW_KEY_KP_ENTER: return Input::Key::KP_Enter;
    case GLFW_KEY_KP_EQUAL: return Input::Key::KP_Equal;
    default: return Input::Key::Unknown;
    }
}
}  // namespace MMM::Event::Translator::GLFW
