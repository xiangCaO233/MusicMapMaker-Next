/// @file
/// @brief 目录监听的相对路径过滤回归，防止保存配置触发资源重扫循环。
/// 仅调用纯路径判定，不创建真实目录、后台任务或平台监听句柄。
#include "logic/ProjectDirectoryWatcher.h"

#include <filesystem>
#include <iostream>

/// @brief 检查根配置排除与嵌套资源保留规则。
/// @return 所有过滤结果符合约定时返回零，首个失败返回一。
/// 相对路径由 filesystem 拼接，兼容平台目录分隔符差异。
int main()
{
    using MMM::Logic::ProjectDirectoryWatcher;

    // 旧单文件位于项目根；保存自身配置不能立即请求再次扫描资源。
    if ( ProjectDirectoryWatcher::isRelevantProjectPathChange(
             std::filesystem::path("mmm_project.json")) ) {
        std::cerr
            << "project metadata writes must not trigger a resource scan\n";
        return 1;
    }
    // 普通资源子目录中的音频变化必须保留，排除规则不能覆盖整个项目树。
    if ( !ProjectDirectoryWatcher::isRelevantProjectPathChange(
             std::filesystem::path("audio") / "effect.wav") ) {
        std::cerr << "audio resource changes must trigger a resource scan\n";
        return 1;
    }
    // 新配置采用 .mmm 分片目录，内部音频描述的变化同样不属于媒体变化。
    // 判定依据是根目录前缀，而不是 audio_resources.json 这一特定文件名。
    if ( ProjectDirectoryWatcher::isRelevantProjectPathChange(
             std::filesystem::path(".mmm") / "audio_resources.json") ) {
        std::cerr << "split project storage writes must not trigger a resource "
                     "scan\n";
        return 1;
    }
    // 嵌套目录可含同名文件；不能只比较 filename 就忽略所有同名路径。
    // 这个断言约束的是根相对位置，不要求文件在磁盘上实际存在。
    if ( !ProjectDirectoryWatcher::isRelevantProjectPathChange(
             std::filesystem::path("nested") / "mmm_project.json") ) {
        std::cerr << "only root project metadata should be ignored\n";
        return 1;
    }
    // .mmm 只有位于第一段时才是内部配置，资源子目录中的同名目录应保留。
    // 其下 chart.mmm 仍是候选资源，避免配置排除吞掉正常谱面。
    if ( !ProjectDirectoryWatcher::isRelevantProjectPathChange(
             std::filesystem::path("nested") / ".mmm" / "chart.mmm") ) {
        std::cerr
            << "only the root split storage directory should be ignored\n";
        return 1;
    }
    // 显式返回码不依赖 assert，发布构建也能报告过滤规则退化。
    // 后台采样周期和操作系统通知投递不属于这组纯路径断言。
    return 0;
}
