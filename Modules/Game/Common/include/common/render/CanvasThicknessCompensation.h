#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace MMM::Common::Render
{
/// @brief 当前快照的厚度投影参数；关闭时保持所有源几何逐点不变。
/// @note 只补偿贴图局部厚度，不参与轨宽、时间积分或持续区间计算。
/// @note 四角仍留在源平面，现有渲染管线、UV 插值与鼠标逆投影可以继续复用。
/// @note 参数由快照生产者一次发布，所有主体、发光与固定尺寸特效借用同一值。
struct CanvasThicknessCompensation {
    glm::mat4 m_projection{ 1.0F };  ///< 与当前主画布相同的最终投影。
    glm::vec2 m_viewport{ 1.0F };    ///< 投影使用的逻辑像素视口。
    float     m_amount{ 0.0F };  ///< 零保留透视，一表示屏幕厚度等于二维厚度。

    /// @brief 按两条厚度边的中心反解透视，保持中心线与端点跨度不变。
    /// @param points 按左下、右下、右上、左上排列的源平面四角。
    /// @param alignment 纵向定位比例，中心为零，底边为负半高。
    /// @return 成功或关闭返回真；跨越地平线、退化或无有限解时保留整个原图元。
    /// @note 厚度按垂直于投影后中心线的像素距离定义，不能仅用相机距离近似。
    /// @warning 每个可见定高图元固定四点运算；不求逆矩阵、不分配或等待。
    bool compensate(std::array<glm::vec2, 4>& points, float alignment = 0) const
    {
        if ( !(m_amount > 0) ) return true;
        // 两个端点分别锚定，横向连接体不会因增厚改变轨道跨度或时间长度。
        // 锚点按同一对齐规则还原，底边模式的判定位置不能随补偿漂移。
        const auto left  = (points[0] + points[3]) * 0.5F -
                           (points[0] - points[3]) * alignment;
        const auto right = (points[1] + points[2]) * 0.5F -
                           (points[1] - points[2]) * alignment;
        const auto a     = m_projection * glm::vec4(left, 0, 1);
        const auto b     = m_projection * glm::vec4(right, 0, 1);
        if ( !visible(a) || !visible(b) ) return false;
        // 源法线给出未透视时的目标像素厚度，屏幕法线给出实际的可见厚度。
        // 两者分别归一化，才能正确处理非正方形视口以及绕 Z 轴旋转的画布。
        // 端点投影连线就是整个中心线的投影，无需对每个像素求导。
        const auto sourceRow = right - left;
        const auto screenRow =
            (glm::vec2(b) / b.w - glm::vec2(a) / a.w) * (m_viewport * 0.5F);
        const float sourceLength = glm::length(sourceRow);
        const float screenLength = glm::length(screenRow);
        // 侧视退化时不存在有限的均匀厚度，不能把不稳定结果写入顶点缓冲。
        if ( !(sourceLength > 0.0001F) || !(screenLength > 0.0001F) )
            return false;
        const glm::vec2 sourceNormal(-sourceRow.y / sourceLength,
                                     sourceRow.x / sourceLength);
        const glm::vec2 screenNormal(-screenRow.y / screenLength,
                                     screenRow.x / screenLength);
        auto            corrected = points;
        for ( std::size_t i = 0; i < points.size(); ++i ) {
            const bool onLeft = i == 0 || i == 3;
            const auto anchor = onLeft ? left : right;
            const auto clip   = onLeft ? a : b;
            // 四边形两端可以处于不同深度，每条边必须使用自己的齐次分母。
            // 顶点只沿原局部厚度边伸缩，不把横向轨道位置也乘以补偿倍率。
            const auto delta = points[i] - anchor;
            const auto d     = m_projection * glm::vec4(delta, 0, 0);
            // 固定底边上的顶点与锚点重合，无需反解零厚度，也不能判为退化失败。
            if ( delta.x == 0 && delta.y == 0 ) continue;
            const auto original = clip + d;
            if ( !visible(original) ) return false;
            // 齐次分母沿局部厚度方向变化；导数近似会使厚贴图上下边仍然不均匀。
            const float k =
                glm::dot((glm::vec2(d) * clip.w - glm::vec2(clip) * d.w) *
                             (m_viewport * 0.5F),
                         screenNormal);
            // 有符号投影距离保留上下边方向，翻转视角也不能将两边移到同一侧。
            // 用法线分量而非屏幕边长衡量，避免斜向剪切看似加高但实际仍薄。
            const float natural = k / (clip.w * original.w);
            const float uniform =
                std::copysign(std::abs(glm::dot(delta, sourceNormal)), natural);
            const float target = natural + (uniform - natural) *
                                               std::clamp(m_amount, 0.0F, 1.0F);
            // 混合发生在最终屏幕厚度上，百分之五十是自然厚度与目标厚度的中点。
            // 即使运动补偿已经消除了部分透视，也只补偿最终矩阵剩余的缩放。
            const float denominator = k - target * clip.w * d.w;
            // 精确解 target = k*t /
            // (w*(w+t*dw))，不采用固定倍数或距离平方猜测。
            // 正比例约束阻止跨越消失点后翻面；失败时四角一起回退，不能撕裂图元。
            if ( !std::isfinite(denominator) ||
                 std::abs(denominator) < 1.0e-8F )
                return false;
            // 无限解或负解说明指定屏幕厚度已越过该投影的消失点。
            // 此时不能通过任意巨大上限制造闪烁，统一交回原始几何与正常裁剪。
            const float scale = target * clip.w * clip.w / denominator;
            const auto  point = anchor + delta * scale;
            if ( !(scale > 0) || !std::isfinite(point.x) ||
                 !std::isfinite(point.y) || !visible(clip + d * scale) )
                return false;
            corrected[i] = point;
        }
        // 只有完整四边形均有效才提交，UV、颜色与顶点布局无需变化。
        // 临时四角位于栈上，失败回退不需要复制或撤销已追加的 GPU 顶点。
        // 前面的深度检查还保证补偿没有将可见边推到近裁剪面之后。
        points = corrected;
        return true;
    }

private:
    /// @brief 仅接受相机前方且处于近远裁剪范围内的有限齐次点。
    /// @warning 逐顶点常量检查，不访问图形设备或共享状态。
    static bool visible(glm::vec4 point)
    {
        // 屏幕外的 XY 允许保留，最终由真实视口裁剪；负 W 不得镜像投回画布。
        return std::isfinite(point.x) && std::isfinite(point.y) &&
               std::isfinite(point.z) && std::isfinite(point.w) &&
               point.w > 0.0001F && point.z >= 0 && point.z <= point.w;
    }
};
}  // namespace MMM::Common::Render
