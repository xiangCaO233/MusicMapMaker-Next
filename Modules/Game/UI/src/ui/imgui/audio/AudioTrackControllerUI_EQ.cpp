#include "ui/imgui/audio/AudioTrackControllerUI.h"

#include "config/skin/translation/TranslationFormat.h"
#include "imgui.h"
#include "implot.h"
#include "mmm/project/AudioResource.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <span>

#include <ice/config/config.hpp>
#include <ice/core/effect/filter/BiquadFilter.hpp>

namespace MMM::UI
{
namespace
{

/// @brief 十段图示均衡器使用的标准倍频程中心频率。
constexpr std::array<float, 10> TEN_BAND_FREQUENCIES{
    31.25F,  62.5F,   125.0F,  250.0F,  500.0F,
    1000.0F, 2000.0F, 4000.0F, 8000.0F, 16000.0F,
};
/// @brief 十五段图示均衡器使用的扩展中心频率。
constexpr std::array<float, 15> FIFTEEN_BAND_FREQUENCIES{
    25.0F,   40.0F,   63.0F,   100.0F,  160.0F,  250.0F,   400.0F,   630.0F,
    1000.0F, 1600.0F, 2500.0F, 4000.0F, 6300.0F, 10000.0F, 16000.0F,
};

/// @brief 获取持久化 EQ 预设对应的中心频率。
/// @param preset 持久化预设编号。
/// @return 预设无效或关闭时返回空视图。
///
/// 返回 span 只引用静态频率表，不分配内存，也不允许调用方修改频率定义。
[[nodiscard]] std::span<const float> equalizerFrequencies(int preset) noexcept
{
    // 持久化编号与公开 EQPreset 枚举保持一致，避免依赖下拉框局部索引。
    if ( preset == static_cast<int>(Audio::EQPreset::TenBand) ) {
        return TEN_BAND_FREQUENCIES;
    }
    if ( preset == static_cast<int>(Audio::EQPreset::FifteenBand) ) {
        return FIFTEEN_BAND_FREQUENCIES;
    }
    // None 和未知值均无有效频段，交由调用方关闭异常配置。
    return {};
}

/// @brief 补齐项目音频资源 EQ 各频段的持久化参数。
/// @param config 待规范化资源配置。
/// @param bandCount 当前预设的频段数。
/// @return 配置被补齐时返回 true。
///
/// 切换预设时向量尺寸可能不同；新增增益以 0 dB 初始化，新增 Q 以 sqrt(2)
/// 初始化。已有同尺寸内容保持不变，避免每帧覆盖用户调整。
bool ensureEqualizerBandStorage(AudioTrackConfig& config, std::size_t bandCount)
{
    // 两个向量独立校验，因为旧工程可能只缺少其中一组字段。
    bool changed = false;
    if ( config.eqBandGains.size() != bandCount ) {
        config.eqBandGains.resize(bandCount, 0.0F);
        changed = true;
    }
    if ( config.eqBandQs.size() != bandCount ) {
        // sqrt(2) 是当前峰值滤波器的中性默认带宽参数。
        config.eqBandQs.resize(bandCount,
                               static_cast<float>(std::numbers::sqrt2));
        changed = true;
    }
    return changed;
}

/// @brief 构造当前资源配置的无状态 EQ 滤波器，仅用于 UI 曲线预览。
/// @param config 项目音频资源配置。
/// @param frequencies 当前预设的频率表。
/// @param filters 接收滤波器系数的固定容量数组。
///
/// 预览滤波器只保存系数并用于幅频响应计算，不接入实时音频链。配置值先做
/// 有限性检查和范围钳制，避免损坏的项目字段向数学函数传播 NaN。
/// @warning UI 热路径：EQ 可见时每帧按最多十五个频段准备固定数组，无分配。
void prepareEqualizerPreviewFilters(
    const AudioTrackConfig& config, std::span<const float> frequencies,
    std::array<ice::BiquadFilter, FIFTEEN_BAND_FREQUENCIES.size()>& filters)
{
    // 采样率来自音频引擎内部格式，非正值时无法构造数字滤波器。
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return;

    for ( std::size_t index = 0; index < frequencies.size(); ++index ) {
        // ensureEqualizerBandStorage 已保证两个配置向量覆盖频率表长度。
        const float  configuredGain = config.eqBandGains[index];
        const float  configuredQ    = config.eqBandQs[index];
        const double gain =
            // 增益限制与 UI 滑块范围一致，异常值回退到中性 0 dB。
            std::isfinite(configuredGain)
                ? std::clamp(static_cast<double>(configuredGain), -24.0, 24.0)
                : 0.0;
        const double q =
            // Q 必须为正；有效值仍钳制到 UI 暴露的范围。
            std::isfinite(configuredQ) && configuredQ > 0.0F
                ? std::clamp(static_cast<double>(configuredQ), 0.1, 10.0)
                : std::numbers::sqrt2;
        // 每个中心频率对应一个 peaking 滤波器，组合响应在后续按幅度相乘。
        filters[index].set_peaking(sampleRate, frequencies[index], q, gain);
    }
}

/// @brief 计算固定滤波器组在指定频率处的总响应。
/// @param filters 已准备系数的滤波器。
/// @param bandCount 有效滤波器数量。
/// @param frequency 目标频率，单位 Hz。
/// @return 总响应增益，单位 dB。
///
/// 串联滤波器在线性幅度域相乘，最终统一转换为 dB。极小幅度使用 -120 dB
/// 下限，避免 log10(0) 产生负无穷并破坏 ImPlot 坐标。
/// @warning UI 热路径：曲线采样时重复调用；仅遍历最多十五个固定滤波器。
[[nodiscard]] double equalizerResponseDb(
    const std::array<ice::BiquadFilter, FIFTEEN_BAND_FREQUENCIES.size()>&
                filters,
    std::size_t bandCount, double frequency) noexcept
{
    // 与系数准备使用相同采样率，保证预览曲线对应实际引擎格式。
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return 0.0;

    // 单位幅度是串联乘积的幺元，也覆盖 bandCount 为零的防御情况。
    double magnitude = 1.0;
    for ( std::size_t index = 0; index < bandCount; ++index ) {
        magnitude *=
            filters[index].get_magnitude_response(frequency, sampleRate);
    }
    // -120 dB 以下对当前图表范围无可见差异，统一截断以保持有限值。
    if ( magnitude <= 1.0e-6 ) return -120.0;
    return 20.0 * std::log10(magnitude);
}

}  // namespace

/// @brief 绘制主轨均衡器预设、响应曲线、频段增益与 Q 控件。
/// @param config 当前帧可编辑的音频资源配置草稿。
/// @param changed 输入输出变更标志，任何规范化或用户编辑都会置为 true。
///
/// 配置先校验预设和向量尺寸，再构造固定滤波器组绘制预览。频段控件在横向
/// 可滚动子窗口中按列排列，窄窗口不会压缩滑块到不可操作。所有变更仅写入
/// 调用方草稿，持久化由控制器主 update 在本帧末尾统一提交。
///
/// 配置不变量：
/// - eqEnabled 为 false 时预设选择显示 None，不访问频段向量；
/// - eqEnabled 为 true 时 eqPreset 只能是 TenBand 或 FifteenBand；
/// - 增益和 Q 向量长度必须与所选频率表完全一致；
/// - 增益范围为 -24 至 24 dB，Q 范围为 0.1 至 10；
/// - 非有限持久化值在预览计算时使用中性默认值；
/// - 规范化配置与用户操作一样设置 changed，确保修复得到持久化。
///
/// 绘制不变量：
/// - 响应曲线固定采样 128 个对数均匀频点；
/// - 曲线与散点使用同一个频率表和本帧配置草稿；
/// - 频段列宽至少容纳标准滑块和最宽频率标签；
/// - 内容超宽时启用横向滚动，不缩小滑块命中区域；
/// - 子窗口为底部重置按钮预留固定一行空间；
/// - 每个频段通过 PushID 隔离重复控件标签。
///
/// 数值约束：
/// - 频率轴固定为 20 Hz 至 20 kHz 的对数尺度；
/// - 响应图纵轴与增益控件共享正负 24 dB 范围；
/// - 串联响应在线性幅度域相乘后再转换为 dB；
/// - 小于等于 1e-6 的综合幅度显示为 -120 dB；
/// - 十段和十五段预设复用固定十五容量的栈数组；
/// - 关闭预设时不保留“启用但未知编号”的异常状态。
///
/// 生命周期约束：
/// - filters、curveX、curveY、plotX 和 plotY 都只在当前栈帧存在；
/// - ImPlot 在 BeginPlot 成功时才接收数组，并在 EndPlot 前完成读取；
/// - EQSliders 子窗口始终与 BeginChild 成对结束；
/// - 配置向量由调用方持有，本函数不保存其元素引用到下一帧。
/// @warning UI 热路径：EQ 可见时每帧计算固定 128 点曲线；不得增加动态采样、
/// 音频线程查询、文件访问或阻塞同步。
void AudioTrackControllerUI::renderEQSection(AudioTrackConfig& config,
                                             bool&             changed)
{
    // 分隔线把 EQ 从 Clay 渲染的基础控制区中视觉分组。
    ImGui::Separator();

    // 预设下拉框使用固定宽度并在当前内容区水平居中。
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    const float comboWidth   = 200.0F;

    const char* title      = TR("ui.audio_manager.eq_control").data();
    const float titleWidth = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX((contentWidth - titleWidth) * 0.5F);
    ImGui::Text("%s", title);

    // 下拉索引与 EQPreset 持久化编号一一对应：0 关闭、1 十段、2 十五段。
    const char* presets[] = { TR("ui.audio_manager.eq_none").data(),
                              TR("ui.audio_manager.eq_10_band").data(),
                              TR("ui.audio_manager.eq_15_band").data() };
    const bool  validEnabledPreset =
        config.eqEnabled &&
        (config.eqPreset == static_cast<int>(Audio::EQPreset::TenBand) ||
         config.eqPreset == static_cast<int>(Audio::EQPreset::FifteenBand));
    int presetIndex = validEnabledPreset ? config.eqPreset : 0;
    if ( config.eqEnabled && !validEnabledPreset ) {
        // 旧工程或损坏值不能保持“启用但无频率表”的矛盾状态。
        config.eqEnabled = false;
        config.eqPreset  = static_cast<int>(Audio::EQPreset::None);
        changed          = true;
    }
    // 成员预设用于控制器其他视觉状态，始终反映本帧已校验索引。
    m_currentPreset = static_cast<Audio::EQPreset>(presetIndex);

    ImGui::SetCursorPosX((contentWidth - comboWidth) * 0.5F);
    ImGui::SetNextItemWidth(comboWidth);
    if ( ::MMM::UI::FeedbackCombo(
             "##EQPreset", &presetIndex, presets, IM_ARRAYSIZE(presets)) ) {
        config.eqEnabled = presetIndex != 0;
        config.eqPreset  = presetIndex;
        m_currentPreset  = static_cast<Audio::EQPreset>(presetIndex);
        if ( config.eqEnabled ) {
            // 启用新预设时立即调整持久化数组，后续曲线和滑块可安全索引。
            const auto frequencies = equalizerFrequencies(config.eqPreset);
            (void)ensureEqualizerBandStorage(config, frequencies.size());
        }
        changed = true;
    }

    // 关闭状态只显示标题和预设选择，不构造曲线或频段控件。
    if ( !config.eqEnabled ) return;

    const auto frequencies = equalizerFrequencies(config.eqPreset);
    if ( frequencies.empty() ) {
        // 二次防御预设在组合框处理后仍无频率表的异常配置。
        config.eqEnabled = false;
        config.eqPreset  = static_cast<int>(Audio::EQPreset::None);
        m_currentPreset  = Audio::EQPreset::None;
        changed          = true;
        return;
    }
    // 即使用户未操作，修复旧配置尺寸也需要通知主控制器持久化。
    changed |= ensureEqualizerBandStorage(config, frequencies.size());
    const std::size_t bandCount = frequencies.size();

    // 固定十五容量覆盖两种预设，避免 UI 帧动态分配滤波器容器。
    std::array<ice::BiquadFilter, FIFTEEN_BAND_FREQUENCIES.size()> filters;
    prepareEqualizerPreviewFilters(config, frequencies, filters);

    if ( ImPlot::BeginPlot("##EQCurve",
                           ImVec2(-1, 150),
                           ImPlotFlags_NoLegend | ImPlotFlags_NoMenus) ) {
        ImPlot::SetupAxis(ImAxis_X1, TR("ui.audio_manager.freq_hz").data());
        ImPlot::SetupAxis(ImAxis_Y1, TR("ui.audio_manager.gain_db").data());
        // 坐标轴标题来自翻译资源，内部 Plot ID 保持隐藏且稳定。
        // 对数频率轴更接近听觉尺度，20 Hz 到 20 kHz 覆盖常用听域。
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_X1, 20, 20000);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -24, 24);
        // Y 轴范围与增益滑块一致，用户可直接比较控制值和综合响应。
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%.0f dB");

