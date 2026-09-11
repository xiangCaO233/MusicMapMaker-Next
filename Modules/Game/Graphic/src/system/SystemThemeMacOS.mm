#if defined(__APPLE__)

#    include "graphic/system/SystemTheme.h"
#    import <AppKit/AppKit.h>

/// @file
/// @brief 使用 AppKit 将 macOS 当前有效外观映射为跨平台主题枚举。

namespace MMM::Graphic
{
/// @brief 通过 AppKit 有效外观查询 macOS 当前亮暗主题。
/// @return 可识别时返回 Light 或 Dark，否则返回 Unknown。
/// @warning 调用 AppKit 并创建自动释放对象，只应在主线程低频刷新路径执行。
SystemTheme queryMacOSSystemTheme()
{
    // 为本次 Objective-C 消息产生的临时对象限定自动释放生命周期。
    @autoreleasepool {
        // effectiveAppearance 已合并应用、窗口和系统层级的最终外观选择。
        NSAppearance* appearance = NSApp.effectiveAppearance;
        if ( !appearance ) return SystemTheme::Unknown;

        // 只在标准亮暗候选中请求最佳匹配，不依赖具体 macOS 主题名称扩展。
        NSAppearanceName match = [appearance
            bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua,
                                                 NSAppearanceNameDarkAqua ]];
        if ( [match isEqualToString:NSAppearanceNameDarkAqua] ) {
            // Dark Aqua 是系统标准暗色外观。
            return SystemTheme::Dark;
        }
        if ( [match isEqualToString:NSAppearanceNameAqua] ) {
            // Aqua 是系统标准亮色外观。
            return SystemTheme::Light;
        }
        // 自定义或未来外观无法归类时交由上层使用默认主题。
        return SystemTheme::Unknown;
    }
}
}  // namespace MMM::Graphic

#endif
