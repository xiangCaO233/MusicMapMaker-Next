#pragma once
#include "canvas/TimelineCanvas.h"
#include <memory>
#include <unordered_set>

namespace MMM::Canvas
{
struct TimingFunctionEditorState;
class TimingInterpolationPreview;
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
    /// @brief 平台局部编辑附加层；桌面默认不绘制触控把手。
    /// @warning 每帧只借用画布和快照，不发布数据或分配状态。
    virtual void renderOverlay(TimelineCanvas&, const ImVec2&, const ImVec2&) {}
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
    /// @brief 平台独立的贝塞尔输入；默认继续原鼠标控件和命中范围。
    /// @warning 参数帧只编辑窗口副本，不发布谱面命令或访问会话。
    virtual bool bezierControls(TimingInterpolation&, const ImVec2&);
    /// @brief 移动图表建立独立触点区域；桌面不增加子窗口。
    /// @return 已 BeginChild 时为真，调用者必须配对 EndChild。
    /// @warning 每帧只登记布局，区域外仍交给原页面滚动。
    virtual bool beginCurveSurface(const char*, const ImVec2&) { return false; }
    /// @brief 手绘输入已由平台消费时返回真，默认继续原鼠标状态机。
    /// @warning 连续帧只处理固定格点，不编译函数或写入谱面。
    virtual bool sketchInput(TimingFunctionEditorState&, const ImVec2&,
                             const ImVec2&)
    {
        return false;
    }
    /// @brief 预览视野只改变本地显示；默认继续滚轮、拖动和双击。
    /// @warning 每帧处理固定触点，不触发采样重建或复制共享所有权。
    virtual bool previewInput(TimingInterpolationPreview&, const ImVec2&,
                              const ImVec2&)
    {
        return false;
    }
    /// @brief 图表外的显式导航入口，桌面保留原布局不添加按钮。
    virtual void previewButtons(TimingInterpolationPreview&) {}
    /// @brief 只调整移动图表高度，桌面使用原像素尺寸。
    /// @warning 每帧固定标量计算，不读取设备或修改全局字体。
    virtual float curveGraphHeight(float height, float) const { return height; }
    /// @brief 新参数副本不继承旧图表手势；默认桌面无额外状态。
    virtual void resetCurveInput() {}
    /// @brief 触屏记录最近符号的用法，桌面仍在 Hover 显示原说明。
    virtual bool symbolHelp(const char*, const char*, bool) { return false; }
    /// @brief 在符号网格外显示最近用法，不依赖触屏不存在的悬浮。
    virtual void finishSymbols() {}
    /// @brief 未提交的图表被折叠或隐藏时取消本笔，桌面无额外事务。
    /// @warning 每帧只复核本地图表状态，不发布命令或等待后台。
    virtual void finishCurveFrame(TimingInterpolation&,
                                  TimingFunctionEditorState&)
    {
    }
    /// @brief 数值字段由平台决定排版，默认保留桌面右侧标签与原输入参数。
    /// @warning 每帧只提交控件，修改仍限于窗口副本。
    virtual bool curveDouble(const char*, double*, const char*);
    /// @brief 分拍整数沿用原输入语义，移动端只改变标签位置。
    /// @warning 每帧固定控件，不写应用配置或谱面。
    virtual bool curveInt(const char*, int*);
    /// @brief 下拉列表只负责显示，领域枚举顺序不受翻译影响。
    /// @warning 每帧借用静态短列表，不构造选项容器。
    virtual bool curveCombo(const char*, int*, const char* const*, int);

protected:
    /// @brief
    /// 使用原时间投影；吸附仅在明确释放时执行，连续预览不创建临时拍点表。
    /// @warning 拖动帧只反投影；snap=true 是低频释放边沿，不得用于每帧回放。
    static double rangeTime(TimelineCanvas&, const ImVec2& size, float localY,
                            bool snap);
    /// @brief 将范围边界投影到画布本地像素，包含布局补偿。
    /// @warning 每帧只读取已发布的滚动分段，不扫描注册表。
    static float rangeY(TimelineCanvas&, const ImVec2& size, double time);
    /// @brief 抬手提交完整段落定义，沿用原格式元数据、合法性检查与单次撤销。
    static void commitRange(TimelineCanvas&,
                            const Common::Render::TimingInterpolationElement&);
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
