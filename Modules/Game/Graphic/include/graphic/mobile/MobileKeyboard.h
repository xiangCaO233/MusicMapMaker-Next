#pragma once
namespace MMM::Graphic
{
/// @brief 发布本窗口内容区域被系统键盘遮挡的底部像素与参考高度。
/// @details 两值作为一个快照发布；不包含焦点、文本或 Java 对象。
/// @warning Android Activity 线程低频写入；UI 线程只读取独立比例，不等待 JNI。
void publishMobileKeyboardOcclusion(int bottom, int contentHeight);
/// @brief 取得底部遮挡比例，桌面没有发布者时恒为零。
/// @warning 每帧只作一次 relaxed 原子读取；该快照不负责发布其它资源。
float mobileKeyboardBottomFraction();
}  // namespace MMM::Graphic
