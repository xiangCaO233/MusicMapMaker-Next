#include "ui/imgui/CLayoutTestUI.h"
#include "imgui.h"

// 必须在其中一个源文件中定义实现
#define CLAY_IMPLEMENTATION
#include <clay.h>

namespace MMM::Graphic::UI
{


// 准备 Clay 所需的内存竞技场 (Arena)
static uint64_t   clayMemorySize = Clay_MinMemorySize();
static Clay_Arena clayArena;
static bool       clayInitialized = false;

// 将变量转换为 Clay_String 的辅助函数
inline Clay_String ToClayString(const char* str)
{
    return Clay_String{ .isStaticallyAllocated = false,
                        .length                = (int32_t)strlen(str),
                        .chars                 = str };
}

CLayoutTestUI::CLayoutTestUI()
{
    if ( !clayInitialized ) {
        clayArena = Clay_CreateArenaWithCapacityAndMemory(
            clayMemorySize, malloc(clayMemorySize));
        Clay_Initialize(clayArena, { 800, 600 }, { [](Clay_ErrorData error) {
                            // 简单的错误回调
                        } });
        clayInitialized = true;
    }
}

CLayoutTestUI::~CLayoutTestUI() {}

void CLayoutTestUI::update()
{
    ImGui::Begin("Clay Powered HBox");

    // --- 1. 准备阶段 ---
    ImVec2 windowContentStart =
        ImGui::GetCursorScreenPos();  // 记录 ImGui 当前内容的起始绝对坐标
    ImVec2 availableSize = ImGui::GetContentRegionAvail();  // 获取可用空间

    // 告诉 Clay 现在的画布有多大
    Clay_SetLayoutDimensions({ availableSize.x, availableSize.y });
    Clay_BeginLayout();

    // --- 2. 定义布局 (HBox) ---
    // 在 v0.14 中，CLAY 宏第一个参数是 ID，后面直接跟初始化列表
    CLAY(CLAY_ID("MainHBox"), { 
    .layout = { 
        .sizing = { 
            .width = CLAY_SIZING_GROW(), 
            .height = CLAY_SIZING_GROW() // 修复点：高度也设为 GROW，填满窗口
        }, 
        .layoutDirection = CLAY_LEFT_TO_RIGHT, 
        .padding = { 10, 10, 10, 10 }, 
        .childGap = 15,
        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } // 此时垂直居中才会生效
    }
})
    {
        CLAY(CLAY_ID("Btn1Slot"),
             { .layout = { .sizing = { .width  = CLAY_SIZING_GROW(),
                                       .height = CLAY_SIZING_GROW() } } })
        {
        }

        CLAY(CLAY_ID("Btn2Slot"),
             { .layout = { .sizing = { .width  = CLAY_SIZING_GROW(),
                                       .height = CLAY_SIZING_GROW() } } })
        {
        }
    }

    Clay_EndLayout();

    // 映射函数
    auto drawSlot = [&](const char* idStr, const char* label) {
        // 使用手动构造的 Clay_String 替代宏
        Clay_String clayIdStr = { .isStaticallyAllocated = false,
                                  .length = (int32_t)strlen(idStr),
                                  .chars  = idStr };

        // 获取 ID 数据
        Clay_ElementData data =
            Clay_GetElementData(Clay_GetElementId(clayIdStr));

        if ( data.found ) {
            ImGui::SetCursorScreenPos(
                ImVec2(windowContentStart.x + data.boundingBox.x,
                       windowContentStart.y + data.boundingBox.y));
            ImGui::Button(
                label, ImVec2(data.boundingBox.width, data.boundingBox.height));
        } else {
            ImGui::SetCursorScreenPos(windowContentStart);
            ImGui::Text("Error: ID %s not found!", idStr);
        }
    };

    drawSlot("Btn1Slot", "Button 1");
    drawSlot("Btn2Slot", "Button 2");

    // --- 4. 收尾阶段 ---
    // 为了不让 ImGui 接下来的控件乱套，需要把光标放回到布局结束后的位置
    // 或者简单地占个位
    ImGui::SetCursorScreenPos(windowContentStart);
    ImGui::Dummy(availableSize);
    ImGui::End();
}

}  // namespace MMM::Graphic::UI
