#include "ui/imgui/menu/items/MainMenuEdit.h"
#include "config/skin/SkinConfig.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include "ui/imgui/menu/items/MainMenuSeparatorItem.h"
#include "ui/imgui/menu/items/MainMenuToggleItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造编辑菜单并注册默认菜单项。
/// @details 菜单按历史、剪贴板、选择属性、编辑模式、播放与谱面设置分组。
/// @warning 注册顺序参与快捷键冲突优先级，调整项目时须同步索引路由。
/// @note 每个项目以 unique_ptr 持有自身动作处理器，生命周期归菜单所有。
MainMenuEdit::MainMenuEdit()
{
    // 撤销入口位于首位，直接对应最近一条可撤销编辑命令。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_UNDO,
        "ui.edit.undo",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+Z",
        createUndoAction()));
    // 重做同时展示两套常见组合键，但由同一动作统一消费。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_REDO,
        "ui.edit.redo",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+Y / Ctrl+Shift+Z",
        createRedoAction()));
    // 历史操作与剪贴板操作使用分隔符区分。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    // 剪切先导出选择到剪贴板，再通过逻辑命令删除原对象。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SCISSORS,
        "ui.edit.cut",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+X",
        createCutAction()));
    // 复制只更新编辑器和系统剪贴板，不改变谱面选择。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COPY,
        "ui.edit.copy",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+C",
        createCopyAction()));
    // 普通粘贴按编辑器设置决定新对象是否自动选中。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_PASTE,
        "ui.edit.paste",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+V",
        createPasteAction()));
    // 镜像粘贴使用可配置快捷键，因此不提供静态提示文本。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MIRROR,
        "ui.edit.mirror_paste",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createMirrorPasteAction()));
    // 镜像当前选择同样从用户配置动态生成快捷键提示。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MIRROR,
        "ui.edit.mirror",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createMirrorAction()));
    // 分隔剪贴板操作与选择范围、属性编辑操作。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    // Ctrl+A 只选择鼠标所在轨道区域内的对象。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SELECT_ALL,
        "ui.edit.select_all",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+A",
        createSelectAllAction()));
    // Ctrl+Shift+A 显式扩大到所有轨道区域。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SELECT_ALL,
        "ui.edit.select_all_objects",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+Shift+A",
        createSelectAllObjectsAction()));
    // 元数据动作打开当前选中音符的结构化属性编辑入口。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COG,
        "ui.edit.note_metadata",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenNoteMetadataAction()));
    // 音量编辑动作以批量窗口处理当前选中对象。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_VOLUME_HIGH,
        "ui.edit.selected_volume",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createEditSelectedObjectVolumeAction()));
    // 批注动作将选择快照转换为可编辑的谱面批注。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COMMENT,
        "ui.edit.add_selected_annotation",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+R",
        createAddSelectedObjectAnnotationAction()));
    // 分隔对象编辑与全局工作模式、辅助表格入口。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    // 专业模式直接绑定配置布尔值，并使用滑杆图标提示全局设置。
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.edit.professional_mode",
        MainMenuItemTextKind::TranslationKey,
        createProfessionalModeToggleAction(),
        ICON_MMM_SLIDERS));
    // Timing 表由 Timeline 画布能力接口打开，菜单不依赖具体视图类型。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_BARS,
        "ui.timeline.menu.open_timing_table",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenTimingPointsTableAction()));
    // 批注表通过辅助窗口能力接口激活，保持菜单层解耦。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COMMENT,
        "ui.annotation.menu.open_table",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenAnnotationTableAction()));
    // 分隔辅助窗口与谱面格式专用编辑模式。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    // BMS 编辑开关只影响对应格式功能的可用状态。
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.edit.bms_editing",
        MainMenuItemTextKind::TranslationKey,
        createBmsEditingToggleAction(),
        ICON_MMM_KEYBOARD));
    // 折线编辑模式独立于 BMS 模式，允许按谱面需求分别切换。
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.edit.polyline_editing",
        MainMenuItemTextKind::TranslationKey,
        createPolylineEditingToggleAction(),
        ICON_MMM_POLYLINE));
    // 播放控制从编辑模式中独立分组，降低误触风险。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    // 播放项图标和快捷键提示均由动作按当前状态动态提供。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_PLAY,
        "ui.edit.play_pause",
        MainMenuItemTextKind::TranslationKey,
        "Space",
        createTogglePlaybackAction()));
    // 谱面设置属于低频项目级操作，单独置于菜单末尾。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    // 设置入口打开当前活动谱面的配置窗口，不直接修改字段。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FILE,
        "ui.edit.beatmap_settings",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenBeatmapSettingsAction()));
    // 构造完成后集合固定，逐帧更新和绘制不会改变项目索引。
}

