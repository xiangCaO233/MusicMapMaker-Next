#define CLAY_IMPLEMENTATION
#include "ui/layout/CLayWrapperCore.h"
#include "imgui.h"
#include "log/colorful-log.h"

namespace MMM::Graphic::UI
{
// 错误处理回调
static void HandleClayError(Clay_ErrorData errorData)
{
    XWARN("CLAY WARN: {}", errorData.errorText.chars);
}


CLayWrapperCore& CLayWrapperCore::instance()
{
    static CLayWrapperCore core;
    return core;
}

CLayWrapperCore::CLayWrapperCore()
{
    // 1. 计算内存需求并申请
    clayMemorySize = Clay_MinMemorySize();
    void* memory   = std::malloc(clayMemorySize);

    // 2. 创建 Arena
    clayArena = Clay_CreateArenaWithCapacityAndMemory(clayMemorySize, memory);

    // 3. 初始化 Clay，指定屏幕尺寸（初始可以设为0，后续随窗口resize更新）
    Clay_Initialize(clayArena, { 0, 0 }, { HandleClayError });
}

CLayWrapperCore::~CLayWrapperCore()
{
    // 释放 Arena 内存
    if ( clayArena.memory ) {
        std::free(clayArena.memory);
    }
}

CLayWrapperCore::WindowContext CLayWrapperCore::createWindowContext()
{
    uint32_t   memSize = Clay_MinMemorySize();
    void*      memory  = std::malloc(memSize);
    Clay_Arena arena   = Clay_CreateArenaWithCapacityAndMemory(memSize, memory);

    // 初始化并返回。注意：Clay_Initialize 会自动将其设为 Current
    Clay_Context* ctx = Clay_Initialize(arena, { 0, 0 }, { HandleClayError });
    setupClayTextMeasurement();  // 每个上下文都需要设置一次

    return { ctx, arena };
}

void CLayWrapperCore::destroyWindowContext(WindowContext& ctx)
{
    // 1. 安全检查：如果销毁的是当前正在使用的上下文，先切回 nullptr
    if ( ctx.context == Clay_GetCurrentContext() ) {
        Clay_SetCurrentContext(nullptr);
    }

    // 2. 释放 Arena 内存
    if ( ctx.arena.memory ) {
        // 因为 Clay_Context 指针就指向这块内存的开头，
        // 所以释放 memory 就会同时销毁 context 数据
        std::free(ctx.arena.memory);

        // 3. 句柄置空，防止野指针
        ctx.arena.memory = nullptr;
        ctx.context      = nullptr;
    }
}

// 告诉 Clay 怎么算 ImGui 里的文字宽度
static Clay_Dimensions MeasureTextForImGui(Clay_StringSlice        text,
                                           Clay_TextElementConfig* config,
                                           void*                   userData)
{
    // 使用 ImGui 的字体 API 测量
    std::string s(text.chars, text.length);
    ImVec2      size = ImGui::GetFont()->CalcTextSizeA(
        (float)config->fontSize, FLT_MAX, 0.0f, s.c_str());
    return { size.x, size.y };
}

void CLayWrapperCore::setupClayTextMeasurement()
{
    Clay_SetMeasureTextFunction(MeasureTextForImGui, nullptr);
}

}  // namespace MMM::Graphic::UI
