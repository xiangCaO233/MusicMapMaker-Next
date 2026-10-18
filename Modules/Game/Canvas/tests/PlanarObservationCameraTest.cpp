#include "canvas/PlanarObservationCamera.h"
#include "canvas/PlanarObservationInput.h"
#include "common/render/CanvasCameraMath.h"
#include "config/VisualConfig.h"
#include "log/colorful-log.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

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

/// @brief 按坐标尺度比较输入映射往返结果。
/// @param a 实际坐标。
/// @param b 预期坐标。
/// @return 绝对或相对误差处于单精度投影范围时通过。
bool nearInputCoordinate(float a, float b)
{
    // 屏幕点先量化为
    // float，高倾角与远距离逆透视会放大该误差；按坐标尺度设界且保持亚像素。
    return std::abs(a - b) <= std::max(0.0001F, std::abs(b) * 0.000005F);
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

/// @brief 验证投影与逆投影对倾角、方位、距离和不同视口保持往返一致。
/// @return 所有合法组合均恢复原始谱面坐标时通过。
/// @note 播放补偿只经实际场景矩阵进入投影，往返结果不额外偏移。
/// @note 三种视口同时覆盖宽高比变化与像素密度放大，输入仍是各自逻辑像素。
/// @note 各配置组合都从生产相机函数取得矩阵，避免测试和实现共享错误假设。
/// @note 容差按输入坐标量级放宽，涵盖屏幕坐标返回 float 时的一次舍入。
/// @note 谱面采样围绕播放偏移后的视口中心，避免把摄像机背后点当作往返样本。
/// @note 深度裁剪回归另用已知近平面、远平面之外的点验证空值结果。
/// @note 不同画布尺寸复用同一逻辑像素约定，不将 DPI 因子重复乘入输入。
bool testInputProjectionRoundTrip()
{
    using namespace MMM::Canvas;
    // 视口覆盖常见比例和高 DPI 物理尺寸；坐标仍按每个视口的逻辑像素解释。
    const std::array<glm::vec2, 3> viewports{
        glm::vec2(320.0F, 240.0F),
        glm::vec2(800.0F, 600.0F),
        glm::vec2(2560.0F, 1440.0F),
    };
    for ( const auto viewport : viewports ) {
        for ( const float pitch : { 0.0F, 30.0F, 75.0F } ) {
            for ( const float yaw : { -180.0F, -90.0F, 90.0F, 180.0F } ) {
                for ( const float distance : { 0.5F, 1.0F, 4.0F } ) {
                    // 真实矩阵带非零播放偏移，逆投影必须直接给出原始 sceneXY。
                    const auto matrix = planarObservationProjection(
                        viewport.x, viewport.y, 37.0F, pitch, yaw, distance);
                    const auto projection =
                        makePlanarObservationInputProjection(
                            matrix, viewport.x, viewport.y, true);
                    if ( !projection.m_enabled || !projection.m_valid )
                        return false;
                    // 零倾角仍能单独检验 yaw 与
                    // distance，因为任一参数都可启用观察模式。 ±90
                    // 度测试横向轴交换，±180 度覆盖周期边界的浮点近似。
                    // 距离两端覆盖接近相机和显著拉远时的齐次尺度差异。
                    // 点覆盖中心附近及时间轴两侧，避免仅中心点掩盖映射误差。
                    for ( const glm::vec2 scene :
                          { glm::vec2(viewport.x * 0.5F,
                                      viewport.y * 0.5F + 37.0F),
                            glm::vec2(viewport.x * 0.53F,
                                      viewport.y * 0.52F + 37.0F),
                            glm::vec2(viewport.x * 0.46F,
                                      viewport.y * 0.47F + 37.0F) } ) {
                        const auto screen =
                            planarObservationProject(projection, scene);
                        if ( !screen ) return false;
                        const auto recovered =
                            planarObservationUnproject(projection, *screen);
                        if ( !recovered ||
                             !nearInputCoordinate(recovered->x, scene.x) ||
                             !nearInputCoordinate(recovered->y, scene.y) )
                            return false;
                    }
                    // 三个点分散在视口范围内，确保播放补偿不会只在摄像机中心相消。
                    // 点位同时包含零坐标和正坐标，覆盖矩阵平移项与缩放项。
                    // 每个合法组合都断言缓存有效，防止求逆退化被往返断言偶然掩盖。
                }
            }
        }
    }
    return true;
}

/// @brief 验证关闭模式恒等映射及画布外输入保留原始平面几何。
/// @return 精确二维恒等、画布外往返和连续拖动共线时通过。
/// @note 画布外的屏幕点合法，调用端可在释放前持续更新拖动位置。
/// @note 测试没有构造 GLFW 窗口，所有坐标与生产逻辑视口约定一致。
/// @note 平面直线映射为直线是 homography 性质，也是连续拖动的几何约束。
/// @note 拖动采样沿屏幕段取值，验证逆投影不因事件间隔而积累坐标漂移。
/// @note 恒等映射不使用 viewport，保留原二维调用端的默认构造行为。
bool testInputIdentityAndDragGeometry()
{
    using namespace MMM::Canvas;
    // 默认构造代表未启用状态，精确恒等不依赖宽高或矩阵是否初始化。
    const PlanarObservationInputProjection defaultProjection;
    const glm::vec2                        arbitrary(-123.25F, 4567.75F);
    const auto                             identityScreen =
        planarObservationProject(defaultProjection, arbitrary);
    const auto identityScene =
        planarObservationUnproject(defaultProjection, arbitrary);
    if ( !identityScreen || !identityScene ||
         identityScreen->x != arbitrary.x || identityScreen->y != arbitrary.y ||
         identityScene->x != arbitrary.x || identityScene->y != arbitrary.y )
        return false;

    // 关闭态专门使用无视口默认值，防止二维输入被迫依赖新摄像机状态。
    // 输入含负值和大数值，能发现多余的边界裁剪或单位换算。
    // 高倾角和旋转下选取画布外两端，证明未对拖动坐标进行窗口钳位。
    const auto matrix = planarObservationProjection(800, 600, 18, 75, -90, 1);
    const auto projection =
        makePlanarObservationInputProjection(matrix, 800, 600, true);
    const glm::vec2 outsideScene(900.0F, 1700.0F);
    const auto      outsideScreen =
        planarObservationProject(projection, outsideScene);
    if ( !outsideScreen || (outsideScreen->x >= 0 && outsideScreen->x <= 800 &&
                            outsideScreen->y >= 0 && outsideScreen->y <= 600) )
        return false;
    // 使用同一真实矩阵完成正反映射，不通过测试专用近似公式生成期望值。
    const auto outsideRecovered =
        planarObservationUnproject(projection, *outsideScreen);
    if ( !outsideRecovered ||
         !nearInputCoordinate(outsideRecovered->x, outsideScene.x) ||
         !nearInputCoordinate(outsideRecovered->y, outsideScene.y) )
        return false;
    // 画布外仍要求投影的分母位于摄像机前方，因此合法拖动点应成功恢复。

    // 屏幕直线逐点逆投影仍落在源平面同一条直线上，适配连续鼠标拖动。
    const glm::vec2 start(120.0F, 180.0F);
    const glm::vec2 finish(760.0F, 510.0F);
    const float     segmentLength =
        std::hypot(finish.x - start.x, finish.y - start.y);
    const auto startScreen  = planarObservationProject(projection, start);
    const auto finishScreen = planarObservationProject(projection, finish);
    if ( !startScreen || !finishScreen ) return false;
    // 屏幕空间的等距采样在谱面参数上不必等距，但其几何仍应保持共线。
    // 该约束让交互层可以按当前鼠标位置逐次更新，不需累积上次事件误差。
    for ( const float t : { 0.0F, 0.2F, 0.5F, 0.8F, 1.0F } ) {
        const glm::vec2 screen =
            *startScreen + (*finishScreen - *startScreen) * t;
        const auto scene = planarObservationUnproject(projection, screen);
        if ( !scene ) return false;
        const float cross = (scene->x - start.x) * (finish.y - start.y) -
                            (scene->y - start.y) * (finish.x - start.x);
        // 叉积除以线段长度得到垂直像素偏差，容忍千分之一像素舍入。
        if ( std::abs(cross) > segmentLength * 0.001F ) return false;
    }
    // 端点也纳入采样，检查插值参数边界没有单独的符号或舍入分支。
    // 叉积按像素平方计量，阈值只覆盖最终 glm::vec2 单精度舍入。
    return true;
}

/// @brief 验证退化矩阵、无效视口、近平行交点与摄像机背后点被拒绝。
/// @return 每类无效输入均返回空 optional 时通过。
/// @note 矩阵整体按最大元素归一化，测试不依赖绝对 viewport 尺度。
/// @note 这些失败应在映射边界明确表达为空值，不以无穷大或钳位坐标代替。
/// @note 测试分别触达构造期拒绝与鼠标事件期拒绝，覆盖缓存生命周期两端。
/// @note 无效值统一用 optional 空状态表达，测试不会依赖特定 NaN 传播结果。
/// @note 深度边界使用实际 Vulkan ZO 投影，和透视矩阵的裁剪定义保持一致。
/// @note 测试点刻意保持正向齐次分母，确保失败原因是近远裁剪范围。
bool testInputProjectionRejection()
{
    using namespace MMM::Canvas;
    const glm::mat4 identity(1.0F);
    if ( makePlanarObservationInputProjection(identity, 0, 600, true).m_valid ||
         makePlanarObservationInputProjection(
             identity, std::numeric_limits<float>::quiet_NaN(), 600, true)
             .m_valid )
        return false;
    // 非正和非有限尺寸会令 NDC 到像素换算失去定义，必须早于求逆拒绝。
    // 视口无效时即使矩阵本身可逆，也不能构造有意义的屏幕映射。
    // 零矩阵的平面投影没有唯一逆解，必须在构造阶段标为无效。
    // 宽高异常则单独验证视口检查，不依赖退化矩阵掩盖该分支。
    if ( makePlanarObservationInputProjection(glm::mat4(0.0F), 800, 600, true)
             .m_valid )
        return false;
    // 相关平面元素出现 NaN 时，不能依靠行列式偶然产生一个可用逆矩阵。
    glm::mat4 nonFinite(1.0F);
    nonFinite[1][0] = std::numeric_limits<float>::quiet_NaN();
    if ( makePlanarObservationInputProjection(nonFinite, 800, 600, true)
             .m_valid )
        return false;

    // 人工平面令摄像机齐次分母为 0.1*x+1，可精确构造近平行与背向情况。
    glm::mat4 horizon(1.0F);
    horizon[0][3] = 0.1F;
    const auto projection =
        makePlanarObservationInputProjection(horizon, 100, 100, true);
    if ( !projection.m_valid ) return false;
    // 有效性断言隔离测试夹具错误，后续空值必须来自交点本身而非构造失败。
    // 使用 viewport 外坐标刻意越过无穷远线，验证函数不会钳位后伪造交点。
    // NDC x=10 对应视口外 x=550，射线平行于平面，不能产生极大伪坐标。
    if ( planarObservationUnproject(projection, { 550.0F, 50.0F }) )
        return false;
    // x=-10 位于相机平面，x<-10 的谱面点则位于摄像机背后。
    if ( planarObservationProject(projection, { -10.0F, 0.0F }) ||
         planarObservationProject(projection, { -20.0F, 0.0F }) )
        return false;
    // 分母在零点两侧变号，确保实现检查可见方向而非仅检查绝对值。

    // 使用 Vulkan ZO 透视矩阵验证真实近平面和远平面深度裁剪。
    // 近点小于 near=0.01，远点大于 far=100；两者的齐次 w 都为正。
    // 这能区分深度区间判断与仅拒绝摄像机背后交点的旧行为。
    const auto perspective = glm::perspectiveRH_ZO(1.0F, 1.0F, 0.01F, 100.0F);
    const auto beforeNear =
        perspective *
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -0.005F));
    const auto beyondFar =
        perspective *
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -110.0F));
    const auto nearProjection =
        makePlanarObservationInputProjection(beforeNear, 100, 100, true);
    const auto farProjection =
        makePlanarObservationInputProjection(beyondFar, 100, 100, true);
    // 两种无效深度都分别从平面点和屏幕点入口检查，避免只覆盖单向 API。
    if ( !nearProjection.m_valid || !farProjection.m_valid ||
         planarObservationProject(nearProjection, { 0.0F, 0.0F }) ||
         planarObservationUnproject(nearProjection, { 50.0F, 50.0F }) ||
         planarObservationProject(farProjection, { 0.0F, 0.0F }) ||
         planarObservationUnproject(farProjection, { 50.0F, 50.0F }) )
        return false;

    // 非有限屏幕输入与构造失败缓存都走同一空值契约，调用方可直接保留旧状态。
    // 无效模式拒绝非有限鼠标值，失效缓存不得误用未初始化逆矩阵。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if ( planarObservationUnproject(projection, { nan, 0.0F }) ||
         planarObservationProject(
             makePlanarObservationInputProjection(identity, 0, 1, true),
             { 0.0F, 0.0F }) )
        return false;
    return true;
}

