#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"

#include <cmath>

namespace MMM::UI
{
/// @brief 只向历史测试开放手工输入事务与可观察状态。
/// @details 不引入生产环境测试接口；友元只提供状态设置与断言。
/// 使用真实工具方法验证合并、恢复、空操作过滤和切轨清理。
struct BpmMeasurementHistoryTestAccess {
    /// @brief 还原测试期间修改的内存偏好，避免析构时写入用户配置。
    /// @param tool 被测工具视图。
    /// @note 配置变化只留在内存，测试结束前还原原始偏好。
    static void suppressPreferenceSave(BpmMeasurementToolView& tool)
    {
        tool.m_userPreferencesDirty = false;
        // 析构时不得把夹具视图中心写入用户配置文件。
        tool.m_userPreferencesSaveDelaySeconds = 0.0;
    }

    /// @brief 模拟同一音轨上的连续段落编辑和视野移动。
    /// @param tool 被测工具视图。
    /// @return 合并为一条历史且状态符合预期时返回 true。
    /// @note 三次位置变化模拟连续鼠标拖动，只在终点调用 finish。
    /// 同一手势应使 undo 与 redo 栈各发生一次状态转移。
    static bool checkCoalescedGesture(BpmMeasurementToolView& tool)
    {
        tool.m_selectedAudioTrackId = "history-track";
        tool.m_duration             = 60.0;
        tool.beginMeasurementHistoryGesture();
        // 起点快照必须早于所有模拟输入。
        // 三次中间位置代表持续拖动；结束时只能生成单条历史。
        tool.m_viewCenter                    = 4.0;
        tool.m_viewCenter                    = 6.0;
        tool.m_viewCenter                    = 8.0;
        tool.m_measurementHistoryViewChanged = true;
        tool.finishMeasurementHistoryGesture();
        // 聚合视野标识随本次提交消耗，不可传给下一次操作。
        if ( tool.m_measurementUndoHistory.size() != 1 ||
             !tool.canUndoMeasurement() || tool.canRedoMeasurement() ) {
            return false;
        }
        tool.undoMeasurementFromShortcut();
        // 撤销只移动一条历史，不能生成额外 undo 条目。
        if ( std::abs(tool.m_viewCenter -
                      tool.m_measurementRedoHistory.back().before.viewCenter) >
                 1e-9 ||
             !tool.canRedoMeasurement() ) {
            return false;
        }
        tool.redoMeasurementFromShortcut();
        // 重做恢复终点，undo 深度仍为一条。
        return std::abs(tool.m_viewCenter - 8.0) < 1e-9 &&
               tool.canUndoMeasurement() && !tool.canRedoMeasurement() &&
               tool.m_measurementUndoHistory.size() == 1;
    }

    /// @brief 检查播放自动推进不会单独制造历史，切轨清除历史。
    /// @param tool 被测工具视图。
    /// @return 两个边界均正确时返回 true。
    /// @note 普通播放推进未显式设置 seekChanged，不能形成历史。
    /// 切轨须清除未提交手势、undo 与 redo 栈。
    static bool checkNoOpAndClear(BpmMeasurementToolView& tool)
    {
        tool.clearMeasurementHistory();
        // 空历史起点保证后续断言不依赖前一测试的状态。
        tool.beginMeasurementHistoryGesture();
        // 不置 seekChanged 时，后端播放头变化不是用户操作。
        tool.finishMeasurementHistoryGesture();
        // 若播放头推进，此差异仍应被忽略。
        if ( tool.canUndoMeasurement() || tool.canRedoMeasurement() ) {
            return false;
        }
        tool.beginMeasurementHistoryGesture();
        tool.m_markerWidthMs = 96.0;
        // 拍框宽度确实变化才需要生成撤销条目。
        tool.finishMeasurementHistoryGesture();
        if ( !tool.canUndoMeasurement() ) return false;
        tool.clearMeasurementHistory();
        // 清除后旧音轨快捷键不能再次访问历史。
        return !tool.canUndoMeasurement() && !tool.canRedoMeasurement() &&
               !tool.m_measurementGestureBefore.has_value();
    }

    /// @brief 验证异步跳转保存请求值、新操作截断重做分支。
    /// @param tool 被测工具，不加载真实音频后端。
    /// @return 目标时间与历史分支符合用户输入语义时为 true。
    static bool checkSeekAndRedoBranch(BpmMeasurementToolView& tool)
    {
        tool.clearMeasurementHistory();
        tool.beginMeasurementHistoryGesture();
        // 不回读音频后端，模拟逻辑线程尚未消费的用户跳转请求。
        tool.m_measurementHistorySeekChanged    = true;
        tool.m_measurementHistorySeekCanvasTime = 12.5;
        tool.finishMeasurementHistoryGesture();
        if ( tool.m_measurementUndoHistory.size() != 1 ) return false;
        const auto& entry = tool.m_measurementUndoHistory.back();
        // 空 transport 不执行跳转，但冻结的 after 必须仍保存正确目标。
        if ( !entry.seekChanged || entry.after.seekCanvasTime != 12.5 )
            return false;
        tool.undoMeasurementFromShortcut();
        if ( !tool.canRedoMeasurement() ) return false;
        tool.beginMeasurementHistoryGesture();
        // 无变化点击不会截断 redo，只有真正的新编辑才建立新分支。
        tool.finishMeasurementHistoryGesture();
        if ( !tool.canRedoMeasurement() ) return false;
        tool.beginMeasurementHistoryGesture();
        tool.m_markerWidthMs += 4.0;
        tool.finishMeasurementHistoryGesture();
        // 分支重建不应复用已撤销的 seek，新的 redo 栈必须完全为空。
        return tool.canUndoMeasurement() && !tool.canRedoMeasurement();
    }
};
}  // namespace MMM::UI

/// @brief 验证 BPM 工具本地历史合并、恢复、无操作过滤和切轨清理。
/// @return 所有历史行为符合预期时返回 0。
/// @note 测试不创建 ImGui 帧、GPU 纹理或真实音轨资源。
/// 先保存内存偏好，避免历史恢复结果污染个人设置。
int main()
{
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    const auto originalPreferences = settings.bpmMeasurementToolPreferences;
    MMM::UI::BpmMeasurementToolView tool("BpmMeasurementHistoryTest");
    const bool                      passed =
        MMM::UI::BpmMeasurementHistoryTestAccess::checkCoalescedGesture(tool) &&
        MMM::UI::BpmMeasurementHistoryTestAccess::checkNoOpAndClear(tool) &&
        MMM::UI::BpmMeasurementHistoryTestAccess::checkSeekAndRedoBranch(tool);
    // 测试只验证内存中的历史；偏好回滚后不安排实际配置落盘。
    settings.bpmMeasurementToolPreferences = originalPreferences;
    MMM::UI::BpmMeasurementHistoryTestAccess::suppressPreferenceSave(tool);
    return passed ? 0 : 1;
}
