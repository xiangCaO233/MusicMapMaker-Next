#pragma once

#include "canvas/TrackLayoutEditing.h"
#include "common/CanvasComponentLayout.h"
#include "common/ChartObjectKind.h"
#include "config/visual/CanvasComponentConfig.h"
#include "event/core/EventBus.h"
#include "mmm/annotation/BeatmapAnnotation.h"
#include <array>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMM::Logic
{
struct RenderSnapshot;
}

namespace MMM::UI
{
class UIManager;
}

namespace MMM::Canvas
{

class Basic2DCanvasInteraction
{
public:
    Basic2DCanvasInteraction(const std::string& canvasName,
                             const std::string& cameraId);
    ~Basic2DCanvasInteraction();

    void update(UI::UIManager*               sourceManager,
                const Logic::RenderSnapshot* currentSnapshot, float targetWidth,
                float targetHeight);

    /// @brief 推进并绘制交互层的临时 UI。
    /// @warning UI 热路径：每帧最多绘制一个播放速度提示窗口。
    void updateTransientUi();

    /// @brief 处理当前鼠标所在主画布上的 Ctrl/Command/Alt 修饰键滚轮。
    /// @param currentSnapshot 当前渲染快照。
    /// @param allowSelectionScroll 是否允许 Ctrl 滚轮移动活动框选。
    /// @return 本帧滚轮已被修饰键命令消费时返回 true。
    /// @warning UI 输入路径：仅在滚轮事件发生时调用；可能发布逻辑命令或广播
    /// 编辑器配置更新，禁止放入无条件每帧路径。
    bool handleModifierWheel(const Logic::RenderSnapshot* currentSnapshot,
                             bool allowSelectionScroll = true);

private:
    struct PendingDrop {
        std::vector<std::string> paths;
        glm::vec2                pos;
    };

    /// @brief 上一次发送给逻辑线程的鼠标状态，用于过滤重复交互命令。
    struct LastMouseCommand {
        /// @brief 是否已经记录过一次鼠标命令。
        bool valid{ false };
        /// @brief 上一次发送的本地鼠标坐标。
        glm::vec2 pos{ 0.0f, 0.0f };
        /// @brief 上一次发送的视口宽度。
        float viewportWidth{ 0.0f };
        /// @brief 上一次发送的视口高度。
        float viewportHeight{ 0.0f };
        /// @brief 上一次发送的窗口悬浮状态。
        bool isHovering{ false };
        /// @brief 上一次发送的鼠标拖拽状态。
        bool isDragging{ false };
    };

    /// @brief 上一次发送的连续拖动编辑命令，用于过滤同一手势内的重复更新。
    struct LastContinuousEditCommand {
        /// @brief 是否已经记录过一次拖动编辑命令。
        bool valid{ false };
        /// @brief 上一次发送的本地鼠标坐标。
        glm::vec2 pos{ 0.0f, 0.0f };
        /// @brief 上一次发送时的画布视觉时间。
        double visualTime{ 0.0 };
        /// @brief 上一次发送时的可见时间范围起点。
        double visibleTimeStart{ 0.0 };
        /// @brief 上一次发送时的可见时间范围终点。
        double visibleTimeEnd{ 0.0 };
        /// @brief 上一次发送时的垂直渲染缩放。
        float renderScaleY{ 1.0f };
        /// @brief 上一次发送时的分拍吸附分母。
        int beatDivisor{ 4 };
        /// @brief 上一次发送时的主修饰键状态。
        bool primaryModifier{ false };
        /// @brief 上一次发送时的副修饰键状态。
        bool secondaryModifier{ false };
    };

    /// @brief 同步变换开始时的单轨 KPS 布局与像素边界。
    struct SynchronizedKpsTransformStart {
        /// @brief 从零开始的轨道序号。
        std::int64_t instanceIndex{ 0 };
        /// @brief 缩放开始时的布局。
        Config::CanvasComponentPlacement placement;
        /// @brief 缩放开始时的实际文字边界。
        Logic::CanvasComponentBounds bounds;
        /// @brief 该实例允许占用的布局区域。
        Logic::CanvasComponentBounds region;
    };

    /// @brief 当前快照中一个可见物件合并后的布局包围框。
    struct NoteLayoutInstance {
        /// @brief 对应的物件实体。
        entt::entity entity{ entt::null };
        /// @brief 物件全部可交互渲染部位的合并像素边界。
        Logic::CanvasComponentBounds bounds;
    };

    std::string              m_canvasName;
    std::string              m_cameraId;
    std::vector<PendingDrop> m_pendingDrops;
    Event::SubscriptionID    m_dropSubId;

