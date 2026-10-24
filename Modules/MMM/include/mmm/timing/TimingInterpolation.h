#pragma once

#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>

namespace MMM
{
class TimingFunction;
class TimingBeatAxis;

/// @brief 函数自变量的计量单位；红线仅允许时间轴，避免 BPM 定义循环。
enum class TimingVariable { Time, Beat };

/// @brief 插值段落支持的归一化曲线；枚举顺序同时用于编辑器下拉选择。
enum class TimingCurve {
    Linear,
    EaseIn,
    EaseOut,
    SmoothStep,
    Sine,
    Exponential,
    Bezier,
    Cubic,
    Quartic,
    Quintic,
    SquareRoot,
    CubeRoot,
    Logarithmic,
    Reciprocal,
    SineIn,
    SineOut,
    HyperbolicTangent,
    Custom,
};

/// @brief 一个时间点后的连续参数变化，时间长度统一使用秒。
/// @details 起始值由所属 Timing 提供；原生格式保留本结构，外部格式只写采样。
struct TimingInterpolation {
    /// @brief 段落持续时间；不随 Timing 模型的毫秒单位转换。
    double m_duration{ 1.0 };
    /// @brief 段落终点参数值。
    double m_endValue{ 1.0 };
    /// @brief 外部格式写出密度，单位为每秒采样次数。
    double m_samplesPerSecond{ 16.0 };
    /// @brief 自变量单位；时间模式保持旧文件的秒域语义。
    TimingVariable m_variable{ TimingVariable::Time };
    /// @brief 每次输出前进的拍数分子；分母共同表示有理分拍。
    int m_beatNumerator{ 1 };
    /// @brief 分拍分母，默认每半拍插入一个事件。
    int m_beatDenominator{ 2 };
    /// @brief 从 BPM 时间线推导的段内拍长，也是节拍函数的定义域。
    double m_beatDuration{ 1.0 };
    /// @brief 不可变拍位映射；只在范围或 BPM 数据变化的低频入口重建。
    /// @warning 快照跨线程拥有缓存以保障生命周期，逐帧求值仅借用引用。
    std::shared_ptr<const TimingBeatAxis> m_beatAxis;
    /// @brief 段落采用的归一化曲线。
    TimingCurve m_curve{ TimingCurve::Linear };
    /// @brief 贝塞尔第一控制点的自变量比例。
    double m_controlX1{ 0.25 };
    /// @brief 贝塞尔第一控制点的参数比例。
    double m_controlY1{ 0.25 };
    /// @brief 贝塞尔第二控制点的自变量比例。
    double m_controlX2{ 0.75 };
    /// @brief 贝塞尔第二控制点的参数比例。
    double m_controlY2{ 0.75 };
    /// @brief 自定义绝对参数函数；预设曲线不使用此缓存。
    /// @warning 不可变程序跨逻辑与 UI 快照共享，避免实体删除后旧快照悬空。
    /// 只在描述复制时保留所有权；逐次求值必须借用引用，不复制指针。
    std::shared_ptr<const TimingFunction> m_function;
    /// @brief 按源表达式比较语义，不比较编译缓存地址。
    bool operator==(const TimingInterpolation&) const;
};

/// @brief 序列化段落定义；时间与密度单位固定为秒和每秒次数。
void to_json(nlohmann::json& json, const TimingInterpolation& interpolation);

/// @brief 从不可信 JSON 读取段落；字段类型或曲线非法时返回空。
std::optional<TimingInterpolation> readTimingInterpolation(
    const nlohmann::json& json);
}  // namespace MMM
