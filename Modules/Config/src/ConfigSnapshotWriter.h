#pragma once

#include <filesystem>
#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{
/// @brief 同步提交完整配置快照，与后台写入共用顺序门禁。
/// @return 实际落盘成功时为真。
/// @warning 低频阻塞接口，仅用于显式保存与手势结束，不在连续拖动中调用。
bool saveConfigSnapshot(const std::filesystem::path& path,
                        nlohmann::json               snapshot);
/// @brief 发布最新不可变快照并立即返回，覆盖未消费的旧请求。
/// @warning 用户布局修改边沿调用；后台线程不访问任何实时配置字段。
/// @note 程序退出时收尾最后请求，不使用固定延时等待或 UI 线程轮询。
void queueConfigSnapshot(const std::filesystem::path& path,
                         nlohmann::json               snapshot);
/// @brief 等待已发布请求完成，供测试与退出阶段核验持久化。
/// @warning 低频阻塞接口，禁止在渲染和逻辑更新中调用。
void flushConfigSnapshots();
}  // namespace MMM::Config
