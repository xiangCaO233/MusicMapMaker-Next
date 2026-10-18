#include "ui/imgui/manager/LayoutPresetControls.h"
#include "config/AppConfig.h"
#include "config/LayoutPresets.h"
#include "config/Utf8Path.h"
#include "logic/EditorEngine.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
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
        state.m_status =
            Config::exportLayoutPreset(path, library.current(), error)
                ? "布局预设已导出。"
                : error;
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
    if ( app.getEditorSettings().filePickerStyle ==
         Config::FilePickerStyle::Native ) {
        PlayPopupOpenFeedback();
        nfdu8char_t*            path = nullptr;
        const nfdu8filteritem_t filter{ "MusicMapMaker Layout", "mmlayout" };
        const auto              result =
            exporting ? NativeFileDialog::saveFile(
                            &path, &filter, 1, nullptr, "layout.mmlayout")
                      : NativeFileDialog::openFile(&path, &filter, 1, nullptr);
        // NFD 路径使用配套释放接口，无论选择的是中文还是 ASCII 文件名。
        if ( result == NFD_OKAY && path )
            completeFile(path, exporting);
        else if ( result == NFD_ERROR || result == NFD_OKAY )
            state.m_status = "无法打开文件选择器。";
        if ( path ) NFD_FreePathU8(path);
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
