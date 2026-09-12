#define CLAY_IMPLEMENTATION
#include "ui/layout/CLayWrapperCore.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/layout/CLayDefs.h"

/// @file CLayWrapperCore.cpp
/// @brief Clay Arena 生命周期与 ImGui 字体测量适配的实现。
/// @details Clay 上下文直接构造在调用方分配的 Arena 中，因此释放 Arena 即
/// 销毁对应上下文；当前上下文指针必须先解除绑定。

namespace MMM::UI
{
/// @brief 将 Clay 布局诊断转发到项目日志系统。
/// @param errorData Clay 提供的错误文本视图。
static void HandleClayError(Clay_ErrorData errorData)
{
    XWARN("CLAY WARN: {}", errorData.errorText.chars);
}
/// @brief 获取进程内唯一 Clay 包装器。
/// @return 静态生命周期的包装器引用。
CLayWrapperCore& CLayWrapperCore::instance()
{
    static CLayWrapperCore core;
    return core;
}

/// @brief 分配并初始化单例使用的默认 Clay Arena。
CLayWrapperCore::CLayWrapperCore()
{
    // Clay_MinMemorySize 与当前编译的 Clay 配置保持一致。
    clayMemorySize = Clay_MinMemorySize();
    void* memory   = std::malloc(clayMemorySize);

    // Arena 借用分配的连续内存，所有上下文数据都位于其中。
    clayArena = Clay_CreateArenaWithCapacityAndMemory(clayMemorySize, memory);

    // 初始尺寸为零，实际窗口布局开始前再同步当前可用尺寸。
    Clay_Initialize(clayArena, { 0, 0 }, { HandleClayError });
}

/// @brief 释放默认 Clay Arena。
CLayWrapperCore::~CLayWrapperCore()
{
    // 上下文存储在 Arena 内，无需单独析构 Clay_Context。
    if ( clayArena.memory ) {
        std::free(clayArena.memory);
    }
}

/// @brief 创建一个窗口独占的 Clay 上下文和 Arena。
/// @return 已成为当前上下文的 WindowContext。
CLayWrapperCore::WindowContext CLayWrapperCore::createWindowContext()
{
    // 各窗口使用完整最小容量，避免共享 Arena 引起布局状态串扰。
    uint32_t   memSize = Clay_MinMemorySize();
    void*      memory  = std::malloc(memSize);
    Clay_Arena arena   = Clay_CreateArenaWithCapacityAndMemory(memSize, memory);

    // Clay_Initialize 自动把新建上下文设为当前上下文。
    Clay_Context* ctx = Clay_Initialize(arena, { 0, 0 }, { HandleClayError });
    // 测量回调属于上下文状态，每个新上下文都需要安装一次。
    setupClayTextMeasurement();

    return { ctx, arena };
}

/// @brief 销毁窗口上下文拥有的 Arena 并清空句柄。
/// @param ctx 由 createWindowContext 创建的窗口上下文。
void CLayWrapperCore::destroyWindowContext(WindowContext& ctx)
{
    // 当前上下文不能继续指向即将释放的 Arena。
    if ( ctx.context == Clay_GetCurrentContext() ) {
        Clay_SetCurrentContext(nullptr);
    }

    // 空 Arena 允许重复清理调用安全地跳过释放。
    if ( ctx.arena.memory ) {
        // Clay_Context 位于这块内存中，释放 Arena 即同时销毁上下文数据。
        std::free(ctx.arena.memory);

        // 清空两个观察句柄，防止调用方继续切换到已销毁上下文。
        ctx.arena.memory = nullptr;
        ctx.context      = nullptr;
    }
}

/// @brief 使用项目 ImGui 字体测量 Clay 文本节点。
/// @param text Clay 提供的非空结尾字符切片。
/// @param config 文本字体 ID 与样式配置。
/// @param userData 当前未使用的扩展上下文。
/// @return 与实际 ImGui 字体渲染一致的文本宽高。
/// @warning 布局热路径：每个文本节点可能调用，不得分配持久资源或阻塞。
static Clay_Dimensions MeasureTextForImGui(Clay_StringSlice        text,
                                           Clay_TextElementConfig* config,
                                           void*                   userData)
{
    using namespace MMM::Config;
    auto&   skinMgr = SkinManager::instance();
    ImFont* font    = nullptr;

    // Clay FontID 与皮肤字体角色一一映射，缺失项稍后回退默认字体。
    switch ( static_cast<FontID>(config->fontId) ) {
    case FontID::Content: font = skinMgr.getFont("content"); break;
    case FontID::Title: font = skinMgr.getFont("title"); break;
    case FontID::Menu: font = skinMgr.getFont("menu"); break;
    case FontID::FileManager: font = skinMgr.getFont("filemanager"); break;
    case FontID::SideBar: font = skinMgr.getFont("side_bar"); break;
    case FontID::SettingInternal:
        font = skinMgr.getFont("setting_internal");
        break;
    case FontID::PureIcons: font = skinMgr.getFont("pure_icons"); break;
    default: font = ImGui::GetFont(); break;
    }

    // 皮肤未提供指定角色时仍需返回可用度量。
    if ( !font ) font = ImGui::GetFont();

    // 使用实际加载尺寸而不是 Clay 配置值，保证布局与 ImGui 渲染一致。
    // ImGui 1.92+ 可按任意尺寸渲染，但 LegacySize 保留 AddFont 的基准像素。
    // 临时字符串补上空字符，以满足 CalcTextSizeA 的输入契约。
    std::string s(text.chars, text.length);
    float       actualSize = font->LegacySize * font->Scale;
    ImVec2      sz = font->CalcTextSizeA(actualSize, FLT_MAX, 0.0f, s.c_str());

    return { sz.x, sz.y };
}

/// @brief 为当前 Clay 上下文注册 ImGui 文本测量回调。
void CLayWrapperCore::setupClayTextMeasurement()
{
    // 回调所需状态均来自全局皮肤与 ImGui 上下文，无需 userData。
    Clay_SetMeasureTextFunction(MeasureTextForImGui, nullptr);
}

}  // namespace MMM::UI