/// @brief 对实际透视视锥验证两侧、远端及近平面跨越的源坐标筛选。
/// @return 所有投影后可见的采样点均落入保守包围框时为真。
/// @note 网格点只在测试中遍历；生产路径始终使用固定六平面裁剪。
/// @note 测试保留负 X 和超过二维宽度的坐标，不允许用旧窗口范围作答案。
/// @note 相机高度和水平偏移独立变化，覆盖用户的抬高与左右移动场景。
bool testVisiblePlaneBounds()
{
    using namespace MMM::Common::Render;
    MMM::Config::VisualConfig visual;
    visual.canvasCameraPoseEnabled = true;
    visual.canvasCameraRotation    = { 45, 0, 0 };
    // 轨道长度三倍，横向覆盖左右辅助区；近平面交点也由有限场景限制。
    const CanvasPlaneBounds scene{ -600, -1200, 1600, 600 };
    bool                    sawLeft = false, sawRight = false, sawFar = false;
    // 低机位覆盖视锥穿过轨道的情况，高机位覆盖全长和两侧同时可见。
    // 横向位置使用世界单位，不能误当作窗口像素平移。
    for ( float height : { 0.3F, 0.9F, 2.5F } ) {
        for ( float x : { -0.6F, 0.0F, 0.6F } ) {
            visual.canvasCameraPosition = { x, -0.8F, height };
            // 每组参数都重新构造正式相机矩阵，不复用上一机位的包围框。
            // 以非正方形视口验证宽高比也参与左右可见范围的计算。
            const auto matrix = canvasCameraPoseProjection(800, 600, 0, visual);
            const auto bounds = canvasPlaneVisibleBounds(matrix, scene);
            // 独立点投影只作可见性真值，不复制多边形裁剪算法。
            // 只要任一可见点丢失，即表明候选窗口存在错误剔除。
            for ( float py = -1200; py <= 600; py += 50 ) {
                for ( float px = -600; px <= 1600; px += 50 ) {
                    const auto clip = matrix * glm::vec4(px, py, 0, 1);
                    if ( clip.w <= 0 || std::abs(clip.x) > clip.w ||
                         std::abs(clip.y) > clip.w || clip.z < 0 ||
                         clip.z > clip.w )
                        continue;
                    if ( !bounds || px < bounds->m_left - 0.01F ||
                         px > bounds->m_right + 0.01F ||
                         py < bounds->m_top - 0.01F ||
                         py > bounds->m_bottom + 0.01F )
                        return false;
                    // 三个标志证明用例确实命中过旧窗口之外，不能空遍历通过。
                    // 被测包围框既须覆盖这些点，也须留在有限的轨道场景内。
                    sawLeft |= px < 0;
                    sawRight |= px > 800;
                    sawFar |= py < 0;
                }
            }
        }
    }
    // 完全在相机背面的平面必须为空，不能因负 w 将其镜像误认成可见。
    // 背面负例使用零旋转，确保整个有限矩形都在相机背后而非仅部分越界。
    // 它与上面的近裁剪交叉案例一起覆盖无交集及部分可见两类返回值。
    // 不采用二维窗口夹取作为期望值，以免测试和旧错误逻辑一致。
    visual.canvasCameraPosition = { 0, 0, -1 };
    visual.canvasCameraRotation = { 0, 0, 0 };
    const auto behind           = canvasPlaneVisibleBounds(
        canvasCameraPoseProjection(800, 600, 0, visual), scene);
    return sawLeft && sawRight && sawFar && !behind;
}
}  // namespace

