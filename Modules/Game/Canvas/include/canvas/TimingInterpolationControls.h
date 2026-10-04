#pragma once

struct ImVec2;

namespace MMM
{
enum class TimingEffect;
struct TimingInterpolation;
namespace Config
{
struct TimingInterpolationPreferences;
}
namespace Canvas
{
/// @brief 将工具选项应用到新段，保留当前段的时间范围与起终参数。
/// @param effect 新段的当前类型，BPM 始终强制时间自变量。
/// @param preferences 已由配置读取器检查坐标范围和密度的默认选项。
/// @note 自定义源码由函数编辑器在真实定义域准备后编译。
void applyTimingInterpolationPreferences(
    TimingInterpolation& curve, TimingEffect effect,
    const Config::TimingInterpolationPreferences& preferences);
/// @brief 捕获上次确认的曲线、采样选项和源式，不复制函数或拍轴所有权。
/// @return 可跟随应用配置保存的值对象，不含实体身份或段落位置。
Config::TimingInterpolationPreferences captureTimingInterpolationPreferences(
    const TimingInterpolation& curve);
/// @brief 移动一个贝塞尔控制点，限制单位范围与单调 X 轴。
/// @return 有效输入改变坐标时返回真；非法索引和非有限值不改模型。
/// @warning 连续拖动热路径，只更新四个标量，不发布命令或等待网络。
bool moveTimingBezierControl(TimingInterpolation& curve, int index, double x,
                             double y);
/// @brief 绘制带两枚可拖拽控制点的归一化贝塞尔编辑图。
/// @return 控制点在本帧发生改变时返回真。
/// @warning 每帧固定几何预算，无模型所有权复制、函数编译或文件访问。
bool renderTimingBezierControls(TimingInterpolation& curve, const ImVec2& size);
}  // namespace Canvas
}  // namespace MMM