        // 固定 128 点在曲线平滑度与每帧滤波器响应计算之间取得上限明确的平衡。
        constexpr std::size_t                  CURVE_SAMPLE_COUNT = 128U;
        std::array<double, CURVE_SAMPLE_COUNT> curveX{};
        std::array<double, CURVE_SAMPLE_COUNT> curveY{};
        for ( std::size_t index = 0; index < CURVE_SAMPLE_COUNT; ++index ) {
            // 20 * 1000^ratio 在对数域均匀覆盖 20 到 20000 Hz。
            const double ratio = static_cast<double>(index) /
                                 static_cast<double>(CURVE_SAMPLE_COUNT - 1U);
            const double frequency = 20.0 * std::pow(1000.0, ratio);
            curveX[index]          = frequency;
            curveY[index] = equalizerResponseDb(filters, bandCount, frequency);
        }

        // 青色响应线展示全部 peaking 滤波器串联后的综合增益。
        ImPlot::PlotLine(TR("ui.audio_manager.response").data(),
                         curveX.data(),
                         curveY.data(),
                         static_cast<int>(curveX.size()),
                         ImPlotSpec(ImPlotProp_LineColor,
                                    ImVec4(0.2F, 0.8F, 1.0F, 1.0F),
                                    ImPlotProp_LineWeight,
                                    2.0F));

