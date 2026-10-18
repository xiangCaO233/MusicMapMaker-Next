#pragma once

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace MMM::Canvas
{
/// @brief 缓存 z=0 谱面平面与逻辑像素视口之间的投影关系。
/// @details
/// 非观察模式默认保持精确二维恒等映射；开启时存储归一化平面矩阵及其逆。
/// @details 矩阵以列向量约定保存，屏幕坐标采用左上原点和正向下的 y 轴。
/// @details 缓存绑定到调用方提供的单帧矩阵，不能跨摄像机更新复用。
/// @details 深度行独立缓存，确保反投影只接受当前 Vulkan 近远平面内的交点。
/// @details 关闭状态不要求初始化 viewport，兼容原二维输入路径的值语义。
/// @details 有效标志在缓存构造完成后发布，调用期间按只读快照使用。
struct PlanarObservationInputProjection {
    /// @brief 表示观察模式状态，关闭时映射忽略视口及矩阵缓存。
    bool m_enabled = false;
    /// @brief 表示开启模式下视口与平面矩阵均可安全求逆。
    bool m_valid = true;
    /// @brief 将谱面平面齐次坐标映射到裁剪空间 x、y、w 的矩阵。
    /// @details 矩阵整体归一化不改变齐次投影，便于检查相对退化程度。
    glm::dmat3 m_sceneToClipPlane{ 1.0 };
    /// @brief 将裁剪空间齐次坐标映射回谱面平面的缓存逆矩阵。
    /// @details 只在构造函数中求逆，输入热路径只读取该已发布值。
    glm::dmat3 m_clipToScenePlane{ 1.0 };
    /// @brief 将谱面平面齐次坐标映射到 Vulkan 裁剪深度的行向量。
    /// @details 与二维投影矩阵采用同一归一化系数，深度比较保持齐次一致。
    glm::dvec3 m_sceneToClipDepthPlane{ 0.0, 0.0, 0.0 };
    /// @brief 当前逻辑视口宽度，用于裁剪坐标与屏幕像素的转换。
    double m_width = 0.0;
    /// @brief 当前逻辑视口高度，用于裁剪坐标与屏幕像素的转换。
    double m_height = 0.0;
};

namespace Internal
{
/// @brief 判断双精度结果能否安全转换为有限单精度值。
/// @param value 待转换的坐标。
/// @return 坐标有限且未超过 float 表示范围时返回 true。
inline bool planarObservationInputRepresentable(double value)
{
    return std::isfinite(value) &&
           std::abs(value) <=
               static_cast<double>(std::numeric_limits<float>::max());
}

/// @brief 检查齐次分母是否远离由浮点舍入造成的不稳定零点。
/// @param denominator 齐次变换的分母。
/// @param magnitude 分母各项绝对值之和，作为相对尺度。
/// @return 分母在当前运算尺度下可稳定区分于零时返回 true。
inline bool planarObservationInputStableDenominator(double denominator,
                                                    double magnitude)
{
    // 相对误差界不依赖矩阵整体缩放或屏幕 DPI，避免固定像素阈值误拒合法矩阵。
    constexpr double multiplier = 16.0;
    // 输入矩阵来自 float，误差界以其精度为准，即使后续求逆使用 double。
    // 相对各项绝对值之和比较，可识别消减误差而不绑定齐次矩阵尺度。
    const double threshold =
        multiplier * std::numeric_limits<float>::epsilon() * magnitude;
    return std::isfinite(denominator) && std::isfinite(magnitude) &&
           magnitude > 0.0 && std::abs(denominator) > threshold;
}

/// @brief 检查平面点是否位于 Vulkan 零至一裁剪深度范围。
/// @param clipZ 投影后的裁剪深度。
/// @param clipW 投影后的齐次分母。
/// @param magnitudeZ 深度点积各项的绝对值之和。
/// @param magnitudeW 齐次分母点积各项的绝对值之和。
/// @return 深度在可见裁剪区间内或仅有可解释舍入误差时返回 true。
/// @note Vulkan 的 ZO 裁剪要求深度处于零到齐次分母之间。
inline bool planarObservationInputDepthVisible(double clipZ, double clipW,
                                               double magnitudeZ,
                                               double magnitudeW)
{
    // 容差随矩阵点积规模变化，覆盖 float 矩阵舍入但不放宽实际近平面裁剪。
    constexpr double multiplier = 16.0;
    const double     tolerance  = multiplier *
                             std::numeric_limits<float>::epsilon() *
                             (magnitudeZ + magnitudeW);
    return std::isfinite(clipZ) && std::isfinite(clipW) &&
           std::isfinite(tolerance) && clipZ >= -tolerance &&
           clipZ <= clipW + tolerance;
}
}  // namespace Internal

/// @brief 从已发布的场景矩阵构造逻辑屏幕与 z=0 谱面平面的映射缓存。
/// @param sceneToClip 当前帧实际绘制使用的场景到裁剪空间矩阵。
/// @param width 逻辑视口宽度，必须为有限正数。
/// @param height 逻辑视口高度，必须为有限正数。
/// @param enabled 是否启用观察模式；关闭时不读取或校验其余输入。
/// @return 可供每次鼠标事件复用的投影及逆投影缓存。
/// @note 深度范围来自场景矩阵，不以屏幕矩形代替 Vulkan 的裁剪可见性。
/// @warning 每帧最多构造一次；求逆只在此处执行，鼠标映射仅作固定规模矩阵运算。
/// @note 返回值对应场景矩阵输入平面坐标；播放 yOffset 仅通过实际矩阵参与映射。
inline PlanarObservationInputProjection makePlanarObservationInputProjection(
    const glm::mat4& sceneToClip, float width, float height, bool enabled)
{
    PlanarObservationInputProjection result;
    result.m_enabled = enabled;
    if ( !enabled ) return result;

    result.m_valid = false;
    if ( !std::isfinite(width) || !std::isfinite(height) || width <= 0.0F ||
         height <= 0.0F )
        return result;

    // 齐次平面只依赖输入 z=0 时矩阵的 x、y、w 三行，保留真实投影中的播放补偿。
    // GLM 按列访问矩阵；第三列对应齐次常数项，来源是 mat4 的平移列。
    // 忽略 z 输出是安全的，因为屏幕位置仅由裁剪 x、y 与齐次 w 决定。
    glm::dmat3 homography(0.0);
    for ( int column = 0; column < 3; ++column ) {
        const int sourceColumn = column == 2 ? 3 : column;
        // 前两列分别作用于 scene x/y，mat4 第四列则作用于平面齐次常数一。
        homography[column][0] = sceneToClip[sourceColumn][0];
        homography[column][1] = sceneToClip[sourceColumn][1];
        homography[column][2] = sceneToClip[sourceColumn][3];
    }
    // 深度不参与屏幕 homography，但 Vulkan 近远裁剪仍决定该交点是否可见。
    glm::dvec3 depthRow(0.0);
    for ( int column = 0; column < 3; ++column ) {
        const int sourceColumn = column == 2 ? 3 : column;
        depthRow[column]       = sceneToClip[sourceColumn][2];
        if ( !std::isfinite(depthRow[column]) ) return result;
    }

    // 先按最大元素归一化，再看行列式，避免大 DPI
    // 或矩阵整体尺度触发绝对阈值误判。
    double scale = 0.0;
    for ( int column = 0; column < 3; ++column ) {
        for ( int row = 0; row < 3; ++row ) {
            const double value = homography[column][row];
            if ( !std::isfinite(value) ) return result;
            scale = std::max(scale, std::abs(value));
        }
    }
    if ( scale == 0.0 ) return result;
    homography /= scale;
    depthRow /= scale;

    // 归一化令行列式阈值表达矩阵形状退化，而不是取决于像素单位大小。
    // 例如相同视场只改变 DPI 时，各元素会一起缩放，不应使可逆性改变。
    const double determinant = glm::determinant(homography);
    // 行列式阈值相对于归一化矩阵，排除数值退化平面但允许不同视口尺寸。
    constexpr double determinantTolerance = 1.0e-14;
    if ( !std::isfinite(determinant) ||
         std::abs(determinant) <= determinantTolerance )
        return result;

    result.m_sceneToClipPlane      = homography;
    result.m_clipToScenePlane      = glm::inverse(homography);
    result.m_sceneToClipDepthPlane = depthRow;
    // 缓存正逆矩阵使用相同归一化，齐次坐标可抵消其公共尺度。
    result.m_width  = width;
    result.m_height = height;
    // GLM 对可逆矩阵返回有限结果；显式扫描可阻断极端输入污染后续事件。
    for ( int column = 0; column < 3; ++column ) {
        for ( int row = 0; row < 3; ++row ) {
            if ( !std::isfinite(result.m_clipToScenePlane[column][row]) )
                return result;
        }
    }
    // 仅在完整逆矩阵已确认有限后发布有效标志，调用端无需重复检查内部元素。
    result.m_valid = true;
    return result;
}

/// @brief 将逻辑屏幕像素反投影到原始 z=0 谱面平面坐标。
/// @param projection 当前帧构造的投影缓存。
/// @param screenLocal 左上角为原点、与画布绘制一致的逻辑像素坐标。
/// @return 可见前向平面交点；无效投影、近平行、背后或不可表示时为空。
/// @note viewport 外坐标仍进行几何求交，拖动生命周期由交互层决定。
/// @warning
/// 鼠标事件调用仅执行固定矩阵乘法，不求逆、不分配、不访问配置或会话锁。
/// @note 允许屏幕坐标超出视口，以支持拖出画布后再释放。
/// @note 逆映射还检查对应平面点的裁剪深度，近平面之前的交点不可编辑。
inline std::optional<glm::vec2> planarObservationUnproject(
    const PlanarObservationInputProjection& projection, glm::vec2 screenLocal)
{
    // 关闭模式是现有二维输入路径，精确保留任意像素值且不依赖显式 viewport。
    if ( !projection.m_enabled ) return screenLocal;
    if ( !projection.m_valid || !std::isfinite(screenLocal.x) ||
         !std::isfinite(screenLocal.y) )
        return std::nullopt;

    const double ndcX =
        2.0 * static_cast<double>(screenLocal.x) / projection.m_width - 1.0;
    const double ndcY =
        2.0 * static_cast<double>(screenLocal.y) / projection.m_height - 1.0;
    const glm::dvec3 clip(ndcX, ndcY, 1.0);
    // 逆齐次变换的第三分量决定平面交点；相消严重时舍入会放大成无界位置。
    const glm::dvec3 scene = projection.m_clipToScenePlane * clip;
    const double     denominatorMagnitude =
        std::abs(projection.m_clipToScenePlane[0][2] * clip.x) +
        std::abs(projection.m_clipToScenePlane[1][2] * clip.y) +
        std::abs(projection.m_clipToScenePlane[2][2]);
    if ( !Internal::planarObservationInputStableDenominator(
             scene.z, denominatorMagnitude) )
        return std::nullopt;

    const double sceneX = scene.x / scene.z;
    const double sceneY = scene.y / scene.z;
    if ( !Internal::planarObservationInputRepresentable(sceneX) ||
         !Internal::planarObservationInputRepresentable(sceneY) )
        return std::nullopt;

    // 逆映射后的齐次尺度可为负；用正向分母符号判定交点是否位于摄像机前方。
    const double forwardW = projection.m_sceneToClipPlane[0][2] * sceneX +
                            projection.m_sceneToClipPlane[1][2] * sceneY +
                            projection.m_sceneToClipPlane[2][2];
    const double forwardMagnitude =
        std::abs(projection.m_sceneToClipPlane[0][2] * sceneX) +
        std::abs(projection.m_sceneToClipPlane[1][2] * sceneY) +
        std::abs(projection.m_sceneToClipPlane[2][2]);
    // 方向由原矩阵的 w 符号决定，负齐次缩放不能将相机背后交点误认为可见。
    if ( !Internal::planarObservationInputStableDenominator(forwardW,
                                                            forwardMagnitude) ||
         forwardW <= 0.0 )
        return std::nullopt;
    const double clipZ = projection.m_sceneToClipDepthPlane[0] * sceneX +
                         projection.m_sceneToClipDepthPlane[1] * sceneY +
                         projection.m_sceneToClipDepthPlane[2];
    const double depthMagnitude =
        std::abs(projection.m_sceneToClipDepthPlane[0] * sceneX) +
        std::abs(projection.m_sceneToClipDepthPlane[1] * sceneY) +
        std::abs(projection.m_sceneToClipDepthPlane[2]);
    if ( !Internal::planarObservationInputDepthVisible(
             clipZ, forwardW, depthMagnitude, forwardMagnitude) )
        return std::nullopt;
    return glm::vec2(static_cast<float>(sceneX), static_cast<float>(sceneY));
}

/// @brief 将原始 z=0 谱面坐标投影到逻辑屏幕像素。
/// @param projection 当前帧构造的投影缓存。
/// @param scene 场景矩阵输入平面上的 sceneXY 坐标。
/// @return 前向摄像机可见且可表示的逻辑像素坐标，否则为空。
/// @note 深度裁剪只检查 z，不对横纵屏幕坐标施加窗口边界。
/// @warning
/// 鼠标事件调用仅执行固定矩阵乘法，不求逆、不分配、不访问配置或会话锁。
/// @note 不钳制画布边界，投影结果可用于画布外连续拖动。
/// @note 超出 Vulkan 近远深度区间的点没有可见投影，返回空值供调用方忽略。
inline std::optional<glm::vec2> planarObservationProject(
    const PlanarObservationInputProjection& projection, glm::vec2 scene)
{
    // 二维模式必须逐位返回输入，默认 Projection 因而可用于旧测试和调用端。
    if ( !projection.m_enabled ) return scene;
    if ( !projection.m_valid || !std::isfinite(scene.x) ||
         !std::isfinite(scene.y) )
        return std::nullopt;

    const glm::dvec3 plane(
        static_cast<double>(scene.x), static_cast<double>(scene.y), 1.0);
    const glm::dvec3 clip = projection.m_sceneToClipPlane * plane;
    const double     denominatorMagnitude =
        std::abs(projection.m_sceneToClipPlane[0][2] * plane.x) +
        std::abs(projection.m_sceneToClipPlane[1][2] * plane.y) +
        std::abs(projection.m_sceneToClipPlane[2][2]);
    if ( !Internal::planarObservationInputStableDenominator(
             clip.z, denominatorMagnitude) ||
         clip.z <= 0.0 )
        return std::nullopt;

    const double clipZ = projection.m_sceneToClipDepthPlane[0] * plane.x +
                         projection.m_sceneToClipDepthPlane[1] * plane.y +
                         projection.m_sceneToClipDepthPlane[2];
    const double depthMagnitude =
        std::abs(projection.m_sceneToClipDepthPlane[0] * plane.x) +
        std::abs(projection.m_sceneToClipDepthPlane[1] * plane.y) +
        std::abs(projection.m_sceneToClipDepthPlane[2]);
    if ( !Internal::planarObservationInputDepthVisible(
             clipZ, clip.z, depthMagnitude, denominatorMagnitude) )
        return std::nullopt;

    const double ndcX = clip.x / clip.z;
    const double ndcY = clip.y / clip.z;
    // 视口映射沿用渲染裁剪矩形使用的左上原点规则，不在此处翻转 Y。
    const double screenX = (ndcX + 1.0) * projection.m_width * 0.5;
    const double screenY = (ndcY + 1.0) * projection.m_height * 0.5;
    if ( !Internal::planarObservationInputRepresentable(screenX) ||
         !Internal::planarObservationInputRepresentable(screenY) )
        return std::nullopt;
    return glm::vec2(static_cast<float>(screenX), static_cast<float>(screenY));
}
}  // namespace MMM::Canvas
