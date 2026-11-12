#pragma once
#include "canvas/TimelineCanvas.h"
#include <memory>
#include <unordered_set>

namespace MMM::Canvas
{
/// @brief 时间线菜单语义，不携带移动控件或桌面快捷键定义。
/// @details 命令统一经过时间线原有业务入口，平台只决定输入意图。
enum class TimingMenuAction {
    Copy,
    Paste,
    Cut,
    Delete,
    Undo,
    Redo,
    SelectAll,
    Clear,
    Replace,
    Add,
    Subtract,
    Edit,
    Snap,
    Seek
};
/// @brief 时间线平台边界；公共画布头只前置声明，不传播移动私有类型。
/// @details 默认实现原样转交桌面输入，安卓派生对象只保存手势和弹层状态。
/// 生命周期随画布，构造后不逐帧创建解释器，也不持有会话所有权。
class TimelineInteractionPresentation
{
public:
    /// @brief 销毁时不等待逻辑线程，画布更新已结束。
    virtual ~TimelineInteractionPresentation() = default;
    /// @brief 将一帧已通过方案路由的输入交给目标平台。
    /// @warning 每帧调用；禁止文件访问、逻辑长锁和逐帧对象分配。
    virtual void update(TimelineCanvas& canvas, const ImVec2& origin,
                        const ImVec2& size, bool hovered, bool focused,
                        bool profileAlreadyChecked);
    /// @brief 待定触点和导航同样占有输入，切换方案前必须全部结束。
    /// @warning 每帧仅访问平台本地状态，不读取逻辑注册表。
    virtual bool hasGesture() const { return false; }
    /// @brief 鼠标可以使用悬浮说明，触屏改由长按参数入口承担。
    /// @warning 每帧只查询平台能力，不分配对象或读取会话状态。
    virtual bool showHoverDetails() const { return true; }
    /// @brief 隐藏视图前允许移动实现取消未提交预览，桌面默认不改变行为。
    virtual void suspend(TimelineCanvas&) {}
    /// @brief 桌面保留默认按钮高度，移动端提供触摸下限而不改全局样式。
    /// @warning 参数窗口可每帧查询，只进行本地尺寸计算。
    virtual float parameterButtonHeight(float) const { return 0; }
    /// @brief 参数窗口尺寸提示；桌面保持既有首开与最小尺寸。
    /// @warning 弹窗帧仅返回两个标量，不读取设备或创建布局对象。
    virtual ImVec2 parameterWindowSize(bool minimum, float) const
    {
        return minimum ? ImVec2(420, 320) : ImVec2(620, 820);
    }
    /// @brief 移动参数正文独立滚动，桌面不创建额外子窗口。
    /// @return 已调用 BeginChild 时返回真，调用者必须无条件配对 EndChild。
    /// @warning 弹窗帧只建立布局，不复制草稿或发布编辑命令。
    virtual bool beginParameterBody(float) { return false; }
    /// @brief 平台输入来源文案，桌面仍使用原鼠标点击位置。
    /// @warning 每帧返回静态翻译键，不构造字符串。
    virtual const char* pointPositionKey(bool snapped) const
    {
        return snapped ? "ui.timeline.event_creator.pos_click_snapped"
                       : "ui.timeline.event_creator.pos_click";
    }
    /// @brief 移动端替换修饰键说明，空键表示继续原桌面说明。
    /// @warning 弹窗帧只返回静态键，不执行工具动作。
    virtual const char* rangeHintKey(bool) const { return nullptr; }

protected:
    /// @brief 只借用当前不可变快照，不复制共享所有权。
    /// @warning 每帧只读取 UI 本地观察指针，禁止跨帧保存或等待逻辑线程。
    static const Common::Render::RenderSnapshot* snapshot(
        TimelineCanvas& canvas);
    /// @brief 显示补偿参与捏合锚点，不能用未经补偿的图像原点。
    /// @warning 每帧只读取已应用的本地标量，不重测布局或同步相机。
    static float contentYOffset(TimelineCanvas& canvas);
    /// @brief 回放已分类的指针边沿，作用域结束还原平台中立标志。
    /// @warning 每帧同步调用；所有编辑仍经过原有批量命令与撤销边界。
    static void route(TimelineCanvas& canvas, const ImVec2& origin,
                      const ImVec2& size, bool hovered, bool focused,
                      bool checked, bool tapOnly, bool finalUpdate);
    /// @brief 接管只丢弃 UI 预览，Timing 拖动在释放之前尚未写入谱面。
    /// @details 选区由移动实现保存起笔基线，取消后单独恢复。
    static void cancel(TimelineCanvas& canvas);
    /// @brief 打开边沿锁存目标和吸附时间，菜单移动不改变粘贴位置。
    static void prepareMenu(TimelineCanvas& canvas, const ImVec2& origin,
                            const ImVec2& size);
    /// @brief 校验谱面实例与冻结菜单身份，不能向重载后的同路径谱面发旧操作。
    /// @warning 菜单帧只比较已发布身份，不访问 ECS 或取得会话长锁。
    static bool validMenu(TimelineCanvas& canvas);
    /// @brief 根据当前选择与目标决定菜单可用性，不读取系统剪贴板。
    /// @warning 每个可见菜单项固定查询，不复制选区或检查外部资源。
    static bool enabled(TimelineCanvas& canvas, TimingMenuAction action);
    /// @brief 激活动作共用已有业务实现，属性编辑须在弹层退出后调用。
    static void perform(TimelineCanvas& canvas, TimingMenuAction action);
    /// @brief 选区只在起笔复制，取消恢复不生成修改命令或撤销历史。
    static std::unordered_set<entt::entity>& selection(TimelineCanvas& canvas);
};
/// @brief 构造期由 CMake 选择唯一平台工厂，不在公共头出现平台宏。
std::unique_ptr<TimelineInteractionPresentation>
createTimelineInteractionPresentation();
}  // namespace MMM::Canvas
