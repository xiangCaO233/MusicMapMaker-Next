#include "ui/imgui/menu/actions/tools/BpmAutoDetector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fftw3.h>
#include <limits>
#include <mutex>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 自动检测要求的最短音频长度，单位为秒。
constexpr double MIN_DETECT_SECONDS = 10.0;

/// @brief 特征自相关允许的最大 FFT 点数。
constexpr size_t FFT_MAX_N = size_t{ 1 } << 20;

/// @brief 边缘检测滤波器延迟补偿，单位为毫秒。
constexpr double FILTER_DELAY_MS = 1.2;

/// @brief 自动检测允许吸附前的最高 BPM。
constexpr double MAX_BPM = 220.0;

/// @brief 预处理使用的子频段数量。
constexpr size_t SUBBAND_COUNT = 4;

/// @brief 各频段混合权重。
constexpr std::array<float, SUBBAND_COUNT> SUBBAND_WEIGHTS{
    0.3f, 0.2f, 0.2f, 0.3f
};

/// @brief 子频段和后续滤波器索引配置。
constexpr unsigned FILTERS[][2] = {
    { 0, 2 }, { 2, 4 }, { 6, 4 },  { 10, 2 }, { 12, 1 }, { 0, 0 },
    { 0, 0 }, { 0, 0 }, { 13, 2 }, { 15, 1 }, { 16, 1 },
};

/// @brief 各频段滤波器的延迟补偿，单位为采样点。
constexpr std::array<int, SUBBAND_COUNT> SUBBAND_FILTER_DELAYS{
    -320, -64, -32, 0
};

/// @brief 44.1kHz 下的二阶节滤波系数。
constexpr double FILTER_COEFF_SOS_44[][5] = {
    { 2, 1, -1.9296472648815026, 0.93671950987931574, 0.0017680612494532478 },
    { 2, 1, -1.8470012302151446, 0.85377057373666410, 0.0016923358803798848 },
    { 0, -1, -1.9701832899735581, 0.97774356614503610, 0.042048320411797346 },
    { 0, -1, -1.9307644878934767, 0.95804211074743828, 0.042048320411797346 },
    { 0, -1, -1.9237340683861910, 0.93435845766892844, 0.041138010165536872 },
    { 0, -1, -1.8951712655794619, 0.91375057048949460, 0.041138010165536872 },
    { 0, -1, -1.9259686517338530, 0.95581997938114360, 0.082730627558081263 },
    { 0, -1, -1.8121187013381126, 0.91831227931931725, 0.082730627558081263 },
    { 0, -1, -1.8314525533284556, 0.87252152856112386, 0.079370925545494644 },
    { 0, -1, -1.7636927184274693, 0.83473849906751341, 0.079370925545494644 },
    { -2, 1, -1.6699250371362808, 0.77254617806529502, 0.86061780380039399 },
    { -2, 1, -1.4385561035314660, 0.52695903501152652, 0.74137878463574813 },
    { 0, 0, -1.948, 0.9481, 0.048 },
    { 2, 1, -1.9660249635383409, 0.96782223970722425, 0.00044931904222082926 },
    { 2, 1, -1.9222869522443087, 0.92404424454379519, 0.00043932307487161041 },
    { 0, 0, -1.4, 0.48, 0.2 },
    { 0, -1, -1.8799483399273036, 0.88366532316014612, 0.058167338419926939 },
};

/// @brief 48kHz 下的二阶节滤波系数。
constexpr double FILTER_COEFF_SOS_48[][5] = {
    { 2, 1, -1.9357148371211979, 0.94170045160372695, 0.0014964036206322460 },
    { 2, 1, -1.8590762659582099, 0.86482489876726276, 0.0014371582022632194 },
    { 0, -1, -1.9731491828203316, 0.97953721714347519, 0.038683376541251063 },
    { 0, -1, -1.9383006635720235, 0.96137281475624847, 0.038683376541251063 },
    { 0, -1, -1.9305470566393397, 0.93953993813607839, 0.037909869457216396 },
    { 0, -1, -1.9047367208661594, 0.92047699481829581, 0.037909869457216396 },
    { 0, -1, -1.9341140031258925, 0.95936683579188464, 0.076211068056843939 },
    { 0, -1, -1.8345468976158030, 0.92460252591545578, 0.076211068056843939 },
    { 0, -1, -1.8474800548242554, 0.88234057507713604, 0.073338217988390020 },
    { 0, -1, -1.7867084943628910, 0.84711889379273642, 0.073338217988390020 },
    { -2, 1, -1.7009643319435259, 0.78849973981529797, 0.87236601793970592 },
    { -2, 1, -1.4796742169311932, 0.55582154328248878, 0.75887394005342046 },
    { 0, 0, -1.952225, 0.95230941015625, 0.0441 },
    { 2, 1, -1.9688774973857579, 0.97039660175711517, 0.00037977609283935493 },
    { 2, 1, -1.9285084850826344, 0.92999644239525459, 0.00037198932815510181 },
    { 0, 0, -1.4, 0.48, 0.2 },
    { 0, -1, -1.8799483399273036, 0.88366532316014612, 0.058167338419926939 },
};

