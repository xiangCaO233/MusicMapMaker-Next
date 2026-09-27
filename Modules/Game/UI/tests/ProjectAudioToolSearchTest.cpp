#include "ui/imgui/manager/ProjectAudioToolSearch.h"

#include <cmath>
#include <optional>

/// @file ProjectAudioToolSearchTest.cpp
/// @brief 项目音频工具的结果面板高度与相似搜索排序回归测试。
/// @details 测试覆盖空结果、空间上下限、ASCII 归一化和 UTF-8 原始字节子串；
/// 所有候选均为内存字符串，不访问项目目录。

namespace
{

using MMM::UI::ProjectAudioToolSearch::scoreCandidate;

/// @brief 使用小容差比较搜索布局高度。
/// @param lhs 实际计算高度。
/// @param rhs 期望高度。
/// @return 差值小于浮点容差时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 验证搜索结果区域默认显示五行并服从内容和主画布空间上限。
/// @return 空结果和四种空间约束均符合预期时返回 true。
bool testResultPaneHeight()
{
    using MMM::UI::ProjectAudioToolSearch::calculateResultPaneHeight;
    // 无候选时结果面板完全收起。
    return near(calculateResultPaneHeight(
                    100.0F, 20.0F, 0, 4.0F, 500.0F, 100.0F),
                0.0F) &&
           // 可用空间充足时按默认五行与间距计算高度。
           near(
               calculateResultPaneHeight(0.0F, 20.0F, 12, 4.0F, 500.0F, 100.0F),
               108.0F) &&
           // 候选少于五个时只占实际行数。
           near(calculateResultPaneHeight(
                    300.0F, 20.0F, 3, 4.0F, 500.0F, 100.0F),
                68.0F) &&
           // 主画布保留空间优先，结果面板被压缩到上限。
           near(calculateResultPaneHeight(
                    300.0F, 20.0F, 12, 4.0F, 180.0F, 100.0F),
                80.0F) &&
           // 极窄空间仍至少保留一行可点击结果。
           near(
               calculateResultPaneHeight(1.0F, 20.0F, 12, 4.0F, 500.0F, 100.0F),
               28.0F);
}

/// @brief 验证精确、前缀、子串和顺序模糊匹配的优先级。
/// @return 四类匹配都有分数且严格按质量降序时返回 true。
bool testMatchPriority()
{
    // 精确匹配同时验证 ASCII 大小写归一化。
    const auto exact     = scoreCandidate("loop_001.wav", "LOOP_001.WAV");
    const auto prefix    = scoreCandidate("loop_001.wav", "loop");
    const auto substring = scoreCandidate("lead_loop_001.wav", "loop");
    const auto fuzzy     = scoreCandidate("loop_001.wav", "lp01");
    // 四个分数必须存在，并保持精确、前缀、子串、模糊的严格顺序。
    return exact && prefix && substring && fuzzy && *exact > *prefix &&
           *prefix > *substring && *substring > *fuzzy;
}

/// @brief 验证搜索词空白裁切、ASCII 大小写和不匹配分支。
/// @return 清洗后的精确词命中且无关词返回空 optional 时返回 true。
bool testTrimAndMissingMatch()
{
    const auto trimmed = scoreCandidate("Fx_001.WAV", "  fx_001.wav\t");
    const auto missing = scoreCandidate("loop_001.wav", "kick");
    return trimmed.has_value() && !missing.has_value();
}

/// @brief 验证 UTF-8 文件名可通过原始字节子串稳定匹配。
/// @return 完整多字节子串被找到时返回 true。
bool testUtf8Substring()
{
    const auto match = scoreCandidate("初音ミク_重音テト.wav", "重音テト");
    return match.has_value();
}

}  // namespace

/// @brief 运行项目音频工具实时相似搜索测试。
/// @return 0 表示全部通过，1 至 4 分别标识布局、排序、清洗和 UTF-8 场景。
int main()
{
    // 独立退出码让 CTest 无需额外日志即可定位失败类别。
    if ( !testResultPaneHeight() ) return 1;
    if ( !testMatchPriority() ) return 2;
    if ( !testTrimAndMissingMatch() ) return 3;
    if ( !testUtf8Substring() ) return 4;
    return 0;
}
