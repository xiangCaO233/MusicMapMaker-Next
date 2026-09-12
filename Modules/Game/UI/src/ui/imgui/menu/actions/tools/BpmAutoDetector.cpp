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
/// @note 过短片段无法提供足够周期用于可靠自相关回归。
constexpr double MIN_DETECT_SECONDS = 10.0;

/// @brief 特征自相关允许的最大 FFT 点数。
/// @note 上限同时约束内存占用和 FFTW 计划规模。
constexpr size_t FFT_MAX_N = size_t{ 1 } << 20;

/// @brief 边缘检测滤波器延迟补偿，单位为毫秒。
/// @note offset 估计结束后减去该固定群延迟。
constexpr double FILTER_DELAY_MS = 1.2;

/// @brief 自动检测允许吸附前的最高 BPM。
/// @note 更短周期会尝试选择其可靠倍频候选，避免过高 BPM。
constexpr double MAX_BPM = 220.0;

/// @brief 预处理使用的子频段数量。
/// @note 与 SUBBAND_WEIGHTS 和 SUBBAND_FILTER_DELAYS 长度保持一致。
constexpr size_t SUBBAND_COUNT = 4;

/// @brief 各频段混合权重。
/// @details 低频与高频瞬态权重较高，中间两个频段权重较低。
/// @note 权重总和为 1，便于不同频段压缩后保持统一量级。
constexpr std::array<float, SUBBAND_COUNT> SUBBAND_WEIGHTS{
    0.3f, 0.2f, 0.2f, 0.3f
};

/// @brief 子频段和后续滤波器索引配置。
/// @details 前四项定义频段滤波链，中间四项预留后级，末三项用于组合特征处理。
/// @note 每项依次存储 FILTER_COEFF_SOS 表中的起始节索引与节数量。
constexpr unsigned FILTERS[][2] = {
    { 0, 2 }, { 2, 4 }, { 6, 4 },  { 10, 2 }, { 12, 1 }, { 0, 0 },
    { 0, 0 }, { 0, 0 }, { 13, 2 }, { 15, 1 }, { 16, 1 },
};

/// @brief 各频段滤波器的延迟补偿，单位为采样点。
/// @note 负值从源序列前部跳过，正值从目标序列前部留空。
constexpr std::array<int, SUBBAND_COUNT> SUBBAND_FILTER_DELAYS{
    -320, -64, -32, 0
};

/// @brief 44.1kHz 下的二阶节滤波系数。
/// @details 每行保存 b1、b2、负反馈 a1、a2 与输入增益 g。
/// @note 系数只适用于精确 44100 Hz，其他采样率不得近似复用。
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
/// @details 拓扑与 44.1kHz 表一致，但极点和增益按 48kHz 重新设计。
/// @note coefficientsForSampleRate 负责选择，调用点不直接索引采样率表。
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
/// @details 支持常见低采样率解码结果，保持与其余表相同节索引布局。
/// @note FILTERS 中的最大索引必须始终小于本表行数。
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
/// @details 从整数到高精度依次尝试；只有估值距离网格小于不确定度倍数才吸附。
/// @note 顺序体现优先选择易读 BPM 的策略，不应按数值重新排序。
constexpr double BPM_SNAP[][2] = {
    { 1.0, 3.0 },  { 2.0, 2.5 },   { 3.0, 2.0 },   { 10.0, 2.0 },
    { 20.0, 1.5 }, { 100.0, 1.5 }, { 200.0, 1.0 },
};

/// @brief BPM 线性回归中间结果。
/// @details 同时保存原始速度、不确定度及从倍频候选推断的拍号和细分。
struct BpmEstimate {
    /// @brief 估算 BPM。
    /// @note 吸附发生在最终公开结果构造阶段，不修改该原始值。
    double bpm{ 0.0 };

    /// @brief 估算不确定度。
    /// @note 由回归误差传播得到，与 bpm 使用相同单位。
    double uncertainty{ 0.0 };

    /// @brief 估计小节拍数。
    /// @note 由更长可靠周期相对主拍周期的整数倍推断。
    uint32_t signature{ 1 };

    /// @brief 估计拍内细分数。
    /// @note 由更短可靠周期相对主拍周期的整数倍推断。
    uint32_t division{ 1 };
};

/// @brief 获取 FFTW planner 互斥锁。
/// @return 保护 FFTW 全局 planner 状态的互斥锁。
/// @note FFTW 计划创建和销毁不是线程安全操作，执行已创建计划无需此锁。
std::mutex& fftwPlanMutex()
{
    // 函数静态对象避免跨翻译单元初始化顺序问题。
    static std::mutex mutex;
    return mutex;
}

/// @brief 四舍五入到 32 位整数。
/// @param value 输入浮点值。
/// @return 四舍五入后的整数。
/// @note lrint 遵循当前舍入模式，结果只用于受限采样索引和周期估算。
int32_t roundToInt(double value)
{
    return static_cast<int32_t>(std::lrint(value));
}

/// @brief 返回大于等于输入值的 2 次幂。
/// @param value 输入值。
/// @return 成功时返回 2 次幂，过大时返回 0。
/// @note 返回 0 同时表达输入无效和超过 FFT_MAX_N。
size_t nextPowerOfTwo(size_t value)
{
    // 零长度和已超上限值不能构造 FFT。
    if ( value == 0 || value > FFT_MAX_N ) {
        return 0;
    }

    // 从最小 2 次幂开始逐次左移，避免浮点对数精度问题。
    size_t result = 1;
    while ( result < value ) {
        if ( result > FFT_MAX_N / 2 ) {
            // 下一次左移将超过上限，提前返回失败并避免溢出。
            return 0;
        }
        result <<= 1;
    }
    // result 是首个不小于 value 的 2 次幂。
    return result;
}