/// @brief 32kHz 下的二阶节滤波系数。
constexpr double FILTER_COEFF_SOS_32[][5] = {
    { 2, 1, -1.9006465638071275, 0.91391293369153381, 0.0033165924711015399 },
    { 2, 1, -1.7915876967777840, 0.80409284398316283, 0.0031262868013446823 },
    { 0, -1, -1.9551331294901764, 0.96942359724192628, 0.057589864308760577 },
    { 0, -1, -1.8914384351956604, 0.94273848531403681, 0.057589864308760577 },
    { 0, -1, -1.8906481524757157, 0.91056795618768371, 0.055913207754023600 },
    { 0, -1, -1.8483764376432956, 0.88306736308808476, 0.055913207754023600 },
    { 0, -1, -1.8832665479670578, 0.93936089151864399, 0.11261473189959464 },
    { 0, -1, -1.6928128227129573, 0.88994695879396346, 0.11261473189959464 },
    { 0, -1, -1.7518851739273100, 0.82786888860462982, 0.10660246744674093 },
    { 0, -1, -1.6489134150085212, 0.77932148942151369, 0.10660246744674093 },
    { -2, 1, -1.5182418440638745, 0.70396265666726210, 0.80555112518278416 },
    { -2, 1, -1.2554404734849929, 0.40901378318031245, 0.66611356416632628 },
    { 0, 0, -1.9283375, 0.9285274228515625, 0.06615 },
    { 2, 1, -1.9525426196393316, 0.95593497333644439, 0.00084808842427817996 },
    { 2, 1, -1.8935423413365597, 0.89683218776432150, 0.00082246160694037732 },
    { 0, 0, -1.4, 0.48, 0.2 },
    { 0, -1, -1.8799483399273036, 0.88366532316014612, 0.058167338419926939 },
};

/// @brief BPM 吸附表：单位精度和不确定度倍数。
constexpr double BPM_SNAP[][2] = {
    { 1.0, 3.0 },  { 2.0, 2.5 },   { 3.0, 2.0 },   { 10.0, 2.0 },
    { 20.0, 1.5 }, { 100.0, 1.5 }, { 200.0, 1.0 },
};

/// @brief BPM 线性回归中间结果。
struct BpmEstimate {
    /// @brief 估算 BPM。
    double bpm{ 0.0 };

    /// @brief 估算不确定度。
    double uncertainty{ 0.0 };

    /// @brief 估计小节拍数。
    uint32_t signature{ 1 };

    /// @brief 估计拍内细分数。
    uint32_t division{ 1 };
};

/// @brief 获取 FFTW planner 互斥锁。
/// @return 保护 FFTW 全局 planner 状态的互斥锁。
std::mutex& fftwPlanMutex()
{
    static std::mutex mutex;
    return mutex;
}

/// @brief 四舍五入到 32 位整数。
/// @param value 输入浮点值。
/// @return 四舍五入后的整数。
int32_t roundToInt(double value)
{
    return static_cast<int32_t>(std::lrint(value));
}

/// @brief 返回大于等于输入值的 2 次幂。
/// @param value 输入值。
/// @return 成功时返回 2 次幂，过大时返回 0。
size_t nextPowerOfTwo(size_t value)
{
    if ( value == 0 || value > FFT_MAX_N ) {
        return 0;
    }

    size_t result = 1;
    while ( result < value ) {
        if ( result > FFT_MAX_N / 2 ) {
            return 0;
        }
        result <<= 1;
    }
    return result;
}

/// @brief 二次函数插值找极值点对应的横坐标。
/// @param x0 中心点横坐标。
/// @param qx 采样间隔。
/// @param ym1 左侧采样值。
/// @param y0 中心采样值。
/// @param y1 右侧采样值。
/// @return 插值后的极值横坐标。
double peak(double x0, double qx, double ym1, double y0, double y1)
{
    const double denom = (y0 - y1) + (y0 - ym1);
    if ( std::abs(denom) <= std::numeric_limits<double>::epsilon() ) {
        return x0;
    }
    return x0 + 0.5 * (y1 - ym1) / denom * qx;
}

