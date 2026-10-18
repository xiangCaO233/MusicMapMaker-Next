#include "ui/imgui/manager/ProjectAudioToolView.h"

#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "mmm/project/Project.h"
#include "runtime/AppThreadPool.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/ProjectAudioPreviewControls.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/// @file ProjectAudioToolScrollTest.cpp
/// @brief 使用真实工具视图验证停靠、标签切换和双轴滚动。
/// @details 不启动 GPU；连续试听通过 SDL 虚拟设备消费构建树中的合成 PCM。
/// 所有项目写入都位于构建树，ImGui 禁止写入个人工作区 ini。
namespace
{
/// @brief 查找真实工具创建的画布子窗口。
/// @return 已创建的子窗口；首次提交前返回空指针。
/// @note 翻译只改变前缀，子窗口名称中的稳定画布 ID 不变。
ImGuiWindow* findCanvas()
{
    // 不依赖具体语言，避免翻译缓存影响测试识别。
    for ( auto* window : ImGui::GetCurrentContext()->Windows ) {
        if ( std::string_view(window->Name).find("/ProjectAudioToolCanvas_") !=
             std::string_view::npos ) {
            return window;
        }
    }
    // 查找失败属于夹具或视图创建异常，不回退到宿主滚动条。
    return nullptr;
}

/// @brief 提交一帧真实工具和共享停靠标签。
/// @param manager 工具依赖的 UI 管理器。
/// @param view 本次测试唯一的工具实例。
/// @param docked 是否停靠；浮动阶段验证同一实例的原有滚动行为。
/// @param wheel 纵向滚轮增量，负值向下。
/// @param focusOther 是否激活同节点中的另一个标签。
/// @param height 工具所在宿主的高度。
/// @param shift 是否按住 Shift，显式请求横向滚动。
/// @param pointer 指针位置，表格场景使用真实行坐标选择资源。
/// @param horizontalWheel 后端已转换的横轴滚轮增量，负值向右。
/// @note 横轴与纵轴分别注入，允许测试真实触控板事件而非只模拟传统鼠标。
/// @note Shift 状态仍独立进入队列，不能通过移除修饰键来绕过平台行为。
/// @note 本入口接收 ImGui 后端事件，不负责模拟操作系统的 Shift 轴转换。
/// @warning 测试帧入口：只推进 ImGui 内存状态，不等待真实时间。
void drawFrame(MMM::UI::UIManager& manager, MMM::UI::ProjectAudioToolView& view,
               bool docked, float wheel = 0.0F, bool focusOther = false,
               float height = 450.0F, bool shift = false,
               ImVec2 pointer         = ImVec2(200.0F, 250.0F),
               float  horizontalWheel = 0.0F)
{
    auto& io = ImGui::GetIO();
    // 指针放在画布空白处，避免点击音频预览或拖动资源。
    io.AddMousePosEvent(pointer.x, pointer.y);
    // 输入必须先于 NewFrame 排队，与真实 GLFW 后端的事件时序一致。
    io.AddMouseWheelEvent(horizontalWheel, wheel);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    ImGui::NewFrame();
    // 固定宿主几何，隔离系统 DPI、用户布局及动画时钟。
    ImGui::SetNextWindowPos({ 0.0F, 0.0F }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ 600.0F, height }, ImGuiCond_Always);
    ImGui::Begin("AudioToolScrollHost", nullptr, ImGuiWindowFlags_NoTitleBar);
    const auto dockId = ImGui::GetID("AudioToolScrollDock");
    // 每帧沿用同一个节点，不重建 Dock 树掩盖窗口迁移或滚动状态丢失。
    ImGui::DockSpace(dockId);
    ImGui::End();
    // 同一工具先浮动再停靠，覆盖运行中窗口身份迁移。
    ImGui::SetNextWindowDockID(docked ? dockId : 0, ImGuiCond_Always);
    if ( !docked ) {
        ImGui::SetNextWindowPos({ 0.0F, 0.0F }, ImGuiCond_Always);
        ImGui::SetNextWindowSize({ 600.0F, height }, ImGuiCond_Always);
    }
    // 测试显式选择工具标签，避免新建的辅助标签默认抢走初始焦点。
    // focusOther 分支故意不给工具焦点，以真实经过隐藏内容帧。
    // 活动弹层必须保留自身焦点，不能由测试夹具每帧强行聚焦背景窗口。
    if ( !focusOther && ImGui::GetCurrentContext()->OpenPopupStack.empty() )
        view.requestFocus();
    view.update(&manager);
    // 真实标签切换会隐藏工具内容；重新激活后必须仍可滚动。
    if ( docked ) {
        ImGui::SetNextWindowDockID(dockId, ImGuiCond_Always);
        if ( focusOther ) ImGui::SetNextWindowFocus();
        ImGui::Begin("OtherAudioToolTab");
        ImGui::End();
    }
    // 即使无 GPU，也要结束帧并完成滚动范围及窗口悬停状态的发布。
    ImGui::Render();
}

/// @brief 构造超过视口的资源列，并保留横向溢出的独立资源。
/// @param project 测试项目，禁止借用用户当前项目。
/// @details 最后一张方块位于视口外，滚到末端才能查看；额外远端方块验证横轴。
void prepareResources(MMM::Project& project)
{
    // 清除上次运行的磁盘恢复状态，资源布局只由本次夹具决定。
    project.m_audioResources.clear();
    auto& workspace = project.m_settings.m_workspace;
    // 每轮布局夹具显式模拟旧项目：只有选中采样曾保存画笔倍率。
    // 其余采样没有独立记录，升级后应保持 100，不能全继承 65。
    workspace.m_projectAudioToolSelectedResourceId = "pf1_0";
    workspace.m_projectAudioToolBrushVolume        = 0.65F;
    auto& placements = workspace.m_projectAudioToolPlacements;
    // 避免同名旧 placement 合并后改变首帧资源位置和 Z 序。
    placements.clear();
    for ( int index = 0; index < 24; ++index ) {
        // ID 与布局记录一一对应；文件名外观无需真实 wav 文件支持。
        const auto id = "pf1_" + std::to_string(index);
        project.m_audioResources.push_back(
            MMM::AudioResource{ .m_id   = id,
                                .m_path = "preview.wav",
                                .m_type = MMM::AudioTrackType::Effect });
        // 固定宽高保证溢出来自位置，而不是字体或主题自动尺寸。
        placements.push_back(
            MMM::ProjectAudioToolItemPlacement{ .m_audioResourceId = id,
                                                .m_x               = 24.0F,
                                                .m_y = 24.0F + index * 110.0F,
                                                .m_width  = 140.0F,
                                                .m_height = 100.0F,
                                                .m_zOrder = index });
    }
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "horizontal",
                            .m_path = "preview.wav",
                            .m_type = MMM::AudioTrackType::Effect });
    // 高宿主可容纳整列，只留下横向溢出，Shift 滚轮可查看右侧资源。
    placements.push_back(
        MMM::ProjectAudioToolItemPlacement{ .m_audioResourceId = "horizontal",
                                            .m_x               = 1000.0F,
                                            .m_y               = 24.0F,
                                            .m_width           = 140.0F,
                                            .m_height          = 100.0F,
                                            .m_zOrder          = 24 });
}