    void handleDrops(UI::UIManager* sourceManager);
    void handleHotkeys(const Logic::RenderSnapshot* currentSnapshot);
    void handleInteractions(const Logic::RenderSnapshot* currentSnapshot,
                            float targetWidth, float targetHeight);
    /// @brief 在移动工具下绘制当前悬浮物件的独立音频试听按钮。
    /// @param currentSnapshot 当前主画布渲染快照。
    /// @param canvasScreenX 画布左上角屏幕横坐标。
    /// @param canvasScreenY 画布左上角屏幕纵坐标。
    /// @param targetWidth 画布宽度。
    /// @param targetHeight 画布高度。
    /// @param pointerX 指针相对画布左侧的像素坐标。
    /// @param pointerY 指针相对画布顶部的像素坐标。
    /// @return 指针位于试听按钮或物件到按钮的过渡热区时返回 true。
    /// @warning UI 热路径：移动工具下每帧只扫描当前可见拾取盒并提交三个
    /// ImGui 按钮；音频资源查找与加载仅在点击后发生。
    bool renderObjectAudioPreviewControls(
        const Logic::RenderSnapshot& currentSnapshot, float canvasScreenX,
        float canvasScreenY, float targetWidth, float targetHeight,
        float pointerX, float pointerY);
    /// @brief 绘制批注标记区、悬浮详情和时间戳批注编辑弹窗。
    /// @param currentSnapshot 当前主画布渲染快照。
    /// @param canvasScreenX 画布左上角屏幕横坐标。
    /// @param canvasScreenY 画布左上角屏幕纵坐标。
    /// @param targetWidth 画布宽度。
    /// @param targetHeight 画布高度。
    /// @param pointerX 指针相对画布左侧的横坐标。
    /// @param pointerY 指针相对画布顶部的纵坐标。
    /// @param canvasHovered 指针是否位于当前画布。
    /// @return 批注标记区或弹窗正在取得画布交互所有权时返回 true。
    /// @warning UI 热路径：只遍历当前快照已裁剪的可见批注标记；不访问 ECS、
    /// 文件系统或完整谱面。
    bool renderAnnotationGutter(const Logic::RenderSnapshot& currentSnapshot,
                                float canvasScreenX, float canvasScreenY,
                                float targetWidth, float targetHeight,
                                float pointerX, float pointerY,
                                bool canvasHovered);
    /// @brief 绘制并处理轨道、判定线与可选画布组件的布局编辑。
    /// @param pointerX 指针相对画布左侧的像素坐标。
    /// @param pointerY 指针相对画布顶部的像素坐标。
    /// @param canvasScreenX 画布左上角屏幕横坐标。
    /// @param canvasScreenY 画布左上角屏幕纵坐标。
    /// @param targetWidth 画布宽度。
    /// @param targetHeight 画布高度。
    /// @param isHovered 指针是否悬停在当前画布。
    /// @param currentSnapshot 当前画布渲染快照。
    /// @warning UI 热路径：布局模式下每帧调用；仅允许常量级命中测试、
    /// ImGui 绘制和配置变更广播。
    void handleLayoutEditing(float pointerX, float pointerY,
                             float canvasScreenX, float canvasScreenY,
                             float targetWidth, float targetHeight,
                             bool                         isHovered,
                             const Logic::RenderSnapshot& currentSnapshot);
    /// @brief 从当前渲染快照的物件拾取盒重建逐物件布局包围框。
    /// @param currentSnapshot 当前画布渲染快照。
    /// @warning UI 布局热路径：每帧只遍历当前可见拾取盒，不得扫描 ECS
    /// 或完整谱面。
    void rebuildNoteLayoutInstances(
        const Logic::RenderSnapshot& currentSnapshot);
    /// @brief 结束布局拖动，并在发生修改时持久化一次编辑器配置。
    /// @warning 低频路径：鼠标释放或退出布局模式时调用，允许写入配置文件。
    void finishLayoutEditing();
    /// @brief 判断连续拖动编辑命令是否需要发送，并在需要时更新缓存。
    /// @param last 上一次发送的拖动编辑命令状态。
    /// @param pos 当前本地鼠标坐标。
    /// @param snapshot 当前渲染快照。
    /// @param primaryModifier 当前主修饰键状态。
    /// @param secondaryModifier 当前副修饰键状态。
    /// @return 需要发送命令时返回 true。
    /// @warning UI 热路径：拖动编辑期间每帧调用；只做常量级数值比较。
    bool shouldSendContinuousEditCommand(LastContinuousEditCommand&   last,
                                         glm::vec2                    pos,
                                         const Logic::RenderSnapshot& snapshot,
                                         bool primaryModifier,
                                         bool secondaryModifier);
    /// @brief 清空同一左键手势下的连续拖动编辑命令缓存。
    void resetContinuousEditCommands();