/// @brief 使用二阶节 IIR 滤波器处理序列。
/// @param sections 二阶节数量。
/// @param coeff 滤波器系数。
/// @param samples 待处理序列。
void filterSos(unsigned            sections, const double (*coeff)[5],
               std::vector<float>& samples)
{
    const size_t len = samples.size();
    for ( unsigned section = 0; section < sections; ++section ) {
        const float b1  = static_cast<float>(coeff[section][0]);
        const float b2  = static_cast<float>(coeff[section][1]);
        const float ma1 = static_cast<float>(-coeff[section][2]);
        const float ma2 = static_cast<float>(-coeff[section][3]);
        const float g   = static_cast<float>(coeff[section][4]);
        float       s1  = 0.0f;
        float       s2  = 0.0f;

        for ( size_t i = 0; i < len; ++i ) {
            const float s0 = samples[i] * g + s1 * ma1 + s2 * ma2;
            samples[i]     = s0 + s1 * b1 + s2 * b2;
            s2             = s1;
            s1             = s0;
        }
    }
}

/// @brief 近似计算非负浮点序列中位数。
/// @param samples 输入序列。
/// @param bin 精度分桶参数。
/// @return 近似中位数。
float medianApprox(const std::vector<float>& samples, int bin)
{
    if ( samples.empty() || bin < -22 || bin > 8 ) {
        return 0.0f;
    }

    const int           roundBits   = 23 + bin;
    const uint32_t      roundOffset = UINT32_C(1) << (roundBits - 1);
    std::vector<size_t> count(static_cast<size_t>(1 << (8 - bin)) + 1, 0);

    for ( float value : samples ) {
        uint32_t bits{ 0 };
        std::memcpy(&bits, &value, sizeof(float));
        bits &= (bits >> 31) - 1U;
        ++count[(bits + roundOffset) >> roundBits];
    }

    size_t cumulative = 0;
    for ( uint32_t i = 0; i < count.size(); ++i ) {
        cumulative += count[i];
        if ( cumulative >= samples.size() / 2 ) {
            uint32_t bits = i << roundBits;
            float    result{ 0.0f };
            std::memcpy(&result, &bits, sizeof(float));
            return result;
        }
    }

    return 0.0f;
}

/// @brief 非线性压缩并按延迟累加到目标特征序列。
/// @param source 输入能量序列。
/// @param target 输出特征序列。
/// @param mul 输入放大倍率。
/// @param weight 频段权重。
/// @param delay 延迟补偿，单位为采样点。
void compressApprox(const std::vector<float>& source,
                    std::vector<float>& target, float mul, float weight,
                    int delay)
{
    const size_t absDelay =
        static_cast<size_t>(std::abs(static_cast<long>(delay)));
    if ( source.size() <= absDelay || target.empty() ) {
        return;
    }

    const size_t sourceOffset = delay < 0 ? absDelay : 0;
    const size_t targetOffset = delay > 0 ? absDelay : 0;
    if ( targetOffset >= target.size() ) {
        return;
    }
    const size_t len =
        std::min(source.size() - absDelay, target.size() - targetOffset);
    const float wln2 = weight * 0.69314718056f;

    for ( size_t i = 0; i < len; ++i ) {
        const float mxp1 = source[sourceOffset + i] * mul + 1.0f;
        int         expo = 0;
        const float mag  = std::frexp(std::max(mxp1, 1e-12f), &expo);
        target[targetOffset + i] +=
            (static_cast<float>(expo - 2) + mag * 2.0f) * wln2;
    }
}

/// @brief 使用临近点采样重采样序列。
/// @param source 输入序列。
/// @param rate 输出长度相对输入长度的倍率。
/// @return 重采样后的序列。
std::vector<float> resampleNearest(const std::vector<float>& source,
                                   double                    rate)
{
    if ( source.empty() || rate <= 0.0 ) {
        return {};
    }

    std::vector<float> result(
        static_cast<size_t>(std::ceil(source.size() * rate)), 0.0f);
    for ( size_t i = 0; i < result.size(); ++i ) {
        const size_t sourceIndex = static_cast<size_t>(roundToInt(i / rate));
        if ( sourceIndex < source.size() ) {
            result[i] = source[sourceIndex];
        }
    }
    return result;
}