/// @brief 写入两秒单声道 PCM，避免测试依赖皮肤或用户项目媒体。
/// @param root 构建树中的临时项目目录。
/// @return 文件完整写入时为真。
/// @details 固定 PCM 格式避免各平台编码器的预延迟影响源位置判断。
/// 文件保留在构建树，重复运行直接覆盖本测试自己的输出。
bool writePreviewFixture(const std::filesystem::path& root)
{
    // 手工输出固定小端 WAV 头，跨宿主端序保持一致。
    std::ofstream output(root / "preview.wav", std::ios::binary);
    /// @brief 按 WAV 小端编码写入指定宽度的无符号字段。
    /// @param value 宿主字节序中的字段值。
    /// @param bytes 文件字段宽度，只使用标准 WAV 的二或四字节字段。
    const auto writeInteger = [&](std::uint32_t value, int bytes) {
        for ( int i = 0; i < bytes; ++i )
            output.put(static_cast<char>((value >> (8 * i)) & 0xff));
    };
    // 两秒长度足以让测试暂停在中途，不能让自然播完冒充停止重置。
    // 单声道十六位每帧两字节，RIFF 长度不包含前八个字节。
    constexpr std::uint32_t frames = 96000;
    output.write("RIFF", 4);
    writeInteger(36 + frames * 2, 4);
    // PCM 的 fmt 块固定十六字节，不写浮点扩展或压缩格式附加字段。
    output.write("WAVEfmt ", 8);
    writeInteger(16, 4);
    writeInteger(1, 2);
    writeInteger(1, 2);
    // 字节率等于采样率乘块对齐，和后面的数据长度必须一致。
    writeInteger(48000, 4);
    writeInteger(96000, 4);
    writeInteger(2, 2);
    writeInteger(16, 2);
    // data 长度只描述有效 PCM，禁止把资源写到只读源码夹具目录。
    output.write("data", 4);
    writeInteger(frames * 2, 4);
    // 静音仍有完整源游标；断言读取源位置而非猜测墙钟或实际听感。
    const std::vector<char> silence(frames * 2, 0);
    output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    // 流状态承载创建或写入失败，不依赖异常机制继续运行测试。
    return output.good();
}

/// @brief 连续点击同一资源，验证每次选择都从零重播。
/// @param manager 真实 UI 管理器。
/// @param view 正在显示的工具实例。
/// @return 方块与表格均通过重复点击、双击和暂停后重播时为真。
/// @details 后续点击前让真实音源前进，交替暂停以区分续播和重新起播。
/// 同资源不切换 ID，因此可检测仅在选中资源变化时播放的错误门控。
/// 所有点击保持同一坐标和按键，不以移动指针强制重建悬浮状态。
/// 音源长度远大于断言阈值，自然播完不能满足测试的前进条件。
/// ImGui 帧时间连续推进，不插入双击超时来规避第二次点击被吞。
/// 等待只用于测试后端消费 PCM，不加入任何生产交互路径。
bool checkRapidPreview(MMM::UI::UIManager&            manager,
                       MMM::UI::ProjectAudioToolView& view)
{
    auto* project = MMM::Logic::ProjectController::instance().currentProject();
    if ( !project ) return false;
    // 当前项目由控制器拥有，整个点击序列中资源列表保持稳定。
    // 预览池由 AudioManager 管理，视图只通过公开状态查询验证行为。
    auto& workspace = project->m_settings.m_workspace;
    auto& audio     = MMM::Audio::AudioManager::instance();
    workspace.m_projectAudioToolPreviewEffectOnSelection = true;
    // 两种模式使用相同独立池键；切换模式不能掩盖既有暂停状态。
    for ( bool tableMode : { false, true } ) {
        // 先稳定模式切换和表格测量；首次创建时的隐藏帧不属于点击序列。
        // 同一视图跨模式复用，不能以销毁旧实例的方式清除暂停位置。
        view.setTableView(tableMode);
        for ( int frame = 0; frame < 4; ++frame )
            drawFrame(manager, view, false);
        ImVec2 pointer;
        if ( tableMode ) {
            // 稳定表格 ID 不依赖翻译标题，避免 CI 语言差异影响命中。
            // 只选当前活动窗口，不能误用上一模式留下的非活动表格。
            ImGuiWindow* table = nullptr;
            for ( auto* window : ImGui::GetCurrentContext()->Windows )
                if ( window->Active && std::string_view(window->Name)
                                               .find("ProjectAudioToolTable") !=
                                           std::string_view::npos )
                    table = window;
            if ( !table ) return false;
            pointer = { table->InnerRect.Min.x + 30,
                        table->InnerRect.Min.y +
                            ImGui::GetFrameHeightWithSpacing() + 5 };
        } else {
            // 方块主体避开底部播放按钮与边缘控制柄。
            const auto* canvas = findCanvas();
            if ( !canvas ) return false;
            pointer = { canvas->DC.CursorStartPos.x + 94,
                        canvas->DC.CursorStartPos.y + 64 };
        }
        /// @brief 在稳定窗口几何下推进同坐标点击的一个输入边沿。
        /// @warning 测试帧只驱动生产入口，不注入额外的选择或重播命令。
        const auto at = [&] {
            drawFrame(manager, view, false, 0, false, 450, false, pointer);
        };
        at();
        // 四次同坐标点击覆盖单击、双击的第二次按下和下一组点击。
        for ( int click = 0; click < 4; ++click ) {
            // 第一击负责创建预览池，后续点击验证既有池的重复触发。
            // 不在各击之间重新注册资源，避免资源替换掩盖播放状态错误。
            if ( click > 0 ) {
                const auto key = MMM::UI::makeProjectAudioPreviewPoolKey(
                    "tool/" + workspace.m_projectAudioToolSelectedResourceId);
                // 超时只决定测试失败，不能把设备未前进当作零点重播成功。
                // 使用单调时钟避免系统时间调整延长等待或提前结束。
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(2);
                // 首次重播后的源位置必须真正前进，暂停状态才有续播意义。
                while ( audio.getSFXPlaybackTime(key) < 0.15 &&
                        std::chrono::steady_clock::now() < deadline )
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if ( audio.getSFXPlaybackTime(key) < 0.15 ) return false;
                // 交替覆盖正在播放和暂停两种状态；两者都必须重置源位置。
                if ( click % 2 != 0 ) audio.pauseSoundEffect(key);
            }
            // 真实后端按下和释放分帧；不直接操作 ImGui 的双击计数。
            // 因而连续点击必须经过与用户相同的双击判定与拖动状态机。
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            at();
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            at();
            const auto key = MMM::UI::makeProjectAudioPreviewPoolKey(
                "tool/" + workspace.m_projectAudioToolSelectedResourceId);
            // 新播放周期不能继承暂停位置，也不能继续保持暂停。
            // 读取的是最新实例源游标，旧实例自然排空不会使本断言误过。
            // 容许音频线程在点击帧之间消费少量 PCM，但不容许保留旧进度。
            // 没有加载成功的空池也必须失败，不能仅以零位置判断重播成功。
            if ( !audio.isSFXPlaying(key) ||
                 audio.getSFXPlaybackTime(key) >= 0.15 ) {
                XERROR("Preview restart failed: table={}, click={}, time={}",
                       tableMode,
                       click,
                       audio.getSFXPlaybackTime(key));
                return false;
            }
        }
    }
    // 后续几何测试不需要自动试听，恢复方块模式与项目开关。
    // 保留点击生成的布局，后续吸附夹具会通过正常保存事件重新建立基线。
    // 这里只恢复测试开关，不卸载再重建池来修饰序列中的播放结果。
    workspace.m_projectAudioToolPreviewEffectOnSelection = false;
    view.setTableView(false);
    return true;
}

