/// @file AnnotationSubmitTest.cpp
/// @brief 用真实批注弹窗、输入队列与会话命令验证回车提交，不模拟保存回调。
/// @details 谱面仅在内存中创建；不启动音频、图形设备或后台引擎线程。
/// @par 输入与持久化边界
/// - 字符必须由正式输入队列写入控件，测试不得预填 action 私有缓冲。
/// - 换行后检查真实谱面仍为空，排除任何提前保存。
/// - 提交后核对完整正文，不能只以弹窗关闭或批注数量判断成功。
/// - 两种提交偏好分别与主回车、小键盘回车组合，互不借用结果。
/// - 不启动引擎线程，会话仅在每帧末尾显式执行正常命令队列。
/// - 权限和身份在正文有效后改变，保证门禁测试不被空正文掩盖。
/// - 配置仅在隔离进程中改变，退出不调用 AppConfig 保存入口。
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/annotation/BeatmapAnnotation.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/utils/UIWidgetUtils.h"
#include <imgui.h>
#include <memory>
#include <string>

namespace
{
/// @brief 弹窗上下文需要状态接口，测试不向实际应用发布消息。
struct Sink final : MMM::UI::IStatusMessageSink {
    /// @brief 忽略测试进程内的临时状态，不访问日志以外的输出设备。
    /// @param message 测试不展示的状态文本，按值接收后直接释放。
    /// @param durationSeconds 不使用真实墙钟计时，保持 CPU 帧完全可重复。
    void showStatusMessage(std::string message, float durationSeconds) override
    {
    }
};

/// @brief 每个场景独占 CPU ImGui 上下文和一个实际谱面会话。
/// @details 输入全部进入正式 action，只有 update 才把排队命令应用到谱面。
struct Fixture {
    /// @brief 比菜单上下文活得更久，避免借用失效。
    Sink m_sink;
    /// @brief 与主菜单相同的依赖，不安装测试专用的输入或命令代理。
    MMM::UI::MainMenuContext m_context{ m_sink };
    /// @brief 内存谱面，检查的是命令处理后的真实批注记录。
    std::shared_ptr<MMM::BeatMap> m_map = std::make_shared<MMM::BeatMap>();
    /// @brief 当前测试的命令接收端，不与引擎后台循环并发运行。
    std::shared_ptr<MMM::Logic::BeatmapSession> m_session;
    /// @brief 正式批注动作，不创建测试专用的保存或键盘处理器。
    std::unique_ptr<MMM::UI::IMainMenuItemActionHandler> m_action;
    /// @brief 只关闭本测试创建的会话，不保存工作区。
    int m_index{};
    /// @brief 从真实 ImGui 弹窗状态读取，不使用 action 私有标记。
    bool m_open{};

    /// @brief 创建一个真实可选物件，并通过正常载图建立稳定批注目标。
    Fixture()
    {
        // 各场景重新创建上下文，上一弹窗的焦点和输入边沿不能污染下一场景。
        // 默认字体只计算 CPU 几何，不创建平台窗口或上传纹理。
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        // 不继承用户工作区；事件一次消费使每个按下和抬起精确对应测试帧。
        io.IniFilename                  = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.DisplaySize                  = { 1200, 900 };
        io.DeltaTime                    = .02f;
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        // 静音只影响反馈，实际控件和业务命令均保留。
        MMM::UI::SetInteractionFeedbackEnabled(false);
        MMM::UI::Interaction::synchronizeProfile("classic-desktop", true);
        MMM::UI::Interaction::synchronizeProfile("classic-desktop", true);
        // 普通单键避开折线子项选择；固定时间用于产生稳定物件目标。
        // sync 先建立源模型引用，正式载入才能同时生成 ECS 和持久化身份。
        auto& note       = m_map->m_noteData.notes.emplace_back();
        note.m_timestamp = 1000;
        note.m_track     = 0;
        m_map->sync();
        auto& engine = MMM::Logic::EditorEngine::instance();
        // 显式切换到当前会话，避免上一场景关闭后产生的 Logo 占位成为目标。
        m_index = engine.createSession(m_map, "Annotation submit");
        engine.setActiveSessionIndex(m_index);
        m_session = engine.getActiveSession();
        // 初始化队列先于选择；不能让后续 CmdLoadBeatmap 清掉测试目标。
        m_session->update(0, engine.getEditorConfig(), true);
        // 通过正式全选命令建立交互组件与选择索引，不能假设加载器预先创建它们。
        // 夹具只有一个物件，全轨道区消除了未提供鼠标悬浮轨道时的区域歧义。
        engine.pushCommand(MMM::Logic::CmdSelectAll{
            MMM::Logic::SelectAllScope::AllTrackAreas });
        m_session->update(0, engine.getEditorConfig(), true);
        m_action = MMM::UI::createAddSelectedObjectAnnotationAction();
        // 首帧先建立宿主作用域，OpenPopup 随后在与绘制相同的 ID 栈里执行。
        frame();
        // 用户打开动作后多推进一帧，让正常的默认键盘焦点协议完成。
        frame(true);
        frame();
    }

