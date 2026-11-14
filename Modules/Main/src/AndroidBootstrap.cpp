#include "main/AndroidBootstrap.h"
#include "config/Utf8Path.h"
#include "graphic/mobile/MobileKeyboard.h"
#include "graphic/window/BackNavigation.h"
#include "graphic/window/InputDevices.h"
#include "log/colorful-log.h"
#include "network/CurlHandle.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <filesystem>
#include <jni.h>
#include <string>

namespace MMM::Main
{
namespace
{
/// @brief 在网络线程创建前复制平台默认信任锚，不保留跨线程 JNI 引用。
/// @return 平台异常、方法缺失或空信任集均返回空串，由网络层拒绝 HTTPS 请求。
/// @details 平台负责选择当前 Android 版本的证书来源，原生层不猜测系统目录。
/// @note 每次进程启动重新取样，不在请求工作线程读取可变的平台信任状态。
/// @warning 仅启动阶段执行一次，包含平台证书枚举与一次 PEM 内存复制。
std::string readSystemCaCertificates()
{
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    // JNIEnv 只属于当前 SDL 线程，不得保存到网络层供其他线程访问。
    if ( !env ) return {};
    // 局部帧覆盖以下所有 Java 引用，提前返回也不能泄漏 Activity 或数组。
    if ( env->PushLocalFrame(8) < 0 ) {
        env->ExceptionClear();
        return {};
    }
    std::string certificates = [env]() -> std::string {
        // SDL 返回局部引用，后续整个读取过程都位于上面的局部帧内。
        auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
        if ( !activity || env->ExceptionCheck() ) return {};
        auto type = env->GetObjectClass(activity);
        // 每次 JNI 调用后先检查挂起异常，不能带着异常继续解析方法或数组。
        if ( !type || env->ExceptionCheck() ) return {};
        // 使用实际 Activity 的类加载器，原生工作线程不自行猜测应用类名。
        auto method = env->GetMethodID(type, "systemCaCertificates", "()[B");
        if ( !method || env->ExceptionCheck() ) return {};
        auto data =
            static_cast<jbyteArray>(env->CallObjectMethod(activity, method));
        if ( !data || env->ExceptionCheck() ) return {};
        auto size = env->GetArrayLength(data);
        if ( env->ExceptionCheck() ) return {};
        // 正常空数组保留为空值，由调用方区分信任源不可用而非放宽验证。
        // 显式长度复制保证 PEM 尾部不依赖 Java 数组以 NUL 结束。
        std::string result(static_cast<std::size_t>(size), '\0');
        env->GetByteArrayRegion(
            data, 0, size, reinterpret_cast<jbyte*>(result.data()));
        return result;
    }();
    // Java 检查型失败在 JNI 边界转为显式空值，不传播到无异常 C++ 路径。
    if ( env->ExceptionCheck() ) {
        env->ExceptionClear();
        certificates.clear();
    }
    // 只有原生值越过本函数边界；Activity、Class 和 byte[] 均在此释放。
    env->PopLocalFrame(nullptr);
    return certificates;
}
}  // namespace

/// @brief 为共享启动下载流程绑定 Android 私有目录与 HTTPS 信任。
/// @details 不创建资源目录，也不改动旧版已解压的资源；下载和校验交给公共入口。
/// @warning 仅启动阶段执行目录和平台证书读取，不能在主帧循环调用。
bool prepareAndroidRuntime()
{
    // JNI 在 SDL_main 前已经建立 AssetManager 和应用目录关联。
    // 这里不能用 /sdcard 等共享路径替代失败结果，否则会改变资源隔离契约。
    // 私有配置根必须先于 Config 和日志单例首次解析路径。
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
    // 配置根确定后才允许首次日志输出，避免日志单例缓存错误的桌面目录。
    // 信任快照必须先于任何后台请求发布；资源已缓存时也不能跳过。
    // 失败只影响联网，离线编辑器仍可启动，绝不通过关闭校验继续连接。
    if ( Network::initializeCurlTrust(readSystemCaCertificates()) )
        XINFO("Android system TLS trust anchors initialized");
    else
        XERROR("Android system TLS trust anchors could not be initialized");
    // 资源目录由共享的启动同步服务管理，首次安装不能提前创建空 assets 目录。
    // 否则同步服务会把它当作已有安装，无法选择完整资源包的冷启动下载路径。
    // 已有版本的配置、皮肤和随包解压标记保持原位，联网更新与离线回退都可复用。
    return true;
}

/// @brief 资源下载完成后才开放工程文件授权引导，下载本身无需共享存储权限。
/// @warning 启动低频 JNI 调用；只投递 Java UI 任务，不阻塞或等待用户选择。
void notifyAndroidEditorReady()
{
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if ( !env ) return;
    // 所有 Activity 和 Class 引用只服务这次通知，不跨线程保存。
    if ( env->PushLocalFrame(4) < 0 ) {
        env->ExceptionClear();
        return;
    }
    // 通知只借用当前 SDL Activity；资源和工程的所有权都保留在原生启动流程。
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    // 每个 JNI 阶段先确认没有挂起异常，避免把可恢复通知失败升级为进程错误。
    if ( activity && !env->ExceptionCheck() ) {
        auto type = env->GetObjectClass(activity);
        if ( type && !env->ExceptionCheck() ) {
            auto method =
                env->GetMethodID(type, "notifyEditorResourcesReady", "()V");
            if ( method && !env->ExceptionCheck() )
                env->CallVoidMethod(activity, method);
        }
    }
    if ( env->ExceptionCheck() ) {
        // 不影响已准备成功的资源；显式文件设置入口仍可请求授权。
        env->ExceptionClear();
        XWARN("Could not notify Android editor resource readiness");
    }
    // 本地引用在 UI 任务投递后即可释放，Java lambda 由 Activity 自身管理。
    env->PopLocalFrame(nullptr);
}
}  // namespace MMM::Main

