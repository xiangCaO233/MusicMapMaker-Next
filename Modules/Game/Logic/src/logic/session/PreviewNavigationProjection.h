#pragma once
#include "config/EditorConfig.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <cmath>

namespace MMM::Logic
{
/// @brief 预览绘制和触控反投影共用的纵向压缩比例。
/// @param height 当前预览内容高度，不包含标题或密度栏装饰。
/// @param ratio 独立预览范围，越大显示的时间跨度越大。
/// @details 主相机尚未注册时沿用绘制器的当前预览高度过渡基准。
/// 主画布上下边界与预览像素边距属于不同坐标口径，不能直接相减。
/// @warning 逻辑热路径只读取当前会话标量与已有相机，无锁或额外遍历。
inline float previewNavigationScale(const SessionContext&       ctx,
                                    const Config::EditorConfig& config,
                                    float height, float ratio)
{
    // 相机表由同一逻辑线程持有，本函数不取得跨线程快照所有权。
    // 绘制和反投影调用同一比例，避免手机边距或布局改变后出现点击偏移。
    const auto* main = SessionUtils::findMainCanvasCamera(ctx.cameras);
    // 兼容过渡值只在主相机缺失时使用，不作为正常触控的固定时间比例。
    const float mainHeight = main ? main->viewportHeight : height;
    const auto& visual     = config.visual;
    const float effective =
        (visual.trackLayout.bottom - visual.trackLayout.top) * mainHeight;
    const float drawable = height - visual.previewConfig.margin.top -
                           visual.previewConfig.margin.bottom;
    // 退化窗口和损坏配置没有可逆投影；调用方必须停止这笔输入。
    // 拒绝无穷或非正尺度，不能用单位比例伪造落点。
    if ( !std::isfinite(effective) || !std::isfinite(drawable) ||
         !std::isfinite(ratio) || effective <= .0001f || drawable <= .0001f ||
         ratio <= .0001f )
        return 0;
    return drawable / (effective * ratio);
}
}  // namespace MMM::Logic