        // 散点只展示各频段中心增益，便于对应下方竖直滑块。
        std::array<double, FIFTEEN_BAND_FREQUENCIES.size()> plotX{};
        std::array<double, FIFTEEN_BAND_FREQUENCIES.size()> plotY{};
        for ( std::size_t index = 0; index < bandCount; ++index ) {
            plotX[index] = frequencies[index];
            plotY[index] = config.eqBandGains[index];
        }
        ImPlot::PlotScatter(TR("ui.audio_manager.bands").data(),
                            plotX.data(),
                            plotY.data(),
                            static_cast<int>(bandCount),
                            ImPlotSpec(ImPlotProp_Marker, ImPlotMarker_Circle));

        // EndPlot 只在 BeginPlot 成功分支调用，维持 ImPlot 栈平衡。
        ImPlot::EndPlot();
    }

    ImGui::Separator();

    // 竖滑块宽度取标准控件高度，标签列再按最宽频率文本扩展。
    const float sliderWidth   = ImGui::GetFrameHeight();
    const float eqSpacing     = 8.0F;
    float       maxLabelWidth = 0.0F;
    for ( const float frequency : frequencies ) {
        // 千赫以上使用 k 后缀缩短标签，低频保留整数 Hz。
        char label[32]{};
        if ( frequency >= 1000.0F ) {
            std::snprintf(label, sizeof(label), "%.1fk", frequency / 1000.0F);
        } else {
            std::snprintf(label, sizeof(label), "%.0f", frequency);
        }
        maxLabelWidth = std::max(maxLabelWidth, ImGui::CalcTextSize(label).x);
    }

    // 每列必须同时容纳滑块与标签，避免频率文本相互覆盖。
    const float colWidth = std::max(sliderWidth, maxLabelWidth);
    const float totalWidth =
        static_cast<float>(bandCount) * (colWidth + eqSpacing) - eqSpacing;
    const float availWidth           = ImGui::GetContentRegionAvail().x;
    const float totalWidthWithBuffer = totalWidth + 2.0F;
    const float childWidth = std::min(totalWidthWithBuffer, availWidth);
    if ( childWidth < availWidth ) {
        // 全部频段能放下时居中子窗口；超宽时占满并启用横向滚动。
        ImGui::SetCursorPosX((availWidth - childWidth) * 0.5F);
    }

    const ImGuiWindowFlags childFlags =
        // 只有内容实际超宽时显示横向滚动条，避免无效滚动占用高度。
        totalWidthWithBuffer > availWidth ? ImGuiWindowFlags_HorizontalScrollbar
                                          : ImGuiWindowFlags_None;
    // 为子窗口之后的重置按钮保留一行高度和标准间距。
    const float footerHeight =
        ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const float availHeight = ImGui::GetContentRegionAvail().y;
    // 最低 220 像素保证两个竖滑块和频率标签均具有基本可操作空间。
    const float childHeight = std::max(220.0F, availHeight - footerHeight);

    // 子窗口移除内边距，列起点由 START_X 精确控制。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool opened = ImGui::BeginChild("EQSliders",
                                          ImVec2(childWidth, childHeight),
                                          ImGuiChildFlags_None,
                                          childFlags);
    ImGui::PopStyleVar();

    if ( opened ) {
        // 横向列间距固定，纵向间距只影响同列标签和两个滑块。
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(eqSpacing, 4));

        constexpr float START_X = 1.0F;
        const float     startY  = ImGui::GetCursorPosY();
        // 扣除标签和余量后，增益占六成高度，Q 占四成且各有最小值。
        const float totalSlidersHeight = childHeight - 48.0F;
        const float gainSliderHeight =
            std::max(60.0F, totalSlidersHeight * 0.6F);
        const float qSliderHeight = std::max(40.0F, totalSlidersHeight * 0.4F);

        for ( std::size_t index = 0; index < bandCount; ++index ) {
            // 频段索引建立独立 ImGui ID 空间，使重复 Gain/Q 标签不会冲突。
            ImGui::PushID(static_cast<int>(index));
            ImGui::SetCursorPos(ImVec2(
                START_X + static_cast<float>(index) * (colWidth + eqSpacing),
                startY + 3.0F));
            ImGui::BeginGroup();
            // Group 让同一频段的标签、增益和 Q 作为完整布局单元。

            float& gain = config.eqBandGains[index];
            float& q    = config.eqBandQs[index];
            // 引用直接指向本帧配置草稿，滑块修改立即反映到预览的下一帧。
            char        label[32]{};
            const float frequency = frequencies[index];
            if ( frequency >= 1000.0F ) {
                // 以一位小数保留 6.3k 等非整数千赫中心频率。
                std::snprintf(
                    label, sizeof(label), "%.1fk", frequency / 1000.0F);
            } else {
                // 低于 1 kHz 的标准中心频率按整数标签展示。
                std::snprintf(label, sizeof(label), "%.0f", frequency);
            }

            // 标签和两个滑块分别按列宽居中，保持所有频段轴线对齐。
            const float textWidth = ImGui::CalcTextSize(label).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (colWidth - textWidth) * 0.5F);
            ImGui::TextUnformatted(label);

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (colWidth - sliderWidth) * 0.5F);
            if ( ::MMM::UI::FeedbackVSliderFloat(
                     "##Gain",
                     ImVec2(sliderWidth, gainSliderHeight),
                     &gain,
                     -24.0F,
                     24.0F,
                     "") ) {
                // 空格式隐藏滑块旁常驻值，精确值仅在 Tooltip 中显示。
                changed = true;
            }
            // 拖动或悬停时显示精确 dB，常态只保留紧凑频率标签。
            if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    "%s",
                    TR_FMT("ui.audio_manager.eq_tooltip", label, gain).c_str());
            }

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (colWidth - sliderWidth) * 0.5F);
            if ( ::MMM::UI::FeedbackVSliderFloat(
                     "##Q",
                     ImVec2(sliderWidth, qSliderHeight),
                     &q,
                     0.1F,
                     10.0F,
                     "") ) {
                // Q 与增益共享 changed 标志，由主控制器合并为一次配置命令。
                changed = true;
            }
            // Q 提示独立于增益，避免在狭窄列中长期显示数值文本。
            if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    "%s",
                    TR_FMT("ui.audio_manager.q_factor_tooltip", q).c_str());
            }

            ImGui::EndGroup();
            // 每轮频段结束后恢复 ID，避免影响重置按钮和后续窗口控件。
            ImGui::PopID();
        }
        // ItemSpacing 只覆盖滑块子窗口内容，循环结束后立即恢复。
        ImGui::PopStyleVar();
    }
    // BeginChild 无论返回值如何都必须以 EndChild 配对。
    ImGui::EndChild();

    // 重置按钮按翻译文本自适应宽度，并在当前内容区居中。
    const char* resetLabel  = TR("ui.audio_manager.reset_eq").data();
    const float buttonWidth = ImGui::CalcTextSize(resetLabel).x +
                              ImGui::GetStyle().FramePadding.x * 2.0F;
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - buttonWidth) *
                         0.5F);
    if ( ::MMM::UI::FeedbackButton(resetLabel) ) {
        // 重置保持当前预设启用，只恢复所有频段的中性增益和默认 Q。
        std::fill(config.eqBandGains.begin(), config.eqBandGains.end(), 0.0F);
        std::fill(config.eqBandQs.begin(),
                  config.eqBandQs.end(),
                  static_cast<float>(std::numbers::sqrt2));
        // changed 触发本帧末尾持久化，并让下一帧曲线重新按中性参数生成。
        changed = true;
    }
}

}  // namespace MMM::UI