/// @brief 二次函数插值找极值点对应的横坐标。
/// @param x0 中心点横坐标。
/// @param qx 采样间隔。
/// @param ym1 左侧采样值。
/// @param y0 中心采样值。
/// @param y1 右侧采样值。
/// @return 插值后的极值横坐标。
/// @note 使用三个等距样点拟合抛物线，提高整数采样峰值的亚采样精度。
double peak(double x0, double qx, double ym1, double y0, double y1)
{
    // 分母描述中心相对两侧的总曲率。
    const double denom = (y0 - y1) + (y0 - ym1);
    if ( std::abs(denom) <= std::numeric_limits<double>::epsilon() ) {
        // 平坦或退化样点无法稳定插值，回退中心坐标。
        return x0;
    }
    // 极值偏移限制由局部样点形状自然决定，并按采样间隔缩放。
    return x0 + 0.5 * (y1 - ym1) / denom * qx;
}

/// @brief 使用二阶节 IIR 滤波器处理序列。
/// @param sections 二阶节数量。
/// @param coeff 滤波器系数。
/// @param samples 待处理序列。
/// @warning 后台计算路径：原地遍历完整序列，每个二阶节依次串联。
/// @note 系数布局必须与预先设计的采样率表一致。
void filterSos(unsigned            sections, const double (*coeff)[5],
               std::vector<float>& samples)
{
    // 所有节处理相同固定长度，函数不改变容器大小。
    const size_t len = samples.size();
    for ( unsigned section = 0; section < sections; ++section ) {
        // 每节系数转换为 float，与输入特征精度一致。
        const float b1  = static_cast<float>(coeff[section][0]);
        const float b2  = static_cast<float>(coeff[section][1]);
        const float ma1 = static_cast<float>(-coeff[section][2]);
        const float ma2 = static_cast<float>(-coeff[section][3]);
        const float g   = static_cast<float>(coeff[section][4]);
        float       s1  = 0.0f;
        float       s2  = 0.0f;

        // Direct Form II 状态只保留前两个内部样点。
        for ( size_t i = 0; i < len; ++i ) {
            // 先计算反馈后的当前内部状态。
            const float s0 = samples[i] * g + s1 * ma1 + s2 * ma2;
            // 前向系数组合产生本节输出，并直接覆盖输入序列。
            samples[i] = s0 + s1 * b1 + s2 * b2;
            // 状态按时间推进，供下一采样使用。
            s2 = s1;
            s1 = s0;
        }
    }
}

/// @brief 近似计算非负浮点序列中位数。
/// @param samples 输入序列。
/// @param bin 精度分桶参数。
/// @return 近似中位数。
/// @warning 后台计算路径：分配直方图并完整遍历输入序列。
/// @note 仅针对非负 IEEE-754 float 的位序分桶有效。
float medianApprox(const std::vector<float>& samples, int bin)
{
    // bin 范围保证位移数量合法且直方图规模受控。
    if ( samples.empty() || bin < -22 || bin > 8 ) {
        return 0.0f;
    }

    // roundBits 决定保留的指数与高位尾数精度。
    const int roundBits = 23 + bin;
    // 加半桶偏移实现近似四舍五入而非截断。
    const uint32_t roundOffset = UINT32_C(1) << (roundBits - 1);
    // 桶数量覆盖所有非负 float 高位组合并附加末端槽位。
    std::vector<size_t> count(static_cast<size_t>(1 << (8 - bin)) + 1, 0);

    for ( float value : samples ) {
        uint32_t bits{ 0 };
        // memcpy 在不违反严格别名规则的前提下读取浮点位型。
        std::memcpy(&bits, &value, sizeof(float));
        // 负值通过符号掩码折叠为零，算法只关注非负能量。
        bits &= (bits >> 31) - 1U;
        // 根据舍入后的高位索引累加直方图。
        ++count[(bits + roundOffset) >> roundBits];
    }

    // 前缀计数首次越过半数的位置即近似中位桶。
    size_t cumulative = 0;
    for ( uint32_t i = 0; i < count.size(); ++i ) {
        cumulative += count[i];
        if ( cumulative >= samples.size() / 2 ) {
            // 将桶索引恢复为代表性 float 位型。
            uint32_t bits = i << roundBits;
            float    result{ 0.0f };
            std::memcpy(&result, &bits, sizeof(float));
            // 立即返回避免继续扫描其余桶。
            return result;
        }
    }

    // 理论上总计数应覆盖输入，此分支防御位型或索引异常。
    return 0.0f;
}

