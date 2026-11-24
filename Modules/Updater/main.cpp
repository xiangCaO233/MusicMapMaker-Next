/**
 * @file main.cpp
 * @brief 独立更新器辅助程序
 *
 * 用法: MusicMapMaker-Updater <downloaded_file> <target_path> <parent_pid>
 *
 * 流程:
 *   1. 等待父进程 (parent_pid) 退出
 *   2. Windows/Linux 替换单个可执行文件，macOS 解压并替换完整 .app
 *   3. 写入更新成功标记
 *   4. 启动新版本，启动失败时撤下本次标记
 *
 * 完全独立，不依赖任何项目库，仅使用标准库和平台 API。
 */

#include "UpdaterPlatform.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

namespace MMM::Update
{
/// @brief 向标准错误流写入独立更新器诊断。
/// @param message 诊断内容。
///
/// 更新器不链接主项目日志库，stderr 是替换失败时唯一可靠的诊断通道。
/// 使用 string_view 长度写入，消息不必以空字符结束。
void writeStderr(std::string_view message)
{
    // 空消息不调用 C I/O，避免无意义的零长度写入。
    if ( message.empty() ) return;
    // fwrite 不抛出异常；异常路径只做尽力诊断，不递归报告写入失败。
    std::fwrite(message.data(), 1, message.size(), stderr);
}

namespace
{
/// @brief 尽力写入标记，新进程可能在启动后立即读取。
void writeUpdateSuccessMarker(const std::filesystem::path& targetPath)
{
    const auto markerPath = updateSuccessMarkerPath(targetPath);
    if ( markerPath.empty() ) return;
    std::ofstream marker(markerPath);
    // 路径编码由实际平台决定，新版本按同一契约核对安装身份。
    if ( marker.is_open() ) writeMarkerContents(marker, targetPath);
}
}  // namespace

/// @brief 执行独立更新器流程。
/// @param argc 命令行参数数量。
/// @param argv 原生字符表示的下载文件、目标和父进程 PID。
/// @return 更新并启动成功时返回 0，否则返回非零值。
///
/// 主流程假设下载文件已经由主程序完成完整性校验。Windows 与 Linux
/// 均在目标卷准备完整文件， 再用同卷改名发布，macOS 交给完整 bundle
/// 安装事务。替换完成后准备成功标记并启动新版本；启动失败撤下标记。
///
/// 退出码约定：
/// - 参数数量或 PID 非法返回 1，且不触碰下载文件与目标；
/// - 平台目标规范化失败或无法确认父进程退出时返回 1；
/// - 备份、复制、重命名或 bundle 验证失败返回 1；
/// - 新版本启动请求失败返回 1，但保留已安装文件；
/// - 全部步骤成功返回 0。
///
/// 更新器不删除失败时仍可能用于恢复或重试的下载文件。各平台只在新目标
/// 已安全就位后清理下载源；临时和备份路径均从明确目标及当前 PID 推导，
/// 禁止把未验证的命令行目录作为递归删除范围。
int runUpdater(int argc, std::filesystem::path::value_type* argv[])
{
    // 参数数量必须精确，避免把缺失 PID 或多余字符串误当作路径片段。
    if ( argc != 4 ) {
        writeStderr(
            "Usage: MusicMapMaker-Updater <downloaded_file> <target_path> "
            "<parent_pid>\n");
        return 1;
    }

    // Windows 从宽入口直接构造原生路径；POSIX 保留原始字节。
    // 存在性和可操作性在平台替换步骤验证。
    const std::filesystem::path downloadedFile = argv[1];
    std::filesystem::path       targetPath     = argv[2];
    const long                  parentPid      = parseParentPid(argv[3]);
    if ( parentPid == 0 ) {
        // 无效 PID 时不能安全判断主程序是否已释放目标文件。
        writeStderr("Invalid parent_pid\n");
        return 1;
    }

    if ( !normalizeTarget(targetPath) ) return 1;

    // 确认主程序释放目标后才进入平台安装事务。
    const bool parentExited = waitForParent(parentPid);
    if ( !parentExited ) {
        // 正式目标、下载源和成功标记都必须留待下一次安全重试。
        writeStderr("Cannot confirm that the parent process exited\n");
        return 1;
    }
    // 父进程退出后留出短暂时间给系统释放映像和防病毒扫描句柄。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if ( !install(downloadedFile, targetPath) ) return 1;

    // 启动中的新应用可以立即消费标记，不能等启动返回后补写。
    // 若装载或系统请求失败，新应用尚未启动，撤下本次预先写入的标记。
    writeUpdateSuccessMarker(targetPath);

    // 启动失败不回滚新版本，保留更新结果供用户手工启动和诊断。
    if ( !launchTarget(targetPath) ) {
        std::error_code markerError;
        const auto      markerPath = updateSuccessMarkerPath(targetPath);
        if ( !markerPath.empty() )
            std::filesystem::remove(markerPath, markerError);
        if ( markerError )
            writeStderr("Failed to clear update marker: " +
                        markerError.message() + "\n");
        writeStderr("Failed to launch updated application\n");
        return 1;
    }
    return 0;
}

}  // namespace MMM::Update