/// @brief 通过真实鼠标操作验证音量松手、保存重建及移开指针均不关闭弹层。
/// @param manager 真实工具管理器，测试不调用视图私有状态。
/// @param view 与后续滚动测试共用的生产视图。
/// @return 只有明确点击外部才关闭时为真。
/// @details 使用实际 Popup 的窗口与激活状态断言，不能仅检查残留的打开栈。
/// 资源保存事件走真实订阅回调，覆盖缓存重建导致锚定 ID 被误清除的路径。
/// @details 夹具使用旧版只保存当前画笔倍率的工作区，首先确认兼容值没有丢失。
/// 弹层标题长度与滑块宽度不同，命中点必须位于真实输入控件内。
/// Ctrl 输入 175 同时覆盖整数百分数输入和一倍以上的增益范围。
/// 文本提交后应完成一次工作区保存，再接受外部保存通知重建资源缓存。
/// 该通知模拟自动保存或资源列表刷新，不意味着用户要求关闭弹层。
/// 音量归属按稳定资源 ID 判断，不能假定激活后的方块还位于数组首项。
/// 第一张卡片应得到新的独立倍率，其余旧记录首次保存后应明确为 100。
/// 最后仍通过外部鼠标点击结束弹层，确保后续场景不继承输入捕获。
bool checkVolumePopup(MMM::UI::UIManager&            manager,
                      MMM::UI::ProjectAudioToolView& view)
{
    auto* project = MMM::Logic::ProjectController::instance().currentProject();
    if ( !project ) return false;
    project->m_settings.m_workspace.m_projectAudioToolPreviewEffectOnSelection =
        false;
    // 夹具第一张卡片固定为 140x100，位于逻辑坐标 24,24。
    for ( int frame = 0; frame < 4; ++frame ) drawFrame(manager, view, false);
    auto* canvas = findCanvas();
    if ( !canvas ) return false;
    // 第一次选择保存前，直接点开旧项目选中采样，兼容倍率必须仍是 65。
    const auto& before = project->m_settings.m_workspace;
    if ( before.m_projectAudioToolBrushVolume != 0.65F ) return false;
    const auto& style  = ImGui::GetStyle();
    const float button = ImGui::GetFrameHeight();
    const float spacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.x, 4.0F));
    // 固定四按钮居中排列，最后一个为音量按钮；不直接调用 OpenPopup。
    const ImVec2 buttonPoint{ canvas->DC.CursorStartPos.x + 24 +
                                  (140 - (button * 4 + spacing * 3)) * 0.5F +
                                  (button + spacing) * 3 + button * 0.5F,
                              canvas->DC.CursorStartPos.y + 24 + 100 -
                                  std::max(1.0F, style.FramePadding.y) -
                                  button * 0.5F };
    /// @brief 固定工具窗口几何，只推进指针位置及此前排队的输入事件。
    /// @param pointer 本帧鼠标屏幕坐标，弹层内外都通过真实窗口命中处理。
    /// @note 不直接设置私有锚定状态，确保缓存重建走完整生产交互路径。
    auto frameAt = [&](ImVec2 pointer) {
        drawFrame(manager, view, false, 0, false, 450, false, pointer);
    };
    frameAt(buttonPoint);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frameAt(buttonPoint);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frameAt(buttonPoint);
    auto& context = *ImGui::GetCurrentContext();
    if ( context.OpenPopupStack.empty() ||
         !context.OpenPopupStack.back().Window )
        return false;
    // 稳定 PopupId 表示同一次编辑上下文，不应关闭后自动重开一个新弹层。
    // 同时检查窗口 Active，避免把未被绘制的残留打开栈误判为可继续使用。
    const auto popupId = context.OpenPopupStack.back().PopupId;
    auto*      popup   = context.OpenPopupStack.back().Window;
    // 弹层先完成一帧自动测量，避免首帧尺寸影响滑块点击坐标。
    frameAt({ popup->Pos.x + 15, popup->Pos.y + 10 });
    if ( !popup->Active ) return false;
    // 输入区位于最后一行，宽度为十二个字高，弹层可能被更长的标题撑宽。
    // 按输入区宽度定位 Ctrl 点击，不能以整个弹层宽度误击旁边空白。
    // 不借用视图私有音量指针，通过真实文本输入覆盖大于 100 的倍率。
    const ImVec2 sliderPoint{ popup->Pos.x + popup->WindowPadding.x +
                                  ImGui::GetFontSize() * 12.0F * 0.7F,
                              popup->Pos.y + popup->Size.y -
                                  popup->WindowPadding.y - button * 0.5F };
    frameAt(sliderPoint);
    // AddKeyEvent 接收平台原始修饰键，macOS 的 Command(Super) 会映射成逻辑
    // Ctrl。 直接注入物理 Ctrl
    // 会变成右键菜单，既无法进入文本编辑也不会保存音量。
    const ImGuiKey inputModifier =
        ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    ImGui::GetIO().AddKeyEvent(inputModifier, true);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frameAt(sliderPoint);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frameAt(sliderPoint);
    // 原生 Ctrl 点击会选中当前数值；释放 Ctrl 后输入完整的新百分数。
    // 不用直接写入 InputText 状态，文本必须经过控件自己的解析和钳制。
    // 在发送文本前验证真实控件已进入输入态，避免将按键模拟失败误报为音量串值。
    if ( context.TempInputId == 0 ) {
        XERROR("Card sample volume text input not activated: macOS={}",
               ImGui::GetIO().ConfigMacOSXBehaviors);
        return false;
    }
    ImGui::GetIO().AddKeyEvent(inputModifier, false);
    ImGui::GetIO().AddInputCharactersUTF8("175");
    frameAt(sliderPoint);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    frameAt(sliderPoint);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    frameAt(sliderPoint);
    // 释放后不再有任何按住的鼠标按钮，弹层持续显示不能依赖 active 滑块。
    // 保存事件是用户提交音量的正常后果，不代表用户要求结束编辑。
    // 松手提交音量后，额外触发保存通知，强制走方块缓存重建路径。
    // 移开鼠标不构成关闭意图，Popup 应仍每帧被真实提交。
    MMM::Event::EventBus::instance().publish(MMM::Event::ProjectSavedEvent{});
    // 指针移到画布空白区，排除仍悬浮在原方块附近而被动保持显示的可能。
    const ImVec2 outside{ 580, 400 };
    // 两个稳定帧覆盖事件消费与后续普通显示，均不进行真实时间等待。
    frameAt(outside);
    frameAt(outside);
    if ( context.OpenPopupStack.empty() ||
         context.OpenPopupStack.back().PopupId != popupId || !popup->Active )
        return false;
    // 编辑第一张卡片后，其他采样必须仍为 100；保存事件重建也不得串值。
    // 这里检查真实鼠标编辑产生的工作区，不直接写入视图缓存或调用编辑 helper。
    const auto& placements =
        project->m_settings.m_workspace.m_projectAudioToolPlacements;
    for ( const auto& placement : placements ) {
        const bool edited = placement.m_audioResourceId == "pf1_0";
        if ( !placement.m_audioVolume ||
             (edited ? *placement.m_audioVolume <= 1.0F
                     : *placement.m_audioVolume != 1.0F) ) {
            XERROR(
                "Card sample volume leaked to {}: saved={}, brush={}, "
                "point={},{} popup={},{} size={},{}",
                placement.m_audioResourceId,
                placement.m_audioVolume.value_or(-1.0F),
                project->m_settings.m_workspace.m_projectAudioToolBrushVolume,
                sliderPoint.x,
                sliderPoint.y,
                popup->Pos.x,
                popup->Pos.y,
                popup->Size.x,
                popup->Size.y);
            return false;
        }
    }
    // 只有在外部按下鼠标后才允许原生 Popup 规则关闭。
    // 按下和释放分别推进，避免同一帧两个边沿被 ImGui 输入队列合并。
    // 结束后继续复用工具实例，后面的滚动测试能检出遗留输入捕获。
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frameAt(outside);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frameAt(outside);
    return context.OpenPopupStack.empty() && !popup->Active;
}

