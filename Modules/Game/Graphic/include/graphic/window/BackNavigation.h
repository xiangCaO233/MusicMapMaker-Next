#pragma once
namespace MMM::Graphic
{
/// @brief 从 Activity 线程投递返回导航，实际确认由 SDL/UI 线程处理。
/// @details 不访问 UI 对象，不直接退出或修改工作区；仅 Android 后端实现。
/// @warning 用户返回手势的低频入口，禁止将其当作逐帧状态查询。
void requestMobileBackNavigation();
}  // namespace MMM::Graphic