/// @brief JNI 入口直接链接进 libmain，避免静态归档隐藏导出符号。
/// @warning Activity 线程仅投递导航，不在此执行界面或退出流程。
extern "C" JNIEXPORT void JNICALL
Java_top_xiang233_musicmapmaker_MainActivity_requestNativeBack(JNIEnv*, jclass)
{
    MMM::Graphic::requestMobileBackNavigation();
}

/// @brief JNI 只发布实际指针设备能力，设置显隐和方案切换由 SDL UI 线程处理。
/// @warning Activity 设备通知低频调用，不访问原生窗口或持有逻辑锁。
extern "C" JNIEXPORT void JNICALL
Java_top_xiang233_musicmapmaker_MainActivity_updateMouseConnection(
    JNIEnv*, jclass, jboolean connected)
{
    // Activity 通知可能早于 SDL 窗口构造，发布入口必须与窗口指针解耦。
    // 不附带资源所有权；下一 UI 帧会应用设备策略和安全切换协议。
    MMM::Graphic::publishExternalMouse(connected == JNI_TRUE);
}

/// @brief 键盘 JNI 直接导出于 libmain，静态 Graphic 归档继续保持隐藏符号。
/// @details 平台线程只复制几何，不读取 JNIEnv 或保存任何 Java 对象。
/// @warning Activity Insets／动画回调仅发布独立快照，不操作 UI、窗口或业务锁。
extern "C" JNIEXPORT void JNICALL
Java_top_xiang233_musicmapmaker_SoftKeyboard_publishOcclusion(JNIEnv*, jclass,
                                                              jint bottom,
                                                              jint height)
{
    // 跟原生窗口生命周期解耦，首次 Insets 可以早于 SDL 创建 Surface。
    MMM::Graphic::publishMobileKeyboardOcclusion(bottom, height);
}
