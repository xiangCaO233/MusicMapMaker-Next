#pragma once

#include "ui/ISubView.h"
#include "ui/layout/box/CLayBox.h"
#include <cstdint>
#include <deque>
#include <filesystem>
#include <vector>

namespace MMM::UI
{

class BeatMapManagerView : public ISubView
{
public:
    BeatMapManagerView(const std::string& subViewName) : ISubView(subViewName)
    {
    }
    BeatMapManagerView(BeatMapManagerView&&)                 = default;
    BeatMapManagerView(const BeatMapManagerView&)            = default;
    BeatMapManagerView& operator=(BeatMapManagerView&&)      = delete;
    BeatMapManagerView& operator=(const BeatMapManagerView&) = delete;
    ~BeatMapManagerView() override                           = default;

    /// @brief 内部绘制逻辑 (Clay/ImGui)
    /// @param layoutContext 当前 Clay/ImGui 布局上下文。
    /// @param sourceManager 打开新建谱面向导所需的 UI 管理器。
    /// @warning UI 热路径：谱面管理器可见时每帧执行；
    /// 避免额外增加项目资源遍历和所有权复制。
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

    /// @brief 获取谱面管理器中不可再换行控件所需的最小内容尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 谱面管理器最小内容尺寸。
    /// @warning UI 热路径：子视图可见时每帧查询；仅保留轻量文本测量。
    ImVec2 getMinContentSize(float dpiScale) const override;

    /// @brief 谱面表格文件系统元数据缓存。
    struct FileMetadata {
        /// @brief 文件大小字节数；读取失败时保持为 0。
        std::uintmax_t size{ 0 };

        /// @brief 是否成功读取文件大小。
        bool hasSize{ false };

        /// @brief 文件最后修改时间；读取失败时不参与精确排序。
        std::filesystem::file_time_type lastWriteTime{};

        /// @brief 是否成功读取最后修改时间。
        bool hasLastWriteTime{ false };
    };

private:
    /// @brief 谱面表格排序字段。
    enum class BeatmapSortKey {
        /// @brief 按谱面名称排序。
        Name,

        /// @brief 按谱面文件类型排序。
        Type,

        /// @brief 按谱面相对路径排序。
        Path,

        /// @brief 按谱面文件大小排序。
        Size,

        /// @brief 按谱面文件最后修改时间排序。
        ModifiedTime
    };

    /// @brief 谱面表格排序方向。
    enum class SortDirection {
        /// @brief 升序。
        Ascending,

        /// @brief 降序。
        Descending
    };

    bool m_showBeatmapList = true;

    /// @brief 当前谱面表格排序字段。
    BeatmapSortKey m_beatmapSortKey{ BeatmapSortKey::Name };

    /// @brief 当前谱面表格排序方向。
    SortDirection m_beatmapSortDirection{ SortDirection::Ascending };

    /// @brief 排序后的谱面索引缓存，避免改变项目内谱面顺序。
    std::vector<size_t> m_sortedBeatmapIndices;

    /// @brief 与项目谱面数组下标对齐的文件元数据缓存。
    std::vector<FileMetadata> m_beatmapFileMetadata;

    /// @brief 上次构造排序缓存时的谱面数量。
    size_t m_cachedBeatmapCount{ 0 };

    /// @brief 上次构造排序缓存时的项目指针，仅用于识别项目切换。
    const void* m_cachedBeatmapProject{ nullptr };

    /// @brief 谱面排序缓存是否需要重建。
    bool m_beatmapSortCacheDirty{ true };

    // --- 谱面管理相关 ---
    std::string m_manageBeatmapPath;
    bool        m_openManageModal{ false };

    // --- 布局池 (用于避免热路径堆分配) ---
    std::deque<CLayHBox> m_rows;
    std::deque<CLayVBox> m_vboxes;

    CLayHBox& getRow(size_t index)
    {
        while ( m_rows.size() <= index ) {
            m_rows.emplace_back();
        }
        return m_rows[index];
    }
};

}  // namespace MMM::UI