    float m_speedTooltipTimer{ 0.0f };
    float m_speedTooltipValue{ 1.0f };
    /// @brief 当前鼠标下所有悬浮候选层的签名，用于检测是否切换到其他物件集合。
    std::string m_hoverLayerSignature;
    /// @brief 当前生效的悬浮候选层索引。
    int m_hoverLayerIndex{ 0 };
    /// @brief 当前鼠标下可切换的悬浮候选层数量。
    int m_hoverLayerCount{ 0 };
    /// @brief 当前悬浮物件试听按钮的跨帧锚点。
    struct AudioPreviewOverlayState {
        /// @brief 是否持有有效物件与屏幕边界。
        bool valid{ false };
        /// @brief 试听物件实体。
        entt::entity entity{ entt::null };
        /// @brief 试听物件所在 ECS 注册表。
        Logic::ChartObjectKind objectKind{ Logic::ChartObjectKind::PlayerNote };
        /// @brief 试听引用的项目音频资源 ID。
        std::string audioResourceId;
        /// @brief 只属于当前物件的 AudioManager 试听池标识。
        std::string previewPoolKey;
        /// @brief 物件自身音量倍率。
        float volume{ 1.0F };
        /// @brief 玩家 Polyline 子物件索引；负值表示物件本体或自动采样。
        std::int32_t sampleBindingSubIndex{ -1 };
        /// @brief 音量编辑弹窗是否保持打开，用于跨帧锁定当前物件。
        bool volumeEditorOpen{ false };
        /// @brief 当前锚定拾取盒左边界。
        float left{ 0.0F };
        /// @brief 当前锚定拾取盒上边界。
        float top{ 0.0F };
        /// @brief 当前锚定拾取盒右边界。
        float right{ 0.0F };
        /// @brief 当前锚定拾取盒下边界。
        float bottom{ 0.0F };
        /// @brief 当前试听控制面板左边界。
        float controlsLeft{ 0.0F };
        /// @brief 当前试听控制面板上边界。
        float controlsTop{ 0.0F };
        /// @brief 当前试听控制面板右边界。
        float controlsRight{ 0.0F };
        /// @brief 当前试听控制面板下边界。
        float controlsBottom{ 0.0F };
    };

    /// @brief 允许指针从物件移动到按钮而不使按钮消失的试听覆盖层状态。
    AudioPreviewOverlayState m_audioPreviewOverlay;
    /// @brief 批注编辑弹窗的跨帧状态。
    struct AnnotationEditorState {
        /// @brief 下一帧需要打开弹窗。
        bool requestOpen{ false };
        /// @brief 空字符串表示新建独立时间戳批注。
        std::string annotationId;
        /// @brief 编辑既有批注时显示的原批注人。
        std::string author;
        /// @brief 新建批注的时间戳，单位秒。
        double timestamp{ 0.0 };
        /// @brief Markdown 正文编辑缓冲区。
        std::array<char, ::MMM::MAX_BEATMAP_ANNOTATION_CONTENT_BYTES + 1U>
            content{};
    };

