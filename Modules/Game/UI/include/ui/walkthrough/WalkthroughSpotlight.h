#pragma once

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::UI::Walkthrough
{
/// @brief 突出当前演练目标，并用可确认提示推进配置中的目标阶段。
///
/// 引导目标使用与本地化文本无关的语义 ID。一个步骤可以提供多个按流程排列的
/// 候选目标。正常演练按可见目标推进，显式返回时锁定回看阶段，避免已完成的
/// 业务状态把用户立即推回后续目标。
class Spotlight
{
public:
    /// @brief 当前解析目标的屏幕空间边界，供诊断和无 GPU 测试使用。
    struct TargetBounds {
        ImVec2 minimum{};  ///< 合并后的左上角。
        ImVec2 maximum{};  ///< 合并后的右下角。
    };

    /// @brief 开始新 UI 帧并清除上一帧上报的易失控件几何。
    /// @warning UI 热路径：每帧调用，只复位值状态，不释放目标配置容量。
    void beginFrame();

    /// @brief 启动或替换当前引导步骤。
    /// @param targets 按流程先后排列的语义目标 ID，后出现的可见目标优先。
    /// @param prompt 纯文字步骤的操作提示或目标旁的说明气泡。
    /// @param previousStepAvailable 所属路线是否还有前一个可引导步骤。
    /// @param reviewing 是否由返回操作进入；回看不继承旧业务完成状态。
    /// @param requiresAction 是否禁止“知道了”跳过当前目标。
    /// @warning 用户显式进入引导时调用；允许复制字符串，不得每帧重复启动。
    void start(const std::vector<std::string>& targets, std::string prompt,
               bool previousStepAvailable = false, bool reviewing = false,
               bool requiresAction = false);

    /// @brief 当前目标或所属路线存在前序步骤时允许返回。
    bool canGoBack() const;
    /// @brief 回退一个目标；当前为首目标时向路线持有者请求前一步。
    /// @warning 仅执行业务层显式登记的练习回滚，不清理学习记录。
    void requestPrevious();
    /// @brief 当前步骤本次启动的唯一身份，供绘制命令标记产物。
    std::uint64_t stepToken() const { return m_stepToken; }
    /// @brief 登记当前目标的练习回滚；只在实际成功提交时调用。
    void registerRollback(std::string_view      targetId,
                          std::function<void()> rollback);
    /// @brief 路线持有者消费一次跨步骤返回请求。
    bool consumePreviousStepRequest();
    /// @brief 回看阶段不接受已有业务状态驱动的自动推进。
    bool reviewing() const { return m_reviewing; }

    /// @brief 结束当前引导并立即停止后续目标采集。
    void stop();

    /// @brief 查询是否存在已启动的引导。
    [[nodiscard]] bool active() const;

    /// @brief 查询当前步骤的全部目标是否已经完成。
    /// @return 业务成功或“知道了”完成最后目标后返回 true，直到 start 或 stop。
    [[nodiscard]] bool completed() const;

    /// @brief 查询当前尚待完成的阶段是否正对应指定语义目标。
    /// @param targetId 与演练配置中的 targets 项一致。
    /// @return 引导活动、未完成且该目标恰为当前阶段时返回 true。
    /// @warning UI 热路径：仅比较当前阶段字符串，不查找控件或分配内存。
    [[nodiscard]] bool awaitingTarget(std::string_view targetId) const;

    /// @brief 标记当前引导仍由本帧可见的演练页面持有。
    /// @warning UI 热路径：只写布尔值，不创建窗口或改变焦点。
    void keepAlive();

    /// @brief 确认当前可见阶段，隐藏其遮罩并等待后续目标出现。
    /// 最后一个目标被确认时结束本次引导；没有可见目标时不改变流程。
    void acknowledgeCurrentStage();

    /// @brief 回看强制操作步骤且目标已满足时，允许显式确认继续。
    /// @warning UI 热路径：业务目标须每帧重新报告，不能沿用旧快照判断。
    void reportReviewedActionSatisfied();

    /// @brief 通知突出层某个语义目标已经由业务逻辑正确完成。
    /// @param targetId 与演练配置中的 targets 项一致。
    /// @warning 只更新引导状态，不执行控件动作；调用方必须先确认业务成功。
    /// @param explicitAction 是否为回看后新发生的操作，而非持续满足的状态。
    void completeTarget(std::string_view targetId, bool explicitAction = false);

    /// @brief 用最近提交的 ImGui 控件矩形上报语义目标。
    /// @param targetId 与演练配置中的 targets 项一致。
    /// @warning 必须紧跟目标控件调用；仅在目标属于当前流程时保存矩形。
    void reportLastItem(std::string_view targetId);

    /// @brief 上报自定义区域，使非标准控件也能参与突出引导。
    /// @param targetId 与演练配置中的 targets 项一致。
    /// @param minimum 屏幕空间左上角。
    /// @param maximum 屏幕空间右下角。
    /// @param viewport 区域所在视口；为空时使用当前窗口视口。
    /// @param drawOutline 是否绘制 Spotlight 自身的脉冲外框。
    /// @param fallback 同一目标的画布实体可见时，由实体矩形替换此备用入口。
    /// @warning UI 热路径：仅比较当前目标列表并复制固定大小几何。
    void reportTarget(std::string_view targetId, const ImVec2& minimum,
                      const ImVec2& maximum, ImGuiViewport* viewport = nullptr,
                      bool drawOutline = true, bool fallback = false);

    /// @brief 为当前目标保留另一个不暗化的操作区域，例如布局设置面板。
    /// @param targetId 当前阶段的语义目标。
    /// @param minimum 额外亮区的屏幕空间左上角。
    /// @param maximum 额外亮区的屏幕空间右下角。
    /// @param viewport 亮区所在视口；为空时使用当前窗口视口。
    /// @warning UI 热路径：只保存一个固定大小矩形，不合并到主目标。
    void reportCompanionRegion(std::string_view targetId, const ImVec2& minimum,
                               const ImVec2&  maximum,
                               ImGuiViewport* viewport = nullptr);

    /// @brief 绘制暗化遮罩、目标描边和带确认按钮的引导提示。
    /// @param dpiScale 当前内容缩放，用于逻辑间距和线宽。
    /// @param acknowledgeLabel 当前语言的阶段确认按钮文本。
    /// @param previousLabel 当前语言的返回按钮文本；为空时不绘制该按钮。
    /// @warning UI 热路径：每帧创建固定 ID 的小提示窗口，不拦截窗口外输入。
    void render(float dpiScale, const char* acknowledgeLabel,
                const char* previousLabel = nullptr);

    /// @brief 返回本帧最终采用的目标 ID，供诊断和无 GPU 测试使用。
    /// @return 没有可见候选目标时返回空视图。
    [[nodiscard]] std::string_view resolvedTargetId() const;

    /// @brief 返回本帧解析目标的合并矩形。
    /// @return 没有可见候选目标时返回空值。
    [[nodiscard]] std::optional<TargetBounds> resolvedTargetBounds() const;

    /// @brief 返回本帧确认按钮中心，供自动化输入与 UI 诊断使用。
    /// @return 没有目标遮罩或按钮未提交时返回空值。
    [[nodiscard]] std::optional<ImVec2> acknowledgeButtonCenter() const;
    /// @brief 本帧上一步按钮中心，用于无 GPU 输入回归测试。
    [[nodiscard]] std::optional<ImVec2> previousButtonCenter() const;

private:
    /// @brief 突出引导逐帧状态；阶段完成后只能单调向后推进。
    enum class State : std::uint8_t {
        Inactive,      ///< 没有正在运行的引导。
        Waiting,       ///< 等待当前阶段目标在本帧上报。
        Highlighting,  ///< 当前阶段拥有本帧有效矩形，可以绘制高亮层。
        Completed      ///< 当前步骤已完成，等待路线会话衔接下一步。
    };

    /// @brief 当前帧选中的目标矩形及所属视口。
    struct Anchor {
        /// @brief 候选目标在配置列表中的索引，数值越大优先级越高。
        std::size_t priority{ 0 };
        /// @brief 目标屏幕空间左上角。
        ImVec2 minimum{};
        /// @brief 目标屏幕空间右下角。
        ImVec2 maximum{};
        /// @brief 只借用当前帧有效的 ImGui 视口。
        ImGuiViewport* viewport{ nullptr };
        /// @brief 是否绘制 Spotlight 外框；自绘精确目标框时可关闭。
        bool drawOutline{ true };
        /// @brief 备用设置入口只在没有画布实体时生效，不与实体合并。
        bool fallback{ false };
    };

    /// @brief 完成指定目标阶段并保持状态机单调前进。
    /// @param priority 已由业务结果或“知道了”确认的目标索引。
    void completeStage(std::size_t priority);

    /// @brief 当前引导候选目标，仅在用户进入另一引导时替换。
    std::vector<std::string> m_targets;
    /// @brief 一个目标的业务补偿，不持有画布或页面指针。
    struct Rollback {
        std::string           targetId;  ///< 原始绘制目标，不依赖界面标签。
        std::function<void()> action;    ///< 发布定向撤销命令的低频回调。
    };
    /// @brief 本轮尚未回退的绘制产物；正常结束只释放，不撤销成果。
    std::vector<Rollback> m_rollbacks;
    /// @brief 逐次启动递增且停止时不复位，避免旧历史匹配重练步骤。
    std::uint64_t m_stepToken{ 0 };
    /// @brief 消费指定目标的全部回滚，重复返回不会触发普通撤销。
    void rollbackTarget(std::string_view targetId);
    /// @brief 当前引导提示文本，来自已验证的演练配置。
    std::string m_prompt;
    /// @brief 本帧优先级最高的可见目标。
    std::optional<Anchor> m_anchor;
    /// @brief 当前目标的额外可操作亮区，与主目标分开挖孔。
    std::optional<Anchor> m_companion;
    /// @brief 当前目标索引；正常自动推进，只有显式返回才减小。
    std::size_t m_stage{ 0 };
    /// @brief 路线持有者提供的跨步骤返回能力。
    bool m_previousStepAvailable{ false };
    /// @brief 待路线持有者消费的返回请求，重复点击不会累积。
    bool m_previousStepRequested{ false };
    /// @brief 显式回看期间锁定目标，不被后续可见目标或业务完成抢占。
    bool m_reviewing{ false };
    /// @brief 当前逐帧引导状态。
    State m_state{ State::Inactive };
    /// @brief 当前帧演练页面是否仍可见，防止关闭页面后残留遮罩。
    bool m_keepAlive{ false };
    /// @brief 鼠标左键上一帧状态，用于不依赖模态 HoveredWindow 的边沿判断。
    bool m_acknowledgeMouseWasDown{ false };
    /// @brief 左键是否从确认按钮内按下且尚未拖出按钮矩形。
    bool m_acknowledgePressed{ false };
    /// @brief 当前步骤只能由业务调用 completeTarget 完成。
    bool m_requiresAction{ false };
    /// @brief 回看时本帧已核实删除目标完成，不自动跳过当前步骤。
    bool m_reviewedActionSatisfied{ false };
    /// @brief 本帧实际提交的确认按钮中心，下一帧开始时失效。
    std::optional<ImVec2> m_acknowledgeButtonCenter;
    /// @brief 返回按钮的模态补充点击状态，与确认按钮相互独立。
    bool m_previousMouseWasDown{ false };
    /// @brief 返回按钮内按下后未拖出的手势锁存。
    bool m_previousPressed{ false };
    /// @brief 当前帧返回按钮的实际中心，不沿用旧布局。
    std::optional<ImVec2> m_previousButtonCenter;
};
}  // namespace MMM::UI::Walkthrough