/// @brief 非线性压缩并按延迟累加到目标特征序列。
/// @param source 输入能量序列。
/// @param target 输出特征序列。
/// @param mul 输入放大倍率。
/// @param weight 频段权重。
/// @param delay 延迟补偿，单位为采样点。
/// @warning 后台计算路径：对有效重叠范围执行逐样本累加。
/// @note 压缩近似 weight*ln(1+mul*x)，降低强瞬态对组合特征的支配。
void compressApprox(const std::vector<float>& source,
                    std::vector<float>& target, float mul, float weight,
                    int delay)
{
    // 延迟绝对值决定源与目标可重叠的最大样本范围。
    const size_t absDelay =
        static_cast<size_t>(std::abs(static_cast<long>(delay)));
    if ( source.size() <= absDelay || target.empty() ) {
        // 无有效重叠时保持目标不变。
        return;
    }

    // 负延迟跳过源前部，正延迟推迟目标写入起点。
    const size_t sourceOffset = delay < 0 ? absDelay : 0;
    const size_t targetOffset = delay > 0 ? absDelay : 0;
    if ( targetOffset >= target.size() ) {
        // 正延迟超过目标长度时没有可写样本。
        return;
    }
    // 长度同时受源剩余和目标剩余容量限制。
    const size_t len =
        std::min(source.size() - absDelay, target.size() - targetOffset);
    // 0.693... 是 ln(2)，用于把 frexp 分解换算为自然对数近似。
    const float wln2 = weight * 0.69314718056f;

    for ( size_t i = 0; i < len; ++i ) {
        // 加一保证零能量映射为零，并用 mul 进行中位数归一化。
        const float mxp1 = source[sourceOffset + i] * mul + 1.0f;
        int         expo = 0;
        const float mag  = std::frexp(std::max(mxp1, 1e-12f), &expo);
        // 指数与尾数线性近似 log2，再乘 ln2 和频段权重。
        target[targetOffset + i] +=
            (static_cast<float>(expo - 2) + mag * 2.0f) * wln2;
    }
}

/// @brief 使用临近点采样重采样序列。
/// @param source 输入序列。
/// @param rate 输出长度相对输入长度的倍率。
/// @return 重采样后的序列。
/// @warning 后台计算路径：按输出长度分配并逐样本映射最近输入索引。
/// @note 该算法用于降采样节拍包络，不承担高保真音频重采样。
std::vector<float> resampleNearest(const std::vector<float>& source,
                                   double                    rate)
{
    // 空输入或非正倍率没有可定义输出。
    if ( source.empty() || rate <= 0.0 ) {
        return {};
    }

    // ceil 保证覆盖输入末端对应的输出区间。
    std::vector<float> result(
        static_cast<size_t>(std::ceil(source.size() * rate)), 0.0f);
    for ( size_t i = 0; i < result.size(); ++i ) {
        // 输出位置反算到输入坐标并取最近整数样本。
        const size_t sourceIndex = static_cast<size_t>(roundToInt(i / rate));
        if ( sourceIndex < source.size() ) {
            // 末端舍入越界时保持初始化零值。
            result[i] = source[sourceIndex];
        }
    }
    return result;
}

/// @brief 使用 FFTW 计算自相关的前若干个偏移。
/// @param source 输入特征序列。
/// @param length 需要输出的偏移数量。
/// @return 自相关结果。
/// @warning 后台耗时路径：分配三个 FFT 缓冲并创建、执行 FFTW 计划。
/// @note 使用零填充避免循环相关污染所需的前 length 个偏移。
std::vector<float> autocorr(const std::vector<float>& source, size_t length)
{
    // 输入或输出长度为零时不创建 FFTW 资源。
    if ( source.empty() || length == 0 ) {
        return {};
    }

    // 线性自相关所需卷积长度为 source.size()+length-1。
    const size_t fftSize = nextPowerOfTwo(source.size() + length - 1);
    if ( fftSize == 0 ||
         fftSize > static_cast<size_t>(std::numeric_limits<int>::max()) ) {
        // FFTW 一维接口接收 int 长度，同时受项目内存上限约束。
        return {};
    }

    // 输入尾部零填充；频谱数组用双 double 存储复数实部和虚部。
    std::vector<double> input(fftSize, 0.0);
    std::vector<double> spectrum((fftSize / 2 + 1) * 2, 0.0);
    std::vector<double> inverse(fftSize, 0.0);
    for ( size_t i = 0; i < source.size(); ++i ) {
        // 将 float 特征提升到 FFTW double 精度输入。
        input[i] = source[i];
    }

    // spectrum 连续布局与 fftw_complex 的双 double ABI 对齐。
    auto* complexSpectrum = reinterpret_cast<fftw_complex*>(spectrum.data());
    fftw_plan forwardPlan = nullptr;
    fftw_plan inversePlan = nullptr;
    {
        // FFTW planner 使用全局状态，创建两个计划期间必须串行化。
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
        // 任一计划失败都销毁已成功创建的另一计划。
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        if ( forwardPlan ) {
            fftw_destroy_plan(forwardPlan);
        }
        if ( inversePlan ) {
            fftw_destroy_plan(inversePlan);
        }
        return {};
    }

    // 正向实数 FFT 将时域特征转换为半谱复数序列。
    fftw_execute(forwardPlan);
    for ( size_t i = 0; i <= fftSize / 2; ++i ) {
        // 自相关频域形式为频谱乘以自身共轭，即幅度平方。
        const double real   = spectrum[i * 2];
        const double imag   = spectrum[i * 2 + 1];
        spectrum[i * 2]     = real * real + imag * imag;
        spectrum[i * 2 + 1] = 0.0;
    }
    // 逆变换得到未归一化的线性自相关序列。
    fftw_execute(inversePlan);

    {
        // FFTW 计划销毁同样受全局 planner 锁保护。
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        fftw_destroy_plan(forwardPlan);
        fftw_destroy_plan(inversePlan);
    }

    // 只复制调用方需要的前 length 个偏移，并应用 FFT 尺度归一化。
    std::vector<float> result(length, 0.0f);
    const double       scale = 1.0 / static_cast<double>(fftSize);
    for ( size_t i = 0; i < length; ++i ) {
        result[i] = static_cast<float>(inverse[i] * scale);
    }
    // 返回值不依赖 FFTW 缓冲或计划生命周期。
    return result;
}