    /// @brief 当前批注编辑弹窗状态。
    AnnotationEditorState m_annotationEditor;
    /// @brief 当前悬浮标记的首条批注 ID，用于检测标记变化。
    std::string m_annotationHoverMarkerId;
    /// @brief 当前悬浮标记中由滚轮选择的详细批注索引。
    std::size_t m_annotationHoverDetailIndex{ 0U };
    /// @brief 当前保留正文滚动位置的连线卡片批注 ID。
    std::string m_annotationDetailScrollItemId;
    /// @brief 当前连线卡片 Markdown 正文的纵向滚动偏移。
    float m_annotationDetailScrollY{ 0.0F };
    /// @brief 上一次发送给逻辑线程的鼠标状态。
    LastMouseCommand m_lastMouseCommand;
    /// @brief 上一次发送给逻辑线程的悬浮实体。
    entt::entity m_lastHoveredEntity{ entt::null };
    /// @brief 上一次悬停实体所在的独立 ECS 注册表。
    Logic::ChartObjectKind m_lastHoveredObjectKind{
        Logic::ChartObjectKind::PlayerNote
    };
    /// @brief 上一次发送给逻辑线程的悬浮部位。
    uint8_t m_lastHoveredPart{ 0 };
    /// @brief 上一次发送给逻辑线程的悬浮子索引。
    int m_lastHoveredSubIndex{ -1 };
    /// @brief 是否已经发送过悬浮状态。
    bool m_hasLastHovered{ false };
    /// @brief 左键按下时是否位于画布内。
    bool m_leftPressStartedOnCanvas{ false };
    /// @brief 左键按下时是否位于轨道布局内。
    bool m_leftPressStartedInTrackLayout{ false };
    /// @brief 左键按下时是否命中实体。
    bool m_leftPressStartedOnEntity{ false };
    /// @brief 当前左键手势是否已经向逻辑线程发起物件拖拽。
    bool m_leftPressStartedObjectDrag{ false };
    /// @brief 当前左键手势是否已经发生拖动。
    bool m_leftPressDragged{ false };
    /// @brief 当前中键手势是否正在二维平移主画布。
    bool m_isMiddleCanvasPanning{ false };
    /// @brief 上一次中键平移输入的画布局部逻辑像素坐标。
    glm::vec2 m_lastMiddlePanMousePosition{ 0.0F, 0.0F };
    /// @brief 当前配色笔刷/橡皮拖动手势中已经处理过的实体。
    std::unordered_set<entt::entity> m_colorStrokeEntities;
    /// @brief 当前右键擦除手势是否已经向逻辑线程发送开始命令。
    bool m_rightEraseActive{ false };
    /// @brief 上一次发送的框选拖动更新。
    LastContinuousEditCommand m_lastMarqueeUpdateCommand;
    /// @brief 上一次发送的绘制笔刷拖动更新。
    LastContinuousEditCommand m_lastBrushUpdateCommand;
    /// @brief 上一次发送的移动拖拽更新。
    LastContinuousEditCommand m_lastMoveUpdateCommand;
    /// @brief 上一次发送的擦除拖动更新。
    LastContinuousEditCommand m_lastEraseUpdateCommand;
    /// @brief 当前轨道布局拖拽部位。
    TrackLayoutDragHandle m_trackLayoutDragHandle{
        TrackLayoutDragHandle::None
    };
    /// @brief 轨道布局拖动开始时的矩形。
    Config::TrackLayout m_trackLayoutDragStart;
    /// @brief 轨道布局拖动开始时的归一化指针坐标。
    glm::vec2 m_trackLayoutPointerStart{ 0.0f, 0.0f };
    /// @brief 当前快照中可见物件的合并布局包围框。
    std::vector<NoteLayoutInstance> m_noteLayoutInstances;
    /// @brief 重建物件布局包围框时复用的实体到数组下标映射。
    std::unordered_map<entt::entity, std::size_t> m_noteLayoutIndexScratch;
    /// @brief 当前正在通过包围框缩放的物件实体。
    std::optional<entt::entity> m_noteScaleDragTarget;
    /// @brief 当前物件缩放拖动的角点。
    Logic::CanvasComponentDragHandle m_noteScaleDragHandle{
        Logic::CanvasComponentDragHandle::None
    };
    /// @brief 物件缩放开始时的合并像素边界。
    Logic::CanvasComponentBounds m_noteScaleDragStartBounds;
    /// @brief 物件缩放开始时的横纵比例。
    Logic::NoteRenderScale m_noteScaleDragStart;
    /// @brief 当前正在拖动的可选画布组件。
    std::optional<Config::CanvasComponentType> m_canvasComponentDragTarget;
    /// @brief 当前可选画布组件的移动或缩放部位。
    Logic::CanvasComponentDragHandle m_canvasComponentDragHandle{
        Logic::CanvasComponentDragHandle::None
    };
    /// @brief 组件拖动开始时的完整布局。
    Config::CanvasComponentPlacement m_canvasComponentDragStart;
    /// @brief 组件缩放开始时的像素边界。
    Logic::CanvasComponentBounds m_canvasComponentDragStartBounds;
    /// @brief 组件拖动开始时实例允许占用的像素区域。
    Logic::CanvasComponentBounds m_canvasComponentDragRegion;
    /// @brief 组件拖动开始时实例中心相对指针的像素偏移。
    glm::vec2 m_canvasComponentPointerOffset{ 0.0f, 0.0f };
    /// @brief 当前组件吸附到的纵向参考线横坐标。
    std::optional<float> m_canvasComponentSnapGuideX;
    /// @brief 当前组件吸附到的横向参考线纵坐标。
    std::optional<float> m_canvasComponentSnapGuideY;
    /// @brief 布局拖动期间复用的纵向吸附目标线缓存。
    std::vector<float> m_canvasComponentSnapTargetsX;
    /// @brief 布局拖动期间复用的横向吸附目标线缓存。
    std::vector<float> m_canvasComponentSnapTargetsY;
    /// @brief 当前拖动的组件实例序号；KPS 总计使用负一。
    std::int64_t m_canvasComponentDragInstanceIndex{ 0 };
    /// @brief 当前同步移动或缩放手势开始时的全部单轨 KPS 状态。
    std::vector<SynchronizedKpsTransformStart> m_synchronizedKpsTransformStarts;
    /// @brief 当前布局配置拖动手势是否实际修改过配置。
    bool m_layoutConfigurationChanged{ false };
};

}  // namespace MMM::Canvas
