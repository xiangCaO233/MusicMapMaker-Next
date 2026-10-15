#include "canvas/PlanarObservationCamera.h"
#include "config/VisualConfig.h"
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace
{
/// @brief 比较投影坐标，容差仅覆盖浮点矩阵运算舍入。
/// @param a 实际齐次投影分量。
/// @param b 期望分量。
/// @return 差值小于容差时通过。
bool near(float a, float b)
{
    return std::abs(a - b) < 0.0001f;
}
/// @brief 验证零角矩阵保留旧二维坐标和播放偏移。
/// @return 左上角映射与旧画布一致时通过。
/// @note 非零补偿覆盖播放中的坐标约定。
bool testFlat()
{
    const auto matrix =
        MMM::Canvas::planarObservationProjection(800, 600, 20, 0);
    // 顶边加补偿后应在屏幕顶端，而不是改变画布数据。
    const auto corner = matrix * glm::vec4(0, -20, 0, 1);
    const auto old    = glm::ortho(0.0f, 800.0f, -20.0f, 580.0f, -1.0f, 1.0f);
    // 比较全部分量，避免仅左上角正确却改变缩放或深度。
    for ( int column = 0; column < 4; ++column ) {
        for ( int row = 0; row < 4; ++row ) {
            if ( matrix[column][row] != old[column][row] ) return false;
        }
    }
    return near(corner.x, -1) && near(corner.y, -1);
}

/// @brief 验证实验角度的配置往返及边界保护。
/// @return 默认关闭、合法值恢复和越界钳制均正确时通过。
/// @note 仅使用内存 JSON，不读写用户设置文件。
/// @note 直接调用生产序列化入口，避免测试复制配置钳制算法。
/// @note 调试摄像机配置不属于谱面格式，不写入谱面文件。
bool testConfig()
{
    MMM::Config::VisualConfig config;
    // 旧配置没有该键时，不能意外打开实验观察。
    nlohmann::json empty = nlohmann::json::object();
    from_json(empty, config);
    if ( config.debugCanvasCameraAngleDegrees != 0 ) return false;
    config.debugCanvasCameraAngleDegrees = 30;
    // 新配置持久化后恢复原角度，不依赖设置页重新初始化。
    nlohmann::json stored;
    to_json(stored, config);
    from_json(stored, config);
    if ( config.debugCanvasCameraAngleDegrees != 30 ) return false;
    // 外部编辑配置超过范围时，读取入口应限制到最大倾角。
    stored["debugCanvasCameraAngleDegrees"] = 100;
    from_json(stored, config);
    if ( config.debugCanvasCameraAngleDegrees != 75 ) return false;
    // 新观察参数缺失时保持默认方向与距离，不改变旧存档的观察比例。
    if ( config.debugCanvasCameraYawDegrees != 0 ||
         config.debugCanvasCameraDistance != 1 )
        return false;
    config.debugCanvasCameraYawDegrees = -60;
    config.debugCanvasCameraDistance   = 2.5F;
    to_json(stored, config);
    from_json(stored, config);
    if ( config.debugCanvasCameraYawDegrees != -60 ||
         config.debugCanvasCameraDistance != 2.5F )
        return false;
    // 手工编辑配置中的异常范围在读取边界校正，不能扩散到渲染矩阵。
    stored["debugCanvasCameraYawDegrees"] = 1000;
    stored["debugCanvasCameraDistance"]   = -1;
    from_json(stored, config);
    if ( config.debugCanvasCameraYawDegrees != 180 ||
         config.debugCanvasCameraDistance != 0.5F )
        return false;
    // 错误类型和非有限值独立回退，手工编辑损坏不能触发类型转换异常。
    stored["debugCanvasCameraAngleDegrees"] = "invalid";
    stored["debugCanvasCameraYawDegrees"]   = nullptr;
    stored["debugCanvasCameraDistance"] =
        std::numeric_limits<double>::infinity();
    // 配置读取边界须在矩阵计算前修复值，不能只依靠渲染端掩盖损坏状态。
    from_json(stored, config);
    return config.debugCanvasCameraAngleDegrees == 0 &&
           config.debugCanvasCameraYawDegrees == 0 &&
           config.debugCanvasCameraDistance == 1;
}
/// @brief 验证最大倾角下可见平面的深度与中心位置。
/// @return 中心、深度和裁剪边界满足契约时通过。
/// @note 验证矩阵约束，不代替 GPU 或皮肤显示验收。
/// @note 包围裁剪不要求等同精确透视梯形。
bool testTilt()
{
    const auto matrix =
        MMM::Canvas::planarObservationProjection(800, 600, 0, 45);
    const auto center = matrix * glm::vec4(400, 300, 0, 1);
    // 观察中心不漂移，四角均处在 Vulkan 合法深度范围内。
    if ( !near(center.x, 0) || !near(center.y, 0) ) return false;
    // 高度一、倾角四十五度时，到平面中心的视距为根号二。
    if ( !near(center.w, std::sqrt(2.0f)) ) return false;
    // 两端齐次分母不同才是真正透视，不是二维缩放冒充倾斜。
    const auto top    = matrix * glm::vec4(400, 0, 0, 1);
    const auto bottom = matrix * glm::vec4(400, 600, 0, 1);
    if ( near(top.w, bottom.w) ) return false;
    for ( float x : { 0.0f, 800.0f } ) {
        for ( float y : { 0.0f, 600.0f } ) {
            const auto point = matrix * glm::vec4(x, y, 0, 1);
            if ( !std::isfinite(point.w) || point.w <= 0 || point.z < 0 ||
                 point.z > point.w )
                return false;
        }
    }
    // 投影裁剪必须在逻辑视口内，不能产生负尺寸或越界。
    const auto rect = MMM::Canvas::planarObservationScissor(
        { 0, 0, 800, 600 }, matrix, 800, 600);
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
           rect.x + rect.width <= 800 && rect.y + rect.height <= 600;
}
/// @brief 验证独立背景层始终铺满视口，轨道仍保留观察角度和播放补偿。
/// @return 固定背景四角及场景矩阵保持各自契约时通过。
/// @note CPU 回归只检查生产投影函数，不代替实际 GPU 显示验收。
bool testBackgroundLayer()
{
    const auto background = MMM::Canvas::canvasBackgroundProjection(800, 600);
    // 非正方形视口覆盖横纵比例，不能把固定层误当成单位正方形。
    // 背景四角的齐次分母必须恒定，不能随摄像机倾斜形成梯形。
    for ( float x : { 0.0F, 800.0F } ) {
        for ( float y : { 0.0F, 600.0F } ) {
            const auto point = background * glm::vec4(x, y, 0, 1);
            // 逐角检查而非只看中心；中心不变仍可能存在背景缩小问题。
            if ( !near(point.w, 1) || !near(point.x, x == 0 ? -1 : 1) ||
                 !near(point.y, y == 0 ? -1 : 1) )
                return false;
        }
    }
    // 轨道录制独立保留亚帧补偿，在关闭观察模式时也不能移动固定背景。
    const auto flat = MMM::Canvas::planarObservationProjection(800, 600, 20, 0);
    // 最大合法倾角与非零补偿组合，覆盖最明显的背景倾斜回归。
    const auto tilted =
        MMM::Canvas::planarObservationProjection(800, 600, 20, 45);
    const auto origin           = glm::vec4(0, 0, 0, 1);
    const auto backgroundOrigin = background * origin;
    // 正交轨道的纵向偏移与透视轨道的齐次分母分别验证两个独立约束。
    const auto flatOrigin   = flat * origin;
    const auto tiltedOrigin = tilted * origin;
    // 背景不依赖轨道矩阵，拒绝再次把整个画布统一应用摄像机。
    return !near(backgroundOrigin.y, flatOrigin.y) &&
           !near(backgroundOrigin.w, tiltedOrigin.w);
}

/// @brief 验证全周水平旋转、拉远距离与更大俯仰角仍围绕同一轨道中心。
/// @return 中心位置、距离和深度均保持有效时通过。
/// @note 不要求旋转后的矩形始终完整出现在视锥内，放大允许裁切边缘。
/// @note 遍历多个宽高比，防止使用方形视口掩盖横纵比例错误。
bool testOrbitAndDistance()
{
    // 组合矩阵必须对所有合法参数保持有限，而不仅是默认倾角的一张截图。
    // 窄视口和宽视口分别覆盖旋转后横向范围的不同约束。
    using namespace MMM::Canvas;
    for ( const float width : { 480.0F, 800.0F, 1600.0F } ) {
        for ( const float pitch : { 0.0F, 30.0F, 75.0F } ) {
            for ( const float yaw : { -180.0F, -90.0F, 0.0F, 90.0F, 180.0F } ) {
                for ( const float distance : { 0.5F, 1.0F, 4.0F } ) {
                    // 非零播放补偿在所有组合中一致，控制新增后不能使中心漂移。
                    const auto matrix = planarObservationProjection(
                        width, 600, 20, pitch, yaw, distance);
                    // 模型先补偿播放偏移，旋转与拉远都不能改变观察锚点。
                    // y=280 是高度六百像素时向上补偿二十像素的真实中心。
                    const auto center =
                        matrix * glm::vec4(width * 0.5F, 280, 0, 1);
                    if ( !near(center.x, 0) || !near(center.y, 0) ||
                         !std::isfinite(center.w) || center.w <= 0 )
                        return false;
                    // 原二维路径使用正交深度约定，其余组合使用 Vulkan
                    // 零至一深度。
                    // 只检查中心深度，近距离旋转允许部分边角超出视锥。
                    if ( planarObservationEnabled(pitch, yaw, distance) &&
                         (center.z < 0 || center.z > center.w) )
                        return false;
                    // 屏幕内外裁剪都必须产生有界矩形，不能因旋转出现无符号下溢。
                    // 向外取整允许少量边缘溢出，最终仍须限制到视口。
                    const auto rect = planarObservationScissor(
                        { 0, 0, static_cast<std::uint32_t>(width), 600 },
                        matrix,
                        width,
                        600);
                    if ( rect.x < 0 || rect.y < 0 ||
                         rect.x + rect.width > width ||
                         rect.y + rect.height > 600 )
                        return false;
                }
            }
        }
    }
    // 水平九十度后右侧点转向纵向，且更远距离使相同偏移投影更小。
    // 右侧一百像素的固定点在零俯仰下排除了额外纵向透视缩短。
    // 同一方向拉远只影响尺寸，不能让屏幕方向反转。
    const auto rotated = planarObservationProjection(800, 600, 0, 0, 90);
    const auto right   = rotated * glm::vec4(500, 300, 0, 1);
    const auto far     = planarObservationProjection(800, 600, 0, 0, 90, 4) *
                     glm::vec4(500, 300, 0, 1);
    return near(right.x, 0) && !near(right.y, 0) &&
           std::abs(far.y / far.w) < std::abs(right.y / right.w);
}

/// @brief 验证拖动、滚轮的即时变化及边界保护，不依赖平台输入设备。
/// @return 标准视图判定、环绕角连续性与距离边界全部正确时通过。
/// @note 无输入不改变视角，非有限平台增量不能传播为 NaN。
bool testControls()
{
    // 测试数值增量不依赖帧率、平台鼠标后端或 GPU。
    // 共用生产函数，避免另写一份不同的角度更新算法。
    using namespace MMM::Canvas;
    // 独立改变水平角或距离同样启用只读观察，不可误回到二维编辑分支。
    if ( planarObservationEnabled(0) || !planarObservationEnabled(0, 20) ||
         !planarObservationEnabled(0, 0, 2) )
        return false;
    // 横向八像素对应两度，跨越一百八十度后应连续绕回。
    // 纵向四十像素对应十度，不能误当成整段手势累计距离。
    const auto drag = planarObservationDrag(20, 179, 8, 40);
    if ( !near(drag.x, 30) || !near(drag.y, -179) ) return false;
    // 越过俯仰范围时钳位，不越过平面或翻转摄像机。
    const auto limit = planarObservationDrag(70, 0, 0, 100);
    if ( limit.x != 75 ) return false;
    // 正八格使距离减半，负八格使距离加倍；已到上界时保持稳定。
    // 断言精确倍率，避免错误的线性缩放仍满足单调性而假通过。
    if ( !near(planarObservationZoom(1, 8), 0.5F) ||
         !near(planarObservationZoom(1, -8), 2) ||
         !near(planarObservationZoom(4, -16), 4) )
        return false;
    // 平台无效输入退回当前合法值，配置损坏则恢复默认二维视角。
    const float nan     = std::numeric_limits<float>::quiet_NaN();
    const auto  invalid = planarObservationDrag(20, 30, nan, 2);
    // 损坏配置全部回退后不能误启用只读模式。
    // 无效鼠标增量保留合法视角，窗口失焦时不应跳变。
    return near(invalid.x, 20) && near(invalid.y, 30) &&
           !planarObservationEnabled(nan, nan, nan) &&
           near(planarObservationZoom(2, nan), 2);
}
}  // namespace

/// @brief 运行不依赖 GPU 的实验摄像机回归。
/// @return 所有数学契约成立时返回零。
/// @note 非有限角度不允许传播到渲染矩阵。
/// @note 不创建窗口、纹理或音频设备。
int main()
{
    // 配置损坏回退二维，越界角度不得穿越近平面。
    using MMM::Canvas::planarObservationAngle;
    if ( planarObservationAngle(-2) != 0 || planarObservationAngle(80) != 75 ||
         planarObservationAngle(std::numeric_limits<float>::quiet_NaN()) != 0 )
        return 1;
    // 分别验证关闭实验模式与观察模式，失败统一返回非零。
    return testFlat() && testTilt() && testConfig() && testBackgroundLayer() &&
                   testOrbitAndDistance() && testControls()
               ? 0
               : 1;
}