/// @brief 根据采样率选择 Emiria AutoTiming 使用的滤波器表。
/// @param sampleRate 采样率。
/// @return 成功时返回滤波器系数表，否则返回空指针。
/// @note 只支持系数经过验证的 32k、44.1k 与 48kHz 输入。
const double (*coefficientsForSampleRate(uint32_t sampleRate))[5]
{
    // 精确匹配采样率，禁止对未知速率近似选取相邻表。
    switch ( sampleRate ) {
    case 32000: return FILTER_COEFF_SOS_32;
    case 44100: return FILTER_COEFF_SOS_44;
    case 48000: return FILTER_COEFF_SOS_48;
    // 调用方以空特征报告不支持采样率。
    default: return nullptr;
    }
}

/// @brief 提取用于 BPM 和 offset 估计的一维节拍特征。
/// @param audioData 单声道音频采样。
/// @param sampleRate 采样率。
/// @return 1kHz 特征序列。
/// @warning 后台耗时路径：为每个频段复制完整音频并多次执行 IIR 滤波。
/// @note 输出每个索引约对应一毫秒，供 BPM 与 offset 算法直接使用。
std::vector<float> preprocess(const std::vector<float>& audioData,
                              uint32_t                  sampleRate)
{
    // 系数表必须与解码采样率精确匹配。
    const double (*filterCoeffSos)[5] = coefficientsForSampleRate(sampleRate);
    if ( !filterCoeffSos || audioData.empty() ) {
        // 不支持采样率或空音频不能生成可靠特征。
        return {};
    }

    // combined 与原音频等长，累计四个延迟对齐后的频段特征。
    const size_t       len = audioData.size();
    std::vector<float> combined(len, 0.0f);
    for ( size_t band = 0; band < SUBBAND_COUNT; ++band ) {
        // 每个频段独立复制原始音频，避免前一滤波链污染后一频段。
        std::vector<float> bandData(audioData);
        // FILTERS 前四项选择各频段带通或低高通二阶节。
        filterSos(
            FILTERS[band][1], &filterCoeffSos[FILTERS[band][0]], bandData);

        // 平方将带通信号转换为瞬时能量包络。
        for ( float& value : bandData ) {
            value *= value;
        }

        // 后级滤波平滑或强调该频段的能量变化。
        filterSos(FILTERS[SUBBAND_COUNT + band][1],
                  &filterCoeffSos[FILTERS[SUBBAND_COUNT + band][0]],
                  bandData);

        // 中位能量建立对异常峰值稳健的归一化尺度。
        const float median = std::max(medianApprox(bandData, -4), 1e-12f);
        // 非线性压缩后按频段权重和群延迟累加到公共序列。
        compressApprox(bandData,
                       combined,
                       2.0f / median,
                       SUBBAND_WEIGHTS[band],
                       SUBBAND_FILTER_DELAYS[band]);
    }

    // 组合频段完成后执行统一包络后处理滤波。
    filterSos(FILTERS[SUBBAND_COUNT * 2][1],
              &filterCoeffSos[FILTERS[SUBBAND_COUNT * 2][0]],
              combined);

    // 降采样到 1kHz，使时间索引可直接解释为毫秒。
    std::vector<float> feature =
        resampleNearest(combined, 1000.0 / static_cast<double>(sampleRate));
    const size_t featureLen = feature.size();
    if ( featureLen < 2 ) {
        // 双向滤波和差分至少需要两个采样。
        return {};
    }

    // 反转副本用于反向滤波，组合后抵消单向相位偏移。
    std::vector<float> reversed(featureLen);
    std::copy(feature.rbegin(), feature.rend(), reversed.begin());
    filterSos(FILTERS[SUBBAND_COUNT * 2 + 1][1],
              &filterCoeffSos[FILTERS[SUBBAND_COUNT * 2 + 1][0]],
              feature);
    filterSos(FILTERS[SUBBAND_COUNT * 2 + 1][1],
              &filterCoeffSos[FILTERS[SUBBAND_COUNT * 2 + 1][0]],
              reversed);

    // 末端没有下一采样，使用相邻值的相反数维持差分边界。
    feature[featureLen - 1] = -feature[featureLen - 2];
    // 正向与反向滤波结果组合为零相位近似边缘特征。
    for ( size_t i = featureLen - 2; i > 0; --i ) {
        feature[i] = reversed[featureLen - i - 2] - feature[i - 1];
    }
    // 首样本使用反向序列对应边界值单独处理。
    feature[0] = reversed[featureLen - 2];

    // 返回独立 1kHz 特征，不保留音频副本引用。
    return feature;
}

/// @brief 将可靠的 BPM 估算吸附到常见精度。
/// @param bpm 原始 BPM。
/// @param uncertainty BPM 不确定度。
/// @return 吸附后的 BPM。
/// @note 从粗到细尝试常见精度，首个落入统计误差窗口的网格获选。
double snapBpm(double bpm, double uncertainty)
{
    // 每项第一列是每 BPM 单位的网格数，第二列是不确定度容忍倍数。
    for ( const auto& snap : BPM_SNAP ) {
        // remainder 给出原始 BPM 到最近候选网格的有符号距离。
        if ( std::abs(std::remainder(bpm, 1.0 / snap[0])) <
             uncertainty * snap[1] ) {
            // 在所选精度上进行整数舍入后恢复 BPM 单位。
            return static_cast<double>(roundToInt(bpm * snap[0])) / snap[0];
        }
    }
    // 所有常见网格都超出可信窗口时保留原始估值。
    return bpm;
}