/// @brief 使用 FFTW 计算自相关的前若干个偏移。
/// @param source 输入特征序列。
/// @param length 需要输出的偏移数量。
/// @return 自相关结果。
std::vector<float> autocorr(const std::vector<float>& source, size_t length)
{
    if ( source.empty() || length == 0 ) {
        return {};
    }

    const size_t fftSize = nextPowerOfTwo(source.size() + length - 1);
    if ( fftSize == 0 ||
         fftSize > static_cast<size_t>(std::numeric_limits<int>::max()) ) {
        return {};
    }

    std::vector<double> input(fftSize, 0.0);
    std::vector<double> spectrum((fftSize / 2 + 1) * 2, 0.0);
    std::vector<double> inverse(fftSize, 0.0);
    for ( size_t i = 0; i < source.size(); ++i ) {
        input[i] = source[i];
    }

    auto* complexSpectrum = reinterpret_cast<fftw_complex*>(spectrum.data());
    fftw_plan forwardPlan = nullptr;
    fftw_plan inversePlan = nullptr;
    {
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        forwardPlan = fftw_plan_dft_r2c_1d(static_cast<int>(fftSize),
                                           input.data(),
                                           complexSpectrum,
                                           FFTW_ESTIMATE);
        inversePlan = fftw_plan_dft_c2r_1d(static_cast<int>(fftSize),
                                           complexSpectrum,
                                           inverse.data(),
                                           FFTW_ESTIMATE);
    }

    if ( !forwardPlan || !inversePlan ) {
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        if ( forwardPlan ) {
            fftw_destroy_plan(forwardPlan);
        }
        if ( inversePlan ) {
            fftw_destroy_plan(inversePlan);
        }
        return {};
    }

    fftw_execute(forwardPlan);
    for ( size_t i = 0; i <= fftSize / 2; ++i ) {
        const double real   = spectrum[i * 2];
        const double imag   = spectrum[i * 2 + 1];
        spectrum[i * 2]     = real * real + imag * imag;
        spectrum[i * 2 + 1] = 0.0;
    }
    fftw_execute(inversePlan);

    {
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        fftw_destroy_plan(forwardPlan);
        fftw_destroy_plan(inversePlan);
    }

    std::vector<float> result(length, 0.0f);
    const double       scale = 1.0 / static_cast<double>(fftSize);
    for ( size_t i = 0; i < length; ++i ) {
        result[i] = static_cast<float>(inverse[i] * scale);
    }
    return result;
}

/// @brief 根据采样率选择 Emiria AutoTiming 使用的滤波器表。
/// @param sampleRate 采样率。
/// @return 成功时返回滤波器系数表，否则返回空指针。
const double (*coefficientsForSampleRate(uint32_t sampleRate))[5]
{
    switch ( sampleRate ) {
    case 32000: return FILTER_COEFF_SOS_32;
    case 44100: return FILTER_COEFF_SOS_44;
    case 48000: return FILTER_COEFF_SOS_48;
    default: return nullptr;
    }
}

/// @brief 提取用于 BPM 和 offset 估计的一维节拍特征。
/// @param audioData 单声道音频采样。
/// @param sampleRate 采样率。
/// @return 1kHz 特征序列。
std::vector<float> preprocess(const std::vector<float>& audioData,
                              uint32_t                  sampleRate)
{
    const double (*filterCoeffSos)[5] = coefficientsForSampleRate(sampleRate);
    if ( !filterCoeffSos || audioData.empty() ) {
        return {};
    }

    const size_t       len = audioData.size();
    std::vector<float> combined(len, 0.0f);
    for ( size_t band = 0; band < SUBBAND_COUNT; ++band ) {
        std::vector<float> bandData(audioData);
        filterSos(
            FILTERS[band][1], &filterCoeffSos[FILTERS[band][0]], bandData);

        for ( float& value : bandData ) {
            value *= value;
        }

        filterSos(FILTERS[SUBBAND_COUNT + band][1],
                  &filterCoeffSos[FILTERS[SUBBAND_COUNT + band][0]],
                  bandData);

        const float median = std::max(medianApprox(bandData, -4), 1e-12f);
        compressApprox(bandData,
                       combined,
                       2.0f / median,
                       SUBBAND_WEIGHTS[band],
                       SUBBAND_FILTER_DELAYS[band]);
    }

    filterSos(FILTERS[SUBBAND_COUNT * 2][1],
              &filterCoeffSos[FILTERS[SUBBAND_COUNT * 2][0]],
              combined);

    std::vector<float> feature =
        resampleNearest(combined, 1000.0 / static_cast<double>(sampleRate));
    const size_t featureLen = feature.size();
    if ( featureLen < 2 ) {
        return {};
    }

    std::vector<float> reversed(featureLen);
    std::copy(feature.rbegin(), feature.rend(), reversed.begin());
    filterSos(FILTERS[SUBBAND_COUNT * 2 + 1][1],
              &filterCoeffSos[FILTERS[SUBBAND_COUNT * 2 + 1][0]],
              feature);
    filterSos(FILTERS[SUBBAND_COUNT * 2 + 1][1],
              &filterCoeffSos[FILTERS[SUBBAND_COUNT * 2 + 1][0]],
              reversed);

    feature[featureLen - 1] = -feature[featureLen - 2];
    for ( size_t i = featureLen - 2; i > 0; --i ) {
        feature[i] = reversed[featureLen - i - 2] - feature[i - 1];
    }
    feature[0] = reversed[featureLen - 2];

    return feature;
}

