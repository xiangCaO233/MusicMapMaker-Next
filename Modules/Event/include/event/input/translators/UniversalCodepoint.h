#pragma once

#include "event/input/MMMInput.h"

namespace MMM::Event::Translator
{
inline char32_t ResolveCodepoint(Input::Key key, const Input::Modifiers& mods)
{
    int k = static_cast<int>(key);

    if ( key >= Input::Key::A && key <= Input::Key::Z ) {
        bool uppercase = (mods.shift != mods.capsLock);
        return uppercase ? (U'A' + (k - static_cast<int>(Input::Key::A)))
                         : (U'a' + (k - static_cast<int>(Input::Key::A)));
    }

    if ( key >= Input::Key::D0 && key <= Input::Key::D9 ) {
        if ( !mods.shift ) return U'0' + (k - static_cast<int>(Input::Key::D0));
        switch ( key ) {
        case Input::Key::D1: return U'!';
        case Input::Key::D2: return U'@';
        case Input::Key::D3: return U'#';
        case Input::Key::D4: return U'$';
        case Input::Key::D5: return U'%';
        case Input::Key::D6: return U'^';
        case Input::Key::D7: return U'&';
        case Input::Key::D8: return U'*';
        case Input::Key::D9: return U'(';
        case Input::Key::D0: return U')';
        default: return 0;
        }
    }

    switch ( key ) {
    case Input::Key::Space: return U' ';
    case Input::Key::GraveAccent: return mods.shift ? U'~' : U'`';
    case Input::Key::Minus: return mods.shift ? U'_' : U'-';
    case Input::Key::Equal: return mods.shift ? U'+' : U'=';
    case Input::Key::LeftBracket: return mods.shift ? U'{' : U'[';
    case Input::Key::RightBracket: return mods.shift ? U'}' : U']';
    case Input::Key::Backslash: return mods.shift ? U'|' : U'\\';
    case Input::Key::Semicolon: return mods.shift ? U':' : U';';
    case Input::Key::Apostrophe: return mods.shift ? U'"' : U'\'';
    case Input::Key::Comma: return mods.shift ? U'<' : U',';
    case Input::Key::Period: return mods.shift ? U'>' : U'.';
    case Input::Key::Slash: return mods.shift ? U'?' : U'/';
    case Input::Key::KP_0: return U'0';
    case Input::Key::KP_1: return U'1';
    case Input::Key::KP_2: return U'2';
    case Input::Key::KP_3: return U'3';
    case Input::Key::KP_4: return U'4';
    case Input::Key::KP_5: return U'5';
    case Input::Key::KP_6: return U'6';
    case Input::Key::KP_7: return U'7';
    case Input::Key::KP_8: return U'8';
    case Input::Key::KP_9: return U'9';
    case Input::Key::KP_Decimal: return U'.';
    case Input::Key::KP_Divide: return U'/';
    case Input::Key::KP_Multiply: return U'*';
    case Input::Key::KP_Subtract: return U'-';
    case Input::Key::KP_Add: return U'+';
    case Input::Key::KP_Equal: return U'=';
    case Input::Key::KP_Enter: return U'\n';
    case Input::Key::Enter: return U'\n';
    case Input::Key::Tab: return U'\t';
    default: return 0;
    }
}
}  // namespace MMM::Event::Translator