/// @brief 将检测到的峰值相位归一化到距离音频 0 点最近的等价首拍。
/// @param phaseMs 原始峰值相位，单位为毫秒。
/// @param beatMs 拍长，单位为毫秒。
/// @return 有符号首拍相位，范围约为 [-beatMs / 2, beatMs / 2]。
/// @pre beatMs 必须为有限正数。
/// @note 选择距零点最近的等价相位，允许负 offset 表示首拍略早于音频起点。
double normalizeNearestBeatPhase(double phaseMs, double beatMs)
{
    // fmod 先折叠到一个拍长内，并保留输入符号。
    double normalized = std::fmod(phaseMs, beatMs);
    if ( normalized <= -beatMs * 0.5 ) {
        // 左半边界及更小值向右平移一个周期。
        normalized += beatMs;
    } else if ( normalized > beatMs * 0.5 ) {
        // 右半区向左平移一个周期，获得更接近零的表示。
        normalized -= beatMs;
    }

    if ( std::abs(normalized) < 1e-9 ) {
        // 消除浮点余数产生的负零和极小噪声。
        return 0.0;
    }
    return normalized;
}

/// @brief 根据特征自相关估算 BPM。
/// @param feature 1kHz 节拍特征序列。
/// @return 成功时返回估计值和可信度等级，否则返回空。
/// @details 算法先从局部峰构造连续谐波列，再用通过原点的线性回归
/// 求取基本周期。随后根据周期倍数关系消除拍内细分歧义，并以最终峰列
/// 的残差估算速度不确定度。整个过程只依赖 1kHz 特征索引，不读取原音频。
/// @warning 后台耗时路径：自相关与多轮候选回归均随输入长度增长。
/// @note 质量码 0 表示稳定，1 表示存在轻微缺峰，16 表示结果不宜吸附。
std::optional<std::pair<BpmEstimate, int>> calcBpm(
    const std::vector<float>& feature)
{
    // 至少保留足够样点，使局部峰搜索窗口能够落在有效区间内。
    const size_t len = feature.size();
    if ( len < 64 ) {
        return std::nullopt;
    }

    // 自相关只需要覆盖候选节拍周期，不必保留完整卷积尾部。
    std::vector<float> correlation;
    if ( len + len / 2 > FFT_MAX_N ) {
        // 超过 FFT 上限时截取可容纳的前段，仍要求截取长度小于原序列。
        if ( FFT_MAX_N * 2 / 3 >= len ) {
            return std::nullopt;
        }
        // 取上限的三分之二，使线性自相关零填充后恰好不超过上限。
        const size_t       partLen = FFT_MAX_N * 2 / 3;
        std::vector<float> part(feature.cbegin(), feature.cbegin() + partLen);
        correlation = autocorr(part, part.size() / 2);
    } else {
        // 常规输入直接使用全部特征，提高长音频上的周期稳定性。
        correlation = autocorr(feature, len / 2);
    }

    // FFT 失败或结果过短时无法进行两侧各 16 点的局部峰判定。
    if ( correlation.size() < 64 ) {
        return std::nullopt;
    }

    // 只搜索前四秒周期，对应算法支持的最低实用速度范围。
    const int rlen =
        static_cast<int>(std::min<size_t>(4000, correlation.size()));
    if ( rlen <= 32 ) {
        return std::nullopt;
    }

    // 收集在正相关区间内、相对左右邻域均不更低的候选峰。
    std::vector<int> peaks;
    for ( int i = 16; i < rlen - 16; ++i ) {
        // 非正相关不能代表重复节奏周期。
        if ( correlation[i] <= 0.0f ) {
            continue;
        }

        // 16ms 邻域抑制同一宽峰上的重复候选。
        bool isPeak = true;
        for ( int j = 1; j < 16; ++j ) {
            if ( correlation[i] < correlation[i - j] ||
                 correlation[i] < correlation[i + j] ) {
                isPeak = false;
                break;
            }
        }
        if ( isPeak ) {
            // peaks 按扫描顺序天然保持周期升序。
            peaks.push_back(i);
        }
    }

    // 第一阶段从每个强峰出发，拟合其整数倍位置形成周期候选。
    double                                 bestAveragePeak = 0.0;
    size_t                                 estimateIndex   = 0;
    std::vector<std::pair<double, double>> estimates;
    for ( size_t i = 0; i < peaks.size() && peaks[i] < rlen; ++i ) {
        // 已有候选后跳过明显弱于当前最佳者的起始峰。
        if ( !estimates.empty() &&
             correlation[peaks[i]] <= bestAveragePeak * 0.8f ) {
            continue;
        }

        // m 是当前回归得到的基本周期，p 是下一预计峰位置。
        double   m           = peaks[i];
        size_t   j           = i;
        int      p           = roundToInt(m);
        uint32_t foundCount  = 0;
        double   sxx         = 0.0;
        double   sxy         = 0.0;
        uint32_t miss        = 0;
        double   averagePeak = 0.0;

        // 沿周期整数倍推进；首个缺峰即终止该候选，避免跨节奏段拼接。
        for ( double k = 1.0; p < rlen - 10 && miss <= 0; k += 1.0 ) {
            double peakEstimate = p;
            // 丢弃已经落在当前预测窗口左侧的峰。
            while ( j < peaks.size() && peaks[j] < p - 10 ) {
                ++j;
            }
            if ( j < peaks.size() && peaks[j] <= p + 10 ) {
                // 命中预测窗口后通过抛物线插值获得亚毫秒峰位。
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
                    // 强制回归通过原点，使倍数序号直接映射到周期位置。
                    m = sxy / sxx;
                }
            } else {
                // 该候选不允许出现断裂峰列。
                ++miss;
            }
            // 使用最新峰位和回归周期预测下一个峰，降低累计舍入漂移。
            p = roundToInt(peakEstimate + m);
        }

        // 峰列必须存在并至少延伸过搜索范围中点，排除短暂重复结构。
        if ( foundCount == 0 || p <= rlen / 2 ) {
            continue;
        }

        // 平均相关强度用于比较不同周期候选的持续可靠性。
        averagePeak /= foundCount;
        if ( averagePeak <= correlation[peaks[i]] * 0.70f ) {
            // 后续倍频峰衰减过多时，起始峰很可能只是局部偶然峰。
            continue;
        }

        if ( averagePeak > bestAveragePeak ) {
            // estimates 同时保留周期与平均峰强度，供倍频消歧使用。
            estimates.emplace_back(sxy / sxx, averagePeak);
            if ( averagePeak > bestAveragePeak * 1.25 ) {
                // 只有显著提升才切换主候选，减少接近强度间的抖动。
                estimateIndex = estimates.size() - 1;
            }
            bestAveragePeak = averagePeak;
        }
    }

    // 无有效峰列时不返回看似合法的默认速度。
    if ( !(bestAveragePeak > 0.0) || estimates.empty() ||
         estimateIndex >= estimates.size() ) {
        return std::nullopt;
    }

    // 过短周期可能是拍内细分，优先尝试可整除且落入速度上限的长周期。
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
                // 10ms 余数窗口容纳峰检测与插值误差。
                estimateIndex = i;
                beatMs        = estimates[estimateIndex].first;
                success       = true;
                break;
            }
        }
        if ( !success ) {
            // 没有可靠倍频关系时保留当前周期，避免无依据降速。
            break;
        }
    }

    // 长于主周期的整数倍候选可提示每小节拍数。
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
                // 大于二的倍频已经具备区分小节结构的意义。
                break;
            }
        }
    }

    // 短于主周期的整数分频候选可提示拍内细分数。
    for ( size_t i = estimateIndex; i-- > 0; ) {
        if ( std::abs(std::remainder(estimates[estimateIndex].first,
                                     estimates[i].first)) <= 10.0 ) {
            estimate.division = static_cast<uint32_t>(
                std::max(1,
                         roundToInt(estimates[estimateIndex].first /
                                    estimates[i].first)));
            if ( estimate.division > 2 ) {
                // 优先采用清晰的高阶细分，不继续被更短谐波覆盖。
                break;
            }
        }
    }

    // 第二阶段围绕最终候选重新拟合，并统计残差用于质量评估。
    size_t   p          = static_cast<size_t>(std::max(1, roundToInt(beatMs)));
    uint32_t foundCount = 0;
    double   sxx        = 0.0;
    double   sxy        = 0.0;
    double   syy        = 0.0;
    uint32_t miss       = 0;
    uint32_t contMiss   = 0;

    // 每轮只在预测位置左右 10ms 内寻找最强峰，限制错误匹配范围。
    for ( double k = 1.0;
          p >= 10 && p + 10 < correlation.size() && miss <= 0 && contMiss <= 0;
          k += 1.0 ) {
        // 半开区间覆盖 p-10 到 p+10 的全部相关样点。
        const auto rangeBegin =
            correlation.begin() + static_cast<std::ptrdiff_t>(p - 10);
        const auto rangeEnd =
            correlation.begin() + static_cast<std::ptrdiff_t>(p + 11);
        // 使用窗口内最大值而非第一阶段峰表，获得一致的残差样本。
        const size_t maxPeak = static_cast<size_t>(
            std::max_element(rangeBegin, rangeEnd) - correlation.begin());
        double peakEstimate = static_cast<double>(p);
        if ( correlation[maxPeak] > 0.0f &&
             (maxPeak < p ? p - maxPeak : maxPeak - p) <= 8 ) {
            // 峰需位于更严格的 8ms 区间，窗口边缘只用于避免漏掉极值形状。
            contMiss = 0;
            ++foundCount;
            peakEstimate = peak(maxPeak,
                                1.0,
                                correlation[maxPeak - 1],
                                correlation[maxPeak],
                                correlation[maxPeak + 1]);
            // 累积通过原点线性回归及残差方差所需的充分统计量。
            sxx += k * k;
            sxy += k * peakEstimate;
            syy += peakEstimate * peakEstimate;
            if ( std::abs(sxx) > std::numeric_limits<double>::epsilon() ) {
                // 用全部已命中峰持续校正周期预测。
                beatMs = sxy / sxx;
            }
        } else {
            // 当前策略在首次缺峰时终止，两个计数保留原算法质量语义。
            ++miss;
            ++contMiss;
        }

        // 从本次插值峰继续推进，避免相对音频起点反复量化。
        const int nextPeak = roundToInt(peakEstimate + beatMs);
        if ( nextPeak <= 0 ) {
            break;
        }
        p = static_cast<size_t>(nextPeak);
    }

    // 少于四个周期不足以可靠估计回归方差。
    if ( foundCount < 4 ||
         std::abs(sxy) <= std::numeric_limits<double>::epsilon() ) {
        return std::nullopt;
    }

    // 1kHz 特征以毫秒为索引，因此用每分钟毫秒数换算 BPM。
    estimate.bpm = 60000.0 * sxx / sxy;
    // 通过原点回归的残差平方和除以自由度得到周期方差。
    const double variance =
        (syy - sxy * sxy / sxx) / static_cast<double>(foundCount - 1);
    if ( variance < 0.0 || !std::isfinite(variance) ) {
        // 负方差仅可能来自退化或数值误差，不对其强行开方。
        return std::nullopt;
    }

    // sigma 衡量自相关峰相对理想整数周期的离散程度。
    const double sigma = std::sqrt(variance);
    // uncertaintyTerm 将回归误差传播到 BPM 相对不确定度。
    const double uncertaintyTerm = (sxx * syy / sxy / sxy - 1.0) /
                                   static_cast<double>(foundCount - 1) *
                                   foundCount;
    if ( uncertaintyTerm < 0.0 || !std::isfinite(uncertaintyTerm) ) {
        // 退化统计量不能生成可解释的不确定度。
        return std::nullopt;
    }
    estimate.uncertainty = std::sqrt(uncertaintyTerm) * estimate.bpm;

    // 质量码控制调用方是否允许将原始 BPM 吸附到常见网格。
    int quality = 0;
    if ( sigma > 2.4 || estimate.uncertainty / estimate.bpm > 0.00005 ) {
        // 明显离散的峰列保留原始估值，避免错误吸附被放大。
        quality = 16;
    } else if ( miss > 0 || sigma > 0.6 ) {
        // 轻度不稳定仍可返回，但标记为次优质量。
        quality = 1;
    }

    // 返回估值与质量码，吸附和 offset 计算由上层按顺序完成。
    return std::make_pair(estimate, quality);
}