/// @brief 将可靠的 BPM 估算吸附到常见精度。
/// @param bpm 原始 BPM。
/// @param uncertainty BPM 不确定度。
/// @return 吸附后的 BPM。
double snapBpm(double bpm, double uncertainty)
{
    for ( const auto& snap : BPM_SNAP ) {
        if ( std::abs(std::remainder(bpm, 1.0 / snap[0])) <
             uncertainty * snap[1] ) {
            return static_cast<double>(roundToInt(bpm * snap[0])) / snap[0];
        }
    }
    return bpm;
}

/// @brief 将检测到的峰值相位归一化到距离音频 0 点最近的等价首拍。
/// @param phaseMs 原始峰值相位，单位为毫秒。
/// @param beatMs 拍长，单位为毫秒。
/// @return 有符号首拍相位，范围约为 [-beatMs / 2, beatMs / 2]。
double normalizeNearestBeatPhase(double phaseMs, double beatMs)
{
    double normalized = std::fmod(phaseMs, beatMs);
    if ( normalized <= -beatMs * 0.5 ) {
        normalized += beatMs;
    } else if ( normalized > beatMs * 0.5 ) {
        normalized -= beatMs;
    }

    if ( std::abs(normalized) < 1e-9 ) {
        return 0.0;
    }
    return normalized;
}

