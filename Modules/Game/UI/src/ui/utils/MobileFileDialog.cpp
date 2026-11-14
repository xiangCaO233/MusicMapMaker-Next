#include "ui/utils/MobileFileDialog.h"
#include "MobileFileDialogImpl.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <algorithm>
#include <imgui.h>
#include <string>

namespace MMM::UI
{
/// @brief 复用上游打开协议，只在新事务重置局部键盘状态。
/// @param key 调用方用于区分导出、目录或资源选择的稳定事务身份。
/// @param title 仅决定可见标题，不参与文件名草稿所有权。
/// @param filters 沿用原过滤器语法，空指针仍表示目录选择。
/// @param config 原目录、默认文件名及校验侧栏按值交给上游保存。
void MobileFileDialog::OpenDialog(const std::string& key,
                                  const std::string& title, const char* filters,
                                  const IGFD::FileDialogConfig& config)
{
    // 已打开的事务不能因重复请求丢掉输入焦点；只在真正开始时清门闩。
    if ( !IsOpened(key) ) m_filenameFocused = m_compact = false;
    IGFD::FileDialog::OpenDialog(key, title, filters, config);
}
/// @brief 由真实安全区及前帧输入归属判断浏览区是否暂时收起。
/// @warning 每帧固定标量访问；禁止跨 JNI 查询或保存布局。
void MobileFileDialog::m_DrawHeader()
{
    const auto area = workspaceTextInputWindowArea();
    const auto full = workspaceFloatingWindowArea();
    // 两种区域都已扣除常驻外壳；两者高度差只表示临时键盘遮挡。
    // 一个逻辑像素容差避免安全区取整造成静止帧反复开合。
    const bool keyboard = area.m_size.y + 1 < full.m_size.y;
    // 按确认时输入框会先失焦，模式必须保持到键盘完全收起或事务关闭。
    // 否则按钮按下和释放之间恢复文件列表，会把释放事件交给别的控件。
    if ( !keyboard || workspaceAllowsHoverFeedback() )
        m_compact = false;
    else if ( m_filenameFocused &&
              !m_FileDialogInternal.fileManager.inputPathActivated &&
              !m_FileDialogInternal.searchManager.searchInputIsActive &&
              !m_FileDialogInternal.getDialogConfig().sidePane )
        m_compact = true;
    // 自定义侧栏可能负责业务校验，不在紧凑模式中跳过这些校验逻辑。
    // 只隐藏纯浏览内容；文件、目录、过滤器和原生输入状态仍留在同一实例。
    if ( !m_compact ) IGFD::FileDialog::m_DrawHeader();
}
/// @brief 仅暂停被键盘挤压的浏览绘制，原目录对象继续存活。
/// @warning 每帧只选取原有绘制路径，不追加文件系统操作。
void MobileFileDialog::m_DrawContent()
{
    // 暂停目录绘制不会清空选择，也不调用 OpenDialog 重新扫描磁盘。
    // 保持原 Display 的父子窗口结构，文件名控件 ID 不随模式改变。
    if ( !m_compact ) IGFD::FileDialog::m_DrawContent();
}
/// @brief 同一个输入控件跨模式存活，绘制后锁存它的真实焦点。
/// @warning 每帧固定样式压栈，输入缓冲仍由原选择器拥有。
bool MobileFileDialog::m_DrawFooter()
{
    if ( workspaceAllowsHoverFeedback() )
        return IGFD::FileDialog::m_DrawFooter();
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    // 字体与主题共用应用缩放，不根据截图分辨率或固定机型推算。
    const auto padding = ImGui::GetStyle().FramePadding;
    // 输入行与下方按钮均保持 48dp；只压缩非交互间距，不缩字号或命中区。
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        { padding.x,
          std::max(padding.y, (48.f * scale - ImGui::GetFontSize()) * .5f) });
    // 保留横向间隔和过滤器宽度，长文件名继续使用原输入框横向滚动。
    // 已经较大的主题行高不能被此处的触摸下限缩小。
    const auto spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(
        ImGuiStyleVar_ItemSpacing,
        { spacing.x,
          m_compact ? std::min(spacing.y, 2.f * scale) : spacing.y });
    const bool done = IGFD::FileDialog::m_DrawFooter();
    // 原文件名控件更新此值；不能通过 WantTextInput 把路径或搜索输入混进来。
    m_filenameFocused = m_FileDialogInternal.fileInputIsActive;
    // 必须在 Display 结束其正文子窗之前弹栈，避免正常样式被当作泄漏恢复。
    // 不在窗口外等待析构，因上游子窗的 EndChild 已有自己的配对检查。
    ImGui::PopStyleVar(2);
    return done;
}
/// @brief 确认和取消共用原结果状态，不绕过原覆盖确认。
/// @warning 每帧只绘制控件，禁止在这里写文件或取得会话锁。
bool MobileFileDialog::m_DrawValidationButtons()
{
    if ( workspaceAllowsHoverFeedback() )
        return IGFD::FileDialog::m_DrawValidationButtons();
    const float  scale = Config::AppConfig::instance().getWindowContentScale();
    const auto   gap   = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 size{ std::max(1.f,
                                (ImGui::GetContentRegionAvail().x - gap) * .5f),
                       48.f * scale };
    auto&        state = m_FileDialogInternal;
    // 继承侧栏校验与空文件名门禁；不能只为让按钮可点而放宽输出条件。
    ImGui::BeginDisabled(!state.canWeContinue ||
                         state.fileManager.fileNameBuffer[0] == '\0');
    const bool accept = FeedbackButton(TR("ui.common.confirm").data(), size);
    ImGui::EndDisabled();
    ImGui::SameLine(0, gap);
    // 取消不受文件名或侧栏门禁限制，权限丢失时也必须能退出。
    // Escape 沿用原 needToExitDialog，不把系统返回当作隐式确认。
    const bool cancel = FeedbackButton(TR("ui.common.cancel").data(), size) ||
                        state.needToExitDialog;
    // Display 仍消费原 isOk 与返回值，覆盖确认和调用方 Close 不需要旁路。
    // 同帧竞争时取消优先；返回完成态以后仍由原调用方读取路径并关闭。
    if ( accept || cancel ) {
        state.isOk = accept && !cancel;
        return true;
    }
    return false;
}
/// @brief 在移动外壳初始化期绑定进程内长生命周期选择器。
void installMobileFileDialog()
{
    // 安装只发生在移动外壳创建期，不能在每帧替换正在编辑的选择器。
    // 函数静态实例活到进程退出；单例借用它，不转移或复制所有权。
    static MobileFileDialog dialog;
    IGFD::FileDialog::Instance(&dialog);
}
/// @brief 保存行和操作行优先占用键盘上方空间，浏览区可以暂时收起。
/// @details 此处与原模态居中 helper 共用同一安全区域，不能把完整视口
/// 高度作为 Display 的最小值，否则它会覆盖前面设置的键盘上限。
/// @param scale 当前应用倍率，与控件高度和窗口字体保持一致。
/// @warning 热路径只有标量计算；不访问目录、不查询系统输入法或取得会话锁。
ImVec2 mobileFileDialogSize(float scale)
{
    // 桌面不安装移动实例；这个负例也保证公共尺寸调用不改变桌面布局。
    if ( workspaceAllowsHoverFeedback() ) return { 600, 400 };
    const auto available = workspaceTextInputWindowArea().m_size;
    // 高键盘时不能再按百分比额外扣掉几十像素，那正是文件名与按钮需要的空间。
    // 四个逻辑像素只作边缘留白；浏览模式仍保留原来的首选最大高度。
    return { std::min(600.f * scale, available.x * .92f),
             std::min(400.f * scale,
                      std::max(1.f, available.y - 4.f * scale)) };
}
}  // namespace MMM::UI