/// @brief 估算首拍相对 0 点的偏移。
/// @param feature 1kHz 节拍特征序列。
/// @param bpm 已估计 BPM。
/// @return 成功时返回偏移，单位为毫秒。
/// @details 将各拍的特征片段折叠到一个周期内，通过叠加强化稳定瞬态；
/// 最强相位再经三点插值与滤波延迟补偿得到原始首拍位置。
/// @warning 后台计算路径：完整遍历特征并按拍长折叠累加。
/// @note 返回值尚未归一化到离零点最近的等价拍，由调用方处理。
std::optional<double> calcOffset(const std::vector<float>& feature, double bpm)
{
    // 非正速度或空特征没有可定义的拍长与相位。
    if ( !(bpm > 0.0) || feature.empty() ) {
        return std::nullopt;
    }

    // 特征索引单位为毫秒，beatMs 可直接作为折叠周期。
    const double beatMs = 60000.0 / bpm;
    // 两侧额外样点为峰搜索和二次插值保留安全边界。
    const size_t foldedLen =
        static_cast<size_t>(std::ceil(beatMs)) + static_cast<size_t>(10);
    if ( foldedLen <= 10 ) {
        return std::nullopt;
    }

    // 将每个节拍周期叠加到同一相位轴，增强重复瞬态并抑制非周期内容。
    std::vector<float> folded(foldedLen, 0.0f);
    const size_t       loopCount =
        static_cast<size_t>(std::ceil(feature.size() / beatMs));
    for ( size_t i = 0; i < loopCount; ++i ) {
        // 浮点拍长逐次换算起点，避免整数周期累计漂移。
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
            // 尾部不足一个周期时仅累加实际存在的样点。
            folded[j] += feature[start + j];
        }
    }

    if ( folded.size() <= 10 ) {
        return std::nullopt;
    }

    // 排除两侧五个样点，确保峰插值邻点有效且不受折叠边界影响。
    const auto   maxIt = std::max_element(folded.begin() + 5, folded.end() - 5);
    const size_t maxPeak = static_cast<size_t>(maxIt - folded.begin());
    // 三点抛物线把整数毫秒峰细化为亚毫秒相位。
    double offset = peak(maxPeak,
                         1.0,
                         folded[maxPeak - 1],
                         folded[maxPeak],
                         folded[maxPeak + 1]);
    // 预处理滤波会引入固定群延迟，公开相位需减去该延迟。
    offset -= FILTER_DELAY_MS;
    return offset;
}