/// @brief 用真实方块拖动验证吸附开关，并覆盖已有锁在关闭后立即释放。
/// @param manager 工具依赖的真实 UI 管理器。
/// @param view 跨场景复用的工具，不通过重建视图隐藏缓存状态问题。
/// @return 开启时对齐、关闭时跟随原始指针，并能中途解除吸附时为真。
/// @note 工作区直接设置模拟已保存偏好，移动与松手仍通过正常鼠标事件提交。
/// @warning 仅测试帧路径，不操作用户媒体文件或等待真实时间。
bool checkSnapToggle(MMM::UI::UIManager&            manager,
                     MMM::UI::ProjectAudioToolView& view)
{
    auto* project = MMM::Logic::ProjectController::instance().currentProject();
    if ( !project ) return false;
    auto& workspace = project->m_settings.m_workspace;
    // 前三轮检验移动的开启、关闭和手势中关闭；后两轮检验右边缘缩放。
    // 截图中的边缘调整必须受同一开关控制，不能只修好方块平移。
    // 每轮重置夹具布局，但保留同一个视图及其缓存，检测残留锁是否干扰新手势。
    for ( int scenario = 0; scenario < 5; ++scenario ) {
        prepareResources(*project);
        workspace.m_projectAudioToolSnapEnabled =
            scenario != 1 && scenario != 4;
        MMM::Event::EventBus::instance().publish(
            MMM::Event::ProjectSavedEvent{});
        for ( int frame = 0; frame < 4; ++frame )
            drawFrame(manager, view, false);
        auto* canvas = findCanvas();
        if ( !canvas ) return false;
        // 卡片内部避开边框缩放柄及底部试听按钮，确保进入正常的单项移动。
        // 屏幕原点取实际子窗口，新增工具栏不能改变手势命中位置。
        const bool resize = scenario >= 3;
        // 右边缘内一像素用于命中实际缩放柄，非边缘轮次仍从方块内部开始。
        const ImVec2 start{ canvas->DC.CursorStartPos.x + (resize ? 163 : 94),
                            canvas->DC.CursorStartPos.y + 64 };
        const ImVec2 target{ start.x + 5, start.y + (resize ? 0 : 12) };
        /// @brief 复用固定浮动窗口，提交同一拖动手势的屏幕指针位置。
        /// @param pointer 已按真实子窗口原点转换的屏幕坐标。
        auto at = [&](ImVec2 pointer) {
            drawFrame(manager, view, false, 0, false, 450, false, pointer);
        };
        at(start);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        at(start);
        // 平移越过拖动阈值，缩放柄则直接响应；两者均进入邻卡边缘的吸附容差。
        // 不直接调用吸附算法，以覆盖工作区开关到实际交互入口的完整路径。
        at(target);
        if ( scenario == 2 ) {
            // 保持鼠标不动来关闭开关，旧滞回锁不应继续决定下一帧位置。
            // 与单独开始一轮禁用状态相比，这还能检出参考线残留的状态错误。
            workspace.m_projectAudioToolSnapEnabled = false;
            at(target);
        }
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        at(target);
        // 松手后只读取正常持久化的布局，不借用视图的私有矩形或吸附锁。
        // 不使用快照或视图缓存作答案，持久位置也必须反映最终开关状态。
        // 方块被提升层级后顺序会改变，因此按资源身份查找，不能只读首项。
        const auto& placements = workspace.m_projectAudioToolPlacements;
        const auto  item       = std::find_if(
            placements.begin(),
            placements.end(),
            [](const MMM::ProjectAudioToolItemPlacement& placement) {
                return placement.m_audioResourceId == "pf1_0";
            });
        if ( item == placements.end() ) return false;
        // 第一轮的底边贴到下一卡片顶边；另两轮必须精确保留指针目标位移。
        // 两个轴分别比较，防止只关闭横轴吸附而遗漏竖轴。
        const float expectedX = resize || scenario == 0 ? 24.0F : 29.0F;
        const float expectedY = resize ? 24.0F : scenario == 0 ? 34.0F : 36.0F;
        // 禁用缩放吸附后保留五像素宽度变化，开启时右边与固定卡片对齐。
        const float expectedWidth = scenario == 4 ? 145.0F : 140.0F;
        if ( std::abs(item->m_x - expectedX) > 0.1F ||
             std::abs(item->m_y - expectedY) > 0.1F ||
             std::abs(item->m_width - expectedWidth) > 0.1F ) {
            XERROR(
                "Project audio snap toggle failed: scenario={}, position=({}, "
                "{}), width={}",
                scenario,
                item->m_x,
                item->m_y,
                item->m_width);
            return false;
        }
    }
    // 后续滚动和表格案例仍使用原始布局与默认吸附状态。
    // 恢复也通过正式资源保存通知使缓存失效，不能直接重建测试专用视图。
    // 所有按下事件已经配对释放，后续弹层与滚轮不继承活动拖动。
    workspace.m_projectAudioToolSnapEnabled = true;
    prepareResources(*project);
    MMM::Event::EventBus::instance().publish(MMM::Event::ProjectSavedEvent{});
    return true;
}

