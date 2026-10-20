#include "main/AndroidBootstrap.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "network/AssetSyncService.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace MMM::Main
{
/// @brief 随包资源只在版本变化时展开，配置始终写到应用私有目录。
/// @details 不把 APK 内的虚拟资源路径或 Android content URI 当作普通文件路径。
/// 首次启动在资源初始化阶段使用现有安全 ZIP 解压服务，之后维持低频版本检查。
/// @warning 启动低频路径：包含资源加载、磁盘写入和解压，不能在主帧循环调用。
bool prepareAndroidRuntime()
{
    // JNI 在 SDL_main 前已经建立 AssetManager 和应用目录关联。
    // 这里不能用 /sdcard 等共享路径替代失败结果，否则会改变资源隔离契约。
    // 资源展开必须先于 Config 和日志单例首次解析路径。
    // SDL 会按自己的方向提示更新 Activity；必须在首个 SDL
    // 子系统初始化前固定横屏。
    // 只约束方向以容纳既有桌面布局，不改变触屏点击或手势行为。
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    const char* storage = SDL_GetAndroidInternalStoragePath();
    if ( !storage || !storage[0] ) return false;
    const std::filesystem::path storageRoot = Config::utf8ToPath(storage);
    std::error_code             error;
    // 使用 error_code 传播文件系统失败，避免在无异常构建中触发终止。
    // 目录本身属于本包内部存储，升级 APK 后保留配置和用户文件。
    std::filesystem::create_directories(storageRoot, error);
    if ( error ) return false;
    // CWD 与配置根都落在当前包的私有目录，不继承桌面 HOME 或 /system/bin。
    std::filesystem::current_path(storageRoot, error);
    if ( error || setenv("MMM_CONFIG_ROOT",
                         Config::pathToUtf8(storageRoot / "config").c_str(),
                         1) != 0 )
        return false;
    // AppPaths 从刚设置的根派生资源路径，保持桌面与 Android 的相对布局相同。
    // 皮肤中的字体、图像和 Lua 相对引用因此无需改写。
    const auto assets = Config::AppPaths::assetsRootPath();
    std::filesystem::create_directories(assets, error);
    if ( error ) return false;
    // SDL 从 APK assets 读取，文件名与 Android 打包脚本的生成入口保持一致。
    std::size_t                                revisionSize = 0;
    std::unique_ptr<void, decltype(&SDL_free)> revisionData(
        SDL_LoadFile("mmm-assets.sha256", &revisionSize), SDL_free);
    if ( !revisionData ) {
        XERROR("Android asset revision missing: {}", SDL_GetError());
        return false;
    }
    // 标记比较的是随包 ZIP 内容，而不是 APK 安装时间或构建时间。
    // 只更新原生代码时保持资源版本稳定，避免每次启动重复展开整个资源包。
    const std::string revision(static_cast<const char*>(revisionData.get()),
                               revisionSize);
    // 旧标记不存在时保留空值，正常进入首次安装路径。
    // 文件读取局限于启动阶段，不把校验逻辑放进逐帧皮肤更新。
    std::string installed;
    {
        std::ifstream marker(assets / ".android-bundle.sha256",
                             std::ios::binary);
        std::getline(marker, installed);
    }
    // 哨兵文件也要存在，避免保留标记但资源目录已损坏时错误跳过修复。
    if ( installed == revision &&
         std::filesystem::is_regular_file(
             Config::AppPaths::defaultSkinFilePath(), error) &&
         !error )
        return true;
    // 仅需要更新时才加载大资源包，常规启动只读取短版本标记。
    // SDL 返回的内存必须通过 SDL_free 释放，不能交给 C++ delete。
    std::size_t                                archiveSize = 0;
    std::unique_ptr<void, decltype(&SDL_free)> archiveData(
        SDL_LoadFile("mmm-assets.zip", &archiveSize), SDL_free);
    if ( !archiveData ) {
        XERROR("Android bundled assets missing: {}", SDL_GetError());
        return false;
    }
    // 解包服务接受普通文件路径，因此先写入私有临时 ZIP。
    // APK 内虚拟资产本身不承诺可被 std::filesystem 直接打开。
    // Android 内部存储路径由系统分配，无需申请共享存储权限。
    // 应用卸载后由系统回收这些目录，主机不清理手机共享文件。
    // 升级保留已有标记，只有 ZIP 内容变化时才重新展开。
    // 临时 ZIP 与目标资源目录分离，不会被当作皮肤文件扫描。
    // 固定的随包资源使首屏不依赖网络下载。
    const auto cache = storageRoot / "cache";
    std::filesystem::create_directories(cache, error);
    if ( error ) return false;
    const auto archive = cache / "mmm-assets.zip";
    // 临时 ZIP 只服务本次解压，不能写入共享存储或请求外部存储权限。
    {
        std::ofstream output(archive, std::ios::binary | std::ios::trunc);
        output.write(static_cast<const char*>(archiveData.get()),
                     static_cast<std::streamsize>(archiveSize));
        if ( !output ) return false;
    }
    // 写盘完成立即释放 ZIP 内存，减少后续解压与字体加载的峰值占用。
    // 输出流离开作用域后已经关闭，解包器不会读到尚未刷新的尾部。
    archiveData.reset();
    // 复用已有路径安全校验，拒绝 ZIP 内越界路径和不支持的条目类型。
    // 部分解压失败不发布新标记，下次启动仍能按同一入口重试。
    std::string extractionError;
    if ( !Network::AssetSyncService::extractZipArchive(
             archive, assets, extractionError) ) {
        XERROR("Android bundled asset extraction failed: {}", extractionError);
        return false;
    }
    // 只有全部展开成功才发布版本标记；启动被打断时下一次仍会重新准备资源。
    {
        std::ofstream marker(assets / ".android-bundle.sha256",
                             std::ios::binary | std::ios::trunc);
        marker << revision;
        if ( !marker ) return false;
    }
    // 标记已经发布后清理中间文件；清理失败不撤销可用的资源版本。
    // 该文件只包含随包资源，不涉及工程文件或用户下载目录。
    std::filesystem::remove(archive, error);
    XINFO("Android bundled assets ready: {}", Config::pathToUtf8(assets));
    return true;
}
}  // namespace MMM::Main
