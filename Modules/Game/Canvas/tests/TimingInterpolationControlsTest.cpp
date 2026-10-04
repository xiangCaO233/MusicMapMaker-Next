#include "canvas/TimingInterpolationControls.h"

#include "config/EditorSettings.h"
#include "config/TimingInterpolationPreferences.h"
#include "log/colorful-log.h"
#include "mmm/timing/Timing.h"
#include "mmm/timing/TimingInterpolation.h"
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>

namespace
{
/// @brief 用真实设置根的 JSON 往返验证工具默认，覆盖旧配置和损坏字段。
/// @return 所有稳定选项可恢复且非法坐标、密度和分拍不会污染模型时为真。
/// @note 只操作内存配置，不读写当前用户目录或活动谱面。
/// @details 当前配置根是实际保存路径的 ADL 类型，不能以独立 DTO 成功代替接线。
/// 损坏字段分别覆盖未知标识、错误类型、非正密度和极大整数。
/// 贝塞尔坐标允许纵轴反向排列，但横轴恢复必须单调。
/// 自定义模式既检查有效源码，又检查超容量源码的整体回退。
/// JSON 往返没有运行期缓存，所有后续数学编译都由新段负责。
bool testPreferences()
{
    MMM::Config::EditorSettings settings;
    auto& preferences              = settings.m_interpolationPreferences;
    preferences.m_curve            = "Bezier";
    preferences.m_useBeats         = true;
    preferences.m_samplesPerSecond = 1000;
    preferences.m_beatNumerator    = 1;
    preferences.m_beatDenominator  = 192;
    // 高分拍值复现 SV 组的密集设置，恢复时不能偷偷退回默认半拍。
    // Y1 大于 Y2 是合法曲率，测试不能错误要求两个维度都排序。
    preferences.m_controls = { .1, .8, .9, .2 };
    // 经过 EditorSettings 根对象而非直接序列化 DTO，确保实际配置接线存在。
    const nlohmann::json encoded = settings;
    // 使用公开 ADL 恢复接口，避免测试自行复制成员而漏掉配置根字段。
    const auto  restored = encoded.get<MMM::Config::EditorSettings>();
    const auto& result   = restored.m_interpolationPreferences;
    // 控制点逐值一致同时覆盖单位比例和数组字段顺序。
    // 相同密度不能证明采样轴相同，拍模式与分拍必须一起核对。
    bool ok = result.m_curve == "Bezier" && result.m_useBeats &&
              result.m_samplesPerSecond == 1000 &&
              result.m_beatNumerator == 1 && result.m_beatDenominator == 192 &&
              result.m_controls == preferences.m_controls;
    // 旧版本没有整个选项节点，不能意外启用自定义函数或拍域。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 旧配置必须延续原工具行为，不能仅因新增字段就开启复杂曲线。
    ok &= legacy.m_interpolationPreferences.m_curve == "Linear" &&
          legacy.m_interpolationPreferences.m_samplesPerSecond == 16 &&
          !legacy.m_interpolationPreferences.m_useBeats;
    // 类型损坏的字段应独立回退；数组仍接受合法坐标并收敛到单位范围。
    const auto damaged = nlohmann::json{
        { "curve", "Unknown" },      { "useBeats", "yes" },
        { "samplesPerSecond", -1 },  { "beatNumerator", 0 },
        { "beatDenominator", 1e20 }, { "controls", { 2, -.5, .1, "bad" } }
    }.get<MMM::Config::TimingInterpolationPreferences>();
    // 先规范每个坐标，再恢复 X 顺序；损坏的 Y2 应保留自身默认值。
    // 分拍上限在窄化前处理，极大 JSON 数值不能溢出到负整数。
    ok &= damaged.m_curve == "Linear" && !damaged.m_useBeats &&
          damaged.m_samplesPerSecond == 16 && damaged.m_beatNumerator == 1 &&
          damaged.m_beatDenominator == 65536 && damaged.m_controls[0] == .1 &&
          damaged.m_controls[1] == 0 && damaged.m_controls[2] == 1 &&
          damaged.m_controls[3] == .75;
    // 自定义源码也经过同一配置读取器；不能只验证预设的整数选择索引。
    const auto custom =
        nlohmann::json{ { "curve", "Custom" }, { "expression", "1+t" } }
            .get<MMM::Config::TimingInterpolationPreferences>();
    // 公式字符不在配置层求值；这里验证源码能传到下一次创建入口。
    ok &= custom.m_curve == "Custom" && custom.m_expression == "1+t";
    // 超长公式整体拒绝，不能把被截断的半条公式作为下次默认。
    const auto oversized = nlohmann::json{
        // 2049 字节超过输入容量一字节，专门覆盖边界而非一般语法错误。
        { "curve", "Custom" },
        { "expression", std::string(2049, 't') }
    }.get<MMM::Config::TimingInterpolationPreferences>();
    ok &= oversized.m_curve == "Linear" && oversized.m_expression.empty();
    // 配置兼容失败单独报告，不被后续鼠标交互用例的结果掩盖。
    if ( !ok ) XERROR("插值工具选项持久化或安全回退失败");
    return ok;
}

/// @brief 新建恢复只改变工具选项，谱面范围与端值仍属于本次段落。
/// @return 曲线、采样方式、控制点及公式源码恢复正确，BPM 不进入拍域时为真。
/// @details 目标段采用不同时间长度和端值，确保旧范围不会借默认恢复进入新谱面。
/// 公式源来自真正的领域编译对象，捕获必须借用而非重新构造假源码。
/// 重新编译的终值由解析函数独立判断，不用旧缓存产生预期答案。
/// 控制点边界与非有限值直接验证，避免界面投影碰巧夹取掩盖模型问题。
bool testApplication()
{
    MMM::TimingInterpolation source;
    source.m_curve            = MMM::TimingCurve::Bezier;
    source.m_variable         = MMM::TimingVariable::Beat;
    source.m_beatDenominator  = 192;
    source.m_samplesPerSecond = 1000;
    source.m_controlX1        = .1;
    // 配置只记工具选择，源段的效应值没有作为可复用默认传递。
    source.m_controlY1 = .8;
    // 使用生产捕获器作为配置来源，检查全部可复用选项而非手工拼接 DTO。
    auto preferences =
        MMM::Canvas::captureTimingInterpolationPreferences(source);
    MMM::TimingInterpolation target;
    target.m_duration = 3;
    // 三秒与端值七都不同于源对象，检查的是保留本次段落而非恢复原对象。
    target.m_endValue = 7;
    MMM::Canvas::applyTimingInterpolationPreferences(
        target, MMM::TimingEffect::SCROLL, preferences);
    bool ok = target.m_duration == 3 && target.m_endValue == 7 &&
              target.m_curve == MMM::TimingCurve::Bezier &&
              target.m_variable == MMM::TimingVariable::Beat &&
              target.m_beatDenominator == 192 && target.m_controlX1 == .1 &&
              target.m_controlY1 == .8;
    // BPM 创建入口必须覆盖拍域偏好，否则会以自己的红线决定自变量。
    MMM::Canvas::applyTimingInterpolationPreferences(
        target, MMM::TimingEffect::BPM, preferences);
    // 相同偏好用于不同类型，BPM 的秒域要求须由消费入口维护。
    ok &= target.m_variable == MMM::TimingVariable::Time;
    source.m_variable = MMM::TimingVariable::Time;
    source.m_duration = 2;
    // 定义域变化是公式缓存生命周期的边界，不能复制函数指针绕过编译。
    double      start = 0;
    std::string error;
    if ( !MMM::setTimingInterpolationFunction(source, "1+t", start, error) )
        return false;
    // 自定义源码使用绝对参数，重新放置后起终值由新域推导。
    preferences = MMM::Canvas::captureTimingInterpolationPreferences(source);
    // 捕获仅持久化文本，不把旧段的编译定义域或拍轴传给新段。
    MMM::Canvas::applyTimingInterpolationPreferences(
        target, MMM::TimingEffect::SCROLL, preferences);
    ok &= preferences.m_expression == "1+t" &&
          target.m_curve == MMM::TimingCurve::Custom && !target.m_function &&
          !target.m_beatAxis;
    // 新定义域重新编译后终值应变为四，不能继续沿用旧两秒段的三。
    if ( !MMM::setTimingInterpolationFunction(
             target, preferences.m_expression, start, error) )
        return false;
    ok &= target.m_endValue == 4;
    // 只有完成新域编译才产生新的端值，默认恢复本身不应抢先计算。
    // 控制点靠近边界时仍保持 X 顺序，非法输入不能改变另一枚手柄。
    // 第一枚不能越过第二枚，第二枚不能越过第一枚，分别检查两个方向。
    // 纵坐标越界独立夹取，防止 X 合法时漏掉 Y 的领域约束。
    target = {};
    ok &= MMM::Canvas::moveTimingBezierControl(target, 0, 2, -.5) &&
          target.m_controlX1 == .75 && target.m_controlY1 == 0;
    ok &= MMM::Canvas::moveTimingBezierControl(target, 1, -.5, 2) &&
          target.m_controlX2 == .75 && target.m_controlY2 == 1;
    // 完整描述比较验证失败调用没有部分修改坐标或曲线种类。
    // NaN 与非法索引来自不同失败入口，二者都必须无副作用返回。
    const auto before = target;
    ok &= !MMM::Canvas::moveTimingBezierControl(target, 2, .5, .5) &&
          !MMM::Canvas::moveTimingBezierControl(
              target, 0, std::numeric_limits<double>::quiet_NaN(), .5) &&
          target == before;
    if ( !ok ) XERROR("插值工具默认应用或控制点约束失败");
    return ok;
}

/// @brief 向实际 ImGui 控制点图发送鼠标输入，验证两枚手柄均可连续拖动。
/// @return 工作副本同帧变更、句柄悬浮反馈与绘制坐标均正确时为真。
/// @note 无渲染后端，使用正式 DrawList 与输入队列，避免只测坐标 helper。
/// @details 输入通过事件队列跨帧推进，包含按下、移动和释放的完整手势。
/// 第一枚向右上拖动，第二枚向左下拖动，覆盖屏幕与参数 Y 轴方向差异。
/// 两个手势都远离边界，预期坐标不能依赖生产夹取 helper 计算。
/// 句柄高亮反馈用正式鼠标光标检查，绘制数据另核对数量和有限值。
/// 用例只改变窗口工作副本，不需要逻辑线程、音频设备或网络会话。
bool testDragging()
{
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    // 禁止读写默认 imgui.ini，实际用户的停靠布局不属于此回归夹具。
    io.DisplaySize = { 500, 400 };
    // 固定帧步长消除输入计时差异，不通过 sleep 等待拖动生效。
    io.DeltaTime = 1.f / 60;
    // 字体图集只驻留内存，无需 GPU、显示服务器或用户布局文件。
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 图集生命周期归上下文，指针只用于建立无后端的渲染前置条件。
    MMM::TimingInterpolation curve;
    curve.m_curve    = MMM::TimingCurve::Bezier;
    const auto frame = [&]() {
        // 每一帧都使用同一个窗口和句柄 ID，模拟真实编辑器的持续交互。
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 500, 400 });
        ImGui::Begin("Bezier test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        // 固定图表区域，输入位置由公开布局和归一坐标独立计算。
        ImGui::SetCursorScreenPos({ 20, 40 });
        const bool changed =
            MMM::Canvas::renderTimingBezierControls(curve, { 400, 220 });
        ImGui::End();
        ImGui::Render();
        return changed;
    };
    // 两帧预热窗口，不能把第一次测量时的隐藏帧误判为输入失败。
    frame();
    frame();
    // 后续输入在可见稳定窗口中执行，鼠标位置不依赖桌面缩放或真实窗口。
    // 内区起点为 (36,56)，宽高为 (368,188)，第一手柄为 (128,197)。
    io.AddMousePosEvent(128, 197);
    // 事件队列与实际后端一致，不直接篡改 ImGui 的 ActiveId 私有状态。
    io.AddMouseButtonEvent(0, true);
    frame();
    // 首次按下不移动控制点，增量拖动避免边缘抓取时突然跳位。
    // ImGui 鼠标位置按整像素处理，预期值直接由独立像素位移换算。
    io.AddMousePosEvent(164, 159);
    bool ok = frame() &&
              std::abs(curve.m_controlX1 - (.25 + 36.0 / 368)) < 1e-6 &&
              std::abs(curve.m_controlY1 - (.25 + 38.0 / 188)) < 1e-6;
    // 像素位置使用 float，坐标比较容许投影舍入但不接受可见偏移。
    ok &= ImGui::GetMouseCursor() == ImGuiMouseCursor_ResizeAll;
    io.AddMouseButtonEvent(0, false);
    frame();
    // 释放第一枚之后抓取第二枚，验证稳定 ID 没有把拖动状态留给旧句柄。
    // 释放事件必须被本帧消费，第二枚手柄不能接手第一枚的活动状态。
    io.AddMousePosEvent(312, 103);
    io.AddMouseButtonEvent(0, true);
    // 到达第二枚的鼠标位移不能被激活帧误当拖动，按下时坐标必须不变。
    ok &= !frame() && curve.m_controlX2 == .75 && curve.m_controlY2 == .75;
    // 第二枚的目标不是第一枚的镜像，能发现两个句柄错误复用同一字段。
    io.AddMousePosEvent(275, 141);
    ok &= frame() && std::abs(curve.m_controlX2 - (.75 - 37.0 / 368)) < 1e-6 &&
          std::abs(curve.m_controlY2 - (.75 - 38.0 / 188)) < 1e-6;
    io.AddMouseButtonEvent(0, false);
    frame();
    // 完成第二次拖动后检查最终几何，不把启动空帧混入绘制结论。
    const auto* data = ImGui::GetDrawData();
    // 真正提交了图形才算通过；有限顶点防止数字输入中间态污染几何。
    ok &= data && data->TotalVtxCount > 100 && data->TotalVtxCount < 10000;
    if ( data )
        for ( const auto* list : data->CmdLists )
            for ( const auto& vertex : list->VtxBuffer )
                ok &=
                    std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y);
    // 上下文在全部鼠标释放后销毁，不能污染同套件的其他测试入口。
    ImGui::DestroyContext();
    // 失败时保留实际坐标，便于区分输入未命中与投影方向或精度错误。
    if ( !ok )
        XERROR("贝塞尔控制点鼠标拖动或实际绘制失败：({}, {}) / ({}, {})",
               curve.m_controlX1,
               curve.m_controlY1,
               curve.m_controlX2,
               curve.m_controlY2);
    return ok;
}
}  // namespace

/// @brief 运行配置持久化、新段默认恢复及实际鼠标拖动三类回归。
/// @return 所有检查通过返回零，不生成谱面或修改用户配置。
int main()
{
    // 不短路用例执行，一类失败后仍验证其余独立职责并提供各自日志。
    bool ok = testPreferences();
    ok &= testApplication();
    ok &= testDragging();
    return ok ? 0 : 1;
}
