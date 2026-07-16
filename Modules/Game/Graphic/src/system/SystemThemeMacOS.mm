#if defined(__APPLE__)

#    include "graphic/system/SystemTheme.h"
#    import <AppKit/AppKit.h>

namespace MMM::Graphic
{
/// @brief 通过 AppKit 有效外观查询 macOS 当前亮暗主题。
/// @return 可识别时返回 Light 或 Dark，否则返回 Unknown。
SystemTheme queryMacOSSystemTheme()
{
    @autoreleasepool {
        NSAppearance* appearance = NSApp.effectiveAppearance;
        if ( !appearance ) return SystemTheme::Unknown;

        NSAppearanceName match = [appearance
            bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua,
                                                 NSAppearanceNameDarkAqua ]];
        if ( [match isEqualToString:NSAppearanceNameDarkAqua] ) {
            return SystemTheme::Dark;
        }
        if ( [match isEqualToString:NSAppearanceNameAqua] ) {
            return SystemTheme::Light;
        }
        return SystemTheme::Unknown;
    }
}
}  // namespace MMM::Graphic

#endif