/// @brief 根据特征自相关估算 BPM。
/// @param feature 1kHz 节拍特征序列。
/// @return 成功时返回估计值和可信度等级，否则返回空。
std::optional<std::pair<BpmEstimate, int>> calcBpm(
    const std::vector<float>& feature)
{
    const size_t len = feature.size();
    if ( len < 64 ) {
        return std::nullopt;
    }

    std::vector<float> correlation;
    if ( len + len / 2 > FFT_MAX_N ) {
        if ( FFT_MAX_N * 2 / 3 >= len ) {
            return std::nullopt;
        }
        const size_t       partLen = FFT_MAX_N * 2 / 3;
        std::vector<float> part(feature.cbegin(), feature.cbegin() + partLen);
        correlation = autocorr(part, part.size() / 2);
    } else {
        correlation = autocorr(feature, len / 2);
    }

    if ( correlation.size() < 64 ) {
        return std::nullopt;
    }

    const int rlen =
        static_cast<int>(std::min<size_t>(4000, correlation.size()));
    if ( rlen <= 32 ) {
        return std::nullopt;
    }

    std::vector<int> peaks;
    for ( int i = 16; i < rlen - 16; ++i ) {
        if ( correlation[i] <= 0.0f ) {
            continue;
        }

        bool isPeak = true;
        for ( int j = 1; j < 16; ++j ) {
            if ( correlation[i] < correlation[i - j] ||
                 correlation[i] < correlation[i + j] ) {
                isPeak = false;
                break;
            }
        }
        if ( isPeak ) {
            peaks.push_back(i);
        }
    }

    double                                 bestAveragePeak = 0.0;
    size_t                                 estimateIndex   = 0;
    std::vector<std::pair<double, double>> estimates;
    for ( size_t i = 0; i < peaks.size() && peaks[i] < rlen; ++i ) {
        if ( !estimates.empty() &&
             correlation[peaks[i]] <= bestAveragePeak * 0.8f ) {
            continue;
        }

        double   m           = peaks[i];
        size_t   j           = i;
        int      p           = roundToInt(m);
        uint32_t foundCount  = 0;
        double   sxx         = 0.0;
        double   sxy         = 0.0;
        uint32_t miss        = 0;
        double   averagePeak = 0.0;

        for ( double k = 1.0; p < rlen - 10 && miss <= 0; k += 1.0 ) {
            double peakEstimate = p;
            while ( j < peaks.size() && peaks[j] < p - 10 ) {
                ++j;
            }
            if ( j < peaks.size() && peaks[j] <= p + 10 ) {
                ++foundCount;
                p = peaks[j];
                averagePeak += correlation[p];
                peakEstimate = peak(p,
                                    1.0,
                                    correlation[p - 1],
                                    correlation[p],
                                    correlation[p + 1]);
                sxx += k * k;
                sxy += k * peakEstimate;
                if ( std::abs(sxx) > std::numeric_limits<double>::epsilon() ) {
                    m = sxy / sxx;
                }
            } else {
                ++miss;
            }
            p = roundToInt(peakEstimate + m);
        }

        if ( foundCount == 0 || p <= rlen / 2 ) {
            continue;
        }

        averagePeak /= foundCount;
        if ( averagePeak <= correlation[peaks[i]] * 0.70f ) {
            continue;
        }

        if ( averagePeak > bestAveragePeak ) {
            estimates.emplace_back(sxy / sxx, averagePeak);
            if ( averagePeak > bestAveragePeak * 1.25 ) {
                estimateIndex = estimates.size() - 1;
            }
            bestAveragePeak = averagePeak;
        }
    }

    if ( !(bestAveragePeak > 0.0) || estimates.empty() ||
         estimateIndex >= estimates.size() ) {
        return std::nullopt;
    }

    double beatMs = estimates[estimateIndex].first;
    while ( beatMs < 60000.0 / MAX_BPM &&
            estimateIndex + 1 < estimates.size() &&
            estimates[estimateIndex + 1].first < 60000.0 / MAX_BPM * 2.0 ) {
        bool success = false;
        for ( size_t i = estimateIndex + 1;
              i < estimates.size() &&
              estimates[i].first < 60000.0 / MAX_BPM * 2.0;
              ++i ) {
            if ( std::abs(std::remainder(estimates[i].first, beatMs)) <=
                 10.0 ) {
                estimateIndex = i;
                beatMs        = estimates[estimateIndex].first;
                success       = true;
                break;
            }
        }
        if ( !success ) {
            break;
        }
    }

    BpmEstimate estimate;
    for ( size_t i = estimateIndex + 1; i < estimates.size(); ++i ) {
        if ( std::abs(std::remainder(estimates[i].first,
                                     estimates[estimateIndex].first)) <=
             10.0 ) {
            estimate.signature = static_cast<uint32_t>(
                std::max(1,
                         roundToInt(estimates[i].first /
                                    estimates[estimateIndex].first)));
            if ( estimate.signature > 2 ) {
                break;
            }
        }
    }

    for ( size_t i = estimateIndex; i-- > 0; ) {
        if ( std::abs(std::remainder(estimates[estimateIndex].first,
                                     estimates[i].first)) <= 10.0 ) {
            estimate.division = static_cast<uint32_t>(
                std::max(1,
                         roundToInt(estimates[estimateIndex].first /
                                    estimates[i].first)));
            if ( estimate.division > 2 ) {
                break;
            }
        }
    }

    size_t   p          = static_cast<size_t>(std::max(1, roundToInt(beatMs)));
    uint32_t foundCount = 0;
    double   sxx        = 0.0;
    double   sxy        = 0.0;
    double   syy        = 0.0;
    uint32_t miss       = 0;
    uint32_t contMiss   = 0;

    for ( double k = 1.0;
          p >= 10 && p + 10 < correlation.size() && miss <= 0 && contMiss <= 0;
          k += 1.0 ) {
        const auto rangeBegin =
            correlation.begin() + static_cast<std::ptrdiff_t>(p - 10);
        const auto rangeEnd =
            correlation.begin() + static_cast<std::ptrdiff_t>(p + 11);
        const size_t maxPeak = static_cast<size_t>(
            std::max_element(rangeBegin, rangeEnd) - correlation.begin());
        double peakEstimate = static_cast<double>(p);
        if ( correlation[maxPeak] > 0.0f &&
             (maxPeak < p ? p - maxPeak : maxPeak - p) <= 8 ) {
            contMiss = 0;
            ++foundCount;
            peakEstimate = peak(maxPeak,
                                1.0,
                                correlation[maxPeak - 1],
                                correlation[maxPeak],
                                correlation[maxPeak + 1]);
            sxx += k * k;
            sxy += k * peakEstimate;
            syy += peakEstimate * peakEstimate;
            if ( std::abs(sxx) > std::numeric_limits<double>::epsilon() ) {
                beatMs = sxy / sxx;
            }
        } else {
            ++miss;
            ++contMiss;
        }

        const int nextPeak = roundToInt(peakEstimate + beatMs);
        if ( nextPeak <= 0 ) {
            break;
        }
        p = static_cast<size_t>(nextPeak);
    }

    if ( foundCount < 4 ||
         std::abs(sxy) <= std::numeric_limits<double>::epsilon() ) {
        return std::nullopt;
    }

    estimate.bpm = 60000.0 * sxx / sxy;
    const double variance =
        (syy - sxy * sxy / sxx) / static_cast<double>(foundCount - 1);
    if ( variance < 0.0 || !std::isfinite(variance) ) {
        return std::nullopt;
    }

    const double sigma           = std::sqrt(variance);
    const double uncertaintyTerm = (sxx * syy / sxy / sxy - 1.0) /
                                   static_cast<double>(foundCount - 1) *
                                   foundCount;
    if ( uncertaintyTerm < 0.0 || !std::isfinite(uncertaintyTerm) ) {
        return std::nullopt;
    }
    estimate.uncertainty = std::sqrt(uncertaintyTerm) * estimate.bpm;

    int quality = 0;
    if ( sigma > 2.4 || estimate.uncertainty / estimate.bpm > 0.00005 ) {
        quality = 16;
    } else if ( miss > 0 || sigma > 0.6 ) {
        quality = 1;
    }

    return std::make_pair(estimate, quality);
}

