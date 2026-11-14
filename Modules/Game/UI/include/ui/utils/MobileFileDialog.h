#pragma once

struct ImVec2;

namespace MMM::UI
{
/// @brief 仅在移动外壳初始化时安装长生命周期的内置文件选择器扩展。
/// @details 不向工作区暴露第三方头文件、宏或内部状态；桌面不调用此入口。
/// 安装必须发生在业务打开选择器之前，不能替换已经输入中的事务。
void installMobileFileDialog();
/// @brief 按当前文本安全区计算文件选择器尺寸，桌面保留原像素尺寸。
/// @warning 每帧只复制几何标量，不跨 JNI 查询键盘或保存布局。
ImVec2 mobileFileDialogSize(float scale);
}  // namespace MMM::UI
