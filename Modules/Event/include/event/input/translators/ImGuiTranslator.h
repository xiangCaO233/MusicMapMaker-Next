#pragma once

#include "event/input/MMMInput.h"
#include <imgui.h>

namespace MMM::Event::Translator::ImGui
{
inline Input::Modifiers GetMods()
{
    Input::Modifiers m;
    ImGuiIO&         io = ::ImGui::GetIO();
    // ImGui 1.89+ 使用 io.KeyMods
    m.shift = (io.KeyMods & ImGuiMod_Shift) != 0;
    m.ctrl  = (io.KeyMods & ImGuiMod_Ctrl) != 0;
    m.alt   = (io.KeyMods & ImGuiMod_Alt) != 0;
    m.super = (io.KeyMods & ImGuiMod_Super) != 0;
    // ImGui 不直接追踪大写锁和数字锁状态
    m.capsLock = false;
    m.numLock  = false;
    return m;
}

inline Input::MouseButton GetMouseButton(int button)
{
    switch ( button ) {
    case ImGuiMouseButton_Left: return Input::MouseButton::Left;
    case ImGuiMouseButton_Right: return Input::MouseButton::Right;
    case ImGuiMouseButton_Middle: return Input::MouseButton::Middle;
    // ImGui 默认通常只处理左中右三个键，除非自定义后端
    default: return Input::MouseButton::Left;
    }
}

inline Input::Key GetKey(ImGuiKey key)
{
    switch ( key ) {
    case ImGuiKey_A: return Input::Key::A;
    case ImGuiKey_B: return Input::Key::B;
    case ImGuiKey_C: return Input::Key::C;
    case ImGuiKey_D: return Input::Key::D;
    case ImGuiKey_E: return Input::Key::E;
    case ImGuiKey_F: return Input::Key::F;
    case ImGuiKey_G: return Input::Key::G;
    case ImGuiKey_H: return Input::Key::H;
    case ImGuiKey_I: return Input::Key::I;
    case ImGuiKey_J: return Input::Key::J;
    case ImGuiKey_K: return Input::Key::K;
    case ImGuiKey_L: return Input::Key::L;
    case ImGuiKey_M: return Input::Key::M;
    case ImGuiKey_N: return Input::Key::N;
    case ImGuiKey_O: return Input::Key::O;
    case ImGuiKey_P: return Input::Key::P;
    case ImGuiKey_Q: return Input::Key::Q;
    case ImGuiKey_R: return Input::Key::R;
    case ImGuiKey_S: return Input::Key::S;
    case ImGuiKey_T: return Input::Key::T;
    case ImGuiKey_U: return Input::Key::U;
    case ImGuiKey_V: return Input::Key::V;
    case ImGuiKey_W: return Input::Key::W;
    case ImGuiKey_X: return Input::Key::X;
    case ImGuiKey_Y: return Input::Key::Y;
    case ImGuiKey_Z: return Input::Key::Z;
    case ImGuiKey_0: return Input::Key::D0;
    case ImGuiKey_1: return Input::Key::D1;
    case ImGuiKey_2: return Input::Key::D2;
    case ImGuiKey_3: return Input::Key::D3;
    case ImGuiKey_4: return Input::Key::D4;
    case ImGuiKey_5: return Input::Key::D5;
    case ImGuiKey_6: return Input::Key::D6;
    case ImGuiKey_7: return Input::Key::D7;
    case ImGuiKey_8: return Input::Key::D8;
    case ImGuiKey_9: return Input::Key::D9;
    case ImGuiKey_F1: return Input::Key::F1;
    case ImGuiKey_F2: return Input::Key::F2;
    case ImGuiKey_F3: return Input::Key::F3;
    case ImGuiKey_F4: return Input::Key::F4;
    case ImGuiKey_F5: return Input::Key::F5;
    case ImGuiKey_F6: return Input::Key::F6;
    case ImGuiKey_F7: return Input::Key::F7;
    case ImGuiKey_F8: return Input::Key::F8;
    case ImGuiKey_F9: return Input::Key::F9;
    case ImGuiKey_F10: return Input::Key::F10;
    case ImGuiKey_F11: return Input::Key::F11;
    case ImGuiKey_F12: return Input::Key::F12;
    case ImGuiKey_Escape: return Input::Key::Escape;
    case ImGuiKey_Enter: return Input::Key::Enter;
    case ImGuiKey_Tab: return Input::Key::Tab;
    case ImGuiKey_Backspace: return Input::Key::Backspace;
    case ImGuiKey_Insert: return Input::Key::Insert;
    case ImGuiKey_Delete: return Input::Key::Delete;
    case ImGuiKey_RightArrow: return Input::Key::Right;
    case ImGuiKey_LeftArrow: return Input::Key::Left;
    case ImGuiKey_DownArrow: return Input::Key::Down;
    case ImGuiKey_UpArrow: return Input::Key::Up;
    case ImGuiKey_PageUp: return Input::Key::PageUp;
    case ImGuiKey_PageDown: return Input::Key::PageDown;
    case ImGuiKey_Home: return Input::Key::Home;
    case ImGuiKey_End: return Input::Key::End;
    case ImGuiKey_CapsLock: return Input::Key::CapsLock;
    case ImGuiKey_ScrollLock: return Input::Key::ScrollLock;
    case ImGuiKey_NumLock: return Input::Key::NumLock;
    case ImGuiKey_PrintScreen: return Input::Key::PrintScreen;
    case ImGuiKey_Pause: return Input::Key::Pause;
    case ImGuiKey_LeftShift: return Input::Key::LeftShift;
    case ImGuiKey_LeftCtrl: return Input::Key::LeftControl;
    case ImGuiKey_LeftAlt: return Input::Key::LeftAlt;
    case ImGuiKey_LeftSuper: return Input::Key::LeftSuper;
    case ImGuiKey_RightShift: return Input::Key::RightShift;
    case ImGuiKey_RightCtrl: return Input::Key::RightControl;
    case ImGuiKey_RightAlt: return Input::Key::RightAlt;
    case ImGuiKey_RightSuper: return Input::Key::RightSuper;
    case ImGuiKey_Menu: return Input::Key::Menu;
    case ImGuiKey_Space: return Input::Key::Space;
    case ImGuiKey_Apostrophe: return Input::Key::Apostrophe;
    case ImGuiKey_Comma: return Input::Key::Comma;
    case ImGuiKey_Minus: return Input::Key::Minus;
    case ImGuiKey_Period: return Input::Key::Period;
    case ImGuiKey_Slash: return Input::Key::Slash;
    case ImGuiKey_Semicolon: return Input::Key::Semicolon;
    case ImGuiKey_Equal: return Input::Key::Equal;
    case ImGuiKey_LeftBracket: return Input::Key::LeftBracket;
    case ImGuiKey_Backslash: return Input::Key::Backslash;
    case ImGuiKey_RightBracket: return Input::Key::RightBracket;
    case ImGuiKey_GraveAccent: return Input::Key::GraveAccent;
    case ImGuiKey_Keypad0: return Input::Key::KP_0;
    case ImGuiKey_Keypad1: return Input::Key::KP_1;
    case ImGuiKey_Keypad2: return Input::Key::KP_2;
    case ImGuiKey_Keypad3: return Input::Key::KP_3;
    case ImGuiKey_Keypad4: return Input::Key::KP_4;
    case ImGuiKey_Keypad5: return Input::Key::KP_5;
    case ImGuiKey_Keypad6: return Input::Key::KP_6;
    case ImGuiKey_Keypad7: return Input::Key::KP_7;
    case ImGuiKey_Keypad8: return Input::Key::KP_8;
    case ImGuiKey_Keypad9: return Input::Key::KP_9;
    case ImGuiKey_KeypadDecimal: return Input::Key::KP_Decimal;
    case ImGuiKey_KeypadDivide: return Input::Key::KP_Divide;
    case ImGuiKey_KeypadMultiply: return Input::Key::KP_Multiply;
    case ImGuiKey_KeypadSubtract: return Input::Key::KP_Subtract;
    case ImGuiKey_KeypadAdd: return Input::Key::KP_Add;
    case ImGuiKey_KeypadEnter: return Input::Key::KP_Enter;
    case ImGuiKey_KeypadEqual: return Input::Key::KP_Equal;
    default: return Input::Key::Unknown;
    }
}
}  // namespace MMM::Event::Translator::ImGui
