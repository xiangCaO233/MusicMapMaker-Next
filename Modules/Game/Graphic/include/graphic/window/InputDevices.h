#pragma once

namespace MMM::Graphic
{
/// @brief 查询是否存在可用于编辑的鼠标或触摸板，桌面始终保留指针能力。
/// @details 安卓读取 Activity
/// 设备监听器发布的独立布尔快照，不把触摸合成鼠标计入。
/// @warning UI 每帧读取一次；安卓使用 relaxed 原子读取，写入者为 Activity
/// 线程。 该值不发布其他资源，仅跨线程传递设备能力，不能因此引入设备枚举或 JNI
/// 轮询。
bool hasExternalMouse();
/// @brief 安卓 Activity 低频设备回调发布当前连接状态。
/// @details 没有窗口依赖，首次原生窗口创建前也可安全发布。
/// @warning 只更新独立原子布尔值，不能访问 UI、会话或配置对象。
void publishExternalMouse(bool connected);
}  // namespace MMM::Graphic
