#pragma once

#include "mmm/timing/TimingFunction.h"
#include "mmm/timing/TimingFunctionFit.h"
#include "mmm/timing/TimingInterpolation.h"
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace MMM::Canvas
{
/// @brief 模态窗口内的函数编辑状态，取消窗口后不写入谱面。
struct TimingFunctionEditorState {
    /// @brief 文本容量与表达式编译器的字符预算一致。
    std::array<char, 2049> m_expression{};
    /// @brief 符号按钮排队插入的模板，交给 InputText 回调处理光标与选择区。
    std::string m_pendingInsertion;
    /// @brief 仅表达式或时长变化时重新编译，常规帧读取缓存。
    double m_compiledDuration{ -1 };
    /// @brief 编译失败说明，保留错误输入供用户修改。
    std::string m_error;
    /// @brief 129 个时间格点覆盖完整绘制区间，不随拖动频率增加数量。
    std::array<double, 129> m_values{};
    /// @brief 区分参考曲线和真正画过的位置，不能把缺失区间静默补齐。
    std::array<bool, 129> m_drawn{};
    /// @brief 可调整的绘制纵轴下界。
    double m_minimum{};
    /// @brief 可调整的绘制纵轴上界。
    double m_maximum{};
    /// @brief 上次手势格点，跨帧拖动时线性补齐途经格点。
    int m_previousIndex{ -1 };
    /// @brief 上次格点的参数值，拖动方向可反转。
    double m_previousValue{};
    /// @brief 最近一次拟合结果，重新绘制后失效。
    std::optional<TimingFunctionFit> m_fit;
    /// @brief 最近拟合的不可变缓存，成功时用于窗口副本，不直接发布到逻辑线程。
    std::shared_ptr<const TimingFunction> m_fitFunction;
    /// @brief 绘制、拟合及领域约束产生的提示。
    std::string m_fitError;

    /// @brief 将合法拟合一次性应用到编辑副本，让公式和输出预览使用同一函数。
    /// @param curve 窗口拥有的段落副本，保存前不修改逻辑会话。
    /// @param effect 当前效果，用于检查完整区间的参数合法性。
    /// @param startValue 成功时更新为 f(0)，失败时保留原值。
    /// @return 候选时长、文本容量和领域校验全部通过时返回真。
    /// @warning 仅拟合完成或显式重新采用时调用，禁止逐帧复制共享所有权。
    /// @note 先在临时副本校验，BPM 过冲或失效候选不能部分覆盖原公式。
    /// @note 写出采样密度及贝塞尔控制点保持原值，拟合只替换数学定义。
    /// @note 输入文本同步切换为自定义函数，使用户可以继续细调拟合系数。
    /// @note 校验期间借用完整区间缓存，不再次拟合或编译同一表达式。
    /// @note 此处不持有实体或会话引用，取消窗口仍能完整丢弃工作副本。
    bool applyFit(TimingInterpolation& curve, TimingEffect effect,
                  double& startValue)
    {
        // 修改范围后旧缓存不再对应秒域，不能只凭候选指针存在就应用。
        if ( !m_fit || !m_fitFunction ||
             timingFunctionDuration(*m_fitFunction) != curve.m_duration ||
             m_fit->m_expression.size() >= m_expression.size() ) {
            m_fitError = "拟合结果已失效，请重新拟合。";
            return false;
        }
        auto candidate       = curve;
        candidate.m_curve    = TimingCurve::Custom;
        candidate.m_function = m_fitFunction;
        const double start   = evaluateTimingFunction(*m_fitFunction, 0);
        candidate.m_endValue =
            evaluateTimingFunction(*m_fitFunction, curve.m_duration);
        // 完整域校验也覆盖两端均合法、但区间中间越界的 BPM 曲线。
        if ( !isValidTimingInterpolation(candidate, effect, start) ) {
            m_fitError =
                "拟合函数超出当前段落的合法参数范围，请调整后重新拟合。";
            return false;
        }
        // 同一次低频操作提交全部 UI 真值，下一帧不能重新编译旧文本。
        std::copy(m_fit->m_expression.begin(),
                  m_fit->m_expression.end(),
                  m_expression.begin());
        m_expression[m_fit->m_expression.size()] = '\0';
        curve                                    = std::move(candidate);
        startValue                               = start;
        m_compiledDuration                       = curve.m_duration;
        m_error.clear();
        m_fitError.clear();
        return true;
    }
};
}  // namespace MMM::Canvas
