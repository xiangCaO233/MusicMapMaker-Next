#pragma once
namespace MMM::Main
{
/// @brief 在首个配置单例初始化前设置 Android 私有目录与系统 HTTPS 信任。
/// @return 平台路径准备成功时返回 true；资源随后由共享启动同步流程准备。
/// @warning 仅原生入口启动时调用；包含私有目录与证书访问，禁止进入帧循环。
bool prepareAndroidRuntime();

/// @brief 正式窗口创建后允许 Java 展示工程文件授权引导。
/// @warning 仅启动收尾低频调用；JNI 只投递 UI 通知，不等待用户授权。
void notifyAndroidEditorReady();
}  // namespace MMM::Main