/// @brief 运行不依赖 GPU 的实验摄像机回归。
/// @return 所有数学契约成立时返回零。
/// @note 非有限角度不允许传播到渲染矩阵。
/// @note 不创建窗口、纹理或音频设备。
/// @note 九个 helper 全部执行，单一失败不会短路并隐藏其余失败标签。
/// @note 每条 XERROR 明确标出行为检查，可直接从 CTest 日志定位回归范围。
/// @note 用例仅读写局部值，不启动图形设备或修改用户配置文件。
/// @note 检查表保持源码顺序，方便将日志标签与对应 helper 并排核对。
int main()
{
    // 配置损坏回退二维，越界角度不得穿越近平面。
    using MMM::Canvas::planarObservationAngle;
    if ( planarObservationAngle(-2) != 0 || planarObservationAngle(80) != 75 ||
         planarObservationAngle(std::numeric_limits<float>::quiet_NaN()) !=
             0 ) {
        XERROR("PlanarObservationCameraTest: angle-boundary check failed");
        return 1;
    }
    // 初始化器按顺序执行全部
    // helper，失败汇总记录具体职责，避免短路隐藏后续诊断。
    // 标签保留 helper 名称并概括约束，日志无需依赖调试器或本地断点。
    const std::array<std::pair<const char*, bool>, 10> checks{ {
        { "testVisiblePlaneBounds: projected scene culling",
          testVisiblePlaneBounds() },
        { "testFlat: zero-angle compatibility", testFlat() },
        { "testTilt: projection and clipping", testTilt() },
        { "testConfig: camera setting round trip", testConfig() },
        { "testBackgroundLayer: independent background",
          testBackgroundLayer() },
        { "testOrbitAndDistance: yaw and zoom bounds", testOrbitAndDistance() },
        { "testControls: drag and zoom input", testControls() },
        { "testInputProjectionRoundTrip: project and unproject",
          testInputProjectionRoundTrip() },
        { "testInputIdentityAndDragGeometry: identity and drag",
          testInputIdentityAndDragGeometry() },
        { "testInputProjectionRejection: invalid and clipped points",
          testInputProjectionRejection() },
    } };
    bool                                               allPassed = true;
    for ( const auto& [label, passed] : checks ) {
        if ( passed ) continue;
        XERROR("PlanarObservationCameraTest: {} failed", label);
        allPassed = false;
    }
    return allPassed ? 0 : 1;
}
