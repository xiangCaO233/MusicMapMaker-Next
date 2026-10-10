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
    return config.debugCanvasCameraAngleDegrees == 45;
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
}  // namespace

/// @brief 运行不依赖 GPU 的实验摄像机回归。
/// @return 所有数学契约成立时返回零。
/// @note 非有限角度不允许传播到渲染矩阵。
/// @note 不创建窗口、纹理或音频设备。
int main()
{
    // 配置损坏回退二维，越界角度不得穿越近平面。
    using MMM::Canvas::planarObservationAngle;
    if ( planarObservationAngle(-2) != 0 || planarObservationAngle(80) != 45 ||
         planarObservationAngle(std::numeric_limits<float>::quiet_NaN()) != 0 )
        return 1;
    // 分别验证关闭实验模式与观察模式，失败统一返回非零。
    return testFlat() && testTilt() && testConfig() ? 0 : 1;
}