/// @brief 估算首拍相对 0 点的偏移。
/// @param feature 1kHz 节拍特征序列。
/// @param bpm 已估计 BPM。
/// @return 成功时返回偏移，单位为毫秒。
std::optional<double> calcOffset(const std::vector<float>& feature, double bpm)
{
    if ( !(bpm > 0.0) || feature.empty() ) {
        return std::nullopt;
    }

    const double beatMs = 60000.0 / bpm;
    const size_t foldedLen =
        static_cast<size_t>(std::ceil(beatMs)) + static_cast<size_t>(10);
    if ( foldedLen <= 10 ) {
        return std::nullopt;
    }

    std::vector<float> folded(foldedLen, 0.0f);
    const size_t       loopCount =
        static_cast<size_t>(std::ceil(feature.size() / beatMs));
    for ( size_t i = 0; i < loopCount; ++i ) {
        const int roundedStart = roundToInt(beatMs * static_cast<double>(i));
        if ( roundedStart < 0 ) {
            continue;
        }
        const size_t start = static_cast<size_t>(roundedStart);
        if ( start >= feature.size() ) {
            break;
        }

        const size_t copyLen = std::min(foldedLen, feature.size() - start);
        for ( size_t j = 0; j < copyLen; ++j ) {
            folded[j] += feature[start + j];
        }
    }

    if ( folded.size() <= 10 ) {
        return std::nullopt;
    }

    const auto   maxIt = std::max_element(folded.begin() + 5, folded.end() - 5);
    const size_t maxPeak = static_cast<size_t>(maxIt - folded.begin());
    double       offset  = peak(maxPeak,
                         1.0,
                         folded[maxPeak - 1],
                         folded[maxPeak],
                         folded[maxPeak + 1]);
    offset -= FILTER_DELAY_MS;
    return offset;
}

