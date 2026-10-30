/// @file MobileWorkspaceSettings.cpp
/// @brief 对独立客户端布局进行无图形依赖的序列化与防御性读取。
/// @details 像素坐标随设备旋转变化，因此只允许归一化值进入持久化格式。
/// 类型检查发生在启动加载阶段，UI 连续拖动不做 JSON 校验或解析。
#include "config/MobileWorkspaceSettings.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace MMM::Config
{
/// @brief 仅保存平台无关的标量，不将 ImGui 节点树嵌入项目配置。
/// @details 字段名称固定，未来增加字段时旧版本仍可忽略未知项。
/// @param j 输出到客户端根配置下的独立节点。
/// @param v UI 已提交的标量快照，不包含正在变化的窗口对象引用。
void to_json(nlohmann::json& j, const MobileWorkspaceSettings& v)
{
    // 显隐与比例一并冻结，重启不会出现窗口已开而宽度仍是默认的半恢复状态。
    j = { { "toolbar", v.m_toolbar },
          { "timing", v.m_timing },
          { "preview", v.m_preview },
          { "settingsOpen", v.m_settingsOpen },
          { "leftCenter", v.m_leftCenter },
          { "rightCenter", v.m_rightCenter },
          { "timingRatio", v.m_timingRatio },
          { "previewRatio", v.m_previewRatio },
          { "selectedTab", v.m_selectedTab },
          { "resourceTab", v.m_resourceTab } };
}

/// @brief 防御性读取客户端布局，错误字段单独回退，不丢失其余有效设置。
/// @details 不将字符串强转为布尔或数字，避免旧插件写入的异常值影响几何。
/// @param j 用户配置中的独立移动布局节点，允许缺失或非对象。
/// @param v 输出的完整值，读取前统一恢复代码默认。
void from_json(const nlohmann::json& j, MobileWorkspaceSettings& v)
{
    v = {};
    // 先整体恢复默认，重复加载缺字段旧文件时不能残留上一个文件的状态。
    if ( !j.is_object() ) return;
    // 布尔字段保持严格类型，空值不能误触发窗口自动展开。
    const auto boolean = [&](const char* key, bool& value) {
        // find 避免为缺失键插入空值；错误类型按该字段默认处理。
        if ( const auto it = j.find(key); it != j.end() && it->is_boolean() )
            value = it->get<bool>();
    };
    boolean("toolbar", v.m_toolbar);
    boolean("timing", v.m_timing);
    boolean("preview", v.m_preview);
    boolean("settingsOpen", v.m_settingsOpen);
    // 比例先用 double 校验有限性与边界，再缩窄到 float，禁止 NaN 进入停靠树。
    // 每个字段独立处理；一个把手损坏不能重置用户另外一侧的位置。
    const auto number =
        [&](const char* key, float& value, double min, double max) {
            // JSON 数字可能使用不同存储类型，统一提升后再做有限性判断。
            if ( const auto it = j.find(key);
                 it != j.end() && it->is_number() ) {
                const double parsed = it->get<double>();
                if ( std::isfinite(parsed) )
                    // 先夹紧 double 再缩窄，避免超大值转 float 时溢出。
                    value = static_cast<float>(std::clamp(parsed, min, max));
            }
        };
    number("leftCenter", v.m_leftCenter, 0, 1);
    // 左右独立读取，允许单手用户选择与屏幕中心不对称的位置。
    number("rightCenter", v.m_rightCenter, 0, 1);
    number("timingRatio", v.m_timingRatio, .02, .95);
    number("previewRatio", v.m_previewRatio, .02, .95);
    // 至少保留主区的非零比例，像素级最小尺寸仍由当前屏幕上的原生停靠约束。
    const float sum = v.m_timingRatio + v.m_previewRatio;
    // 按相同比例收缩辅助区，修复总量时仍保留用户原来的宽度相对关系。
    if ( sum > .95f ) {
        v.m_timingRatio *= .95f / sum;
        v.m_previewRatio *= .95f / sum;
    }
    // 标签哈希采用固定宽度无符号值，负数和超范围值不得截断后误匹配窗口。
    if ( const auto it = j.find("selectedTab");
         it != j.end() && it->is_number_unsigned() ) {
        const auto id = it->get<std::uint64_t>();
        // 稳定哈希不代表有效窗口，外壳恢复时仍需要等待同身份窗口出现。
        if ( id <= std::numeric_limits<std::uint32_t>::max() )
            v.m_selectedTab = static_cast<std::uint32_t>(id);
    }
    // 类别仅覆盖资源导航，不接受已迁走的设置页枚举作为抽屉内容。
    if ( const auto it = j.find("resourceTab");
         it != j.end() && it->is_number_integer() ) {
        const auto id = it->get<std::int64_t>();
        // 零值用于旧配置；无效类别不允许静态转换成未知导航项。
        if ( id >= 0 && id <= 5 ) v.m_resourceTab = static_cast<int>(id);
    }
}
}  // namespace MMM::Config
