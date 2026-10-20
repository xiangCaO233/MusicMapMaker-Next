#pragma once
namespace MMM::Main
{
/// @brief 在首个配置单例初始化前设置 Android 私有目录并展开随包资源。
/// @return 所有资源就绪且路径可写时返回 true，否则由 Main 中止启动。
/// @warning 仅原生入口启动时调用；包含文件访问与解压，禁止进入帧循环。
bool prepareAndroidRuntime();
}  // namespace MMM::Main