/// @brief 估算节奏峰值相对最终网格的实用不准确度。
/// @param feature 1kHz 节拍特征序列。
/// @param bpm 最终 BPM。
/// @param offsetMs 最终首拍相位，单位为毫秒。
/// @param division 拍内细分数。
/// @return 加权 RMS 不准确度，单位为毫秒。
/// @details 从正特征中筛选相互分离的局部峰，计算每个峰到最近细分网格
/// 的距离，并按超过自适应阈值的强度加权。结果越小表示节奏瞬态越贴近
/// 当前网格，但零值也可能表示有效峰不足，调用方不能将其等同于完美对齐。
/// @warning 后台计算路径：需要两次完整遍历特征序列。
/// @note 该值描述瞬态峰对最终节拍网格的贴合程度，不改变检测结果。
double calcAlignmentInaccuracy(const std::vector<float>& feature, double bpm,
                               double offsetMs, uint32_t division)
{
    // 拒绝非有限输入，避免 remainder 和平方和传播 NaN。
    if ( feature.size() < 3 || !(bpm > 0.0) || !std::isfinite(bpm) ||
         !std::isfinite(offsetMs) ) {
        return 0.0;
    }

    const double beatMs = 60000.0 / bpm;
    if ( !(beatMs > 0.0) || !std::isfinite(beatMs) ) {
        return 0.0;
    }

    // 限制细分数，防止异常元数据生成小于采样分辨率的网格。
    const uint32_t safeDivision = std::clamp<uint32_t>(division, 1U, 16U);
    const double   gridMs       = beatMs / static_cast<double>(safeDivision);
    if ( !(gridMs > 1.0) || !std::isfinite(gridMs) ) {
        return 0.0;
    }

    // 首轮只统计正瞬态，以均值和最大值共同建立自适应阈值。
    double positiveMax   = 0.0;
    double positiveSum   = 0.0;
    size_t positiveCount = 0;
    for ( float value : feature ) {
        if ( value > 0.0f && std::isfinite(value) ) {
            // 非有限或负边缘不参与节拍峰强度统计。
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
    // 阈值至少覆盖均值的 1.5 倍，同时不超过峰值的 95%。
    const double threshold = std::min(
        std::max(positiveMax * 0.20, positiveMean * 1.50), positiveMax * 0.95);
    // 最小峰距抑制同一瞬态宽峰，且不跨越四分之一网格。
    const size_t minPeakDistance =
        static_cast<size_t>(std::max(1.0, std::min(80.0, gridMs * 0.25)));

    double weightedSquareSum = 0.0;
    double weightSum         = 0.0;
    size_t peakCount         = 0;
    size_t lastPeakIndex     = 0;
    bool   hasLastPeak       = false;
    // 第二轮提取局部极大值并计算其到最近网格线的残差。
    for ( size_t i = 1; i + 1 < feature.size(); ++i ) {
        const float value = feature[i];
        if ( !(value > threshold) || !std::isfinite(value) ||
             value < feature[i - 1] || value < feature[i + 1] ) {
            continue;
        }

        if ( hasLastPeak && i - lastPeakIndex < minPeakDistance ) {
            // 距离过近的后续峰视为同一瞬态的旁瓣。
            continue;
        }

        // remainder 直接给出到最近网格倍数的有符号距离。
        const double residual =
            std::abs(std::remainder(static_cast<double>(i) - offsetMs, gridMs));
        const double weight = static_cast<double>(value) - threshold;
        if ( !(weight > 0.0) || !std::isfinite(residual) ) {
            continue;
        }

        // 以超过阈值的峰强为权重，使显著节拍对指标贡献更大。
        weightedSquareSum += residual * residual * weight;
        weightSum += weight;
        ++peakCount;
        lastPeakIndex = i;
        hasLastPeak   = true;
    }

    // 峰数不足时指标不稳定，使用零表示无法提供可靠度量。
    if ( peakCount < 4 || !(weightSum > 0.0) ) {
        return 0.0;
    }

    const double rms = std::sqrt(weightedSquareSum / weightSum);
    if ( !std::isfinite(rms) ) {
        return 0.0;
    }
    // 理论最近网格残差不超过半个网格，钳制浮点边界误差。
    return std::min(rms, gridMs * 0.5);
}

}  // namespace

/// @brief 执行自动 BPM/offset 检测。
/// @param monoSamples 单声道浮点音频采样。
/// @param sampleRate 采样率。
/// @return 成功时返回检测结果，否则返回空。
/// @details 依次完成特征提取、速度回归、可信吸附、相位折叠与网格误差
/// 估算。任一必要阶段失败或产生非有限数值时，整体返回空结果。
/// @pre monoSamples 必须是按 sampleRate 解码的单声道连续 PCM。
/// @note 函数不缓存输入或中间特征，可由后台任务安全管理调用生命周期。
/// @warning 后台耗时路径：会完整预处理音频并执行自相关 FFT，禁止在
/// UI、渲染或逻辑热路径中调用。
std::optional<BpmAutoTimingResult> BpmAutoDetector::detect(
    const std::vector<float>& monoSamples, uint32_t sampleRate)
{
    // 十秒下限保证 BPM 回归拥有足够多的重复周期。
    if ( sampleRate == 0 ||
         monoSamples.size() <
             static_cast<size_t>(MIN_DETECT_SECONDS * sampleRate) ) {
        return std::nullopt;
    }

    // 预处理同时完成频段融合、能量压缩和 1kHz 时间轴转换。
    std::vector<float> feature = preprocess(monoSamples, sampleRate);
    if ( feature.empty() ) {
        return std::nullopt;
    }

    // BPM 是后续 offset 与网格误差计算的共同前置条件。
    auto bpmEstimate = calcBpm(feature);
    if ( !bpmEstimate ) {
        return std::nullopt;
    }

    // 先保留原始回归数据，便于界面展示吸附前的不确定度。
    BpmAutoTimingResult result;
    result.rawBpm            = bpmEstimate->first.bpm;
    result.rawBpmUncertainty = bpmEstimate->first.uncertainty;
    result.signature         = bpmEstimate->first.signature;
    result.division          = bpmEstimate->first.division;
    // 仅可靠或轻度不稳定结果允许吸附，低质量结果保持原值。
    result.bpm = bpmEstimate->second < 16
                     ? snapBpm(result.rawBpm, result.rawBpmUncertainty)
                     : result.rawBpm;

    // offset 必须基于最终 BPM 计算，确保相位与公开节拍网格一致。
    auto rawPhase = calcOffset(feature, result.bpm);
    if ( !rawPhase ) {
        return std::nullopt;
    }

    // 将折叠峰归一化为音频零点附近最直观的有符号首拍偏移。
    const double beatMs = 60000.0 / result.bpm;
    result.offsetMs     = normalizeNearestBeatPhase(*rawPhase, beatMs);
    // 对最终 BPM、offset 和细分共同形成的网格进行独立贴合度评估。
    result.alignmentInaccuracyMs = calcAlignmentInaccuracy(
        feature, result.bpm, result.offsetMs, result.division);

    // 最终边界集中阻止非有限数值进入 UI 或项目数据。
    if ( !std::isfinite(result.bpm) || result.bpm <= 0.0 ||
         !std::isfinite(result.offsetMs) ||
         !std::isfinite(result.alignmentInaccuracyMs) ) {
        return std::nullopt;
    }
    // 返回值完全拥有结果数据，不依赖临时特征序列生命周期。
    return result;
}

}  // namespace MMM::UI