/// @brief 验证浮动、停靠、后台标签恢复及横向溢出的实际滚动结果。
/// @return 所有场景均可查看视口外资源时返回 true。
bool checkScrolling(MMM::UI::UIManager&            manager,
                    MMM::UI::ProjectAudioToolView& view)
{
    // 多帧预热让 ImGui 完成首次内容测量和悬停窗口更新。
    for ( int frame = 0; frame < 4; ++frame ) drawFrame(manager, view, false);
    // 窗口由同一 ImGui context 持有，跨帧借用不涉及资源所有权转移。
    auto* canvas = findCanvas();
    if ( !canvas || !canvas->ScrollbarY || canvas->ScrollMax.y < 2000.0F )
        return false;
    // 下滚与随后无输入的一帧配对，检查延迟滚动目标确实被应用。
    // 仅查看当帧局部相机偏移会漏掉下一帧被 ImGui 夹回零的错误。
    drawFrame(manager, view, false, -1.0F);
    drawFrame(manager, view, false);
    // 浮动阶段先确认输入夹具有效，防止停靠失败实际源于未注入滚轮。
    if ( canvas->Scroll.y <= 0.0F ) return false;

    // 停靠后持续滚到末端，不能仅凭出现滚动条就判定资源可访问。
    for ( int frame = 0; frame < 4; ++frame ) drawFrame(manager, view, true);
    // 固定数量只表示测试输入次数，不构成运行时等待或交互消抖窗口。
    for ( int frame = 0; frame < 60; ++frame )
        drawFrame(manager, view, true, -1.0F);
    drawFrame(manager, view, true);
    // 使用原生 ScrollMax 校验终点，避免自算范围与生产错误恰好一致。
    // 最后一张资源之后保留操作留白，滚到终点才保证下方方块可访问。
    if ( !canvas->ScrollbarY ||
         std::abs(canvas->Scroll.y - canvas->ScrollMax.y) > 1.0F )
        return false;

    // 在另一个标签中停留，再恢复工具；布局变化不能清空滚动范围。
    for ( int frame = 0; frame < 4; ++frame )
        drawFrame(manager, view, true, 0.0F, true);
    for ( int frame = 0; frame < 4; ++frame ) drawFrame(manager, view, true);
    // 恢复后的上滚必须改变位置，防止测试只确认旧滚动值仍然存在。
    const float previousY = canvas->Scroll.y;
    // 正增量反向移动，覆盖到达边界后重新离开边界的状态转换。
    drawFrame(manager, view, true, 1.0F);
    drawFrame(manager, view, true);
    if ( canvas->Scroll.y >= previousY || canvas->ScrollMax.y < 2000.0F )
        return false;

    // 模拟停靠宿主比平台可见区域更高，画布仍必须按被裁剪后的高度滚动。
    // 旧实现会用宿主完整高度计算范围，把下方资源错误地视为已经可见。
    ImGui::GetIO().DisplaySize.y = 700.0F;
    for ( int frame = 0; frame < 4; ++frame )
        drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    // 此断言约束原生子窗口范围，不能只依赖绘制层的临时相机偏移。
    if ( canvas->ScrollMax.y < 2000.0F ) return false;
    // 先离开下边界，避免下滚没有位移被误判为滚动轴失效。
    // 实际滚动目标在后续帧落地，因此再提交一次无输入帧。
    drawFrame(manager, view, true, 1.0F, false, 3200.0F);
    drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    const float clippedY = canvas->Scroll.y;
    // 横轴必须保持不变，单纯观察纵轴有移动不足以发现双轴串扰。
    const float clippedX = canvas->Scroll.x;
    // 普通滚轮必须保持纵轴，不得因宿主尺寸或裁剪状态转向右侧资源。
    drawFrame(manager, view, true, -1.0F, false, 3200.0F);
    drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    if ( canvas->Scroll.y <= clippedY || canvas->Scroll.x != clippedX )
        return false;

    // 扩大平台区域，让整列资源真正可见；此时普通滚轮也不能偷偷换轴。
    // 横向溢出仍存在，只有 Shift 或水平滚轮允许移动横轴。
    ImGui::GetIO().DisplaySize.y = 3400.0F;
    for ( int frame = 0; frame < 4; ++frame )
        drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    if ( canvas->ScrollMax.y > 1.0F || !canvas->ScrollbarX ) return false;
    const float previousX = canvas->Scroll.x;
    drawFrame(manager, view, true, -1.0F, false, 3200.0F);
    drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    // 无纵向溢出意味着纵轮在边界停止，不能隐式重解释为横向请求。
    if ( canvas->Scroll.x != previousX ) return false;
    // 不同平台的后端事件不同：普通平台由 ImGui 处理 Shift，macOS 已由系统换轴。
    // 两种输入在每台测试机上都验证，避免仅在 Linux 上通过而漏掉 Apple
    // 默认行为。
    auto&      io                    = ImGui::GetIO();
    const bool originalMacOSBehavior = io.ConfigMacOSXBehaviors;
    bool       horizontalPassed      = true;
    for ( bool macOSBehavior : { false, true } ) {
        // 仅替换内存输入约定，不启动平台后端或修改用户持久配置。
        // 轮次末尾恢复宿主默认值，后续表格用例仍按原平台规则解释输入。
        io.ConfigMacOSXBehaviors = macOSBehavior;
        // 起点取真实滚动结果，不假设字体、主题与平台决定的每次步长相同。
        // 两个轮次顺序推进，第二轮不能借用第一轮产生的位移通过断言。
        const float before = canvas->Scroll.x;
        // Shift 先独立预热，保证修饰键已在滚轮到来前进入输入状态。
        // macOS 直接提供横向增量，不能再要求生产控件把同一事件换轴一次。
        drawFrame(manager, view, true, 0.0F, false, 3200.0F, true);
        drawFrame(manager,
                  view,
                  true,
                  macOSBehavior ? 0.0F : -1.0F,
                  false,
                  3200.0F,
                  true,
                  { 200.0F, 250.0F },
                  macOSBehavior ? -1.0F : 0.0F);
        // 后续无输入帧让原生滚动目标落地，断言真实子窗口位置而非请求值。
        drawFrame(manager, view, true, 0.0F, false, 3200.0F);
        if ( canvas->Scroll.x <= before ) {
            XERROR(
                "Project audio horizontal scroll failed: before={}, after={}, "
                "macOS={}",
                before,
                canvas->Scroll.x,
                macOSBehavior);
            horizontalPassed = false;
            break;
        }
    }
    // 失败分支也走这里，确保错误日志之后的正常析构不继承临时输入配置。
    // 只要求方向和非零位移，不将平台相关的滚动像素数硬编码为预期结果。
    io.ConfigMacOSXBehaviors = originalMacOSBehavior;
    return horizontalPassed;
}
/// @brief 通过真实多级菜单验证字段排序与排序重置，不直接调用表格排序 API。
/// @param manager 真实工具依赖的 UI 管理器。
/// @param view 当前工具实例，过程中不销毁或重建。
/// @param window 表格的滚动子窗口，供表头右键命中使用。
/// @param table 当前表格状态，仅用于读取最终排序结果。
/// @return 三个字段均能升降序，并能从重置菜单恢复文件名升序时为真。
/// @details 截图中的降序位于三级弹层，必须经过实际的父菜单展开才能复现。
/// 不能直接设置排序规格或全局 CurrentTable，否则会绕过空上下文崩溃。
/// 所有等待只推进测试 UI 帧，不阻塞生产线程或真实等待固定时间。
bool checkTableSortingMenus(MMM::UI::UIManager&            manager,
                            MMM::UI::ProjectAudioToolView& view,
                            ImGuiWindow* window, ImGuiTable* table)
{
    // 保留外层表的观察指针只用于读取结果，不把它写入弹层的全局当前表。
    // 这样测试仍能覆盖原先调用内部 API 时缺少表格上下文的真实失败路径。
    auto&        context = *ImGui::GetCurrentContext();
    const ImVec2 header{ window->InnerRect.Min.x + 30,
                         window->InnerRect.Min.y + 5 };
    /// @brief 向生产工具投递一帧指针状态，让原生菜单处理其展开和关闭。
    const auto at = [&](ImVec2 pointer) {
        drawFrame(manager, view, true, 0, false, 450, false, pointer);
    };
    /// @brief 按弹层实际行高定位菜单项，避免主题行距不同造成相邻项误命中。
    const auto rowPoint = [](ImGuiWindow* popup, int row) {
        // 弹层可能与背景使用不同 ItemSpacing，采用其最后一行的布局步长。
        const float stride =
            popup->DC.CursorPos.y - popup->DC.CursorPosPrevLine.y;
        return ImVec2{ popup->Pos.x + popup->WindowPadding.x + 20,
                       popup->DC.CursorStartPos.y + row * stride +
                           popup->DC.PrevLineSize.y * 0.5F };
    };
    /// @brief 从表头右键打开根菜单，返回完成测量的真实弹层窗口。
    /// @note 根菜单只是入口；后续必须继续展开字段和方向两级菜单。
    const auto openContext = [&]() -> ImGuiWindow* {
        // 先移动到表头，鼠标按下和释放分别进入后端输入队列。
        at(header);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        at(header);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        at(header);
        // 表头可能在行结束时排队打开；额外帧同时完成弹层自动尺寸计算。
        at(header);
        at(header);
        if ( context.OpenPopupStack.empty() ) return nullptr;
        return context.OpenPopupStack.back().Window;
    };
    /// @brief 悬停指定父项并取得其子菜单，遵循 ImGui 原生悬停展开时序。
    const auto openChild = [&](ImGuiWindow* parent, int row) -> ImGuiWindow* {
        // 打开失败向调用方返回空值，不能跳过菜单路径却继续断言原排序状态。
        if ( !parent ) return nullptr;
        // 使用父层深度识别新弹层，不能把仍在显示的父菜单当成展开成功。
        const auto depth = context.OpenPopupStack.size();
        const auto point = rowPoint(parent, row);
        // 有界推进输入帧，直到子菜单窗口真正出现；不直接篡改打开栈。
        // 子菜单的创建和测量分属不同帧，取得窗口后仍需完成测量。
        for ( int frame = 0; frame < 30; ++frame ) {
            at(point);
            if ( context.OpenPopupStack.size() > depth &&
                 context.OpenPopupStack.back().Window ) {
                at(point);
                return context.OpenPopupStack.back().Window;
            }
        }
        return nullptr;
    };
    /// @brief 点击实际菜单叶项，原生关闭弹层后让外层表格消费排序请求。
    const auto clickRow = [&](ImGuiWindow* popup, int row) {
        if ( !popup ) return false;
        const auto point = rowPoint(popup, row);
        // 移动、按下和释放分帧，保证原生菜单收到独立的点击边沿。
        // 叶项关闭后再推进表格，验证排序在正常宿主上下文中落地。
        at(point);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        at(point);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        at(point);
        // 不主动关闭弹层；若点击未被接受，残留菜单应直接导致用例失败。
        at(header);
        return context.OpenPopupStack.empty();
    };
    // 前三列分别为名称、类型与 ID；同一“降序”文本应命中不同字段的稳定身份。
    // 每个组合都重新打开菜单，不能只覆盖首次排序后沿用旧的弹层上下文。
    for ( int column = 0; column < 3; ++column ) {
        for ( const auto direction : { ImGuiSortDirection_Ascending,
                                       ImGuiSortDirection_Descending } ) {
            auto* sort  = openChild(openContext(), 2);
            auto* field = openChild(sort, column);
            if ( !clickRow(
                     field,
                     direction == ImGuiSortDirection_Ascending ? 0 : 1) ) {
                XERROR(
                    "Project audio sort menu click failed: column={}, "
                    "direction={}",
                    column,
                    static_cast<int>(direction));
                return false;
            }
            // 同时检查表头规格与原始列状态，确保请求没有仅改动菜单内的临时变量。
            // 单列排序必须恰好有一个规格，不能保留先前字段的累加优先级。
            // SortOrder 为零表示该字段确实成为主排序列，而非仅显示了箭头。
            // 同时核验列索引与方向，只有“不崩溃”不能证明点中了预期菜单项。
            const auto& spec = table->SortSpecs;
            if ( spec.SpecsCount != 1 || !spec.Specs ||
                 spec.Specs[0].ColumnIndex != column ||
                 spec.Specs[0].SortDirection != direction ||
                 table->Columns[column].SortOrder != 0 ) {
                XERROR(
                    "Project audio sort menu did not apply: column={}, "
                    "direction={}",
                    column,
                    static_cast<int>(direction));
                return false;
            }
        }
    }
    // “重置排序”原先走同一个危险接口，也必须经真实子菜单验证。
    // 重置之后后续行选择测试仍可沿用文件名升序，不继承最后的 ID 降序。
    // 前一轮最后是资源 ID 降序，和文件名升序不同，重置无效不会误通过。
    // 重置也只能由菜单叶项触发，不能直接改列字段来准备后续测试。
    auto* reset = openChild(openContext(), 3);
    return clickRow(reset, 3) && table->SortSpecs.SpecsCount == 1 &&
           table->SortSpecs.Specs[0].ColumnIndex == 0 &&
           table->SortSpecs.Specs[0].SortDirection ==
               ImGuiSortDirection_Ascending;
}
/// @brief 验证真实表格替代方块绘制，选择资源不改写已有布局。
/// @return 模式切换、行选择和切回方块都保持工作区正确时返回 true。
/// @note 使用同一个工具实例和同一套项目资源，避免新建视图掩盖状态丢失。
/// @details 表格子窗口必须实际出现，不能只检查模式布尔值。
/// 方块窗口必须停止提交，证明切换式展示没有叠加两个交互区域。
/// 真实行点击验证共享画笔选择，而非直接修改工具私有状态。
/// 布局逐项比较包括资源身份和 Z 序，避免数量相同却顺序被改变。
/// 未选中行的音量通过真实 Slider 输入调整，不能先借助行按钮激活。
/// 音量提交后验证画笔倍率与资源 ID 同时保存，排除只改变显示的情况。
/// 横向裁剪下先导航到音量列，再依据实际列边界确定鼠标目标。
/// 切回方块后继续保留刚调节的音量目标，不还原首次点击的资源。
/// @details 独立倍率回归分为编辑、重建、重选和重置四个阶段。
/// 编辑第二行之前保存所有资源的独立记录，作为不受本次编辑影响的基线。
/// 所有非目标 ID 都需保持原倍率，不能只检查当前画笔恰好发生变化。
/// 当前画笔快照仍用于引擎后续放置，但不能反过来覆盖每个资源的记录。
/// 缓存刷新由正常保存事件触发，避免直接重建视图掩盖状态迁移错误。
/// 重新选择第一行时，该资源的旧倍率应恢复到画笔快照。
/// 第二行的独立记录在这次切换后也必须仍等于刚编辑的数值。
/// 这些断言同时覆盖共享滑块值、保存广播和选择继承前一资源三种错误。
/// 重置按钮通过真实鼠标边沿操作，不能只调用底层函数后观察内存。
/// 按钮的命中点使用宿主内容原点和标准控件行高计算，不受滚动表格影响。
/// 重置检查遍历全部持久记录，屏幕外资源同样应恢复到原音量。
/// 当前选择在重置前后保持一致，重置不应意外选择最后一个遍历的采样。
/// 最后切回方块继续使用同一视图，确认两种展示共用同一份资源音量。
bool checkTableView(MMM::UI::UIManager&            manager,
                    MMM::UI::ProjectAudioToolView& view)
{
    auto* project = MMM::Logic::ProjectController::instance().currentProject();
    if ( !project ) return false;
    auto& workspace = project->m_settings.m_workspace;
    // 测试不依赖音频文件；关闭选择自动试听以只验证表格操作契约。
    workspace.m_projectAudioToolPreviewEffectOnSelection = false;
    // 清空持久选择，不能让上次测试保存的值掩盖本次行点击没有生效。
    workspace.m_projectAudioToolSelectedResourceId.clear();
    const auto original = workspace.m_projectAudioToolPlacements;
    // 先保留布局，再切换；重建表格不能以新布局覆盖用来比较的基线。
    view.setTableView(true);
    for ( int frame = 0; frame < 3; ++frame ) drawFrame(manager, view, true);
    ImGuiWindow* table = nullptr;
    // 三帧覆盖首次创建、测量与稳定状态，不引入真实时间等待。
    for ( auto* window : ImGui::GetCurrentContext()->Windows ) {
        // 使用稳定内部 ID 找到表格滚动子窗口，不依赖窗口的本地化标题。
        if ( window->Active &&
             std::string_view(window->Name).find("ProjectAudioToolTable") !=
                 std::string_view::npos )
            table = window;
    }
    if ( !view.isTableView() || !table ||
         (findCanvas() && findCanvas()->Active) )
        return false;
    // 从真实表头右键打开菜单，覆盖 TableHeadersRow 的默认菜单提交时机。
    // 不能直接调用自定义菜单或只检查禁用标记，否则标记设置过晚仍可能漏测。
    const ImVec2 header{ table->InnerRect.Min.x + 30,
                         table->InnerRect.Min.y + 5 };
    /// @brief 在真实表格和弹层之间投递一个菜单输入帧。
    /// @param point 目标屏幕坐标，不直接改写表格的打开状态。
    /// @warning 测试不关闭冲突检测，也不绕过原生右键捕获。
    const auto at = [&](ImVec2 point) {
        drawFrame(manager, view, true, 0, false, 450, false, point);
    };
    at(header);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    at(header);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    at(header);
    // 原生表头可在行结束阶段排队打开请求，下一帧才创建对应弹窗窗口。
    at(header);
    auto& context = *ImGui::GetCurrentContext();
    if ( context.OpenPopupStack.empty() ||
         !context.OpenPopupStack.back().Window ) {
        XERROR("Project audio table header context popup did not open");
        return false;
    }
    auto* popup = context.OpenPopupStack.back().Window;
    // 弹窗首帧测量和下一帧实际尺寸分别推进，不能拿自动测量前的窗口判断高度。
    at(header);
    at(header);
    // 自定义菜单仅十项；原生尺寸、重置和第二套列显隐项不能在上方被追加。
    // 使用字体高度的宽容上界，允许主题留白但拒绝两套完整菜单叠加。
    // 此时没有展开子菜单，父弹窗只能包含自身的显隐及操作入口。
    // 列数固定为六，资源数量增加不能改变该菜单的结构与高度。
    if ( popup->Size.y >= ImGui::GetFrameHeightWithSpacing() * 14 ) {
        XERROR("Project audio table contains duplicated default context menu");
        return false;
    }
    const ImVec2 menuItem{ popup->Pos.x + popup->WindowPadding.x + 20,
                           popup->Pos.y + popup->Size.y -
                               popup->WindowPadding.y -
                               ImGui::GetTextLineHeight() * 0.5F };
    // 悬停最后一项列显隐菜单，与截图中的重复列标签走相同的 ID 检测路径。
    // 连续帧让 ImGui 用上一帧 HoveredId 统计本轮同 ID 的可见项数量。
    for ( int frame = 0; frame < 3; ++frame ) at(menuItem);
    // 不关闭冲突提示选项、不放宽重复 ID，只允许当前菜单项被提交一次。
    // HoveredId 非空还能保证测试实际命中菜单，而非在空白处误判没有冲突。
    if ( context.HoveredWindow != popup || context.HoveredId == 0 ||
         context.HoveredIdPreviousFrameItemCount != 1 ) {
        XERROR("Project audio table context menu ID conflict");
        return false;
    }
    // 同一弹层中的列菜单还必须真实生效，不能只满足 ID 唯一性。
    // 保存所属表格指针仅用于本测试同步帧，不以 popup 的当前表格作目标。
    auto* targetTable = context.Tables.GetByKey(
        table->ParentWindow->GetID("ProjectAudioToolTableV2"));
    if ( !targetTable ) return false;
    // 每列分别隐藏再显示，操作过程中其他五列保持可见。
    // 固定使用物理列号匹配菜单字段，表头的可见位置不代表资源字段身份。
    // 每一轮先恢复完整可见状态，下一轮不会因上轮隐藏项而改变预期集合。
    // 六个物理列都经过实际菜单点击，覆盖固定列、伸缩列及不可排序列。
    for ( int key = 5; key >= 0; --key ) {
        for ( const bool enabled : { false, true } ) {
            if ( context.OpenPopupStack.empty() ) {
                // 菜单通常由上次点击关闭；再次从真实表头右键打开。
                // 隐藏文件名后，同一表头区域仍属于外层表格，右键入口必须可用。
                // 先移回表头再投递按下边沿，让上一帧悬停状态匹配实际窗口。
                at(header);
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right,
                                                   true);
                at(header);
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right,
                                                   false);
                at(header);
                at(header);
            }
            if ( context.OpenPopupStack.empty() ||
                 !context.OpenPopupStack.back().Window )
                return false;
            // 每次重新取本轮弹层窗口，不把旧弹层的几何当作当前命中范围。
            // 这里只读取实际打开栈，不能直接 OpenPopup 来绕过生产表头逻辑。
            popup = context.OpenPopupStack.back().Window;
            // 自动测量完成后按底部六个连续菜单项定位，不依赖中文文本宽度。
            // 弹层可能使用独立行距，必须从最后一行布局读出实际步长。
            // 不能套用测试宿主的 ItemSpacing，累计偏差会让上方菜单项误命中。
            at(header);
            at(header);
            const ImVec2 columnItem{ popup->Pos.x + popup->WindowPadding.x + 20,
                                     popup->DC.CursorPosPrevLine.y +
                                         popup->DC.PrevLineSize.y * 0.5F -
                                         (5 - key) *
                                             (popup->DC.CursorPos.y -
                                              popup->DC.CursorPosPrevLine.y) };
            at(columnItem);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            at(columnItem);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            at(columnItem);
            // 列显隐在下一轮表格布局应用，不能只检查请求位就宣称已隐藏。
            // 同时检查最终状态和下一帧状态，排除被每帧初始化覆盖的情况。
            // IsEnabled 表示用户显隐结果，不用横向裁剪造成的 IsVisible 作判断。
            // 表格较窄时试听列可能暂时在视口外，这不应被当作用户已经隐藏。
            at(header);
            for ( int column = 0; column < 6; ++column ) {
                // 目标列可变，其余列必须保持开启，防止弹层错误关联另一列。
                // 恢复开启也走同一个菜单项，验证取消勾选后仍能找到该资源字段。
                const bool expected = column == key ? enabled : true;
                if ( targetTable->Columns[column].IsEnabled != expected ||
                     targetTable->Columns[column].IsUserEnabledNextFrame !=
                         expected ) {
                    XERROR(
                        "Table column visibility failed: clicked={}, "
                        "column={}, expected={}",
                        key,
                        column,
                        expected);
                    return false;
                }
            }
        }
    }
    // 菜单点击应自行关闭，不能用关闭栈的清理调用掩盖点击未被接受。
    // 后续行选择和音量编辑沿用同一表格，留下的弹层会继续阻挡实际点击。
    // 因此关闭状态同样属于菜单操作契约，不只是测试之间的可选清理。
    if ( !context.OpenPopupStack.empty() ) return false;
    // 排序菜单与列显隐共用弹层，但排序要等退出弹层后才能使用表格上下文。
    if ( !checkTableSortingMenus(manager, view, table, targetTable) )
        return false;
    // 指针放在首行文件名列，按真实按下/松开时序激活公共 Selectable。
    // 使用实际表格几何而非固定坐标，避免顶部工具栏增加后点击到搜索框。
    // 先移动再按下，保持与 GLFW 后端逐帧投递输入的时序一致。
    const ImVec2 pointer(
        table->InnerRect.Min.x + 30.0F,
        table->InnerRect.Min.y + ImGui::GetFrameHeightWithSpacing() + 5.0F);
    drawFrame(manager, view, true, 0.0F, false, 450.0F, false, pointer);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame(manager, view, true, 0.0F, false, 450.0F, false, pointer);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame(manager, view, true, 0.0F, false, 450.0F, false, pointer);
    if ( workspace.m_projectAudioToolSelectedResourceId.empty() ) return false;
    // 选择只通过公共 UI 入口完成，工作区值证明提交已经发生。
    // 选择必须保存画笔资源，但不可像方块抓取那样提升 Z 序或挪动位置。
    const auto& current = workspace.m_projectAudioToolPlacements;
    if ( original.size() != current.size() ) return false;
    for ( std::size_t i = 0; i < current.size(); ++i ) {
        if ( original[i].m_audioResourceId != current[i].m_audioResourceId ||
             original[i].m_x != current[i].m_x ||
             original[i].m_y != current[i].m_y ||
             original[i].m_width != current[i].m_width ||
             original[i].m_height != current[i].m_height ||
             original[i].m_zOrder != current[i].m_zOrder )
            return false;
    }
    // 窄宿主中的音量列位于横向溢出区，先把真实表格滚动到右侧。
    // 调整的是窗口滚动目标，列宽和控件命中仍由生产表格生成。
    ImGui::SetScrollX(table, table->ScrollMax.x);
    for ( int frame = 0; frame < 2; ++frame ) drawFrame(manager, view, true);
    // 第二行尚未选中，直接拖动其音量必须同时激活资源并保存新倍率。
    // 不先点击文件名，防止“先选择后调节”的旧入口伪装成通过。
    auto* columns = ImGui::GetCurrentContext()->Tables.GetByKey(
        table->ParentWindow->GetID("ProjectAudioToolTableV2"));
    if ( !columns || columns->ColumnsCount != 6 ) {
        XERROR("Project audio table columns unavailable");
        return false;
    }
    const auto& volumeColumn = columns->Columns[4];
    const float left =
        std::max(volumeColumn.WorkMinX, volumeColumn.ClipRect.Min.x);
    const float right =
        std::min(volumeColumn.WorkMaxX, volumeColumn.ClipRect.Max.x);
    // 以实际可见列区域定位，不把隐藏或裁剪区域当作可用控件。
    if ( right - left < 10 ) {
        XERROR("Project audio volume column clipped: left={}, right={}",
               left,
               right);
        return false;
    }
    const float rowHeight =
        ImGui::GetFrameHeight() + 9 + ImGui::GetStyle().CellPadding.y * 2;
    // 根据旧倍率选择相反半区，重复运行也不会恰好点中当前滑块值。
    const float fraction =
        workspace.m_projectAudioToolBrushVolume < 1.0F ? 0.8F : 0.2F;
    const ImVec2 volumePointer{ left + (right - left) * fraction,
                                pointer.y + rowHeight };
    const auto   previousResource =
        workspace.m_projectAudioToolSelectedResourceId;
    const float previousVolume = workspace.m_projectAudioToolBrushVolume;
    // 统计真实项目保存通知，拖动期间不得保存整份项目或使资源布局缓存反复失效。
    // 仅观察事件，不屏蔽生产保存回调，也不直接改写工作区来模拟提交。
    int        saves            = 0;
    const auto saveSubscription = MMM::Event::EventBus::instance()
                                      .subscribe<MMM::Event::ProjectSavedEvent>(
                                          [&](const auto&) { ++saves; });
    // 移动、按下和释放各自独立成帧，覆盖真实滑块激活和最终提交。
    drawFrame(manager, view, true, 0, false, 450, false, volumePointer);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame(manager, view, true, 0, false, 450, false, volumePointer);
    // 保持按下并连续移动，覆盖多个实际百分数变化，而非只测试单击设置。
    for ( int frame = 0; frame < 8; ++frame ) {
        const ImVec2 moving{ left + (right - left) * (0.3F + frame * 0.06F),
                             volumePointer.y };
        drawFrame(manager, view, true, 0, false, 450, false, moving);
    }
    // 松开帧即使不再产生数值变化，也必须提交此前积累的最终音量。
    // 使用实际保存事件核对提交次数，避免仅凭界面数值判断已落盘。
    // 测试不允许临时禁用项目事件来掩盖拖动过程中的缓存重建。
    // 后续既有断言还会验证独立采样值，防止合并保存破坏音量归属。
    const int savesWhileDragging = saves;
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame(manager, view, true, 0, false, 450, false, volumePointer);
    // 先取消观察订阅再判断，失败分支也不能留下捕获栈变量的回调。
    MMM::Event::EventBus::instance().unsubscribe<MMM::Event::ProjectSavedEvent>(
        saveSubscription);
    if ( savesWhileDragging != 0 || saves != 1 ) {
        XERROR("Sample volume saves during drag={}, total={}",
               savesWhileDragging,
               saves);
        return false;
    }
    // 断言生产持久化结果，不借用视图私有音量或直接调用选择 helper。
    if ( workspace.m_projectAudioToolSelectedResourceId == previousResource ||
         std::abs(workspace.m_projectAudioToolBrushVolume - previousVolume) <
             0.001F ) {
        XERROR(
            "Unselected table row volume edit failed: resource={} -> {}, "
            "volume={} -> {}, pointer={},{}",
            previousResource,
            workspace.m_projectAudioToolSelectedResourceId,
            previousVolume,
            workspace.m_projectAudioToolBrushVolume,
            volumePointer.x,
            volumePointer.y);
        return false;
    }
    const auto  selected     = workspace.m_projectAudioToolSelectedResourceId;
    const float editedVolume = workspace.m_projectAudioToolBrushVolume;
    // 第二行的新倍率只归属该资源；第一行和所有其他行仍与编辑前完全一致。
    // 对照的是持久化条目，能同时捕获共享滑块和保存时广播倍率的问题。
    for ( const auto& placement : workspace.m_projectAudioToolPlacements ) {
        const auto old = std::find_if(
            original.begin(), original.end(), [&](const auto& previous) {
                return previous.m_audioResourceId ==
                       placement.m_audioResourceId;
            });
        if ( old == original.end() || !placement.m_audioVolume ) return false;
        const float expected = placement.m_audioResourceId == selected
                                   ? editedVolume
                                   : old->m_audioVolume.value_or(1.0F);
        if ( std::abs(*placement.m_audioVolume - expected) > 0.001F ) {
            XERROR("Table sample volume leaked to {}",
                   placement.m_audioResourceId);
            return false;
        }
    }
    // 先触发生产保存刷新，再点回第一行，画笔必须恢复它自己的倍率。
    // 音量列横向溢出，返回文件名列后通过原有 Selectable 入口切换采样。
    MMM::Event::EventBus::instance().publish(MMM::Event::ProjectSavedEvent{});
    ImGui::SetScrollX(table, 0);
    for ( int frame = 0; frame < 3; ++frame ) drawFrame(manager, view, true);
    drawFrame(manager, view, true, 0, false, 450, false, pointer);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame(manager, view, true, 0, false, 450, false, pointer);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame(manager, view, true, 0, false, 450, false, pointer);
    // 只读取原始记录作比较，避免把刚提交的新工作区当成期望值。
    // 原始记录缺失倍率时按旧项目未调节采样的 100 解释。
    const auto restored = std::find_if(
        original.begin(), original.end(), [&](const auto& placement) {
            return placement.m_audioResourceId == previousResource;
        });
    if ( restored == original.end() ||
         workspace.m_projectAudioToolSelectedResourceId != previousResource ||
         workspace.m_projectAudioToolBrushVolume !=
             restored->m_audioVolume.value_or(1.0F) ) {
        XERROR("Reselecting sample did not restore its own volume");
        return false;
    }
    // 第二张卡片的倍率在缓存刷新和切回前一采样后仍应保留。
    const auto edited =
        std::find_if(workspace.m_projectAudioToolPlacements.begin(),
                     workspace.m_projectAudioToolPlacements.end(),
                     [&](const auto& placement) {
                         return placement.m_audioResourceId == selected;
                     });
    if ( edited == workspace.m_projectAudioToolPlacements.end() ||
         edited->m_audioVolume != editedVolume )
        return false;
    // 点击两种视图共用的重置按钮，不直接调用私有 helper。
    // 表格模式在按钮前只有展示选择和自动试听两行，位置取真实宿主内容起点。
    const auto*  host = table->ParentWindow;
    const ImVec2 resetPoint{ host->DC.CursorStartPos.x + 20.0F,
                             host->DC.CursorStartPos.y +
                                 ImGui::GetFrameHeightWithSpacing() * 2.0F +
                                 ImGui::GetFrameHeight() * 0.5F };
    drawFrame(manager, view, true, 0, false, 450, false, resetPoint);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame(manager, view, true, 0, false, 450, false, resetPoint);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame(manager, view, true, 0, false, 450, false, resetPoint);
    // 全部资源包括屏幕外行都恢复为 100；选择保留，下一次放置也使用 100。
    for ( const auto& placement : workspace.m_projectAudioToolPlacements ) {
        if ( placement.m_audioVolume != 1.0F ) {
            XERROR("Reset all sample volumes missed {}",
                   placement.m_audioResourceId);
            return false;
        }
    }
    // 记录全部重置之外，还要确认引擎读取的当前画笔快照同步恢复。
    // 只重置列表会留下“显示为 100，下一次放置仍用旧音量”的不一致。
    if ( workspace.m_projectAudioToolBrushVolume != 1.0F ) return false;
    // 复用原实例切回，不能依靠析构重建恢复默认布局来通过测试。
    view.setTableView(false);
    for ( int frame = 0; frame < 3; ++frame ) drawFrame(manager, view, true);
    // 切回后方块恢复绘制，表格选中的资源仍然是当前音频画笔。
    return !view.isTableView() && findCanvas() && findCanvas()->Active &&
           workspace.m_projectAudioToolSelectedResourceId == previousResource;
}
}  // namespace

