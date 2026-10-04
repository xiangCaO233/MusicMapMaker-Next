#pragma once

#include "mmm/timing/TimingTemplate.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MMM::Canvas
{
/// @brief 模板工具只在一个明确步骤中展示当前需要的操作。
/// @note 查看只读；添加选择来源；两种创建编辑草稿；放置只调整目标。
enum class TimingTemplateEditorPhase {
    /// @brief 浏览和管理个人库中的已存模板。
    View,
    /// @brief 选择新模板的来源，尚未修改草稿或模板库。
    Add,
    /// @brief 从空白或既有模板编辑草稿，保存后返回查看。
    Create,
    /// @brief 审阅从选中时间点捕获的草稿，保存后返回查看。
    FromSelection,
    /// @brief 根据目标谱面的 BPM 检查落点并一次提交完整点组。
    Place,
};

/// @brief 时间点模板的独立工作副本；关闭窗口不会修改谱面或模板库。
/// @details BPM 快照只在打开、刷新和轴切换时捕获，禁止每帧等待会话锁。
/// 预览仅在输入变更时计算，实际提交由逻辑线程使用最新红线定位。
struct TimingTemplateEditorState {
    /// @brief 当前页面决定输入权限和可见操作，不依赖按钮执行顺序。
    TimingTemplateEditorPhase m_phase{ TimingTemplateEditorPhase::View };
    /// @brief 取消创建返回来源选择，取消修改返回查看，均不写入个人库。
    TimingTemplateEditorPhase m_editReturnPhase{
        TimingTemplateEditorPhase::Add
    };
    /// @brief 删除只在查看页确认，首次点击不立即写入模板库。
    bool m_confirmDelete{ false };
    /// @brief 文件内容仅进入待确认列表，取消不会写入个人库。
    std::vector<TimingTemplate> m_imports;
    /// @brief 待确认文件的显示路径，帮助区分重新选择后的候选来源。
    std::string m_importSource;
    /// @brief 添加页当前审阅的导入模板索引，不对应个人库索引。
    int m_importIndex{ 0 };
    /// @brief 同名替换必须明确选择；新建和导入默认保护既有模板。
    bool m_replaceExisting{ false };
    /// @brief 下一帧从稳定窗口 ID 栈打开模态编辑器。
    bool m_requestOpen{ false };
    /// @brief 弹窗显隐状态，独立于时间线画布显隐。
    bool m_open{ false };
    /// @brief 输入变化标志；创建页清除旧提示，放置页按需重建预览。
    bool m_dirty{ false };
    /// @brief 损坏库禁止覆盖，用户修复文件后重新打开才能保存。
    bool m_libraryWritable{ true };
    /// @brief 允许任意基准和有符号偏移的当前模板。
    TimingTemplate m_draft;
    /// @brief 单次打开时读取的个人模板库，显式保存才写磁盘。
    std::vector<TimingTemplate> m_library;
    /// @brief 来源或目标谱面的完整快照，用于轴换算或放置范围提示。
    std::vector<Timing> m_context;
    /// @brief 已经按目标拍轴计算的实际落点预览。
    std::vector<Timing> m_preview;
    /// @brief 名称输入缓冲区，不使用每帧字符串转换。
    std::array<char, 257> m_name{};
    /// @brief 创建时的轴换算参考位置，进入放置时重设为目标基准秒数。
    double m_anchorSeconds{ 0.0 };
    /// @brief 拍偏移显示为多少个 1/N 拍，支持直接输入 -1 和 N=192。
    int m_division{ 192 };
    /// @brief 没有红线时使用目标谱面的首选 BPM。
    double m_fallbackBpm{ 120.0 };
    /// @brief 当前模板库选项，-1 表示未选择已存模板。
    int m_libraryIndex{ -1 };
    /// @brief 轴换算或放置预览错误，错误不自动修改用户输入。
    std::string m_error;
    /// @brief 模板库读写提示，与预览错误分别保存。
    std::string m_storageMessage;
    /// @brief 工作副本来源实例，防止切换谱面后误提交。
    std::uintptr_t m_instanceId{ 0 };
};
}  // namespace MMM::Canvas