/// @brief 获取编辑菜单标识。
/// @return 编辑菜单标识。
/// @note 标识用于 Alt 导航控制器映射固定请求槽位。
/// @warning 返回值必须与默认菜单注册表中的逻辑位置一致。
MainMenuId MainMenuEdit::id() const
{
    return MainMenuId::Edit;
}

/// @brief 获取编辑菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的编辑菜单文本。
/// @warning 返回指针由翻译系统管理，不得释放或跨语言切换缓存。
/// @note context 预留给未来按会话改变标签的实现，当前无需读取。
const char* MainMenuEdit::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.edit").data();
}

/// @brief 遍历更新编辑菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 更新阶段推进动作持有的窗口、配置缓存和异步反馈状态。
/// @warning 循环中不得执行完整对象遍历、阻塞等待或文件系统操作。
void MainMenuEdit::update(MainMenuContext& context)
{
    // 空槽位被容忍，保证局部动作缺失时其余编辑入口仍可工作。
    for ( auto& item : m_items ) {
        // 菜单容器不解释业务状态，只向有效项目转发上下文。
        if ( item ) item->update(context);
    }
}

/// @brief 遍历编辑菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在编辑菜单展开时执行。
/// @note 绘制顺序严格遵循构造期注册顺序和分组。
/// @warning 项目集合在此期间必须保持稳定，禁止动态注册或移除。
void MainMenuEdit::render(MainMenuContext& context)
{
    // 每个项目自行解析启用状态、图标和当前快捷键提示。
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历编辑菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 镜像相关可配置绑定先于默认剪贴板快捷键判断。
/// @warning 只允许查询输入与轻量交互状态，不得直接修改谱面对象。
/// @note 返回 true 表示本帧按键已归编辑菜单所有，外层应停止路由。
bool MainMenuEdit::handleShortcut(MainMenuContext& context)
{
    // 自定义镜像快捷键需要优先于 Ctrl+V/Ctrl+C 等默认编辑快捷键。
    // 索引对应构造器中的稳定注册位置，新增项时必须同步维护。
    constexpr std::size_t mirrorPasteItemIndex = 6;
    constexpr std::size_t mirrorItemIndex      = 7;
    // 越界时回退为空指针，避免异常注册状态导致非法访问。
    IMainMenuItem* mirrorPasteItem = mirrorPasteItemIndex < m_items.size()
                                         ? m_items[mirrorPasteItemIndex].get()
                                         : nullptr;
    IMainMenuItem* mirrorItem      = mirrorItemIndex < m_items.size()
                                         ? m_items[mirrorItemIndex].get()
                                         : nullptr;

    // 镜像粘贴最先消费，使其绑定与 Ctrl+V 重叠时保持用户预期。
    if ( mirrorPasteItem && mirrorPasteItem->handleShortcut(context) ) {
        return true;
    }
    // 普通镜像随后处理，避免与复制等默认组合键竞争。
    if ( mirrorItem && mirrorItem->handleShortcut(context) ) {
        return true;
    }

    // 其余项目按可见注册顺序处理，首个命中立即终止。
    for ( auto& item : m_items ) {
        if ( item.get() == mirrorPasteItem || item.get() == mirrorItem ) {
            // 两个优先项目已经检查过，跳过可避免同帧重复消费。
            continue;
        }
        if ( item && item->handleShortcut(context) ) return true;
    }
    // 没有动作消费时允许更外层快捷键路由继续处理。
    return false;
}

/// @brief 遍历渲染编辑菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 元数据、音量、批注及设置窗口在菜单栏作用域外绘制。
/// @warning 处理器只能在用户明确交互时进入其低频阻塞路径。
/// @note context 仅在本次转发链内有效，项目不得保存其地址。
void MainMenuEdit::renderDeferred(MainMenuContext& context)
{
    // 菜单关闭后仍需推进已打开的独立编辑窗口。
    for ( auto& item : m_items ) {
        // 无延迟内容的项目通过默认空实现低成本返回。
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册编辑菜单项。
/// @param item 待注册菜单项。
/// @note 空项目被忽略，集合只保存有效独占所有权。
/// @warning 仅在初始化阶段调用，热路径不得扩容或改变索引。
/// @pre item 应是尚未被其他菜单接管的独占对象。
/// @post 有效项目追加到末尾，其相对注册顺序保持不变。
void MainMenuEdit::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        // 转移后由编辑菜单统一管理项目及其动作处理器生命周期。
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