/// @brief 在隔离配置和项目目录中运行真实 UI 回归。
/// @return 滚动、音量编辑与连续重播行为全部通过时返回零。
/// @details 后端在 UI 交互期间消费真实 PCM，不以固定零游标伪造重播成功。
/// 虚拟设备不提供实际听感验收，音频断言只覆盖源游标和播放状态。
int main()
{
    // 单例按生产依赖顺序创建，避免事件订阅者晚于事件总线析构。
    MMM::Logic::EditorEngine::instance();
    auto&           controller = MMM::Logic::ProjectController::instance();
    const auto      root       = std::filesystem::current_path() /
                                 "test_output/ProjectAudioToolScrollTest";
    std::error_code error;
    std::filesystem::create_directories(root, error);
    // 目录失败时不进入控制器；打开失败时不解引用当前项目指针。
    if ( error || !controller.openProject(root).m_opened ) return 1;
    // 开放控制器接口提供生产视图的数据源，不改私有状态或使用测试替身。
    prepareResources(*controller.currentProject());
    if ( !writePreviewFixture(root) ) return 1;
    // CTest 显式选择 dummy 驱动；真实音频图用于验证源游标复位。
    auto& audioSettings =
        MMM::Config::AppConfig::instance().getEditorSettings();
    audioSettings.audioPlaybackBackend = MMM::Config::AudioPlaybackBackend::SDL;
    audioSettings.sdlAudioOutputDeviceName.clear();
    MMM::Runtime::AppThreadPool::instance().init();
    auto& audio = MMM::Audio::AudioManager::instance();
    audio.init();
    // CTest 已隔离 AppConfig 根；这里只修改本进程测试偏好。
    MMM::Config::AppConfig::instance().getEditorSettings().professionalMode =
        true;
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    // 大显示区域容纳最后的高宿主场景，确保画布仍处于可见剪裁区。
    io.DisplaySize = { 900.0F, 3400.0F };
    io.DeltaTime   = 1.0F / 60.0F;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    // 无平台后端也需要字体图集，以满足 NewFrame 的前置条件。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 失败结果仍经过正常析构和项目关闭，不能用提前退出绕过资源释放。
    bool passed = false;
    {
        // UI 对象先于 ImGui context 释放，保持 Clay 和窗口的生命周期配对。
        MMM::UI::UIManager            manager;
        MMM::UI::ProjectAudioToolView view("AudioToolScrollTest");
        // 分阶段记录失败入口，平台差异不能只留下一个笼统的末尾错误。
        // 保留原有短路顺序：弹层失败时，不让后续滚动覆盖最先出现的输入异常。
        // 诊断只输出所属阶段，不跳过断言、不重试输入，也不降低通过条件。
        passed = checkVolumePopup(manager, view);
        if ( !passed ) XERROR("Project audio volume popup regression");
        if ( passed ) {
            passed = checkRapidPreview(manager, view);
            if ( !passed ) XERROR("Project audio repeated preview regression");
        }
        if ( passed ) {
            passed = checkSnapToggle(manager, view);
            if ( !passed ) XERROR("Project audio snapping toggle regression");
        }
        if ( passed ) {
            passed = checkScrolling(manager, view);
            if ( !passed ) XERROR("Project audio scrolling regression");
        }
        if ( passed ) {
            passed = checkTableView(manager, view);
            if ( !passed ) XERROR("Project audio table view regression");
        }
    }
    // 先关闭生产项目数据源，再销毁 ImGui；控制器不保留工具视图的窗口指针。
    controller.closeProject();
    ImGui::DestroyContext();
    // 图形视图先析构再停止音频，避免 UI 的试听状态查询访问已清理的池。
    // 音频后台全部退出后再销毁线程池，准备任务不能失去执行资源。
    audio.shutdown();
    MMM::Runtime::AppThreadPool::instance().shutdown();
    if ( !passed ) XERROR("Project audio tool docked scrolling regression");
    return passed ? 0 : 1;
}
