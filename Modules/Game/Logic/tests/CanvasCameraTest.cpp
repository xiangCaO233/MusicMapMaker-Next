#include "logic/session/CanvasCamera.h"
#include "log/colorful-log.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/context/SessionContext.h"

#include <cmath>

namespace
{

/// @brief 使用小容差比较逻辑像素或时间值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-6;
}

/// @brief 验证横向相机偏移被渲染和拾取共用的轨道投影正确应用。
/// @return 投影边界、轨道宽度和拾取结果正确时返回 true。
bool testTrackProjectionUsesCameraOffset()
{
    const auto projection = MMM::Logic::calculatePlayerTrackProjection(
        1000.0F, 4, 0.2F, 0.6F, 50.0F);
    if ( !projection.valid || !near(projection.leftX, 250.0) ||
         !near(projection.rightX, 650.0) ||
         !near(projection.singleTrackWidth, 100.0) ||
         !projection.contains(250.0F) || projection.contains(200.0F) ||
         projection.trackAt(349.0F, 4) != 0 ||
         projection.trackAt(350.0F, 4) != 1 ||
         projection.trackAt(649.0F, 4) != 3 ) {
        XERROR("Canvas track projection ignored horizontal camera offset");
        return false;
    }
    return true;
}

/// @brief 验证逻辑视口 Resize 后横向位移保持相同比例。
/// @return 偏移随逻辑宽度等比例换算时返回 true。
bool testResizePreservesNormalizedOffset()
{
    const float resized =
        MMM::Logic::resizeCanvasHorizontalOffset(120.0F, 1200.0F, 600.0F);
    const float unchanged =
        MMM::Logic::resizeCanvasHorizontalOffset(120.0F, 0.0F, 600.0F);
    if ( !near(resized, 60.0) || !near(unchanged, 120.0) ) {
        XERROR("Canvas horizontal offset was not stable across resize");
        return false;
    }
    return true;
}

/// @brief 验证二维平移按逻辑像素连续修改横向相机和纵向时间。
/// @return 横向偏移与无吸附纵向换算均正确时返回 true。
bool testPanCommandUsesLogicalPixels()
{
    MMM::Logic::SessionContext context;
    context.currentTime        = 10.0;
    context.animateTime        = 10.0;
    context.mainAudioTotalTime = 100.0;
    context.cameras.emplace(
        "Canvas_7",
        MMM::Logic::CameraInfo{ "Canvas_7", 1000.0F, 600.0F, 50.0F });
    context.timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();

    MMM::Logic::PlaybackController controller(context);
    controller.handleCommand(MMM::Logic::CmdPanCanvas{
        .cameraId       = "Canvas_7",
        .deltaX         = 25.0F,
        .deltaY         = 100.0F,
        .viewportWidth  = 1000.0F,
        .viewportHeight = 600.0F,
        .renderScaleY   = 2.0F,
    });

    const auto cameraIt = context.cameras.find("Canvas_7");
    if ( cameraIt == context.cameras.end() ||
         !near(cameraIt->second.horizontalOffsetX, 75.0) ||
         !near(context.currentTime, 10.1) ||
         !near(context.animateTime,
               context.currentTime +
                   context.lastConfig.visual.getEffectiveVisualOffset()) ||
         context.animateTimeAnimationActive ) {
        XERROR("Canvas pan command did not apply continuous two-axis movement");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdPanCanvas{
        .cameraId       = "Canvas_7",
        .viewportWidth  = 500.0F,
        .viewportHeight = 600.0F,
    });
    if ( !near(cameraIt->second.horizontalOffsetX, 37.5) ) {
        XERROR("Canvas pan command did not preserve offset after resize");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行主画布二维相机换算测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testTrackProjectionUsesCameraOffset() &&
                   testResizePreservesNormalizedOffset() &&
                   testPanCommandUsesLogicalPixels()
               ? 0
               : 1;
}
