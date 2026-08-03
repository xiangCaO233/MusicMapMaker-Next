#include "ui/imgui/manager/ProjectAudioToolSearch.h"

#include <cmath>
#include <optional>

namespace
{

using MMM::UI::ProjectAudioToolSearch::scoreCandidate;

/// @brief 使用小容差比较搜索布局高度。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 验证搜索结果区域默认显示五行并服从内容和主画布空间上限。
bool testResultPaneHeight()
{
    using MMM::UI::ProjectAudioToolSearch::calculateResultPaneHeight;
    return near(calculateResultPaneHeight(
                    100.0F, 20.0F, 0, 4.0F, 500.0F, 100.0F),
                0.0F) &&
           near(
               calculateResultPaneHeight(0.0F, 20.0F, 12, 4.0F, 500.0F, 100.0F),
               108.0F) &&
           near(calculateResultPaneHeight(
                    300.0F, 20.0F, 3, 4.0F, 500.0F, 100.0F),
                68.0F) &&
           near(calculateResultPaneHeight(
                    300.0F, 20.0F, 12, 4.0F, 180.0F, 100.0F),
                80.0F) &&
           near(
               calculateResultPaneHeight(1.0F, 20.0F, 12, 4.0F, 500.0F, 100.0F),
               28.0F);
}

/// @brief 验证精确、前缀、子串和顺序模糊匹配的优先级。
bool testMatchPriority()
{
    const auto exact     = scoreCandidate("loop_001.wav", "LOOP_001.WAV");
    const auto prefix    = scoreCandidate("loop_001.wav", "loop");
    const auto substring = scoreCandidate("lead_loop_001.wav", "loop");
    const auto fuzzy     = scoreCandidate("loop_001.wav", "lp01");
    return exact && prefix && substring && fuzzy && *exact > *prefix &&
           *prefix > *substring && *substring > *fuzzy;
}

/// @brief 验证搜索词空白裁切、ASCII 大小写和不匹配分支。
bool testTrimAndMissingMatch()
{
    const auto trimmed = scoreCandidate("Fx_001.WAV", "  fx_001.wav\t");
    const auto missing = scoreCandidate("loop_001.wav", "kick");
    return trimmed.has_value() && !missing.has_value();
}

/// @brief 验证 UTF-8 文件名可通过原始字节子串稳定匹配。
bool testUtf8Substring()
{
    const auto match = scoreCandidate("初音ミク_重音テト.wav", "重音テト");
    return match.has_value();
}

}  // namespace

/// @brief 运行项目音频工具实时相似搜索测试。
int main()
{
    if ( !testResultPaneHeight() ) return 1;
    if ( !testMatchPriority() ) return 2;
    if ( !testTrimAndMissingMatch() ) return 3;
    if ( !testUtf8Substring() ) return 4;
    return 0;
}
