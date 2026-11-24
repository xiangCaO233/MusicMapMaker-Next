#pragma once

#include "common/LogicCommandFwd.h"
#include "mmm/metadata/BaseMapMeta.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{
class BeatmapSession;

/// @brief 文件线程计算的路径展示值，不在 UI 回调中探测文件。
struct BeatmapSettingsPath {
    /// @brief 源元数据路径，用于判断同一帧刚编辑的引用是否已有检查结果。
    std::filesystem::path source;
    /// @brief 适合对话框显示的相对或原始 UTF-8 路径。
    std::string display;
    /// @brief 为路径提示保存实际文件位置的 UTF-8 显示文本。
    std::string absoluteDisplay;
    /// @brief 上次后台验证时是否存在。
    bool exists{};
};

/// @brief 可选的项目主音频及其可写回的兼容路径。
struct BeatmapSettingsAudio {
    /// @brief 资源稳定标识。
    std::string id;
    /// @brief 项目资源记录中的原始 UTF-8 路径。
    std::string path;
    /// @brief 预先处理历史工程目录前缀后的路径。
    std::filesystem::path selection;
};

/// @brief 设置页所需数据的独立快照，释放会话锁后才检查文件。
struct BeatmapSettingsSnapshot {
    /// @brief 用弱引用绑定原来的队列和载图实例，不能路由到另一张谱面。
    std::weak_ptr<BeatmapSession> session;
    /// @brief 同一会话重载时也不接受旧快照派发的编辑。
    std::weak_ptr<const BeatMap> beatmap;
    /// @brief 与活动工具栏使用相同的会话代次。
    std::uint64_t generation{};
    /// @brief 本次捕获的完整基础元数据。
    BaseMapMeta metadata;
    /// @brief 原工程根目录；独立谱面不一定有工程。
    std::filesystem::path projectRoot;
    /// @brief 是否存在当前工程，独立于路径是否为空。
    bool hasProject{};
    /// @brief 临时工程不允许导入或重命名资源。
    bool readOnly{};
    /// @brief 谱面、音频、封面及背景对应的路径状态。
    std::array<BeatmapSettingsPath, 4> paths;
    /// @brief 工程登记的 Main 音频，不在 UI 借用项目容器。
    std::vector<BeatmapSettingsAudio> audio;
    /// @brief 工程相对图片候选；仅在请求扫描时填充。
    std::vector<std::string> images;
    /// @brief 工程相对视频候选；仅在请求扫描时填充。
    std::vector<std::string> videos;
    /// @brief 区分未请求扫描和已扫描但候选为空。
    bool scanned{};
};

/// @brief 设置页每帧读取的活动代次与计数，文件慢任务不延迟步进器更新。
struct ActiveBeatmapSettingsState {
    /// @brief 活动谱面代次，用于拒绝其他标签返回的文件查询结果。
    std::uint64_t generation{};
    /// @brief 最新发布的持久化草稿轨数量。
    std::int32_t draftTracks{};
    /// @brief 最新发布的持久化 BGM 轨数量。
    std::int32_t bgmTracks{};
};

/// @brief 获取活动设置状态，谱面未就绪时返回空。
/// @warning UI 每帧调用；只读独立的编辑器值槽，不等待会话更新锁或文件任务。
[[nodiscard]] std::optional<ActiveBeatmapSettingsState>
activeBeatmapSettingsState();

/// @brief 请求逻辑线程复制活动谱面，再由文件线程检查路径。
/// @param scanResources 资源弹层是否需要重新枚举图片和视频。
/// @return 没有活动谱面或请求被更新请求替代时得到空结果。
/// @warning 仅限低频交互；提交后只轮询 future，禁止等待。
[[nodiscard]] std::future<std::shared_ptr<const BeatmapSettingsSnapshot>>
requestBeatmapSettings(bool scanResources);

/// @brief 在引擎循环中捕获一次请求，并将文件阶段交给文件线程。
/// @warning 每轮逻辑更新调用；空闲只读标志，不分配、不加锁、不访问文件系统。
void processBeatmapSettingsRequest();

/// @brief 仅向快照绑定的会话入队；模式和谱面实例在消费时再次校验。
/// @warning 设置交互调用，不获取会话锁或执行文件操作。
void submitBeatmapSettingsCommand(const BeatmapSettingsSnapshot& snapshot,
                                  LogicCommand                   command);

}  // namespace MMM::Logic