    /// @brief 清理局部 UI 和会话，禁止把工作区或谱面写到磁盘。
    ~Fixture()
    {
        // 动作先于 ImGui 上下文销毁，避免其跨帧状态借用已释放的输入对象。
        // false 禁止关闭会话时刷新项目工作区，夹具不创建任何用户文件。
        m_action.reset();
        MMM::Logic::EditorEngine::instance().closeSession(m_index, false);
        m_session.reset();
        ImGui::DestroyContext();
    }

    /// @brief 输入帧只绘制生产弹窗，结束后处理真实会话命令队列。
    /// @param open 是否模拟用户从菜单执行一次添加批注动作。
    /// @warning 测试固定推进单帧，不使用 sleep 或后台更新线程。
    void frame(bool open = false)
    {
        // 不直接调用批注保存函数，菜单动作自行解析目标、作者和变更权限。
        // 创建与关闭弹窗由 action 完成，测试只读取公开的弹层状态。
        ImGui::NewFrame();
        ImGui::Begin("AnnotationTestHost");
        if ( open ) m_action->execute(m_context, {});
        m_action->renderDeferred(m_context);
        m_open = ImGui::IsPopupOpen(
            TR("ui.edit.add_selected_annotation.title").data());
        ImGui::End();
        ImGui::EndFrame();
        // 只有正式 UI 提交了命令，正文才会出现在实际 BeatMap 的批注列表中。
        m_session->update(
            0, MMM::Logic::EditorEngine::instance().getEditorConfig(), true);
    }

    /// @brief 文本由输入队列进入正文，不能直接改写 action 的缓冲区。
    /// @param text 当前帧输入的 UTF-8 字符，生命周期覆盖立即入队。
    void type(const char* text)
    {
        // 队列消费由下一次 NewFrame 驱动，焦点错误会导致正文丢失并使对照失败。
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
    }

