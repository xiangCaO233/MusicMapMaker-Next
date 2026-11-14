#pragma once

namespace MMM::UI
{
class UIManager;
/// @brief 在移动文件页直接显示导出、打包和关闭，返回成功派发状态。
/// @warning 每帧只遍历固定入口，实际保存及关闭由原业务处理。
bool drawMobileFileActions(UIManager& manager, float scale);
/// @brief 根据当前文字与可用列宽计算两行按钮总高，不包含父窗口 padding。
/// @warning 每帧只有固定文本测量，不访问目录或可变会话。
float mobileFileActionsHeight(float availableWidth, float scale);

/// @brief 在移动右抽屉中组合工具、查看与帮助入口，不包含桌面菜单布局。
/// @param manager 借用本帧管理器，业务动作仍由已注册的菜单处理器执行。
/// @param scale 当前触摸缩放，长文案按真实宽度增加按钮高度。
/// @return 成功打开工具或弹窗时返回真，外壳据此收起遮罩。
/// @warning 每帧只遍历固定入口；文件、插件与更新操作仅在明确激活后执行。
bool drawMobileDrawerMenus(UIManager& manager, float scale);
}  // namespace MMM::UI
