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