    /// @brief 注入一次完整按下和抬起，支持主回车和小键盘回车。
    /// @param shift 同一帧是否按下 Shift 修饰键。
    /// @param keypad 为真时使用小键盘回车，否则使用主回车。
    void enter(bool shift, bool keypad = false)
    {
        auto&      io  = ImGui::GetIO();
        const auto key = keypad ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
        // 同一帧提交修饰键和主键，保持与平台后端一致的组合语义。
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddKeyEvent(key, true);
        frame();
        // 松开帧再次经过弹窗路由，检查关闭后不会重复保存或穿透。
        io.AddKeyEvent(key, false);
        io.AddKeyEvent(ImGuiMod_Shift, false);
        frame();
    }
};

/// @brief 两种设置都必须让另一组合换行，确认组合只添加一次完整正文。
/// @param shiftSubmit 选择普通 Enter 或 Shift+Enter 为确认组合。
/// @param keypad 使用与主回车等价的小键盘回车。
/// @return 真正保存的批注内容、目标和作者正确，且弹窗关闭时为真。
bool submission(bool shiftSubmit, bool keypad)
{
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    settings.annotationSubmitWithShiftEnter = shiftSubmit;
    settings.defaultCreator                 = "SubmitTest";
    Fixture f;
    // 必须真的打开模态；直接保存空载荷不能替代弹窗输入路径。
    if ( !f.m_open ) {
        // 打开失败单独诊断，不能把没有正文框的结果解释成权限拒绝成功。
        // 同时记录选择数，区分夹具没有选区和模态本身没有出现。
        XERROR("Annotation popup not open: shift={}, keypad={}, selected={}",
               shiftSubmit,
               keypad,
               f.m_session->getContext().selectedNoteEntities.size());
        return false;
    }
    f.type("First");
    f.enter(!shiftSubmit, keypad);
    // 换行必须留在草稿，不能提前生成任何批注或关闭窗口。
    if ( !f.m_open || !f.m_map->m_annotations.empty() ) {
        // 新增记录或弹窗提前关闭都表示换行被错误当成提交。
        // 两个条件独立检查，不能接受关闭弹窗却丢失正文的假成功。
        XERROR(
            "Annotation newline submitted: shift={}, keypad={}, open={}, "
            "count={}",
            shiftSubmit,
            keypad,
            f.m_open,
            f.m_map->m_annotations.size());
        return false;
    }
    f.type("Second");
    // 第二行只能通过先前的换行留在草稿中，提交时不能额外附加尾部换行。
    f.enter(shiftSubmit, keypad);
    // 同时检查唯一数量与内容，避免把插入多余换行的提交误判为成功。
    if ( f.m_open || f.m_map->m_annotations.size() != 1 ) {
        // 正常提交既要产生一条记录，也要关闭输入事务，防止长按重复添加。
        // 保留模式与回车来源诊断，避免主键成功掩盖小键盘路径失效。
        XERROR(
            "Annotation submit failed: shift={}, keypad={}, open={}, count={}",
            shiftSubmit,
            keypad,
            f.m_open,
            f.m_map->m_annotations.size());
        return false;
    }
    const auto& saved = f.m_map->m_annotations.front();
    // 目标类别检查防止误走独立时间戳批注路径而让物件批注仍未修好。
    return saved.m_content == "First\nSecond" &&
           saved.m_author == "SubmitTest" &&
           saved.m_targetKind ==
               MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
}

/// @brief 空正文、空作者和禁止批注权限都不能被键盘提交绕过。
/// @return 三类门禁均保留弹窗且没有写入谱面时为真。
bool rejectedSubmissions()
{
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    settings.annotationSubmitWithShiftEnter = false;
    settings.defaultCreator                 = "SubmitTest";
    Fixture f;
    f.enter(false);
    if ( !f.m_open || !f.m_map->m_annotations.empty() ) return false;
    // 正文有效后单独关闭身份与权限，排除空正文掩盖其他门禁缺失。
    f.type("Draft");
    settings.defaultCreator.clear();
    // 清除作者发生在弹窗打开之后，提交必须读取本帧身份而非打开时缓存。
    f.enter(false);
    if ( !f.m_open || !f.m_map->m_annotations.empty() ) return false;
    settings.defaultCreator = "SubmitTest";
    // 恢复身份后单独撤销权限，确认键和可见按钮必须共用这一安全边界。
    f.m_session->setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::None);
    f.enter(false);
    return f.m_open && f.m_map->m_annotations.empty();
}
}  // namespace

/// @brief 只在隔离测试进程中修改偏好，退出前恢复完整编辑设置。
int main()
{
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    const auto original = settings;
    // 所有样本都执行，失败场景不能通过短路跳过另一种回车或提交偏好。
    bool passed = true;
    // 普通回车和小键盘回车必须在两个模式下保持相同的提交／换行契约。
    passed &= submission(false, false);
    passed &= submission(true, false);
    passed &= submission(false, true);
    passed &= submission(true, true);
    passed &= rejectedSubmissions();
    settings = original;
    if ( !passed ) XERROR("Annotation Enter submit/newline regression failed");
    return passed ? 0 : 1;
}
