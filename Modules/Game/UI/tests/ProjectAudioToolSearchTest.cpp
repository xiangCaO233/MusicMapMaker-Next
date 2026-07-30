#include "ui/imgui/manager/ProjectAudioToolSearch.h"

#include <optional>

namespace
{

using MMM::UI::ProjectAudioToolSearch::scoreCandidate;

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
    if ( !testMatchPriority() ) return 1;
    if ( !testTrimAndMissingMatch() ) return 2;
    if ( !testUtf8Substring() ) return 3;
    return 0;
}
