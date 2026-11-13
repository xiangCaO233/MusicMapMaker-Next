// 布局方案操作同时消费视觉值和编辑偏好，不依赖单例头展开它们。
#include "ui/imgui/manager/LayoutPresetControls.h"
#include "common/platform/DocumentStorage.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/LayoutPresets.h"
#include "config/Utf8Path.h"
#include "config/VisualConfig.h"
#include "logic/EditorEngine.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include "ui/workspace/MobileInputEditor.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <imgui.h>
#include <initializer_list>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 仅保存界面状态；方案内容由 AppConfig 统一拥有。
/// @note 文件对话框结束后也使用此状态回显，不存储方案元素指针。
struct PresetControlsState {
    /// @brief 复制或重命名的草稿，固定容量避免帧内扩容。
    std::array<char, 256> m_name{};
    /// @brief 最近一次操作反馈，成功和失败均可明确识别。
    std::string m_status;
    /// @brief 移动端名称事务：零为空闲，一为复制，二为改名，三为删除确认。
    int m_action{};
    /// @brief 冻结目标名称，外部切换预设后不能提交旧目标的名称。
    std::string m_target;
    /// @brief 新弹窗仅请求一次文本聚焦，Enter 不执行管理动作。
    bool m_focus{};
    /// @brief 文件选择在途时不重复调度，回调不捕获窗口对象。
    bool m_filePending{};
};
/// @brief 当前客户端的方案管理状态，不参与项目序列化。
PresetControlsState state;
/// @brief 发布布局给全部会话，并检查实际保存结果。
/// @warning 仅用于低频用户操作，会发出配置更新并写入磁盘。
/// @details 先更新引擎再持久化，失败提示明确区分应用与保存。
/// 切换只发布配置，不产生谱面撤销记录或修改资源文件。
/// 调用前实时 VisualConfig 已经恢复目标方案的全部布局。
/// 磁盘失败保留当前内存现场，用户仍可导出以保留布局。
void publish()
{
    auto& app = Config::AppConfig::instance();
    // 配置发布驱动画布重新计算透视、轨道布局和交互坐标。
    Logic::EditorEngine::instance().setEditorConfig(app.getEditorConfig());
    state.m_status =
        app.save() ? "布局预设已保存。" : "布局已应用，但配置保存失败。";
}
/// @brief 处理已经确认的 UTF-8 路径，取消选择不调用此函数。
/// @note 导出读取确认时的实时布局，导入只有校验成功才发布。
/// @param filename 文件选择器返回的 UTF-8 完整路径。
/// @param exporting 为真时导出当前项，否则导入新项。
/// @details Windows 路径通过统一 UTF-8 转换函数处理，避免中文目录乱码。
/// 导入结果只在成功后通知引擎，失败内容直接反馈给用户。
/// 文件格式与选择器过滤器统一使用 mmlayout 扩展名。
void completeFile(const std::string& filename, bool exporting)
{
    auto& app     = Config::AppConfig::instance();
    auto& library = app.getLayoutPresets();
    auto  path    = Config::utf8ToPath(filename);
    // 在文件服务之前已完成路径编码转换，服务层只接收原生 path 值。
    if ( exporting ) {
        // 扩展名独立于方案名称，中文名称不参与路径拼接规则。
        if ( path.extension() != ".mmlayout" ) path += ".mmlayout";
        library.captureCurrent(app.getVisualConfig());
        std::string error;
        // Android SAF 先返回工作副本，writer 关闭后还须发布到授权文档。
        // 普通本机路径和桌面在同一接口中直接成功，不新增第二套文件格式。
        const bool written =
            Config::exportLayoutPreset(path, library.current(), error);
        const bool published = written && Common::publishDocument(path, error);
        state.m_status       = published ? "布局预设已导出。" : error;
    } else {
        std::string error;
        // 失败时保持原方案、当前选择和实时画布全部不变。
        if ( library.importFile(path, app.getVisualConfig(), error) )
            publish();
        else
            state.m_status = error;
    }
}
/// @brief 按客户端偏好打开原生或内置文件选择器。
/// @warning 原生选择器是用户触发的低频阻塞调用；内置选择器逐帧推进。
/// @param exporting 区分打开和保存动作，不在弹窗期间缓存方案引用。
/// @details 原生对话框路径由 NFD 分配，所有完成分支均配对释放。
/// 内置对话框保留独立身份，下一帧由公共推进函数收取确认结果。
/// 取消不报告失败，也不覆盖已有布局。
void openFile(bool exporting)
{
    auto& app = Config::AppConfig::instance();
    state.m_status.clear();
    if ( state.m_filePending ) return;
    if ( NativeFileDialog::requiresSystemPicker() ||
         app.getEditorSettings().filePickerStyle ==
             Config::FilePickerStyle::Native ) {
        PlayPopupOpenFeedback();
        // 回调完成之前状态跨帧保留，窗口暂时隐藏也不会重开同一个选择器。
        // 不持有 AppConfig 库元素引用，导入可能重新分配整个预设数组。
        state.m_filePending = true;
        const nfdu8filteritem_t filter{ "MusicMapMaker Layout", "mmlayout" };
        // 完成回调在有效 UI 帧执行；静态状态不持有布局面板或临时路径指针。
        const auto completed = [exporting](nfdresult_t        result,
                                           const std::string& path) {
            // 取消也解除等待，但不清空布局或报告一次虚假的文件写入成功。
            // 只有非空正常结果才交给现有格式校验和文档发布路径。
            state.m_filePending = false;
            if ( result == NFD_OKAY && !path.empty() )
                completeFile(path, exporting);
            else if ( result != NFD_CANCEL )
                state.m_status = "无法打开文件选择器。";
        };
        if ( exporting )
            NativeFileDialog::saveFileAsync(
                &filter, 1, nullptr, "layout.mmlayout", completed);
        else
            NativeFileDialog::openFileAsync(&filter, 1, nullptr, completed);
        return;
    }
    IGFD::FileDialogConfig config;
    config.path              = ".";
    config.countSelectionMax = 1;
    config.fileName          = exporting ? "layout.mmlayout" : "";
    config.flags =
        ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
    // 稳定 ID 与调色盘选择器分离，读取结果时不会误消费其他窗口。
    ImGuiFileDialog::Instance()->OpenDialog(
        exporting ? "LayoutPresetExport" : "LayoutPresetImport",
        exporting ? "导出布局预设" : "导入布局预设",
        ".mmlayout",
        config);
    PlayPopupOpenFeedback();
}
/// @brief 移动端管理动作保持草稿与库分离，只有确认按钮提交。
/// @param action 复制、重命名或删除；库中稳定名称只用于提交前校验。
/// @warning 只在按钮激活时复制短名称，不创建后台任务。
void beginMobileAction(int action)
{
    // 动作只在可见管理按钮激活时创建；输入草稿不借用库元素的地址。
    // 后续复制、导入或删除可能让 vector 重分配，稳定名称才可跨帧校验。
    const auto& preset =
        Config::AppConfig::instance().getLayoutPresets().current();
    state.m_action = action;
    state.m_target = preset.m_name;
    state.m_name.fill(0);
    // 复制默认留空以便命名；重命名预填原值，不在输入期间改变稳定身份。
    if ( action == 2 ) {
        const auto count =
            std::min(preset.m_name.size(), state.m_name.size() - 1);
        std::copy_n(preset.m_name.data(), count, state.m_name.data());
    }
    // 删除没有可编辑字段，因此不主动唤起输入法。
    // 复制和重命名只请求第一次聚焦，避免用户收起键盘后被强制重弹。
    state.m_focus = action != 3;
    state.m_status.clear();
    FeedbackOpenPopup("管理布局预设###MobileLayoutPresetAction");
}
/// @brief 触屏预设入口按整行排布，避免长标签和两枚按钮挤出窄栏。
/// @return 是否通过显式管理动作改变库；文件选择完成在独立回调中发布。
/// @details 每行至少 48dp，主题本身更高时保留原高度，避免文字被裁切。
/// 名称弹窗与确认行使用相同内容倍率，不复用桌面固定像素尺寸。
/// @warning 每帧只借用库，复制、保存和发布均位于按钮边沿。
bool drawMobilePresetControls()
{
    // 此分支仅由移动外壳调用；桌面仍保留原有紧凑控件和内联名称。
    // 图形方案是否使用触摸不改变移动窗口的可用宽度约束。
    auto&       app     = Config::AppConfig::instance();
    auto&       library = app.getLayoutPresets();
    const float scale   = app.getWindowContentScale();
    const float height  = std::max(ImGui::GetFrameHeight(), 48.f * scale);
    bool        changed = false;
    // 标签与下拉分行，留出的整行宽度供长预设名和箭头使用。
    // 不根据名称长度重建窗口尺寸，展开与收起不会横向跳动。
    ImGui::TextUnformatted("布局预设");
    // Popup 条目沿用稳定名称 ID，语言及内置标记不参与库身份。
    // 选择过程可能保存离开时的实时布局，不能在成功后继续遍历旧 vector。
    ImGui::SetNextItemWidth(-1);
    if ( FeedbackBeginCombo("##MobileLayoutPreset",
                            library.current().displayName()) ) {
        for ( const auto& item : library.m_items ) {
            // 每项也保留触摸高度；选择可能插入自定义项，立即停止旧容器枚举。
            ImGui::PushID(item.m_name.c_str());
            const bool selected =
                FeedbackSelectable(item.displayName(),
                                   item.m_name == library.m_selected,
                                   0,
                                   { 0, height });
            ImGui::PopID();
            if ( selected ) {
                changed = library.select(item.m_name, app.getVisualConfig());
                break;
            }
        }
        FeedbackEndCombo();
    }
    // 常态只显示选择和默认恢复，命名、删除与文件操作按需展开。
    // 不用横向 SameLine 堆叠，最大字号下每个操作仍有完整命中行。
    if ( ImGui::CollapsingHeader("管理布局预设") ) {
        ImGui::TextWrapped(
            "每个预设包含全部键数的布局及公共相机参数；名称在独立输入框中确认"
            "。");
        if ( FeedbackButton("复制为新预设", { -1, height }) )
            beginMobileAction(1);
        // 内置项是代码基线，改名和删除不能绕过库的保护规则。
        // 复制仍可用，用户不必先改变布局才能建立自己的命名方案。
        ImGui::BeginDisabled(library.current().isBuiltIn());
        if ( FeedbackButton("重命名", { -1, height }) ) beginMobileAction(2);
        if ( FeedbackButton("删除当前预设", { -1, height }) )
            beginMobileAction(3);
        ImGui::EndDisabled();
        // 系统文档选择异步返回，等待期间不允许同面板反复创建新请求。
        // 普通控件仍继续渲染，不暂停 SDL 事件或 UI 帧推进。
        ImGui::BeginDisabled(state.m_filePending);
        if ( FeedbackButton("导入预设", { -1, height }) ) openFile(false);
        if ( FeedbackButton("导出当前预设", { -1, height }) ) openFile(true);
        ImGui::EndDisabled();
    }
    // 恢复默认仅切回代码基线，库保留其它自定义项供用户再次选择。
    // 这条离散动作与长表单中的数值精调不共享草稿或确认状态。
    if ( FeedbackButton("重置为软件默认", { -1, height }) ) {
        library.resetToDefault(app.getVisualConfig());
        changed = true;
    }
    ImGui::TextWrapped(
        "内置布局首次修改会自动建立自定义副本；各键数分别保存。");
    // 复用 T11 的安全尺寸和正文滚动，键盘出现时确认行仍在可见区。
    // 临时名称只由本模态拥有，Enter 收键盘，不自动执行重命名或复制。
    Utils::CenteredModalPopupScope style(scale);
    Utils::prepareCenteredModalWindow({ 420.f * scale, 0 });
    if ( ImGui::BeginPopupModal("管理布局预设###MobileLayoutPresetAction",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize) ) {
        // 库在其它入口变化后，旧模态不能把草稿提交到新选中的项。
        // 原名称作为稳定身份校验，失败时保留取消入口及原草稿。
        const bool           same = library.m_selected == state.m_target;
        MobileInputBodyScope body("##LayoutPresetNameBody");
        // 提示当前事务的原目标，键盘出现后即使原面板被遮住也可识别作用域。
        // 标题的固定内部 ID 不随目标名称变化，输入焦点因此保持稳定。
        ImGui::TextWrapped("当前预设：%s", state.m_target.c_str());
        if ( state.m_action == 3 )
            ImGui::TextWrapped("删除此自定义预设？其余预设保持不变。");
        else {
            if ( state.m_focus ) {
                ImGui::SetKeyboardFocusHere();
                state.m_focus = false;
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##LayoutPresetName",
                                     "输入预设名称",
                                     state.m_name.data(),
                                     state.m_name.size());
            keepMobileInputItemVisible();
        }
        // 不把外部目标变化当作自动取消，用户仍可阅读提示并明确关闭。
        // 合法性只控制确认按钮，取消在所有情况下均保持可用。
        if ( !same ) ImGui::TextWrapped("当前预设已改变，请取消后重新操作。");
        if ( !state.m_status.empty() )
            ImGui::TextWrapped("%s", state.m_status.c_str());
        // 正文先结束，确认行由父模态持有；键盘只压缩正文滚动空间。
        // 错误提示留在正文，长提示不能把确认与取消挤出安全区域。
        body.end();
        const float width = std::max(1.f,
                                     (ImGui::GetContentRegionAvail().x -
                                      ImGui::GetStyle().ItemSpacing.x) *
                                         .5f);
        // 只有完整目标仍相同时才允许提交，名称合法性继续交给库统一校验。
        // 空白、重名失败都保留原库和草稿，让用户修正后继续确认。
        ImGui::BeginDisabled(!same);
        if ( FeedbackButton(state.m_action == 3 ? "确认删除" : "确认",
                            { width, height }) ) {
            // 复用库自身的冲突、只读和至少一项约束，不在界面复制第二套规则。
            // 删除也必须通过库切换到有效方案，不能只从 vector 擦掉一行。
            if ( state.m_action == 1 )
                changed = library.duplicate(state.m_name.data(),
                                            app.getVisualConfig());
            else if ( state.m_action == 2 )
                changed = library.rename(state.m_name.data());
            else if ( state.m_action == 3 )
                changed = library.remove(app.getVisualConfig());
            // 成功后只结束本事务；持久化在退出模态后统一发布一次。
            // 失败保留草稿便于修正，不先改名再通过回滚模拟校验失败。
            if ( changed ) {
                state.m_action = 0;
                ImGui::CloseCurrentPopup();
            } else
                state.m_status = "名称不能为空、重复或指向内置预设。";
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        // 取消只丢弃临时动作，不能顺便保存当前输入或切换选择。
        // 下一次动作重新初始化名称，避免误用上次未提交的文本。
        if ( FeedbackButton("取消", { width, height }) ) {
            state.m_action = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // 一帧可能遍历多个按钮，但只有真实成功的离散动作走一次发布。
    // 状态文案来自实际保存结果，不从窗口关闭或按钮反馈推断落盘成功。
    if ( changed ) publish();
    if ( !state.m_status.empty() )
        ImGui::TextWrapped("%s", state.m_status.c_str());
    // 返回值只表达本帧离散管理动作，异步文件完成有独立的发布结果。
    // 调用方无需在每一空闲帧捕获布局或重复写入配置文件。
    return changed;
}
}  // namespace

/// @brief 在布局设置顶部提供选择和管理操作。
/// @return 当前帧是否通过方案管理改变了布局或选择。
/// @warning 每帧只借用 AppConfig，容器复制仅发生在明确操作边沿。
/// @details 相机模式和元素模式共用入口，避免切换编辑模式后找不到方案。
/// 名称输入只修改临时草稿，复制与重命名通过库执行冲突检查。
/// 下拉选择可能自动建立自定义副本，因此操作成功后立即结束原容器枚举。
/// 状态文案持续显示，用户无需从日志判断导入是否成功。
bool drawLayoutPresetControls()
{
    if ( !workspaceAllowsHoverFeedback() ) return drawMobilePresetControls();
    auto& app     = Config::AppConfig::instance();
    auto& library = app.getLayoutPresets();
    bool  changed = false;
    // 下拉框显示全部方案，选择前库会捕获当前尚未保存的拖动结果。
    if ( ImGui::BeginCombo("布局预设", library.current().displayName()) ) {
        for ( const auto& item : library.m_items ) {
            // 显示名添加内置标志，ID
            // 仍按稳定名称隔离，避免与同名自定义展示冲突。
            ImGui::PushID(item.m_name.c_str());
            const bool selected = FeedbackSelectable(
                item.displayName(), item.m_name == library.m_selected);
            ImGui::PopID();
            if ( selected ) {
                changed = library.select(item.m_name, app.getVisualConfig());
                // 自动收存内置现场可能插入新项，选择后不再继续使用旧迭代器。
                break;
            }
        }
        ImGui::EndCombo();
    }
    // 不常用的命名和文件操作收进管理分组，常态入口只占一个下拉框。
    // 选择框始终可用，不要求用户先进入相机参数窗口。
    if ( ImGui::CollapsingHeader("管理布局预设") ) {
        ImGui::TextWrapped(
            "每个预设包含所有键数的布局及公共摄像机参数。修改只影响当前预设。");
        // 文本不直接绑定所选项，输入中间态不会改变方案身份。
        // 只有明确点击管理按钮才执行名字校验和持久化。
        ImGui::InputText("名称", state.m_name.data(), state.m_name.size());
        if ( FeedbackButton("复制为新预设") ) {
            changed =
                library.duplicate(state.m_name.data(), app.getVisualConfig());
            if ( !changed ) state.m_status = "名称不能为空或重复。";
        }
        ImGui::SameLine();
        // 改名保留布局和所有键数覆盖，不进行按当前键数裁剪。
        // 内置方案都是固定基准，只允许复制后再命名或编辑。
        ImGui::BeginDisabled(library.current().isBuiltIn());
        if ( FeedbackButton("重命名") ) {
            changed = library.rename(state.m_name.data());
            if ( !changed ) state.m_status = "名称不能为空或重复。";
        }
        ImGui::EndDisabled();
        // 删除最后一个方案会使实时布局失去归属，因此保持禁用。
        ImGui::BeginDisabled(library.current().isBuiltIn() ||
                             library.m_items.size() <= 1);
        if ( FeedbackButton("删除当前预设") )
            changed = library.remove(app.getVisualConfig());
        ImGui::EndDisabled();
        // 导入作为独立项进入库，允许同名文件多次导入而不覆盖旧方案。
        // 原生文件路径立即处理，内置路径则由外部逐帧推进函数处理。
        if ( FeedbackButton("导入预设") ) openFile(false);
        ImGui::SameLine();
        if ( FeedbackButton("导出当前预设") ) openFile(true);
    }
    // 完整重置包含所有键数覆写和摄像机，不只是当前键数的局部恢复。
    if ( FeedbackButton("重置为软件默认") ) {
        library.resetToDefault(app.getVisualConfig());
        changed = true;
    }
    ImGui::TextWrapped(
        "标有(内置)"
        "的布局来自软件代码；首次修改会自动建立自定义预设并实时保存。");
    // 一次交互只发布一次配置；空闲帧不复制方案或保存文件。
    // 管理项引用已在上方失效前使用完毕，发布不会借用旧容器元素。
    // 成功反馈来自实际配置写入结果，避免磁盘满时假报保存成功。
    if ( changed ) publish();
    if ( !state.m_status.empty() )
        ImGui::TextWrapped("%s", state.m_status.c_str());
    return changed;
}
/// @brief 推进已打开的内置方案文件对话框。
/// @warning 每帧只查询对话框状态，确认成功后才进行文件读写。
/// @details 入口独立于布局弹层和摄像机编辑模式。
/// 用户切换工具后也能正确关闭对话框，不遗留拦截输入的隐藏模态。
/// 路径在 Close 前立即消费，避免下一次打开改变查询结果。
/// 原生对话框在启动动作内完成，不由本函数重复处理。
void drawLayoutPresetFileDialogs()
{
    // 与布局弹层无关，即使用户切换工具也能完成当前文件选择。
    // 两个动作互斥，共用系统既有单文件选择器实例。
    for ( const bool exporting : { false, true } ) {
        // 只处理当前已打开的身份，不抢占其他工具的选择器。
        const char* key =
            exporting ? "LayoutPresetExport" : "LayoutPresetImport";
        auto* dialog = ImGuiFileDialog::Instance();
        if ( !dialog->IsOpened(key) ) continue;
        if ( dialog->Display(key, ImGuiWindowFlags_NoCollapse, { 600, 400 }) ) {
            // 取消只关闭窗口，不修改预设状态或回写实时布局。
            if ( dialog->IsOk() )
                completeFile(dialog->GetFilePathName(), exporting);
            // 所有完成分支配对关闭，下一次打开不会消费旧的确认结果。
            // Close 不清空状态文案，回到布局面板后仍可查看失败原因。
            dialog->Close();
        }
    }
}
}  // namespace MMM::UI