/// @brief 估算节奏峰值相对最终网格的实用不准确度。
/// @param feature 1kHz 节拍特征序列。
/// @param bpm 最终 BPM。
/// @param offsetMs 最终首拍相位，单位为毫秒。
/// @param division 拍内细分数。
/// @return 加权 RMS 不准确度，单位为毫秒。
double calcAlignmentInaccuracy(const std::vector<float>& feature, double bpm,
                               double offsetMs, uint32_t division)
{
    if ( feature.size() < 3 || !(bpm > 0.0) || !std::isfinite(bpm) ||
         !std::isfinite(offsetMs) ) {
        return 0.0;
    }

    const double beatMs = 60000.0 / bpm;
    if ( !(beatMs > 0.0) || !std::isfinite(beatMs) ) {
        return 0.0;
    }

    const uint32_t safeDivision = std::clamp<uint32_t>(division, 1U, 16U);
    const double   gridMs       = beatMs / static_cast<double>(safeDivision);
    if ( !(gridMs > 1.0) || !std::isfinite(gridMs) ) {
        return 0.0;
    }

    double positiveMax   = 0.0;
    double positiveSum   = 0.0;
    size_t positiveCount = 0;
    for ( float value : feature ) {
        if ( value > 0.0f && std::isfinite(value) ) {
            const double sample = static_cast<double>(value);
            positiveMax         = std::max(positiveMax, sample);
            positiveSum += sample;
            ++positiveCount;
        }
    }

    if ( positiveCount == 0 || !(positiveMax > 0.0) ) {
        return 0.0;
    }

    const double positiveMean =
        positiveSum / static_cast<double>(positiveCount);
    const double threshold = std::min(
        std::max(positiveMax * 0.20, positiveMean * 1.50), positiveMax * 0.95);
    const size_t minPeakDistance =
        static_cast<size_t>(std::max(1.0, std::min(80.0, gridMs * 0.25)));

    double weightedSquareSum = 0.0;
    double weightSum         = 0.0;
    size_t peakCount         = 0;
    size_t lastPeakIndex     = 0;
    bool   hasLastPeak       = false;
    for ( size_t i = 1; i + 1 < feature.size(); ++i ) {
        const float value = feature[i];
        if ( !(value > threshold) || !std::isfinite(value) ||
             value < feature[i - 1] || value < feature[i + 1] ) {
            continue;
        }

        if ( hasLastPeak && i - lastPeakIndex < minPeakDistance ) {
            continue;
        }

        const double residual =
            std::abs(std::remainder(static_cast<double>(i) - offsetMs, gridMs));
        const double weight = static_cast<double>(value) - threshold;
        if ( !(weight > 0.0) || !std::isfinite(residual) ) {
            continue;
        }

        weightedSquareSum += residual * residual * weight;
        weightSum += weight;
        ++peakCount;
        lastPeakIndex = i;
        hasLastPeak   = true;
    }

    if ( peakCount < 4 || !(weightSum > 0.0) ) {
        return 0.0;
    }

    const double rms = std::sqrt(weightedSquareSum / weightSum);
    if ( !std::isfinite(rms) ) {
        return 0.0;
    }
    return std::min(rms, gridMs * 0.5);
}

}  // namespace

/// @brief 执行自动 BPM/offset 检测。
/// @param monoSamples 单声道浮点音频采样。
/// @param sampleRate 采样率。
/// @return 成功时返回检测结果，否则返回空。
/// @warning 后台耗时路径：会完整预处理音频并执行自相关 FFT，禁止在
/// UI、渲染或逻辑热路径中调用。
std::optional<BpmAutoTimingResult> BpmAutoDetector::detect(
    const std::vector<float>& monoSamples, uint32_t sampleRate)
{
    if ( sampleRate == 0 ||
         monoSamples.size() <
             static_cast<size_t>(MIN_DETECT_SECONDS * sampleRate) ) {
        return std::nullopt;
    }

    std::vector<float> feature = preprocess(monoSamples, sampleRate);
    if ( feature.empty() ) {
        return std::nullopt;
    }

    auto bpmEstimate = calcBpm(feature);
    if ( !bpmEstimate ) {
        return std::nullopt;
    }

    BpmAutoTimingResult result;
    result.rawBpm            = bpmEstimate->first.bpm;
    result.rawBpmUncertainty = bpmEstimate->first.uncertainty;
    result.signature         = bpmEstimate->first.signature;
    result.division          = bpmEstimate->first.division;
    result.bpm               = bpmEstimate->second < 16
                                   ? snapBpm(result.rawBpm, result.rawBpmUncertainty)
                                   : result.rawBpm;

    auto rawPhase = calcOffset(feature, result.bpm);
    if ( !rawPhase ) {
        return std::nullopt;
    }

    const double beatMs          = 60000.0 / result.bpm;
    result.offsetMs              = normalizeNearestBeatPhase(*rawPhase, beatMs);
    result.alignmentInaccuracyMs = calcAlignmentInaccuracy(
        feature, result.bpm, result.offsetMs, result.division);

    if ( !std::isfinite(result.bpm) || result.bpm <= 0.0 ||
         !std::isfinite(result.offsetMs) ||
         !std::isfinite(result.alignmentInaccuracyMs) ) {
        return std::nullopt;
    }
    return result;
}

}  // namespace MMM::UI
