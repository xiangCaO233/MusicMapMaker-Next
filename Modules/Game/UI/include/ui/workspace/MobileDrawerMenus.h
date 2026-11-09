#pragma once

namespace MMM::UI
{
class UIManager;

/// @brief 在移动右抽屉中组合工具、查看与帮助入口，不包含桌面菜单布局。
/// @param manager 借用本帧管理器，业务动作仍由已注册的菜单处理器执行。
/// @param scale 当前触摸缩放，长文案按真实宽度增加按钮高度。
/// @return 成功打开工具或弹窗时返回真，外壳据此收起遮罩。
/// @warning 每帧只遍历固定入口；文件、插件与更新操作仅在明确激活后执行。
bool drawMobileDrawerMenus(UIManager& manager, float scale);
}  // namespace MMM::UI
