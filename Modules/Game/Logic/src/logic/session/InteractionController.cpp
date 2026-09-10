#include "logic/session/InteractionController.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/EditorAction.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SamplePropertyEdit.h"
#include "logic/session/SelectionState.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"
#include "logic/session/tool/GrabTool.h"
#include "logic/session/tool/MarqueeTool.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MMM::Logic
{

namespace
{
/// @brief 屏幕空间选择矩形。
struct SelectionRect {
    /// @brief 左边界。
    float left{ 0.0f };

    /// @brief 上边界。
    float top{ 0.0f };

    /// @brief 右边界。
    float right{ 0.0f };

    /// @brief 下边界。
    float bottom{ 0.0f };

    /// @brief 当前矩形是否有效。
    bool valid{ false };
};

/// @brief 框选屏幕空间投影上下文。
struct SelectionScreenContext {
    /// @brief 当前框选所在视口的 ScrollCache。
    const System::ScrollCache* cache{ nullptr };

    /// @brief 当前视口图集 UV 表读取句柄。
    /// @warning 框选脏刷新时取得一次共享快照，维持引擎更换图集后的读取有效性；
    /// 不在逐候选计算中复制所有权，裸指针不能独立保证旧图集存活。
    std::shared_ptr<const std::unordered_map<uint32_t, glm::vec4>> uvMap;

    /// @brief 当前视口判定线 Y 坐标。
    float judgmentLineY{ 0.0f };

    /// @brief 轨道区左边界。
    float leftX{ 0.0f };

    /// @brief 玩家单轨宽度，Preview 与兼容计算继续使用。
    float singleTrackW{ 0.0f };

    /// @brief 主画布草稿、玩家与 BGM 区域统一横向投影。
    CanvasLaneProjection laneProjection;

    /// @brief 当前上下文是否应按统一区域投影解析横坐标。
    bool usesLaneProjection{ false };

    /// @brief 当前视口纵向渲染缩放。
    float renderScaleY{ 1.0f };

    /// @brief 当前视口的音符基础绘制宽度。
    float noteW{ 0.0f };

    /// @brief 当前视口的音符基础绘制高度。
    float noteH{ 0.0f };

    /// @brief 当前动画时间对应的绝对滚动坐标。
    double currentAbsY{ 0.0 };

    /// @brief 当前上下文是否有效。
    bool valid{ false };
};

/// @brief 已准备好的框选区域，缓存其屏幕矩形与投影上下文。
struct PreparedMarqueeBox {
    /// @brief 原始框选数据。
    MarqueeBox box;

    /// @brief 框选区域屏幕矩形。
    SelectionRect rect;

    /// @brief 该框选区域所属视口的投影上下文。
    SelectionScreenContext screen;
};

/// @brief 带 ECS 注册表领域的框选候选，避免不同 Registry 的实体 ID 冲突。
struct MarqueeSelectionCandidate {
    /// @brief 候选物件所属领域。
    ChartObjectKind kind{ ChartObjectKind::PlayerNote };

    /// @brief 候选物件在对应 Registry 中的实体 ID。
    entt::entity entity{ entt::null };
};

/// @brief 框选候选实体时间范围的保守扩展，覆盖音符纹理高度和特殊 SV 误差。
constexpr double MARQUEE_CANDIDATE_TIME_PADDING_SECONDS = 2.0;

/// @brief 获取指定谱面物件领域对应的 ECS 注册表。
/// @param ctx 会话上下文。
/// @param kind 谱面物件领域。
/// @return 对应的独立注册表。
/// @note 玩家与草稿共用 Note Registry，采样使用独立 Registry。
/// @note 调用方必须携带领域，不能仅凭实体整数判断它属于哪一个容器。
entt::registry& registryForObjectKind(SessionContext& ctx, ChartObjectKind kind)
{
    return kind == ChartObjectKind::AudioSample ? ctx.sampleRegistry
                                                : ctx.noteRegistry;
}

/// @brief 判断玩家物件实体是否允许在当前模式下编辑。
/// @param ctx 会话上下文。
/// @param entity 待判断实体。
/// @return 实体有效且当前编辑模式允许编辑时返回 true。
/// @warning 交互热路径：只访问单个实体组件与配置快照。
bool isEditablePlayerNote(const SessionContext& ctx, entt::entity entity)
{
    // 先验证实体代际和组件存在，避免删除后的悬浮目标被解引用。
    // 编辑模式检查在验证后执行，读取本轮已缓存的配置。
    if ( entity == entt::null || !ctx.noteRegistry.valid(entity) ||
         !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
        return false;
    }
    return SessionUtils::isNoteEditable(
        ctx.noteRegistry.get<const NoteComponent>(entity),
        ctx.lastConfig.settings);
}

/// @brief 判断带领域的谱面物件是否允许在当前模式下编辑。
/// @param ctx 会话上下文。
/// @param kind 物件领域。
/// @param entity 待判断实体。
/// @return 自动采样有效或玩家物件可编辑时返回 true。
/// @warning 交互热路径：只访问单个实体组件与配置快照。
bool isEditableChartObject(const SessionContext& ctx, ChartObjectKind kind,
                           entt::entity entity)
{
    // 草稿与玩家音符共用组件检查，但可编辑性由具体 Note 属性决定。
    // 采样在独立注册表校验，不能让同号音符代替它通过检查。
    if ( kind == ChartObjectKind::PlayerNote ||
         kind == ChartObjectKind::DraftNote ) {
        return isEditablePlayerNote(ctx, entity);
    }
    return entity != entt::null && ctx.sampleRegistry.valid(entity) &&
           ctx.sampleRegistry.all_of<SampleComponent>(entity);
}

/// @brief 清空实体选中状态和框选运行状态。
/// @note 清除实体选中集合以及驱动它的框选状态，两者保持一致。
/// @note 用于明确取消选择；只解除框选驱动时应使用 detachMarqueeSelection。
/// @param ctx 同时保存框选手势与实体选择集合的会话。
void clearSelection(SessionContext& ctx)
{
    // 取消进行中的手势，并使旧框选矩形不再参与后续选择刷新。
    // 实体选择由统一入口清空，避免只删框而留下选中标志。
    ctx.isSelecting             = false;
    ctx.hasMarqueeSelection     = false;
    ctx.marqueeIsAdditive       = false;
    ctx.isMarqueeSelectionDirty = false;
    ctx.marqueeBoxes.clear();

    clearChartObjectSelection(ctx);
}

/// @brief 停止使用框选框驱动选择状态，但保留当前实体选中集合。
/// @note 保留已经解析出的实体集合，后续不会再由旧矩形重新计算它。
/// @note 适用于把空间框选转换为稳定实体选择的状态切换。
/// @param ctx 要解除框选驱动但保留实体选择的会话。
void detachMarqueeSelection(SessionContext& ctx)
{
    // 解除框选驱动时同时清除脏标记，防止下一轮用空矩形覆盖保留的选择。
    // 这里有意不调用 clearChartObjectSelection。
    ctx.isSelecting             = false;
    ctx.hasMarqueeSelection     = false;
    ctx.marqueeIsAdditive       = false;
    ctx.isMarqueeSelectionDirty = false;
    ctx.marqueeBoxes.clear();
}

/// @brief 解析分区全选应使用的轨道区。
/// @param ctx 当前会话与最后主画布鼠标位置。
/// @return 最后主画布坐标位于有效轨道区时返回其类型。
/// @warning Ctrl+A 低频路径：只执行一次相机查找和常量级投影。
[[nodiscard]] std::optional<CanvasLaneKind> selectAllLaneKind(
    const SessionContext& ctx)
{
    // 分区全选依赖最后一次主画布鼠标位置，不能用当前任意辅助视图代替。
    // 尚无主画布上下文时返回空，交由调用方选择后续策略。
    if ( ctx.lastMainCanvasCameraId.empty() ) return std::nullopt;

    const auto camera = ctx.cameras.find(ctx.lastMainCanvasCameraId);
    if ( camera == ctx.cameras.end() ) return std::nullopt;

    const auto projection =
        calculateCanvasLaneProjection(camera->second.viewportWidth,
                                      ctx.trackCount,
                                      ctx.bgmTrackCount,
                                      ctx.lastConfig.visual.trackLayout,
                                      camera->second.horizontalOffsetX,
                                      true,
                                      ctx.lastConfig.settings.enableBmsEditing,
                                      ctx.lastConfig.settings.professionalMode,
                                      ctx.draftTrackCount,
                                      true);
    // 鼠标落在批注沟槽或区域间隙时不会得到轨道地址。
    // 只返回区域类型，不把具体轨号误当作全选范围。
    const auto lane = projection.laneAt(ctx.lastMainCanvasMousePos.x);
    return lane ? std::optional<CanvasLaneKind>{ lane->kind } : std::nullopt;
}

/// @brief 清空已经存在的实体选中标记。
/// @param ctx 会话上下文。
/// @warning 逻辑热路径：框选重算时只遍历已创建 InteractionComponent 的实体，
/// 避免完整扫描 NoteComponent。
void clearSelectedEntityFlags(SessionContext& ctx)
{
    clearChartObjectSelection(ctx);
}

/// @brief 判断屏幕矩形是否与另一个矩形相交。
/// @warning 逻辑热路径：框选重算时按可见框数量调用，只做常量比较。
/// @note 边缘相接也视为命中，半像素容差覆盖投影舍入误差。
/// @note 无效矩形直接拒绝，不能让默认零坐标触发假命中。
/// @param lhs 第一个已归一化屏幕矩形。
/// @param rhs 第二个已归一化屏幕矩形。
/// @return 两矩形有效且在像素容差内相交时为 true。
bool rectIntersects(SelectionRect lhs, SelectionRect rhs)
{
    if ( !lhs.valid || !rhs.valid ) return false;
    // 半像素余量只用于几何比较，不改变实际存储矩形。
    // 边界相等应命中，避免细线或紧贴框边的物件难以选中。
    constexpr float EPS = 0.5f;
    return std::max(lhs.left, rhs.left) <=
               std::min(lhs.right, rhs.right) + EPS &&
           std::max(lhs.top, rhs.top) <= std::min(lhs.bottom, rhs.bottom) + EPS;
}

/// @brief 判断一个屏幕矩形是否完整包含另一个矩形。
/// @warning 逻辑热路径：框选重算时按可见框数量调用，只做常量比较。
/// @note 外框和候选均须有效，候选四边都不得超出容差后的外框。
/// @note 与相交模式使用相同容差，避免模式切换时边界抖动。
/// @param outer 框选外矩形。
/// @param inner 待完整覆盖的候选矩形。
/// @return 四边均在容差范围内被覆盖时为 true。
bool rectContains(SelectionRect outer, SelectionRect inner)
{
    if ( !outer.valid || !inner.valid ) return false;
    // 严格包含仍保留小量浮点余量，但要求四个方向同时满足。
    // 不能用任一角点在内来替代完整包含判定。
    constexpr float EPS = 0.5f;
    return inner.left >= outer.left - EPS && inner.right <= outer.right + EPS &&
           inner.top >= outer.top - EPS && inner.bottom <= outer.bottom + EPS;
}

/// @brief 构造一个屏幕矩形。
/// @warning 逻辑热路径：只做边界归一化和尺寸合法性检查。
/// @note 允许输入任意拖动方向，返回归一化的左右和上下边界。
/// @note 零宽或零高无效，不在此人为扩大单点选择区域。
/// @param left 第一个端点的横坐标。
/// @param top 第一个端点的纵坐标。
/// @param right 第二个端点的横坐标。
/// @param bottom 第二个端点的纵坐标。
/// @return 边界归一化的像素矩形；无正面积时 valid 为 false。
SelectionRect makeRect(float left, float top, float right, float bottom)
{
    SelectionRect rect;
    // 拖动可从右下到左上，先统一边界再比较有效面积。
    // 正面积是后续相交、包含和合并助手共同使用的不变量。
    rect.left   = std::min(left, right);
    rect.right  = std::max(left, right);
    rect.top    = std::min(top, bottom);
    rect.bottom = std::max(top, bottom);
    rect.valid  = rect.right > rect.left && rect.bottom > rect.top;
    return rect;
}

/// @brief 将一个矩形合并进目标矩形。
/// @warning 逻辑热路径：包围盒生成时调用，只做常量比较。
/// @note 无效源不影响目标；无效目标由第一个有效源初始化。
/// @note 求的是轴对齐外包矩形，不保留合并后的空隙或凹部。
/// @param target 要扩展的外包矩形，可从无效值开始。
/// @param source 本次加入的部件矩形，无效值被忽略。
void includeRect(SelectionRect& target, SelectionRect source)
{
    // 无效源跳过，避免默认零矩形把有效目标错误扩张到画布原点。
    // 首次有效源完整赋值，也同时建立目标 valid 状态。
    if ( !source.valid ) return;
    if ( !target.valid ) {
        target = source;
        return;
    }

    // 后续源只扩大外边界，不缩小已有候选覆盖范围。
    // 多段物件的总体选择框因此能包含所有已加入部件。
    target.left   = std::min(target.left, source.left);
    target.top    = std::min(target.top, source.top);
    target.right  = std::max(target.right, source.right);
    target.bottom = std::max(target.bottom, source.bottom);
}

/// @brief 按当前框选模式判断两个屏幕矩形是否命中。
/// @warning 逻辑热路径：框选重算时按候选包围盒调用，只做常量比较。
/// @note 严格模式要求候选整个包围框被覆盖，其他模式按相交处理。
/// @note 函数只选择几何判定方式，不修改实体的选中标记。
/// @param selection 屏幕选择框。
/// @param candidate 已投影的候选部件或整体外包框。
/// @param mode 严格包含或相交选择模式。
/// @return 当前模式下候选是否命中。
bool selectionMatchesRect(SelectionRect selection, SelectionRect candidate,
                          Config::SelectionMode mode)
{
    return mode == Config::SelectionMode::Strict
               ? rectContains(selection, candidate)
               : rectIntersects(selection, candidate);
}

/// @brief 获取图集纹理宽高比。
/// @warning 逻辑热路径：框选重算时只读取当前视口已缓存 UV 表。
/// @note 图块 z、w 表示宽高，返回图集归一化尺寸之比。
/// @note 缺失或高度近零使用调用方回退值，不触发纹理加载。
/// @param uvMap 当前视口图集快照，不获取共享所有权。
/// @param id 要查询的纹理标识。
/// @param fallback 纹理不存在或高度退化时采用的比例。
/// @return 图块宽高比或回退值。
float getTextureAspect(const std::unordered_map<uint32_t, glm::vec4>& uvMap,
                       TextureID id, float fallback)
{
    auto it = uvMap.find(static_cast<uint32_t>(id));
    // 缺图块或高度接近零时不能直接求比值。
    // 回退比例仅用于当前选择几何，不改写图集缓存。
    if ( it == uvMap.end() || std::abs(it->second.w) < 1e-6f ) {
        return fallback;
    }
    return it->second.z / it->second.w;
}

/// @brief 按渲染系统规则计算纹理实际绘制尺寸。
/// @warning 逻辑热路径：框选重算时只读取当前视口已缓存 UV 表。
/// @note 以 Note 图块为尺寸基准，其他部件按各自图块尺寸比例缩放。
/// @note 要求有效 Note 图块宽高非零；本函数只处理缺失项回退。
/// @param uvMap 当前视口图集映射。
/// @param id 候选部件纹理。
/// @param baseW Note 基准绘制宽度，单位像素。
/// @param baseH Note 基准绘制高度，单位像素。
/// @return 按图块相对尺寸换算后的部件像素宽高。
glm::vec2 getTextureDrawSize(
    const std::unordered_map<uint32_t, glm::vec4>& uvMap, TextureID id,
    float baseW, float baseH)
{
    // 所有部件尺寸相对 Note 图块计算，保持框选与渲染使用同一比例约定。
    // 缺少基准纹理时只能保留传入的基础宽高。
    auto itBase = uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == uvMap.end() ) return { baseW, baseH };

    auto it = uvMap.find(static_cast<uint32_t>(id));
    if ( it == uvMap.end() ) return { baseW, baseH };

    return { baseW * (it->second.z / itBase->second.z),
             baseH * (it->second.w / itBase->second.w) };
}

/// @brief 计算指定视口的框选纵向缩放。
/// @warning 逻辑热路径：框选拖拽时调用，只读取当前缓存的视口尺寸和配置。
/// @note 主画布缩放已体现在滚动缓存中，这里只额外计算 Preview 压缩。
/// @note 主视口缺失时暂用当前视口高度，保持初始化阶段可计算。
/// @param ctx 当前会话配置和各视口尺寸。
/// @param camera 要投影的相机。
/// @param cameraId 用于区分 Preview 的视口身份。
/// @return 当前视口相对于滚动缓存的纵向倍率。
float calculateMarqueeRenderScaleY(const SessionContext& ctx,
                                   const CameraInfo&     camera,
                                   const std::string&    cameraId)
{
    // 仅 Preview 需要相对主画布额外压缩。
    // 其他视口返回一，避免把缓存中的 timelineZoom 再应用一次。
    if ( cameraId != "Preview" ) {
        return 1.0f;
    }

    const auto* mainCamera = SessionUtils::findMainCanvasCamera(ctx.cameras);
    const float mainViewportHeight =
        mainCamera ? mainCamera->viewportHeight : camera.viewportHeight;
    // 主画布有效高度采用归一化上下轨道边界，Preview 则使用像素边距。
    // 两者单位统一为像素后再结合 areaRatio 求倍率。
    const float mainEffectiveH = (ctx.lastConfig.visual.trackLayout.bottom -
                                  ctx.lastConfig.visual.trackLayout.top) *
                                 mainViewportHeight;
    const float ty             = ctx.lastConfig.visual.previewConfig.margin.top;
    const float by = camera.viewportHeight -
                     ctx.lastConfig.visual.previewConfig.margin.bottom;
    const float previewDrawH = by - ty;

    // 退化主区域或近零可视比例不能用作除数，回退无额外压缩。
    // 这里只保护现有零值边界，不执行完整配置纠正。
    if ( std::abs(mainEffectiveH) < 1e-6f ||
         std::abs(ctx.lastConfig.visual.previewConfig.areaRatio) < 1e-6f ) {
        return 1.0f;
    }
    return previewDrawH /
           (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
}

/// @brief 计算指定视口的轨道横向布局。
/// @warning 逻辑热路径：框选重算时调用，只读取当前缓存的视口尺寸和配置。
/// @note Preview 采用像素边距，其他画布按玩家区域比例和水平偏移投影。
/// @note 输出只有在返回 true 时可用，窄于约一像素的区域拒绝。
/// @param ctx 包含玩家轨数和最后生效配置的会话。
/// @param camera 当前视口大小及横向平移。
/// @param cameraId 区分预览像素边距和主画布比例布局。
/// @param leftX 成功时写入玩家区域左边界，单位像素。
/// @param rightX 成功时写入玩家区域右边界，单位像素。
/// @return 有正轨数且横向范围足够宽时为 true。
bool calculateMarqueeTrackLayout(const SessionContext& ctx,
                                 const CameraInfo&     camera,
                                 const std::string& cameraId, float& leftX,
                                 float& rightX)
{
    // 玩家单轨宽度需要正轨数，空布局不能继续做平均分配。
    // 这也是后续 Note 基础尺寸计算的前提。
    if ( ctx.trackCount <= 0 ) return false;

    // 预览横向边界是固定像素边距，不跟随主画布水平拖动。
    // 主画布分支则把相机水平偏移纳入玩家投影。
    if ( cameraId == "Preview" ) {
        leftX  = ctx.lastConfig.visual.previewConfig.margin.left;
        rightX = camera.viewportWidth -
                 ctx.lastConfig.visual.previewConfig.margin.right;
    } else {
        const auto projection = calculatePlayerTrackProjection(
            camera.viewportWidth,
            ctx.trackCount,
            ctx.lastConfig.visual.trackLayout.left,
            ctx.lastConfig.visual.trackLayout.right,
            camera.horizontalOffsetX);
        leftX  = projection.leftX;
        rightX = projection.rightX;
    }

    return rightX > leftX + 1.0f;
}

/// @brief 为框选判断准备屏幕投影上下文。
/// @warning 逻辑热路径：框选区域变化时调用；读取一次图集 UV
/// 缓存，不做文件访问。
/// @warning 每个待处理框选上下文取得共享 UV 快照以防图集替换失效；
/// 句柄只移动到结果，后续候选借用它，不得改为逐候选复制。
/// @note 失败时返回 valid=false 的值对象，调用方不得使用其缓存指针投影。
/// @note 每个框选上下文统一保存本轮动画时间和视口参数，避免候选间基准变化。
/// @param ctx 读取本轮配置、相机与动画时间的会话。
/// @param cameraId 框选所在相机。
/// @param cache 生命周期覆盖本轮候选判断的滚动缓存。
/// @return 投影参数及图集快照；失败时 valid 为 false。
SelectionScreenContext makeSelectionScreenContext(
    const SessionContext& ctx, const std::string& cameraId,
    const System::ScrollCache* cache)
{
    SelectionScreenContext screen;
    // 缺滚动映射时无法把时间转换到屏幕，返回无效上下文。
    // 不在框选热路径临时重建缓存或访问文件。
    if ( !cache ) return screen;

    auto cameraIt = ctx.cameras.find(cameraId);
    if ( cameraIt == ctx.cameras.end() ) return screen;

    float leftX  = 0.0f;
    float rightX = 0.0f;
    if ( !calculateMarqueeTrackLayout(
             ctx, cameraIt->second, cameraId, leftX, rightX) ) {
        return screen;
    }

    // 一次取得该视口图集快照，用于本轮所有候选尺寸计算。
    // 随后移动到上下文保存，不逐候选重新获取共享句柄。
    auto        uvMap      = EditorEngine::instance().getAtlasUVMap(cameraId);
    const float baseAspect = getTextureAspect(*uvMap, TextureID::Note, 1.0f);
    const float singleTrackW =
        (rightX - leftX) / static_cast<float>(ctx.trackCount);
    // 正轨宽与非退化纹理比例共同保证基础音符尺寸可以计算。
    // 失败时不填充半有效上下文，调用方统一按 valid 跳过。
    if ( singleTrackW <= 0.0f || std::abs(baseAspect) < 1e-6f ) {
        return screen;
    }

    screen.cache = cache;
    // 缓存指针只是观察引用，图集句柄则维持快照在本次框选期间的生命周期。
    // 即使引擎切换图集版本，本上下文仍使用同一份 UV 数据。
    screen.uvMap = std::move(uvMap);
    screen.judgmentLineY =
        cameraIt->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    screen.leftX        = leftX;
    screen.singleTrackW = singleTrackW;
    if ( SessionUtils::isMainCanvasCameraId(cameraId) ) {
        // 主画布框选必须与独立草稿/BGM 区域使用同一投影。
        screen.laneProjection = calculateCanvasLaneProjection(
            cameraIt->second.viewportWidth,
            ctx.trackCount,
            ctx.bgmTrackCount,
            ctx.lastConfig.visual.trackLayout,
            cameraIt->second.horizontalOffsetX,
            true,
            ctx.lastConfig.settings.enableBmsEditing,
            ctx.lastConfig.settings.professionalMode,
            ctx.draftTrackCount,
            true);
        screen.usesLaneProjection = screen.laneProjection.valid;
    }
    screen.renderScaleY =
        calculateMarqueeRenderScaleY(ctx, cameraIt->second, cameraId);
    screen.noteW = singleTrackW * ctx.lastConfig.visual.noteScaleX;
    screen.noteH =
        (singleTrackW / baseAspect) * ctx.lastConfig.visual.noteScaleY;
    // 用动画时间作为屏幕锚点，与当前显示位置一致。
    // 若改用音频真实时间，平滑滚动期间框选会与可见物件错位。
    screen.currentAbsY = cache->getAbsY(ctx.animateTime);
    screen.valid       = screen.noteW > 0.0f && screen.noteH > 0.0f &&
                         std::abs(screen.renderScaleY) > 1e-6f;
    return screen;
}

/// @brief 将逻辑时间投影到屏幕 Y 坐标。
/// @warning 逻辑热路径：框选重算时只通过 ScrollCache 做一次坐标查询。
/// @note 时间为秒，输出为视口局部像素，Y 轴方向与滚动绝对坐标相反。
/// @note anchorTime 指定显示增量的 HS 锚点，长条末端应沿用其主体起点。
/// @param screen 已验证有效的投影上下文。
/// @param time 被投影的逻辑时间，单位秒。
/// @param anchorTime 主体显示速度的锚点，单位秒。
/// @return 视口局部 Y 坐标。
float timeToScreenY(const SelectionScreenContext& screen, double time,
                    double anchorTime)
{
    // 通过带主体锚点的显示增量处理 HS，而不是简单相减两次绝对位置。
    // 最后应用视口压缩，使主画布和 Preview 共用同一时间计算入口。
    return screen.judgmentLineY -
           static_cast<float>(screen.cache->getDisplayDelta(
               time, screen.currentAbsY, anchorTime)) *
               screen.renderScaleY;
}

/// @brief 获取可交互主体末端时间。
/// @warning 逻辑热路径：框选重算时按候选物件调用；保持纯计算，不得分配。
/// @note 只有 Hold 在时间上延伸，Flick 的轨差不改变其结束时间。
/// @note 不在此钳制负持续时间，数据合法性由编辑入口负责。
/// @param type 主体类型，只有 Hold 具有时间持续段。
/// @param timestamp 主体起点，单位秒。
/// @param duration 主体持续时间，单位秒。
/// @return 主体结束时间。
double carrierEndTime(::MMM::NoteType type, double timestamp, double duration)
{
    // Hold 的可交互主体沿时间延伸，末端取起点加时长。
    // 其他类型返回同一时刻，横向延伸由轨道几何负责。
    if ( type == ::MMM::NoteType::HOLD ) {
        return timestamp + duration;
    }
    return timestamp;
}

/// @brief 获取可交互主体末端 HS 锚点时间。
/// @warning 逻辑热路径：框选重算时按候选物件调用；保持纯计算，不得访问 ECS。
/// @note Hold 尾部仍以头部时间作为 HS 锚点，保持主体映射连续。
/// @note screen 参数沿用统一辅助接口，目前不参与锚点选择。
/// @param screen 兼容签名保留的上下文，当前实现未使用。
/// @param type 用于判断是否保持 Hold 头部锚点。
/// @param timestamp 主体开始时间。
/// @param duration 供末端时间助手使用的持续秒数。
/// @return 主体末端投影应使用的 HS 锚点时间。
double carrierEndAnchorTime(const SelectionScreenContext& screen,
                            ::MMM::NoteType type, double timestamp,
                            double duration)
{
    (void)screen;
    // 尾部时间虽然改变，Hold 的显示速度锚点仍锁定头部。
    // 这样持续体不会在跨 HS 边界时由两套缩放分别计算两端。
    if ( type == ::MMM::NoteType::HOLD ) {
        return timestamp;
    }
    return carrierEndTime(type, timestamp, duration);
}

/// @brief 计算框选区域的屏幕矩形。
/// @warning 逻辑热路径：框选重算时只做两次时间投影和边界归一化。
/// @note 框选端点描述空间区域而非音符主体，使用绝对滚动坐标差投影。
/// @note 轨道端点可带小数，不能当作物件统一轨号取整后再映射。
/// @param box 以连续轨位置与秒时间记录的框选端点。
/// @param screen 框所属视口的投影数据。
/// @return 可与候选几何比较的归一化屏幕矩形。
SelectionRect makeMarqueeScreenRect(const MarqueeBox&             box,
                                    const SelectionScreenContext& screen)
{
    if ( !screen.valid ) return {};

    // 框选轨道端点是在玩家布局基准下记录的连续位置。
    // 保留其小数部分，允许区域边界位于物件之间而非吸附轨道中心。
    const float  x1 = screen.leftX + box.startTrack * screen.singleTrackW;
    const float  x2 = screen.leftX + box.endTrack * screen.singleTrackW;
    const double startAbsY = screen.cache->getAbsY(box.startTime);
    const double endAbsY   = screen.cache->getAbsY(box.endTime);
    const float  y1 = screen.judgmentLineY -
                      static_cast<float>(startAbsY - screen.currentAbsY) *
                          screen.renderScaleY;
    const float  y2 =
        screen.judgmentLineY -
        static_cast<float>(endAbsY - screen.currentAbsY) * screen.renderScaleY;
    // 结束时间或轨道可以小于开始值，统一归一化后再参与命中。
    // 无面积手势由 makeRect 标为无效，不额外扩大选择框。
    return makeRect(x1, y1, x2, y2);
}

/// @brief 框选计算使用的单轨横向几何。
struct SelectionLaneGeometry {
    /// @brief 当前轨道左边界。
    float leftX{ 0.0F };
    /// @brief 当前区域单轨宽度。
    float width{ 0.0F };
};

/// @brief 按绝对轨道解析框选对象所在区域。
/// @warning 逻辑热路径：每个候选对象固定次数调用，只做常量级投影查询。
/// @note 主画布按统一轨号选择真实分区宽度，Preview 保留兼容均匀轨距。
/// @note 分区无法解析时回退线性布局，调用方仍需检查上层上下文有效性。
/// @param screen 含统一分区或兼容玩家布局的上下文。
/// @param absoluteTrack 物件统一轨号，负值可表示草稿区域。
/// @return 所属轨道的像素左边界与宽度。
SelectionLaneGeometry selectionLaneGeometry(
    const SelectionScreenContext& screen, std::int32_t absoluteTrack)
{
    // 先解析统一轨号所属区域，再取该区域真实边界。
    // 不能把负草稿轨或 BGM 轨直接乘玩家单轨宽度。
    if ( screen.usesLaneProjection ) {
        const auto address = CanvasLaneAddress::fromAbsoluteTrack(
            absoluteTrack,
            screen.laneProjection.playerLaneCount,
            screen.laneProjection.draftLaneCount);
        if ( const auto bounds = screen.laneProjection.bounds(address) ) {
            return { bounds->leftX, bounds->rightX - bounds->leftX };
        }
    }
    // 兼容路径按均匀轨距推导，主要服务 Preview 和未解析分区。
    // 返回的宽度随后用于按所在区域缩放纹理，而非所有对象固定宽度。
    return { screen.leftX +
                 static_cast<float>(absoluteTrack) * screen.singleTrackW,
             screen.singleTrackW };
}

/// @brief 计算某个轨道时间点上的纹理矩形。
/// @warning 逻辑热路径：框选重算时只做坐标换算和 UV 尺寸读取。
/// @note 纹理尺寸先按所在分区轨宽缩放，再在该轨内居中。
/// @note 返回轴对齐矩形；轨道参数在解析物件区域时转为整数统一轨号。
/// @param screen 当前视口投影和 UV 快照。
/// @param id 目标部件图块标识。
/// @param track 用于解析物件分区的统一轨号。
/// @param time 部件中心所在逻辑时间。
/// @param anchorTime 显示速度锚点时间。
/// @return 居中的部件矩形，输入上下文无效时返回无效矩形。
SelectionRect makeTextureRect(const SelectionScreenContext& screen,
                              TextureID id, float track, double time,
                              double anchorTime)
{
    if ( !screen.valid || !screen.uvMap ) return {};

    const auto lane =
        selectionLaneGeometry(screen, static_cast<std::int32_t>(track));
    // 分区轨宽与玩家基准轨宽之比同时作用于基础宽高。
    // 保持纹理纵横比例随所属区域变化，而不是只修改 X 位置。
    const float     laneScale = lane.width / screen.singleTrackW;
    const glm::vec2 size      = getTextureDrawSize(
        *screen.uvMap, id, screen.noteW * laneScale, screen.noteH * laneScale);
    // 纹理矩形以真实区域单轨居中，框选命中与渲染完全对齐。
    const float x = lane.leftX + (lane.width - size.x) * 0.5F;
    const float y = timeToScreenY(screen, time, anchorTime);
    return makeRect(x, y - size.y * 0.5f, x + size.x, y + size.y * 0.5f);
}

/// @brief 将 Hold/Flick 的主体矩形合并到屏幕包围盒。
/// @warning 逻辑热路径：框选重算时只根据单个物件局部参数计算。
/// @note 只合并主体，不包含头纹理、尾纹理或箭头，调用方负责组合。
/// @note 零持续 Hold 和零轨差 Flick 不产生独立主体矩形。
/// @param target 待扩展的外包矩形。
/// @param screen 当前框选视口参数。
/// @param type Hold 或 Flick 主体类型。
/// @param timestamp 主体起点时间，单位秒。
/// @param duration Hold 持续秒数。
/// @param trackIndex 主体起轨的统一轨号。
/// @param dtrack Flick 终点相对起轨的轨差。
void includeCarrierRect(SelectionRect&                target,
                        const SelectionScreenContext& screen,
                        ::MMM::NoteType type, double timestamp, double duration,
                        int trackIndex, int dtrack)
{
    if ( !screen.valid || !screen.uvMap ) return;

    const auto  startLane = selectionLaneGeometry(screen, trackIndex);
    const float laneScale = startLane.width / screen.singleTrackW;
    // 正持续 Hold 沿时间方向形成主体，起止 Y 共用头部 HS 锚点。
    // 主体横向宽度按起始轨道所在区域的图块比例计算。
    if ( type == ::MMM::NoteType::HOLD && duration > 0.0 ) {
        const glm::vec2 bodySize =
            getTextureDrawSize(*screen.uvMap,
                               TextureID::HoldBodyVertical,
                               screen.noteW * laneScale,
                               screen.noteH * laneScale);
        const float x = startLane.leftX + (startLane.width - bodySize.x) * 0.5F;
        const float sy = timeToScreenY(screen, timestamp, timestamp);
        const float ey = timeToScreenY(
            screen,
            carrierEndTime(type, timestamp, duration),
            carrierEndAnchorTime(screen, type, timestamp, duration));
        includeRect(target, makeRect(x, sy, x + bodySize.x, ey));
    } else if ( type == ::MMM::NoteType::FLICK && dtrack != 0 ) {
        // Flick 终点可能跨入另一布局区域，必须重新解析终点轨。
        // 轨差只能决定逻辑终点，不能直接乘起轨宽度求像素距离。
        const auto endLane = selectionLaneGeometry(screen, trackIndex + dtrack);
        const glm::vec2 bodySize =
            getTextureDrawSize(*screen.uvMap,
                               TextureID::HoldBodyHorizontal,
                               screen.noteW * laneScale,
                               screen.noteH * laneScale);
        // Flick 主体跨越两个真实轨道中心，支持跨区域拖动后的命中。
        const float startCenter = startLane.leftX + startLane.width * 0.5F;
        const float endCenter   = endLane.leftX + endLane.width * 0.5F;
        const float y           = timeToScreenY(screen, timestamp, timestamp);
        includeRect(target,
                    makeRect(std::min(startCenter, endCenter),
                             y - bodySize.y * 0.5F,
                             std::max(startCenter, endCenter),
                             y + bodySize.y * 0.5F));
    }
}

/// @brief 合并 Polyline 相邻子物件之间的连接段屏幕包围盒。
/// @warning 逻辑热路径：框选重算时只处理当前 Polyline 的相邻子物件。
/// @note 从当前子项主体末端连接到下一子项起点。
/// @note 横向包围范围包含两端各自宽度，不能沿用父折线根轨宽。
/// @param target 要并入过渡段的外包矩形。
/// @param screen 两端共用的视口投影上下文。
/// @param current 前一个子项，连接从它的主体末端出发。
/// @param next 后一个子项，连接到它的起点。
void includePolylineTransitionRect(SelectionRect&                target,
                                   const SelectionScreenContext& screen,
                                   const NoteComponent::SubNote& current,
                                   const NoteComponent::SubNote& next)
{
    if ( !screen.valid || !screen.uvMap ) return;

    // Flick 从箭头终点继续连接，其他子项仍从原轨继续。
    // 只调整轨道，不把 Flick 的轨差误用为时间变化。
    const auto currentEndTrack =
        current.trackIndex +
        (current.type == ::MMM::NoteType::FLICK ? current.dtrack : 0);
    const auto currentLane = selectionLaneGeometry(screen, currentEndTrack);
    const auto nextLane    = selectionLaneGeometry(screen, next.trackIndex);
    // 两端可能分别属于草稿和玩家等不同宽度区域。
    // 各自缩放主体纹理，包围框才能覆盖真实连接边缘。
    const float     currentScale = currentLane.width / screen.singleTrackW;
    const float     nextScale    = nextLane.width / screen.singleTrackW;
    const glm::vec2 currentBodySize =
        getTextureDrawSize(*screen.uvMap,
                           TextureID::HoldBodyVertical,
                           screen.noteW * currentScale,
                           screen.noteH * currentScale);
    const glm::vec2 nextBodySize =
        getTextureDrawSize(*screen.uvMap,
                           TextureID::HoldBodyVertical,
                           screen.noteW * nextScale,
                           screen.noteH * nextScale);
    // 连接段两端分别按所属区域的真实宽度居中。
    const float currentX =
        currentLane.leftX + (currentLane.width - currentBodySize.x) * 0.5F;
    const float nextX =
        nextLane.leftX + (nextLane.width - nextBodySize.x) * 0.5F;
    // Hold 先走完持续主体，再连接到下一节点时间。
    // 显示锚点仍按当前主体规则取得，避免尾端 HS 计算不一致。
    const double currentEndTime =
        carrierEndTime(current.type, current.timestamp, current.duration);
    const double currentEndAnchorTime = carrierEndAnchorTime(
        screen, current.type, current.timestamp, current.duration);
    const float sy =
        timeToScreenY(screen, currentEndTime, currentEndAnchorTime);
    const float ey = timeToScreenY(screen, next.timestamp, next.timestamp);
    // 构造的是过渡几何的轴对齐外包框，可能包含斜线外的空白。
    // 该保守矩形用于框选，不承担逐像素或三角形精确拾取。
    includeRect(
        target,
        makeRect(std::min(currentX, nextX),
                 std::min(sy, ey),
                 std::max(currentX + currentBodySize.x, nextX + nextBodySize.x),
                 std::max(sy, ey)));
}

/// @brief 计算普通物件的屏幕选择包围盒。
/// @warning 逻辑热路径：框选重算时按候选物件调用，只做局部几何计算。
/// @note 普通 Note 使用单纹理，Hold 和 Flick 合并头、身体及末端。
/// @note Polyline 由另一入口逐部件测试，此函数不构造整条折线包围框。
/// @param note 普通根物件组件，不包含折线整体处理。
/// @param screen 用于投影各个部件的有效屏幕上下文。
/// @return 合并后的外包矩形，未支持类型保留无效状态。
SelectionRect makeNoteScreenRect(const NoteComponent&          note,
                                 const SelectionScreenContext& screen)
{
    SelectionRect rect;
    if ( !screen.valid ) return rect;

    // 普通点击只有本体纹理，无需合并不存在的主体或末端。
    // 返回仍带 valid 状态，由纹理投影失败或退化矩形决定。
    if ( note.m_type == ::MMM::NoteType::NOTE ) {
        includeRect(rect,
                    makeTextureRect(screen,
                                    TextureID::Note,
                                    static_cast<float>(note.m_trackIndex),
                                    note.m_timestamp,
                                    note.m_timestamp));
        return rect;
    }

    // Hold 选择框同时包含头、连续主体和尾部图块。
    // 严格框选普通 Hold 时因此要求覆盖这个整体外包范围。
    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        includeRect(rect,
                    makeTextureRect(screen,
                                    TextureID::Note,
                                    static_cast<float>(note.m_trackIndex),
                                    note.m_timestamp,
                                    note.m_timestamp));
        includeCarrierRect(rect,
                           screen,
                           note.m_type,
                           note.m_timestamp,
                           note.m_duration,
                           note.m_trackIndex,
                           note.m_dtrack);
        includeRect(
            rect,
            makeTextureRect(
                screen,
                TextureID::HoldEnd,
                static_cast<float>(note.m_trackIndex),
                note.m_timestamp + note.m_duration,
                carrierEndAnchorTime(
                    screen, note.m_type, note.m_timestamp, note.m_duration)));
        return rect;
    }

    // Flick 在一个时刻横向展开，头部与箭头可使用不同区域轨宽。
    // 合并主体与末端后，跨区域间隙也处于整体选择框中。
    if ( note.m_type == ::MMM::NoteType::FLICK ) {
        includeRect(rect,
                    makeTextureRect(screen,
                                    TextureID::Note,
                                    static_cast<float>(note.m_trackIndex),
                                    note.m_timestamp,
                                    note.m_timestamp));
        includeCarrierRect(rect,
                           screen,
                           note.m_type,
                           note.m_timestamp,
                           note.m_duration,
                           note.m_trackIndex,
                           note.m_dtrack);
        // 零轨差没有独立箭头，避免在头部重复添加一个无方向末端。
        // 非零轨差符号选择左右纹理，尺寸仍由终点区域决定。
        if ( note.m_dtrack != 0 ) {
            const TextureID arrowId = note.m_dtrack < 0
                                          ? TextureID::FlickArrowLeft
                                          : TextureID::FlickArrowRight;
            includeRect(rect,
                        makeTextureRect(screen,
                                        arrowId,
                                        static_cast<float>(note.m_trackIndex +
                                                           note.m_dtrack),
                                        note.m_timestamp,
                                        note.m_timestamp));
        }
    }
    return rect;
}

/// @brief 判断 Polyline 是否与框选区域命中。
/// @warning 逻辑热路径：框选重算时只遍历当前 Polyline 的子物件列表。
/// @note 任一节点、主体、过渡或最终末端满足当前模式即命中整条折线。
/// @note 严格模式针对被检查的单个部件，不要求整个折线同时被外框包含。
/// @param note 含子项数组的折线根组件。
/// @param screen 当前框所属视口的屏幕投影。
/// @param selection 已归一化的框选矩形。
/// @param mode 对各部件分别应用的选择模式。
/// @return 任一受检部件满足选择模式时为 true。
bool polylineMatchesSelection(const NoteComponent&          note,
                              const SelectionScreenContext& screen,
                              SelectionRect                 selection,
                              Config::SelectionMode         mode)
{
    if ( !screen.valid || note.m_subNotes.empty() ) return false;

    // 从根节点开始逐段检查，命中任一部件立即返回。
    // 不先构造整条折线的大矩形，避免空白跨度导致整体误命中。
    for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
        const auto& sub = note.m_subNotes[i];
        if ( selectionMatchesRect(
                 selection,
                 makeTextureRect(screen,
                                 i == 0 ? TextureID::Note : TextureID::Node,
                                 static_cast<float>(sub.trackIndex),
                                 sub.timestamp,
                                 sub.timestamp),
                 mode) ) {
            return true;
        }

        // 每个子项先以独立矩形测试主体，不能混入上一子项范围。
        // 在严格模式下，这允许完整框住一段主体来选择其父折线。
        SelectionRect carrierRect;
        includeCarrierRect(carrierRect,
                           screen,
                           sub.type,
                           sub.timestamp,
                           sub.duration,
                           sub.trackIndex,
                           sub.dtrack);
        if ( selectionMatchesRect(selection, carrierRect, mode) ) {
            return true;
        }

        // 只有存在下一节点才有连接过渡，最后一项留给后面的末端处理。
        // 索引检查在访问下一子项之前，避免最后一轮越界。
        if ( i + 1 < note.m_subNotes.size() ) {
            SelectionRect transitionRect;
            includePolylineTransitionRect(
                transitionRect, screen, sub, note.m_subNotes[i + 1]);
            if ( selectionMatchesRect(selection, transitionRect, mode) ) {
                return true;
            }
        }
    }

    // 末尾子项的箭头或 Hold 尾部不包含在后继连接中，需要单独测试。
    // 中间子项由节点、主体和过渡分支覆盖，不在这里反复遍历。
    const auto& last = note.m_subNotes.back();
    if ( last.type == ::MMM::NoteType::FLICK && last.dtrack != 0 ) {
        const TextureID arrowId = last.dtrack < 0 ? TextureID::FlickArrowLeft
                                                  : TextureID::FlickArrowRight;
        return selectionMatchesRect(
            selection,
            makeTextureRect(screen,
                            arrowId,
                            static_cast<float>(last.trackIndex + last.dtrack),
                            last.timestamp,
                            last.timestamp),
            mode);
    } else if ( last.type == ::MMM::NoteType::HOLD ) {
        return selectionMatchesRect(
            selection,
            makeTextureRect(
                screen,
                TextureID::HoldEnd,
                static_cast<float>(last.trackIndex),
                last.timestamp + last.duration,
                carrierEndAnchorTime(
                    screen, last.type, last.timestamp, last.duration)),
            mode);
    }

    return false;
}

/// @brief 根据物件类型判断是否与框选区域命中。
/// @warning 逻辑热路径：框选重算时按候选物件调用，不做 ECS 查询。
/// @note 普通物件比较合并包围框，折线则逐部件命中后短路。
/// @note 两种判定粒度不同，修改时不能简单替换为统一的大包围框。
/// @param note 已通过可编辑性检查的候选组件。
/// @param screen 当前框的投影参数。
/// @param selection 本轮待测试的屏幕选择框。
/// @param mode 严格包含或相交模式。
/// @return 普通物件整体或折线部件是否命中。
bool noteMatchesSelection(const NoteComponent&          note,
                          const SelectionScreenContext& screen,
                          SelectionRect selection, Config::SelectionMode mode)
{
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        return polylineMatchesSelection(note, screen, selection, mode);
    }
    return selectionMatchesRect(
        selection, makeNoteScreenRect(note, screen), mode);
}

/// @brief 计算自动采样锚点、offset 连线与实际触发 handle 的选择包围盒。
/// @warning 逻辑热路径：按候选采样调用，只做局部坐标换算。
/// @note 选择范围合并锚点、触发手柄和偏移连线，不等于音频播放时长。
/// @note 触发时间可早于锚点，连接框在合并前归一化上下边界。
/// @param sample 包含锚点与毫秒偏移的采样组件。
/// @param screen 用于定位轨道和时间的屏幕上下文。
/// @return 主体、手柄与连线合并后的外包矩形。
SelectionRect makeSampleScreenRect(const SampleComponent&        sample,
                                   const SelectionScreenContext& screen)
{
    SelectionRect rect;
    if ( !screen.valid ) return rect;

    const auto lane = selectionLaneGeometry(
        screen, static_cast<std::int32_t>(sample.m_track));
    const float laneWidth = lane.width;
    // 采样选择主体沿用这里的独立尺寸规则，并设置最小可选宽高。
    // 此计算不读取音频资源或 Note 图块，只依赖所在轨宽。
    const float bodyWidth  = std::max(12.0F, laneWidth * 0.78F);
    const float bodyHeight = std::clamp(laneWidth * 0.24F, 16.0F, 28.0F);
    const float bodyX      = lane.leftX + (laneWidth - bodyWidth) * 0.5F;
    const float anchorY =
        timeToScreenY(screen, sample.m_timestamp, sample.m_timestamp);
    includeRect(rect,
                makeRect(bodyX,
                         anchorY - bodyHeight * 0.5F,
                         bodyX + bodyWidth,
                         anchorY + bodyHeight * 0.5F));

    // 偏移通过 SampleComponent 统一换算为实际秒时间。
    // 触发手柄采用自身时间作为显示锚点，与主体锚点分开投影。
    const double effectiveTime = sample.effectiveTime();
    const float  effectiveY =
        timeToScreenY(screen, effectiveTime, effectiveTime);
    // 手柄大小按轨宽缩放后钳制，保持窄轨下仍可选中。
    // 横向位于轨内固定比例，不能默认放到主体中心。
    const float handleSize    = std::clamp(laneWidth * 0.12F, 8.0F, 14.0F);
    const float handleCenterX = lane.leftX + 0.82F * laneWidth;
    includeRect(rect,
                makeRect(handleCenterX - handleSize * 0.5F,
                         effectiveY - handleSize * 0.5F,
                         handleCenterX + handleSize * 0.5F,
                         effectiveY + handleSize * 0.5F));
    includeRect(rect,
                makeRect(handleCenterX - 1.0F,
                         std::min(anchorY, effectiveY),
                         handleCenterX + 1.0F,
                         std::max(anchorY, effectiveY)));
    return rect;
}

/// @brief 判断自动采样是否命中框选区域。
/// @warning 逻辑热路径：按候选采样调用，不访问 ECS 或文件系统。
/// @note 严格模式检查整个合并框，包含锚点与偏移触发位置。
/// @note 仅测试屏幕几何，不解析资源 ID 或加载音频。
/// @param sample 候选采样组件，不访问音频资源。
/// @param screen 本轮屏幕投影。
/// @param selection 正面积的框选矩形。
/// @param mode 合并包围框应采用的选择规则。
/// @return 采样整体选择框满足模式时为 true。
bool sampleMatchesSelection(const SampleComponent&        sample,
                            const SelectionScreenContext& screen,
                            SelectionRect selection, Config::SelectionMode mode)
{
    return selectionMatchesRect(
        selection, makeSampleScreenRect(sample, screen), mode);
}

/// @brief 获取实体的主时间戳，失效实体排序到末尾。
/// @warning 逻辑热路径：框选候选二分时调用，只做 registry 有效性检查。
/// @note 返回源排序键，调用方需保证实体列表已经按该键排序。
/// @note 无效实体使用正无穷哨兵，不在二分比较内修改索引。
/// @param ctx 持有音符 Registry 的会话。
/// @param entity 排序候选中的实体身份。
/// @return 根时间戳，失效或缺组件时返回正无穷。
double getNoteStartTimeForSelection(const SessionContext& ctx,
                                    entt::entity          entity)
{
    if ( !ctx.noteRegistry.valid(entity) ||
         !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
        // 失效键返回末端哨兵，二分比较只读取而不清理 ECS。
        // 真正删除后的排序索引维护必须在框选之前由脏刷新完成。
        return std::numeric_limits<double>::infinity();
    }
    return ctx.noteRegistry.get<const NoteComponent>(entity).m_timestamp;
}

/// @brief 收集所有主音符实体作为候选。
/// @warning 逻辑热路径兜底：只在排序缓存不可用时完整扫描 NoteComponent。
/// @note 兜底只收集根物件，子节点由折线局部几何检查覆盖。
/// @note 此入口不做权限或最终矩形过滤，结果仍需后续精确判定。
/// @param ctx 提供音符 Registry 的会话。
/// @param candidates 接收追加结果的领域化候选数组。
void collectAllPrimaryNoteCandidates(
    SessionContext& ctx, std::vector<MarqueeSelectionCandidate>& candidates)
{
    auto view = ctx.noteRegistry.view<NoteComponent>();
    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        // 只加入父对象，折线子实体由同一父候选的局部遍历处理。
        // 领域根据草稿标志记录，后续权限检查仍需要这个区别。
        if ( !note.m_isSubNote ) {
            candidates.push_back({ note.m_isDraft ? ChartObjectKind::DraftNote
                                                  : ChartObjectKind::PlayerNote,
                                   entity });
        }
    }
}

/// @brief 根据单个框选框的时间范围收集排序缓存中的候选实体。
/// @warning 逻辑热路径：框选更新时按框数量执行二分和局部线性扫描。
/// @note 返回 false 表示索引或框选上下文不可用，需要调用方执行兜底。
/// @note 返回 true 不保证候选非空；合法时间范围内可以没有物件。
/// @note 多时间段和多个框共享去重集合，避免同一根实体被反复测试。
/// @param ctx 提供预排序实体与结束前缀的会话。
/// @param box 已准备的单个屏幕框及原始端点。
/// @param candidates 追加本框时间窗口中的根候选。
/// @param seen 同一音符领域跨窗口共享的去重集合。
/// @return 索引筛选可用时为 true，不以候选数量判断成功。
bool collectMarqueeBoxCandidates(
    SessionContext& ctx, const PreparedMarqueeBox& box,
    std::vector<MarqueeSelectionCandidate>& candidates,
    std::unordered_set<entt::entity>&       seen)
{
    const auto& entities     = ctx.sortedNoteEntities;
    const auto& maxEndPrefix = ctx.sortedNoteMaxEndPrefix;
    // 索引数量与最大终点前缀必须一一对应，否则二分区间没有意义。
    // 无效投影或选择矩形同样返回失败，让上层决定兜底。
    if ( entities.empty() || maxEndPrefix.size() != entities.size() ||
         !box.screen.valid || !box.screen.cache || !box.rect.valid ) {
        return false;
    }

    // 时间余量仅扩大候选窗口，不等待数据或延迟本地交互。
    // 最终仍以屏幕几何判定命中，所以多收集候选不会直接多选。
    /// @brief 将一个保守时间窗口内的候选追加到当前结果。
    /// @param minTime 窗口较早边界，单位秒。
    /// @param maxTime 窗口较晚边界，单位秒。
    /// @warning 框选脏刷新路径：仅查询有序区间并扫描局部候选，不重建全局排序。
    auto collectTimeRangeCandidates = [&](double minTime, double maxTime) {
        minTime -= MARQUEE_CANDIDATE_TIME_PADDING_SECONDS;
        maxTime += MARQUEE_CANDIDATE_TIME_PADDING_SECONDS;
        // 非有限或反向时间窗不能交给有序索引二分。
        // 忽略该时间段，不修改全局排序数据。
        if ( !std::isfinite(minTime) || !std::isfinite(maxTime) ||
             minTime > maxTime ) {
            return;
        }

        auto startIt =
            // 用前缀最大终点找可能跨入窗口的最早物件。
            // 仅按起始时间查下界会漏掉起点在窗外但持续到窗内的主体。
            std::lower_bound(maxEndPrefix.begin(), maxEndPrefix.end(), minTime);
        std::size_t startIndex = static_cast<std::size_t>(
            std::distance(maxEndPrefix.begin(), startIt));
        if ( startIndex >= entities.size() ) {
            return;
        }

        // 上界按起始时间截断，起点晚于窗口右端的候选无需继续扫描。
        // 下界保证保守包含，上界控制只遍历局部时间段。
        auto endIt = std::upper_bound(
            entities.begin() + static_cast<std::ptrdiff_t>(startIndex),
            entities.end(),
            maxTime,
            [&ctx](double value, entt::entity entity) {
                return value < getNoteStartTimeForSelection(ctx, entity);
            });

        candidates.reserve(
            candidates.size() +
            static_cast<std::size_t>(std::distance(
                entities.begin() + static_cast<std::ptrdiff_t>(startIndex),
                endIt)));
        for ( auto it =
                  entities.begin() + static_cast<std::ptrdiff_t>(startIndex);
              it != endIt;
              ++it ) {
            const auto entity = *it;
            // 同一实体可能出现在多个滚动时间段或多个框内。
            // 先去重再读组件，减少重复候选的局部几何计算。
            if ( !seen.insert(entity).second ) {
                continue;
            }
            if ( !ctx.noteRegistry.valid(entity) ||
                 !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                continue;
            }

            const auto& note =
                ctx.noteRegistry.get<const NoteComponent>(entity);
            // 排序缓存中即使出现子实体，也只保留根物件用于选择。
            // 根折线候选会在几何阶段遍历自己的子项。
            if ( note.m_isSubNote ) continue;
            candidates.push_back({ note.m_isDraft ? ChartObjectKind::DraftNote
                                                  : ChartObjectKind::PlayerNote,
                                   entity });
        }
    };

    // 先按基础音符高度向上下扩展屏幕窗口，覆盖边缘纹理。
    // 再逆映射时间，避免只看框选端点秒数漏掉滚动折返区域。
    const double paddedTopY    = box.rect.top - box.screen.noteH;
    const double paddedBottomY = box.rect.bottom + box.screen.noteH;
    const double absA   = box.screen.currentAbsY +
                          (box.screen.judgmentLineY - paddedTopY) /
                              static_cast<double>(box.screen.renderScaleY);
    const double absB   = box.screen.currentAbsY +
                          (box.screen.judgmentLineY - paddedBottomY) /
                              static_cast<double>(box.screen.renderScaleY);
    auto         ranges = box.screen.cache->getTimeRangesForAbsYWindow(
        std::min(absA, absB), std::max(absA, absB));
    // 缓存没有返回可用时间段时，回退框选记录的原始时间区间。
    // 仍按排序索引筛选，不在这一分支直接扫描全部 Registry。
    if ( ranges.empty() ) {
        collectTimeRangeCandidates(
            std::min(box.box.startTime, box.box.endTime),
            std::max(box.box.startTime, box.box.endTime));
        return true;
    }

    for ( const auto& [minTime, maxTime] : ranges ) {
        collectTimeRangeCandidates(minTime, maxTime);
    }
    return true;
}

/// @brief 收集当前框选框影响到的候选主音符实体。
/// @warning 逻辑热路径：优先使用已排序的时间段缓存；缓存不可用时才全量兜底。
/// @note 任一框无法使用索引时，丢弃部分索引结果并统一收集根物件。
/// @note 输出追加策略由调用方管理，兜底分支会清空当前候选列表。
/// @param ctx 候选数据所属会话。
/// @param boxes 已准备的所有有效选择框。
/// @param candidates 接收根物件候选，兜底时会先清空。
void collectMarqueeSelectionCandidates(
    SessionContext& ctx, const std::vector<PreparedMarqueeBox>& boxes,
    std::vector<MarqueeSelectionCandidate>& candidates)
{
    std::unordered_set<entt::entity> seen;
    seen.reserve(256);

    // 任一框缺少可靠索引就放弃部分结果，避免混合结果遗漏对象。
    // 成功使用索引但结果为空与索引不可用是不同状态。
    bool usedIndexedCandidates = true;
    for ( const auto& box : boxes ) {
        if ( !collectMarqueeBoxCandidates(ctx, box, candidates, seen) ) {
            usedIndexedCandidates = false;
            break;
        }
    }

    if ( usedIndexedCandidates ) {
        return;
    }

    // 全量兜底前清除已收集的局部结果，避免重复候选。
    // 该成本只应出现在缓存不可用分支，普通框选复用预排序数据。
    candidates.clear();
    collectAllPrimaryNoteCandidates(ctx, candidates);
}

/// @brief 获取自动采样可见区间起点，失效实体排序到末尾。
/// @warning 逻辑热路径：框选候选二分时调用，只做 Registry 查询。
/// @note 采样视觉区间从锚点与实际触发点中的较早者开始。
/// @note 必须与采样排序缓存使用同一键，负偏移不能只按锚点排序。
/// @param ctx 提供采样 Registry 的会话。
/// @param entity 采样排序索引中的本地身份。
/// @return 锚点与触发点的较早秒时间，失效时为正无穷。
double getSampleStartTimeForSelection(const SessionContext& ctx,
                                      entt::entity          entity)
{
    if ( !ctx.sampleRegistry.valid(entity) ||
         !ctx.sampleRegistry.all_of<SampleComponent>(entity) ) {
        return std::numeric_limits<double>::infinity();
    }
    const auto& sample = ctx.sampleRegistry.get<const SampleComponent>(entity);
    // 负偏移使触发点早于锚点，区间起点必须取两者较小值。
    // 与采样最大终点前缀配对后才能覆盖完整可交互连线。
    return std::min(sample.m_timestamp, sample.effectiveTime());
}

/// @brief 收集全部自动采样作为索引失效时的框选兜底候选。
/// @warning 逻辑热路径兜底：只在排序缓存不可用时完整扫描 SampleComponent。
/// @note 采样与音符使用不同 Registry，候选必须记录 AudioSample 领域。
/// @note 不解析采样是否有可用资源；选择对象不依赖解码成功。
/// @param ctx 采样数据所属会话。
/// @param candidates 接收追加采样的领域化候选数组。
void collectAllSampleCandidates(
    SessionContext& ctx, std::vector<MarqueeSelectionCandidate>& candidates)
{
    auto view = ctx.sampleRegistry.view<SampleComponent>();
    for ( const auto entity : view ) {
        candidates.push_back({ ChartObjectKind::AudioSample, entity });
    }
}

/// @brief 从单个框选框的时间窗口收集自动采样候选。
/// @warning 逻辑热路径：使用预排序区间与前缀最大终点，只扫描窗口候选。
/// @note 沿采样区间前缀裁剪，长偏移连线进入框选时仍能成为候选。
/// @note 去重集合仅用于这一对象域，不可与 Note 实体号混合去重。
/// @param ctx 提供采样区间索引的会话。
/// @param box 本轮有效选择框。
/// @param candidates 追加采样候选的位置。
/// @param seen 仅在采样领域中使用的去重集合。
/// @return 索引及投影输入可用于筛选时返回 true。
bool collectMarqueeBoxSampleCandidates(
    SessionContext& ctx, const PreparedMarqueeBox& box,
    std::vector<MarqueeSelectionCandidate>& candidates,
    std::unordered_set<entt::entity>&       seen)
{
    const auto& entities     = ctx.sortedSampleEntities;
    const auto& maxEndPrefix = ctx.sortedSampleMaxEndPrefix;
    // 索引数量与最大终点前缀必须一一对应，否则二分区间没有意义。
    // 无效投影或选择矩形同样返回失败，让上层决定兜底。
    if ( entities.empty() || maxEndPrefix.size() != entities.size() ||
         !box.screen.valid || !box.screen.cache || !box.rect.valid ) {
        return false;
    }

    // 时间余量仅扩大候选窗口，不等待数据或延迟本地交互。
    // 最终仍以屏幕几何判定命中，所以多收集候选不会直接多选。
    /// @brief 将一个保守时间窗口内的候选追加到当前结果。
    /// @param minTime 窗口较早边界，单位秒。
    /// @param maxTime 窗口较晚边界，单位秒。
    /// @warning 框选脏刷新路径：仅查询有序区间并扫描局部候选，不重建全局排序。
    auto collectTimeRangeCandidates = [&](double minTime, double maxTime) {
        minTime -= MARQUEE_CANDIDATE_TIME_PADDING_SECONDS;
        maxTime += MARQUEE_CANDIDATE_TIME_PADDING_SECONDS;
        // 非有限或反向时间窗不能交给有序索引二分。
        // 忽略该时间段，不修改全局排序数据。
        if ( !std::isfinite(minTime) || !std::isfinite(maxTime) ||
             minTime > maxTime ) {
            return;
        }

        const auto startIt =
            // 用前缀最大终点找可能跨入窗口的最早物件。
            // 仅按起始时间查下界会漏掉起点在窗外但持续到窗内的主体。
            std::lower_bound(maxEndPrefix.begin(), maxEndPrefix.end(), minTime);
        const std::size_t startIndex = static_cast<std::size_t>(
            std::distance(maxEndPrefix.begin(), startIt));
        if ( startIndex >= entities.size() ) return;

        // 上界按起始时间截断，起点晚于窗口右端的候选无需继续扫描。
        // 下界保证保守包含，上界控制只遍历局部时间段。
        const auto endIt = std::upper_bound(
            entities.begin() + static_cast<std::ptrdiff_t>(startIndex),
            entities.end(),
            maxTime,
            [&ctx](double value, entt::entity entity) {
                return value < getSampleStartTimeForSelection(ctx, entity);
            });
        for ( auto iterator =
                  entities.begin() + static_cast<std::ptrdiff_t>(startIndex);
              iterator != endIt;
              ++iterator ) {
            const auto entity = *iterator;
            if ( !seen.insert(entity).second ||
                 !ctx.sampleRegistry.valid(entity) ||
                 !ctx.sampleRegistry.all_of<SampleComponent>(entity) ) {
                continue;
            }
            candidates.push_back({ ChartObjectKind::AudioSample, entity });
        }
    };

    // 先按基础音符高度向上下扩展屏幕窗口，覆盖边缘纹理。
    // 再逆映射时间，避免只看框选端点秒数漏掉滚动折返区域。
    const double paddedTopY    = box.rect.top - box.screen.noteH;
    const double paddedBottomY = box.rect.bottom + box.screen.noteH;
    const double absA   = box.screen.currentAbsY +
                          (box.screen.judgmentLineY - paddedTopY) /
                              static_cast<double>(box.screen.renderScaleY);
    const double absB   = box.screen.currentAbsY +
                          (box.screen.judgmentLineY - paddedBottomY) /
                              static_cast<double>(box.screen.renderScaleY);
    const auto   ranges = box.screen.cache->getTimeRangesForAbsYWindow(
        std::min(absA, absB), std::max(absA, absB));
    // 缓存没有返回可用时间段时，回退框选记录的原始时间区间。
    // 仍按排序索引筛选，不在这一分支直接扫描全部 Registry。
    if ( ranges.empty() ) {
        collectTimeRangeCandidates(
            std::min(box.box.startTime, box.box.endTime),
            std::max(box.box.startTime, box.box.endTime));
        return true;
    }
    for ( const auto& [minTime, maxTime] : ranges ) {
        collectTimeRangeCandidates(minTime, maxTime);
    }
    return true;
}

/// @brief 将自动采样候选追加到带领域身份的框选候选列表。
/// @warning 逻辑热路径：优先使用采样排序缓存，失效时才完整扫描采样。
/// @note Preview 不参与自动采样框选，音符与采样候选的视口范围不同。
/// @note 索引失效只移除已有采样候选，保留之前收集的音符。
void collectMarqueeSampleCandidates(
    SessionContext& ctx, const std::vector<PreparedMarqueeBox>& boxes,
    std::vector<MarqueeSelectionCandidate>& candidates)
{
    std::unordered_set<entt::entity> seen;
    seen.reserve(256);
    bool usedIndexedCandidates = true;
    for ( const auto& box : boxes ) {
        // 辅助 Preview 不展示可选采样，不用其框构造采样候选。
        // 保留两个既有相机标识的排除规则，避免视口命名差异误选。
        if ( box.box.cameraId == "Preview" ||
             box.box.cameraId == "PreviewCanvas" ) {
            continue;
        }
        if ( !collectMarqueeBoxSampleCandidates(ctx, box, candidates, seen) ) {
            usedIndexedCandidates = false;
            break;
        }
    }
    if ( usedIndexedCandidates ) return;

    // 只清除部分采样索引结果，音符候选属于另一领域。
    // 随后统一追加完整采样候选，避免同一个采样被索引与兜底重复添加。
    candidates.erase(
        std::remove_if(candidates.begin(),
                       candidates.end(),
                       [](const MarqueeSelectionCandidate& candidate) {
                           return candidate.kind ==
                                  ChartObjectKind::AudioSample;
                       }),
        candidates.end());
    collectAllSampleCandidates(ctx, candidates);
}

/// @brief 准备所有有效框选区域的屏幕矩形。
/// @warning 逻辑热路径：框选区域变化时调用；按框数量读取视口和图集缓存。
/// @note 每个框使用其所属相机的投影，不能复用最后活动相机的屏幕参数。
/// @note 无效矩形不进入结果，后续候选筛选只处理可用的框。
std::vector<PreparedMarqueeBox> prepareMarqueeBoxes(
    const SessionContext& ctx, const System::ScrollCache* cache)
{
    std::vector<PreparedMarqueeBox> boxes;
    // 预留当前框数容量，生成过程中不因逐框追加反复扩容。
    // 准备结果仅服务本轮脏刷新，不持久化过时的屏幕坐标。
    boxes.reserve(ctx.marqueeBoxes.size());
    for ( const auto& box : ctx.marqueeBoxes ) {
        PreparedMarqueeBox prepared;
        prepared.box    = box;
        prepared.screen = makeSelectionScreenContext(ctx, box.cameraId, cache);
        prepared.rect   = makeMarqueeScreenRect(box, prepared.screen);
        // 屏幕投影或框面积无效时略过，不向二分筛选传递半有效输入。
        // 移动 prepared 避免额外复制内部 UV 共享句柄。
        if ( prepared.rect.valid ) {
            boxes.push_back(std::move(prepared));
        }
    }
    return boxes;
}
}  // namespace

/// @brief 创建会话持有的移动、框选与画笔工具。
/// @param ctx 生命周期覆盖控制器的会话状态。
/// @note 工具在初始化时创建，命令路由中只查找并借用已有实例。
InteractionController::InteractionController(SessionContext& ctx) : m_ctx(ctx)
{
    m_tools[EditTool::Move]    = std::make_unique<GrabTool>();
    m_tools[EditTool::Marquee] = std::make_unique<MarqueeTool>();
    m_tools[EditTool::Draw]    = std::make_unique<DrawTool>();
}

// --- 交互命令处理 ---

/// @brief 切换带对象领域的悬浮身份，并同步组件反馈。
/// @param cmd 目标实体、领域及命中部件。
/// @warning 输入热路径：只查询旧目标和新目标，不遍历 Registry。
void InteractionController::handleCommand(const CmdSetHoveredEntity& cmd)
{
    // 旧拾取数据可能对应已删除或当前模式禁止编辑的对象。
    // 无效目标归一为空身份，并复位部件和子索引，避免悬浮提示残留。
    const bool targetIsEditable =
        isEditableChartObject(m_ctx, cmd.kind, cmd.entity);
    const entt::entity targetEntity =
        targetIsEditable ? cmd.entity : entt::null;
    const ChartObjectKind targetKind =
        targetIsEditable ? cmd.kind : ChartObjectKind::PlayerNote;
    const std::int32_t targetPart     = targetIsEditable ? cmd.part : 0;
    const std::int32_t targetSubIndex = targetIsEditable ? cmd.subIndex : -1;

    // 实体号与对象领域共同决定是否切换目标。
    // 不同 Registry 可以有相同实体号，不能只比较整数身份。
    if ( (m_ctx.hoveredEntity != targetEntity ||
          m_ctx.hoveredObjectKind != targetKind) &&
         m_ctx.hoveredEntity != entt::null ) {
        // 先清理旧组件的悬浮标志，避免两个对象同时保持悬浮外观。
        // 旧目标可能已删除，所以组件清理前仍须确认有效性。
        auto& previousRegistry =
            registryForObjectKind(m_ctx, m_ctx.hoveredObjectKind);
        if ( previousRegistry.valid(m_ctx.hoveredEntity) &&
             previousRegistry.all_of<InteractionComponent>(
                 m_ctx.hoveredEntity) ) {
            previousRegistry.get<InteractionComponent>(m_ctx.hoveredEntity)
                .isHovered = false;
            previousRegistry.get<InteractionComponent>(m_ctx.hoveredEntity)
                .hoveredPart = static_cast<uint8_t>(HoverPart::None);
        }
    }

    // 会话统一身份先更新，再把相同部件信息写入新对象组件。
    // 后续检视读取会话字段，渲染反馈读取组件，两者需保持一致。
    m_ctx.hoveredEntity     = targetEntity;
    m_ctx.hoveredObjectKind = targetKind;
    m_ctx.hoveredPart       = targetPart;
    m_ctx.hoveredSubIndex   = targetSubIndex;

    auto& registry = registryForObjectKind(m_ctx, m_ctx.hoveredObjectKind);
    if ( m_ctx.hoveredEntity != entt::null &&
         registry.valid(m_ctx.hoveredEntity) ) {
        // 交互组件按需创建，不要求载入时为全部物件预先分配。
        // 同一目标内切换子部件也会刷新部件字段，而不必先清空再设置。
        if ( !registry.all_of<InteractionComponent>(m_ctx.hoveredEntity) ) {
            registry.emplace<InteractionComponent>(m_ctx.hoveredEntity);
        }
        auto& ic     = registry.get<InteractionComponent>(m_ctx.hoveredEntity);
        ic.isHovered = true;
        ic.hoveredPart     = static_cast<std::uint8_t>(targetPart);
        ic.hoveredSubIndex = targetSubIndex;
    }
}

/// @brief 根据点击命令切换或替换实体选择。
/// @param cmd 带领域的目标及是否清除其他选择。
/// @warning 输入路径：单目标检查后使用选择集合入口，不得扫描全部物件。
void InteractionController::handleCommand(const CmdSelectEntity& cmd)
{
    // 空目标且要求替换选择表示点击空白清除当前选择。
    // 空目标的叠加点击不应破坏已有集合。
    if ( cmd.entity == entt::null ) {
        if ( cmd.clearOthers ) {
            clearSelection(m_ctx);
        }
        return;
    }

    // 只有在框选工具模式下才允许通过点击实体修改选中状态。
    if ( m_ctx.currentTool != EditTool::Marquee ) return;

    if ( !isEditableChartObject(m_ctx, cmd.kind, cmd.entity) ) return;

    auto& registry = registryForObjectKind(m_ctx, cmd.kind);
    if ( !registry.valid(cmd.entity) ) return;
    if ( !registry.all_of<InteractionComponent>(cmd.entity) ) {
        registry.emplace<InteractionComponent>(cmd.entity);
    }
    auto& ic          = registry.get<InteractionComponent>(cmd.entity);
    bool  wasSelected = ic.isSelected;

    // 叠加点击按当前状态切换单个实体，并解除旧框对集合的驱动。
    // 否则下一次框选脏刷新可能把手动取消的对象重新选中。
    if ( !cmd.clearOthers ) {
        detachMarqueeSelection(m_ctx);
        setChartObjectSelected(m_ctx, cmd.kind, cmd.entity, !ic.isSelected);
        return;
    }

    // 普通点击已选对象时保留整组选择，便于直接拖动多选集合。
    // 只有点击未选对象才清空其他选择并建立单选。
    if ( wasSelected ) {
        return;
    }

    clearSelection(m_ctx);
    setChartObjectSelected(m_ctx, cmd.kind, cmd.entity, true);
}

/// @brief 按命令范围替换当前物件选择。
/// @param cmd 当前轨道区或所有轨道区的全选命令。
/// @warning 用户触发的低频路径：可遍历可编辑物件 Registry，不得从每帧更新调用。
void InteractionController::handleCommand(const CmdSelectAll& cmd)
{
    const auto laneKind = cmd.scope == SelectAllScope::CurrentTrackArea
                              ? selectAllLaneKind(m_ctx)
                              : std::nullopt;
    // 当前区域无法解析时保留原有选择，而不是先清空后失败。
    // 鼠标处于批注沟槽或没有主画布位置时可能走到这里。
    if ( cmd.scope == SelectAllScope::CurrentTrackArea && !laneKind ) return;

    clearSelection(m_ctx);

    // 全区域分支先短路，保证未解析 laneKind 时不会解引用空值。
    // 仅当前区域时按玩家、草稿或 BGM 分开收集。
    if ( cmd.scope == SelectAllScope::AllTrackAreas ||
         *laneKind != CanvasLaneKind::Bgm ) {
        auto view = m_ctx.noteRegistry.view<NoteComponent>();
        for ( auto entity : view ) {
            const auto& note = view.get<NoteComponent>(entity);
            // 全选仍需遵守模式可编辑性，且不直接选择折线子实体。
            // 草稿还要求专业模式和对应区域范围，避免隐藏对象被批量修改。
            if ( !SessionUtils::isNoteEditable(note,
                                               m_ctx.lastConfig.settings) ||
                 note.m_isSubNote ||
                 (note.m_isDraft &&
                  !m_ctx.lastConfig.settings.professionalMode) ||
                 (cmd.scope == SelectAllScope::CurrentTrackArea &&
                  note.m_isDraft != (*laneKind == CanvasLaneKind::Draft)) ) {
                continue;
            }

            setChartObjectSelected(m_ctx,
                                   note.m_isDraft ? ChartObjectKind::DraftNote
                                                  : ChartObjectKind::PlayerNote,
                                   entity,
                                   true);
        }
    }

    if ( cmd.scope == SelectAllScope::AllTrackAreas ||
         *laneKind == CanvasLaneKind::Bgm ) {
        auto sampleView = m_ctx.sampleRegistry.view<SampleComponent>();
        for ( auto entity : sampleView ) {
            setChartObjectSelected(
                m_ctx, ChartObjectKind::AudioSample, entity, true);
        }
    }
}

/// @brief 校验拖动目标后交给移动工具开始手势。
/// @param cmd 目标身份及拖动起点。
/// @warning 输入热路径：固定工具查找与单实体校验，禁止阻塞等待。
void InteractionController::handleCommand(const CmdStartDrag& cmd)
{
    if ( !isEditableChartObject(m_ctx, cmd.kind, cmd.entity) ) return;
    const auto tool = m_tools.find(EditTool::Move);
    if ( tool != m_tools.end() ) {
        tool->second->handleStartDrag(m_ctx, cmd);
    }
}

/// @brief 将拖动的连续位置更新交给移动工具。
/// @param cmd 当前鼠标位置和修饰键。
/// @warning 拖动热路径：立即转发最新状态，不用定时等待合并交互。
void InteractionController::handleCommand(const CmdUpdateDrag& cmd)
{
    const auto tool = m_tools.find(EditTool::Move);
    if ( tool != m_tools.end() ) {
        tool->second->handleUpdateDrag(m_ctx, cmd);
    }
}

/// @brief 将手势结束交给移动工具提交最终状态。
/// @param cmd 结束拖动的相机与输入状态。
/// @warning 输入热路径：不延迟最终提交，具体事务由工具负责。
void InteractionController::handleCommand(const CmdEndDrag& cmd)
{
    const auto tool = m_tools.find(EditTool::Move);
    if ( tool != m_tools.end() ) {
        tool->second->handleEndDrag(m_ctx, cmd);
    }
}

/// @brief 将项目音频资源按主画布坐标创建为自动采样。
/// @param cmd 项目资源 ID 与主画布放置坐标。
/// @warning UI 输入低频路径：只在 ImGui 拖放释放时执行一次；通过
/// ScrollCache 和拍点缓存换算位置，不允许文件系统访问。
void InteractionController::handleCommand(const CmdCreateAudioSample& cmd)
{
    // 仅允许暂停且已载入谱面的主画布放置，并排除非有限鼠标输入。
    // 此入口不从辅助视图坐标推测 BGM 轨道。
    if ( m_ctx.isPlaying || !m_ctx.currentBeatmap ||
         cmd.audioResourceId.empty() ||
         !SessionUtils::isMainCanvasCameraId(cmd.cameraId) ||
         !std::isfinite(cmd.mouseX) || !std::isfinite(cmd.mouseY) ) {
        return;
    }

    // 协作会话优先使用其项目资源表，离线会话使用当前项目。
    // 这里只查已载入资源引用，不通过资源名扫描文件系统。
    const auto* project = m_ctx.collaborationProject
                              ? m_ctx.collaborationProject.get()
                              : EditorEngine::instance().getCurrentProject();
    const auto& beatmapPath = m_ctx.currentBeatmap->m_baseMapMetadata.map_path;
    const auto* resource =
        project ? ProjectResourceService::findAudioResourceForReference(
                      *project, beatmapPath, cmd.audioResourceId)
                : nullptr;
    // 仅主音轨或效果资源可实例化为采样，其他资源类型拒绝。
    // 失败只写操作反馈，不创建占位实体或部分撤销记录。
    if ( !resource || (resource->m_type != ::MMM::AudioTrackType::Main &&
                       resource->m_type != ::MMM::AudioTrackType::Effect) ) {
        m_ctx.lastActionMessage =
            TR("ui.edit.sample_properties.invalid_resource").data();
        return;
    }

    const auto  cameraIterator = m_ctx.cameras.find(cmd.cameraId);
    const auto* cache =
        m_ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cameraIterator == m_ctx.cameras.end() || !cache ) {
        return;
    }

    const auto& camera     = cameraIterator->second;
    const auto  projection = calculateCanvasLaneProjection(
        camera.viewportWidth,
        m_ctx.trackCount,
        m_ctx.bgmTrackCount,
        m_ctx.lastConfig.visual.trackLayout,
        camera.horizontalOffsetX,
        true,
        m_ctx.lastConfig.settings.enableBmsEditing,
        m_ctx.lastConfig.settings.professionalMode,
        m_ctx.draftTrackCount,
        true);
    // 把水平相机偏移和独立分区布局纳入落点解析。
    // 即使鼠标位于有效玩家轨，也不能把资源放成自动采样。
    const auto lane = projection.laneAt(cmd.mouseX);
    if ( !lane || lane->kind != CanvasLaneKind::Bgm ) {
        m_ctx.lastActionMessage = "音频资源只能放置到 BGM 轨道区";
        return;
    }

    // 纵向使用当前动画锚点逆映射，与鼠标所见画面位置一致。
    // 缓存已包含滚动速度与缩放，不能再次按固定像素每秒换算。
    const float judgmentLineY =
        camera.viewportHeight * m_ctx.lastConfig.visual.judgeline_pos;
    const double currentAbsY = cache->getAbsY(m_ctx.animateTime);
    double timestamp = cache->getTime(currentAbsY + judgmentLineY - cmd.mouseY);
    if ( !std::isfinite(timestamp) ) {
        return;
    }

    SessionUtils::ensureBpmEvents(m_ctx);
    const auto snap = SessionUtils::getSnapResult(
        timestamp,
        cmd.mouseY,
        camera,
        m_ctx.lastConfig,
        m_ctx.bpmEvents,
        m_ctx.timelineRegistry,
        m_ctx.animateTime,
        m_ctx.cameras,
        m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm);
    // Ctrl 暂时绕过吸附，其他情况下采用公共拍点规则。
    // 最后钳制非负时间，资源拖入不创建负时间采样。
    if ( snap.isSnapped && !cmd.isCtrlDown ) {
        timestamp = snap.snappedTime;
    }
    timestamp = std::max(0.0, timestamp);

    // 资源保存规范化 ID，轨道保存统一绝对轨号，偏移初始为零。
    // 实例音量独立于资源音量，初始倍率一表示不额外调整。
    SampleComponent sample{
        .m_timestamp       = timestamp,
        .m_offsetMs        = 0,
        .m_track           = static_cast<std::uint32_t>(lane->absoluteTrack(
            projection.playerLaneCount, projection.draftLaneCount)),
        .m_audioResourceId = resource->m_id,
        .m_volume          = 1.0F,
    };
    const auto entity = m_ctx.sampleRegistry.create();
    // 通过 SampleAction 创建以纳入撤销和派生数据通知。
    // 不直接 emplace 后遗漏选择、同步或统计更新入口。
    m_ctx.actionStack.pushAndExecute(
        std::make_unique<SampleAction>(SampleAction::Type::Create,
                                       entity,
                                       std::nullopt,
                                       std::move(sample)),
        m_ctx);
}

/// @brief 原子更新一个自动采样的资源、BGM 相对轨、偏移与音量。
/// @param cmd 待更新实体及精确属性。
void InteractionController::handleCommand(
    const CmdUpdateAudioSampleProperties& cmd)
{
    if ( m_ctx.isPlaying || cmd.entity == entt::null ||
         !m_ctx.sampleRegistry.valid(cmd.entity) ||
         !m_ctx.sampleRegistry.all_of<SampleComponent>(cmd.entity) ) {
        return;
    }

    const auto* project = m_ctx.collaborationProject
                              ? m_ctx.collaborationProject.get()
                              : EditorEngine::instance().getCurrentProject();
    const std::filesystem::path beatmapPath =
        m_ctx.currentBeatmap ? m_ctx.currentBeatmap->m_baseMapMetadata.map_path
                             : std::filesystem::path{};
    const auto* resource =
        project ? ProjectResourceService::findAudioResourceForReference(
                      *project, beatmapPath, cmd.audioResourceId)
                : nullptr;

    // 复制修改前状态供撤销，属性解析先在值对象上完成。
    // 验证失败时 Registry 中的原采样保持不变。
    const auto before =
        m_ctx.sampleRegistry.get<const SampleComponent>(cmd.entity);
    auto result = resolveSamplePropertyEdit(before,
                                            m_ctx.trackCount,
                                            resource,
                                            cmd.bgmLane,
                                            cmd.offsetMs,
                                            cmd.volume);
    if ( !result.m_sample ) {
        // 将资源、轨道和音量错误分别转换为已有界面消息。
        // 错误分支不写入部分属性，保持一次属性提交的整体性。
        switch ( result.m_issue ) {
        case SamplePropertyEditIssue::MissingResource:
        case SamplePropertyEditIssue::UnsupportedResourceType:
            m_ctx.lastActionMessage =
                TR("ui.edit.sample_properties.invalid_resource").data();
            break;
        case SamplePropertyEditIssue::InvalidPlayerTrackCount:
        case SamplePropertyEditIssue::InvalidBgmLane:
        case SamplePropertyEditIssue::AbsoluteTrackOverflow:
            m_ctx.lastActionMessage =
                TR("ui.edit.sample_properties.invalid_lane").data();
            break;
        case SamplePropertyEditIssue::InvalidVolume:
            m_ctx.lastActionMessage =
                TR("ui.edit.sample_properties.invalid_volume").data();
            break;
        case SamplePropertyEditIssue::None: break;
        }
        return;
    }

    const auto& after = *result.m_sample;
    // 仅比较此命令可修改字段，完全相同则不制造空撤销步骤。
    // 资源解析可能得到同一规范 ID，不能仅按输入字符串判断有变化。
    if ( before.m_audioResourceId == after.m_audioResourceId &&
         before.m_track == after.m_track &&
         before.m_offsetMs == after.m_offsetMs &&
         before.m_volume == after.m_volume ) {
        return;
    }

    m_ctx.actionStack.pushAndExecute(
        std::make_unique<SampleAction>(SampleAction::Type::Update,
                                       cmd.entity,
                                       before,
                                       std::move(*result.m_sample)),
        m_ctx);
    m_ctx.lastActionMessage = TR("ui.edit.sample_properties.updated").data();
}

/// @brief 以单个撤销步骤更新玩家绑定或自动采样的物件音量。
/// @param cmd 带类型的实体、可选 Polyline 子物件索引与音量倍率。
void InteractionController::handleCommand(
    const CmdUpdateObjectSampleVolume& cmd)
{
    if ( cmd.entity == entt::null || !std::isfinite(cmd.volume) ||
         cmd.volume < 0.0F ) {
        m_ctx.lastActionMessage =
            TR("ui.edit.sample_properties.invalid_volume").data();
        return;
    }

    // 自动采样的音量直接属于组件，玩家音效则属于可选绑定。
    // 两种领域需要不同 Action，不能仅凭相同实体号统一修改。
    if ( cmd.kind == ChartObjectKind::AudioSample ) {
        if ( !m_ctx.sampleRegistry.valid(cmd.entity) ||
             !m_ctx.sampleRegistry.all_of<SampleComponent>(cmd.entity) ) {
            return;
        }

        const auto before =
            m_ctx.sampleRegistry.get<const SampleComponent>(cmd.entity);
        if ( before.m_volume == cmd.volume ) return;
        auto after     = before;
        after.m_volume = cmd.volume;
        m_ctx.actionStack.pushAndExecute(
            std::make_unique<SampleAction>(SampleAction::Type::Update,
                                           cmd.entity,
                                           before,
                                           std::move(after)),
            m_ctx);
        m_ctx.lastActionMessage =
            TR("ui.edit.sample_properties.updated").data();
        return;
    }

    if ( !m_ctx.noteRegistry.valid(cmd.entity) ||
         !m_ctx.noteRegistry.all_of<NoteComponent>(cmd.entity) ) {
        return;
    }

    const auto before = m_ctx.noteRegistry.get<const NoteComponent>(cmd.entity);
    if ( !SessionUtils::isNoteEditable(before, m_ctx.lastConfig.settings) ) {
        return;
    }
    auto after = before;
    // 默认选中父对象绑定；有效子索引才改为折线子项绑定。
    // 观察指针指向本地 after 副本，不指向可被命令执行修改的 Registry。
    std::optional<::MMM::AudioSampleBinding>* binding = &after.m_sampleBinding;
    if ( cmd.subIndex >= 0 ) {
        if ( after.m_type != ::MMM::NoteType::POLYLINE ||
             cmd.subIndex >=
                 static_cast<std::int32_t>(after.m_subNotes.size()) ) {
            return;
        }
        binding = &after.m_subNotes[static_cast<std::size_t>(cmd.subIndex)]
                       .sampleBinding;
    }
    // 没有音效绑定时不隐式创建，只更新已经存在的实例音量。
    // 值相同直接返回，避免拖动属性控件反复生成空历史记录。
    if ( !*binding || (*binding)->m_volume == cmd.volume ) return;
    (*binding)->m_volume = cmd.volume;
    m_ctx.actionStack.pushAndExecute(
        std::make_unique<NoteAction>(
            NoteAction::Type::Update, cmd.entity, before, std::move(after)),
        m_ctx);
    m_ctx.lastActionMessage = TR("ui.edit.sample_properties.updated").data();
}

/// @brief 将同一音量应用到全部受支持的选中玩家物件和自动采样。
/// @param cmd 非负有限音量倍率。
void InteractionController::handleCommand(
    const CmdUpdateSelectedObjectSampleVolume& cmd)
{
    if ( !std::isfinite(cmd.volume) || cmd.volume < 0.0F ) {
        m_ctx.lastActionMessage =
            TR("ui.edit.sample_properties.invalid_volume").data();
        return;
    }

    // 只遍历已选实体集合，避免对整个谱面做无关扫描。
    // 先收集前后状态，再统一交给历史栈执行。
    std::vector<BatchNoteAction::Entry> noteEntries;
    noteEntries.reserve(m_ctx.selectedNoteEntities.size());
    for ( const auto entity : m_ctx.selectedNoteEntities ) {
        if ( !m_ctx.noteRegistry.valid(entity) ||
             !m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
            continue;
        }

        const auto& before =
            m_ctx.noteRegistry.get<const NoteComponent>(entity);
        if ( before.m_isSubNote || !SessionUtils::isNoteEditable(
                                       before, m_ctx.lastConfig.settings) ) {
            continue;
        }

        auto after   = before;
        bool changed = false;
        if ( after.m_sampleBinding &&
             after.m_sampleBinding->m_volume != cmd.volume ) {
            after.m_sampleBinding->m_volume = cmd.volume;
            changed                         = true;
        }
        // 选中折线根意味着其已有子项绑定也一起调整。
        // 不创建缺失绑定，并用 changed 避免保存完全没有变化的对象。
        for ( auto& subNote : after.m_subNotes ) {
            if ( !subNote.sampleBinding ||
                 subNote.sampleBinding->m_volume == cmd.volume ) {
                continue;
            }
            subNote.sampleBinding->m_volume = cmd.volume;
            changed                         = true;
        }
        if ( changed ) {
            noteEntries.push_back({
                .entity = entity,
                .before = before,
                .after  = std::move(after),
            });
        }
    }

    // 采样批次单独构造，保证不同 Registry 的实体身份不会混用。
    // 相同音量跳过，结果为空时无需创建这个领域的 Action。
    std::vector<BatchSampleAction::Entry> sampleEntries;
    sampleEntries.reserve(m_ctx.selectedSampleEntities.size());
    for ( const auto entity : m_ctx.selectedSampleEntities ) {
        if ( !m_ctx.sampleRegistry.valid(entity) ||
             !m_ctx.sampleRegistry.all_of<SampleComponent>(entity) ) {
            continue;
        }

        const auto& before =
            m_ctx.sampleRegistry.get<const SampleComponent>(entity);
        if ( before.m_volume == cmd.volume ) continue;
        auto after     = before;
        after.m_volume = cmd.volume;
        sampleEntries.push_back({
            .entity = entity,
            .before = before,
            .after  = std::move(after),
        });
    }

    // 最多组合音符和采样两种批次，用户操作只占一个撤销步骤。
    // 单一领域直接使用原批次，混合选择才构造组合命令。
    std::vector<std::unique_ptr<IEditorAction>> actions;
    actions.reserve(2U);
    const std::string actionName = TR("ui.edit.selected_volume").data();
    if ( !noteEntries.empty() ) {
        actions.push_back(std::make_unique<BatchNoteAction>(
            std::move(noteEntries), actionName));
    }
    if ( !sampleEntries.empty() ) {
        actions.push_back(std::make_unique<BatchSampleAction>(
            std::move(sampleEntries), actionName));
    }
    if ( actions.empty() ) return;

    std::unique_ptr<IEditorAction> action;
    if ( actions.size() == 1U ) {
        action = std::move(actions.front());
    } else {
        action = std::make_unique<CompositeEditorAction>(std::move(actions),
                                                         actionName);
    }
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
}

/// @brief 处理视口鼠标位置、拖拽状态和边缘自动滚动速度。
/// @param cmd 鼠标位置更新指令。
/// @warning 逻辑热路径：UI 可每帧推送；只能更新交互状态和常量时间边缘滚动计算，
/// 禁止 ECS 遍历、文件系统访问和阻塞操作。
void InteractionController::handleCommand(const CmdSetMousePosition& cmd)
{
    const bool hasFiniteMouse =
        std::isfinite(cmd.mouseX) && std::isfinite(cmd.mouseY);
    const bool hasFiniteViewport =
        std::isfinite(cmd.viewportWidth) && std::isfinite(cmd.viewportHeight) &&
        cmd.viewportWidth > 0.0f && cmd.viewportHeight > 0.0f;
    // 有可靠尺寸时检查局部边界；尺寸缺失时保留兼容悬浮判断。
    // 鼠标自身必须有限，不能把 NaN 写入后续相机或滚动状态。
    const bool isInsideViewport =
        hasFiniteMouse &&
        (!hasFiniteViewport ||
         (cmd.mouseX >= 0.0f && cmd.mouseX <= cmd.viewportWidth &&
          cmd.mouseY >= 0.0f && cmd.mouseY <= cmd.viewportHeight));
    const bool isHovering = cmd.isHovering && isInsideViewport;

    bool canUpdate = false;
    if ( isHovering ) {
        // 拖动期间保持鼠标所属相机，其他视口悬浮不得抢走手势。
        // 没有已绑定相机时才允许建立新的归属。
        if ( !m_ctx.isDragging || m_ctx.mouseCameraId == cmd.cameraId ||
             m_ctx.mouseCameraId == "" ) {
            m_ctx.mouseCameraId   = cmd.cameraId;
            m_ctx.isMouseInCanvas = true;
            canUpdate             = true;
        }
    } else if ( cmd.isDragging && hasFiniteMouse && m_ctx.isDragging &&
                m_ctx.mouseCameraId == cmd.cameraId ) {
        // 如果正在往外拖拽，依然允许更新坐标以便主画布跟随
        canUpdate = true;
    }

    if ( canUpdate ) {
        m_ctx.lastMousePos = { cmd.mouseX, cmd.mouseY };
        // 单独记录主画布位置，供之后按区域全选使用。
        // 辅助视图更新不应覆盖这份最后主画布定位。
        if ( isHovering && SessionUtils::isMainCanvasCameraId(cmd.cameraId) ) {
            m_ctx.lastMainCanvasCameraId = cmd.cameraId;
            m_ctx.lastMainCanvasMousePos = m_ctx.lastMousePos;
        }

        // 如果命令携带了直接的时间戳，优先使用它（用于音频视图等非空间映射视口）
        if ( cmd.hoverTime >= 0.0 && std::isfinite(cmd.hoverTime) ) {
            m_ctx.previewHoverTime = cmd.hoverTime;
            m_ctx.isDragging       = cmd.isDragging;
            m_ctx.dragCameraId     = cmd.cameraId;
        } else if ( cmd.cameraId == "AudioWaveform" ||
                    cmd.cameraId == "AudioSpectrum" ) {
            // 处理音频视图的释放操作
            m_ctx.isDragging = cmd.isDragging;
        } else if ( cmd.cameraId == "Preview" ) {
            // 传统的预览区交互
            m_ctx.isDragging = cmd.isDragging;
            if ( m_ctx.isDragging ) m_ctx.dragCameraId = cmd.cameraId;
        }

        // 边缘滚动逻辑
        // 每次有效输入重新计算边缘速度，离开边缘后立即归零。
        // 不能沿用上一条命令的速度导致指针回到中央仍继续滚动。
        m_ctx.previewEdgeScrollVelocity = 0.0;

        if ( cmd.isDragging && cmd.viewportWidth > 0 &&
             cmd.viewportHeight > 0 ) {
            float margin = 20.0f;
            float dist   = 0.0f;

            if ( cmd.cameraId == "Preview" ) {
                // 纵向边缘滚动 (Preview)
                if ( cmd.mouseY < margin )
                    dist = margin - cmd.mouseY;
                else if ( cmd.mouseY > cmd.viewportHeight - margin )
                    dist = (cmd.viewportHeight - margin) - cmd.mouseY;
            } else if ( cmd.cameraId == "AudioWaveform" ||
                        cmd.cameraId == "AudioSpectrum" ) {
                // 横向边缘滚动 (Audio Views)
                if ( cmd.mouseX < margin )
                    dist = cmd.mouseX - margin;
                else if ( cmd.mouseX > cmd.viewportWidth - margin )
                    dist = cmd.mouseX - (cmd.viewportWidth - margin);
            }

            // 边缘距离乘用户灵敏度得到持续滚动速度，方向由视图轴约定决定。
            // 只发布速度，不在输入处理函数中等待滚动达到目标。
            if ( std::abs(dist) > 0.001f ) {
                float sensitivity =
                    m_ctx.lastConfig.visual.previewConfig.edgeScrollSensitivity;
                m_ctx.previewEdgeScrollVelocity =
                    static_cast<double>(dist) * sensitivity;
            }
        }
    } else if ( cmd.cameraId == m_ctx.mouseCameraId ||
                cmd.cameraId == m_ctx.dragCameraId ) {
        m_ctx.previewEdgeScrollVelocity = 0.0;
    }

    // 只有预览和音频视图通过鼠标位置命令拥有这份辅助拖动状态。
    // 普通主画布的对象拖动由对应工具的结束命令管理。
    const bool commandOwnsDragState = cmd.cameraId == "Preview" ||
                                      cmd.cameraId == "AudioWaveform" ||
                                      cmd.cameraId == "AudioSpectrum";
    if ( commandOwnsDragState && !cmd.isDragging &&
         (cmd.cameraId == m_ctx.mouseCameraId ||
          cmd.cameraId == m_ctx.dragCameraId) ) {
        m_ctx.isDragging = false;
        if ( m_ctx.dragCameraId == cmd.cameraId ) {
            m_ctx.dragCameraId.clear();
        }
    }

    // 未悬浮且无活动拖动时才释放鼠标相机归属。
    // 拖出视口但仍按住时保留归属，以便继续更新边缘滚动。
    if ( !isHovering && !m_ctx.isDragging ) {
        if ( m_ctx.mouseCameraId == cmd.cameraId ) {
            m_ctx.mouseCameraId             = "";
            m_ctx.isMouseInCanvas           = false;
            m_ctx.previewEdgeScrollVelocity = 0.0;
        }
    }
}

/// @brief 调整玩家轨数并保持采样的 BGM 相对轨位置。
/// @param cmd 目标玩家轨数，必须为正。
/// @warning 低频布局编辑：遍历采样以准备可撤销映射，不得从逐帧更新调用。
void InteractionController::handleCommand(const CmdUpdateTrackCount& cmd)
{
    // 空布局与相同轨数都不构造历史记录。
    // 检查发生在采样遍历前，避免无效操作触发整表准备。
    if ( cmd.trackCount <= 0 || cmd.trackCount == m_ctx.trackCount ) {
        return;
    }

    const auto oldTrackCount = m_ctx.trackCount;
    std::vector<TrackCountAction::SampleTrackChange> sampleChanges;
    const auto sampleView = m_ctx.sampleRegistry.view<const SampleComponent>();
    sampleChanges.reserve(sampleView.size());
    for ( auto entity : sampleView ) {
        const auto& sample = sampleView.get<const SampleComponent>(entity);
        // 采样存储统一绝对轨号，先扣除旧玩家轨数恢复区内索引。
        // 旧轨号不在 BGM 区时按首轨处理，沿用现有兼容规则。
        const std::uint32_t bgmIndex =
            sample.m_track >= static_cast<std::uint32_t>(oldTrackCount)
                ? sample.m_track - static_cast<std::uint32_t>(oldTrackCount)
                : 0;
        // 用宽整数计算新绝对轨号，检查后才缩窄回组件类型。
        // 轨数增加不能让采样索引绕回低位玩家区。
        const std::uint64_t afterTrack =
            static_cast<std::uint64_t>(cmd.trackCount) + bgmIndex;
        if ( afterTrack > static_cast<std::uint64_t>(
                              std::numeric_limits<std::uint32_t>::max()) ) {
            m_ctx.lastActionMessage =
                "玩家轨道数变更会导致自动采样轨道索引溢出";
            return;
        }
        // 保存每个采样修改前后的轨号，撤销必须恢复同一映射。
        // 此时只收集变化，不边遍历边修改，保证后续失败可以完整取消。
        sampleChanges.push_back({
            .entity      = entity,
            .beforeTrack = sample.m_track,
            .afterTrack  = static_cast<std::uint32_t>(afterTrack),
        });
    }

    // 玩家轨数和所有采样轨映射打包为一次可撤销操作。
    // 不能分多条历史命令，否则撤销中间态会让轨道域不一致。
    auto action = std::make_unique<TrackCountAction>(
        oldTrackCount, cmd.trackCount, std::move(sampleChanges));
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
}

/// @brief 按单步规则增加或移除末尾 BGM 轨道。
/// @param cmd 目标 BGM 轨数，允许为零。
/// @warning 低频布局编辑：减轨检查所有采样占用，禁止放入连续渲染路径。
void InteractionController::handleCommand(const CmdUpdateBgmTrackCount& cmd)
{
    if ( !m_ctx.currentBeatmap || cmd.bgmTrackCount < 0 ||
         cmd.bgmTrackCount == m_ctx.bgmTrackCount ) {
        return;
    }

    // 先提升到有符号宽整数再求差，避免不同方向差值溢出。
    // 此入口只允许相邻数量变化，不处理一次删除多条轨的策略。
    const auto currentCount = static_cast<std::int64_t>(m_ctx.bgmTrackCount);
    const auto targetCount  = static_cast<std::int64_t>(cmd.bgmTrackCount);
    if ( std::abs(targetCount - currentCount) != 1 ) {
        m_ctx.lastActionMessage =
            TR("ui.status.project.bgm_track_single_step").data();
        return;
    }

    if ( targetCount < currentCount ) {
        // 删除末尾轨时，阈值等于玩家轨数加保留的 BGM 数量。
        // 所有不小于阈值的采样都将落在被移除区域，必须拒绝减轨。
        const auto firstRemovedTrack =
            static_cast<std::uint64_t>(std::max(0, m_ctx.trackCount)) +
            static_cast<std::uint64_t>(targetCount);
        const auto sampleView =
            m_ctx.sampleRegistry.view<const SampleComponent>();
        for ( const auto entity : sampleView ) {
            if ( sampleView.get<const SampleComponent>(entity).m_track >=
                 firstRemovedTrack ) {
                m_ctx.lastActionMessage =
                    TR("ui.status.project.bgm_track_occupied").data();
                return;
            }
        }
    }

    // 通过专用 Action 修改布局并进入撤销栈，不直接改元数据。
    // 减轨占用检查全部通过后才创建操作，失败不留下部分状态。
    m_ctx.actionStack.pushAndExecute(
        std::make_unique<BgmTrackCountAction>(m_ctx.bgmTrackCount,
                                              cmd.bgmTrackCount),
        m_ctx);
}

/// @brief 切换后续交互命令使用的工具类型。
/// @param cmd 目标工具枚举。
/// @note 只改变路由状态，当前手势的结束由对应输入流程负责。
void InteractionController::handleCommand(const CmdChangeTool& cmd)
{
    m_ctx.currentTool = cmd.tool;
}

/// @brief 修改画笔的单个颜色覆盖槽。
/// @param cmd 颜色槽与可选覆盖色。
/// @note 只改画笔配置，不重写已经提交的物件。
void InteractionController::handleCommand(const CmdSetBrushNoteColor& cmd)
{
    setNoteColorOverride(m_ctx.brushState.customColors, cmd.slot, cmd.color);
}

/// @brief 一次替换画笔各部件的颜色覆盖。
/// @param cmd 按颜色槽顺序排列的覆盖数组。
/// @note 固定槽数更新，不遍历谱面物件。
void InteractionController::handleCommand(const CmdSetBrushNotePalette& cmd)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteColorOverride(
            m_ctx.brushState.customColors, slot, cmd.colors[i]);
    }
}

/// @brief 保存后续画笔创建使用的音频选择。
/// @param cmd 资源身份、轨道类型与实例音量。
/// @note 不加载或解码资源，非有限音量回退默认倍率。
void InteractionController::handleCommand(const CmdSetBrushAudioResource& cmd)
{
    m_ctx.brushState.selectedAudioResourceId = cmd.audioResourceId;
    m_ctx.brushState.selectedAudioTrackType  = cmd.audioTrackType;
    // 负音量钳制到零，NaN 或无穷使用一倍默认值。
    // 这里只保存画笔偏好，资源类型和最终放置是否合法由创建入口处理。
    m_ctx.brushState.selectedAudioVolume =
        std::isfinite(cmd.volume) ? std::max(0.0F, cmd.volume) : 1.0F;
}

/// @brief 把框选起始命令交给当前工具。
/// @param cmd 框选起点及输入状态。
/// @note 输入路径只转发，是否支持框选由具体工具决定。
void InteractionController::handleCommand(const CmdStartMarquee& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleStartMarquee(m_ctx, cmd);
    }
}

/// @brief 把框选连续位置交给当前工具。
/// @param cmd 当前框选端点与修饰键。
/// @warning 连续交互热路径：立即转发，不阻塞等待或延后本地反馈。
void InteractionController::handleCommand(const CmdUpdateMarquee& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateMarquee(m_ctx, cmd);
    }
}

/// @brief 通知当前工具结束框选手势。
/// @param cmd 框选结束状态。
/// @note 不在控制器重复计算选择，工具负责最终手势状态。
void InteractionController::handleCommand(const CmdEndMarquee& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndMarquee(m_ctx, cmd);
    }
}

/// @brief 请求当前工具移除命中位置的框选框。
/// @param cmd 用于定位既有框选框的输入。
/// @note 工具不存在时保持状态，不临时创建工具对象。
void InteractionController::handleCommand(const CmdRemoveMarqueeAt& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleRemoveMarqueeAt(m_ctx, cmd);
    }
}

/// @brief 建立画笔拖动归属并调用当前工具。
/// @param cmd 画笔起点与所属相机。
/// @note 只在工具存在时进入拖动状态，具体落点合法性由工具检查。
void InteractionController::handleCommand(const CmdStartBrush& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        // 先记录手势相机，后续鼠标更新才能保持归属并支持出界拖动。
        // 工具拿到同一会话状态，不需要另建控制器侧临时实体。
        m_ctx.isDragging   = true;
        m_ctx.dragCameraId = cmd.cameraId;
        m_tools[m_ctx.currentTool]->handleStartBrush(m_ctx, cmd);
    }
}

/// @brief 将画笔最新位置转发给当前工具。
/// @param cmd 画笔当前位置和修饰键。
/// @warning 连续交互热路径：不得用固定时长等待合并手势。
void InteractionController::handleCommand(const CmdUpdateBrush& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateBrush(m_ctx, cmd);
    }
}

/// @brief 通知工具提交画笔并退出拖动状态。
/// @param cmd 画笔结束所在视图。
/// @note 结束时即使工具缺失也清除拖动标志，避免遗留活动手势。
void InteractionController::handleCommand(const CmdEndBrush& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndBrush(m_ctx, cmd);
    }
    // 工具结束回调可能提交最终对象，此时仍能读取拖动上下文。
    // 退出标志放在回调之后，并覆盖工具已不可用的情况。
    m_ctx.isDragging = false;
}

/// @brief 建立擦除拖动归属并调用当前工具。
/// @param cmd 擦除开始所在相机。
/// @note 正式删除事务由工具维护，控制器不直接销毁实体。
void InteractionController::handleCommand(const CmdStartErase& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        // 擦除和画笔共用输入归属，但删除目标集合由具体工具管理。
        // 绑定相机使连续擦除不会被其他视口的悬浮更新抢占。
        m_ctx.isDragging   = true;
        m_ctx.dragCameraId = cmd.cameraId;
        m_tools[m_ctx.currentTool]->handleStartErase(m_ctx, cmd);
    }
}

/// @brief 把擦除过程的新输入转发给当前工具。
/// @param cmd 擦除位置及输入状态。
/// @warning 连续交互热路径：只走既有工具入口，不重建工具或阻塞等待。
void InteractionController::handleCommand(const CmdUpdateErase& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateErase(m_ctx, cmd);
    }
}

/// @brief 结束擦除工具操作并退出拖动状态。
/// @param cmd 擦除结束所在视图。
/// @note 先通知工具完成事务，再清除会话拖动标志。
void InteractionController::handleCommand(const CmdEndErase& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndErase(m_ctx, cmd);
    }
    // 无论工具是否仍注册，都要停止控制器层面的拖动状态。
    // 这里不清空历史栈，擦除提交与撤销记录由结束回调负责。
    m_ctx.isDragging = false;
}

/// @brief 根据当前框选区域重新计算实体选中状态。
/// @warning 逻辑热路径：框选框变化时执行；使用已缓存的排序/视口/图集数据，
/// 禁止文件系统访问、阻塞等待或完整无关 ECS 遍历。
/// @param forceFullSync 忽略脏标记并按现有框替换选择，不保留叠加旧集合。
/// @note 无有效框时不修改已有选择，清空语义由显式选择命令处理。
void InteractionController::updateMarqueeSelection(bool forceFullSync)
{
    // 普通 update 只做脏标记检查，框未变化时跳过候选和几何计算。
    // 强制同步允许历史或外部变化要求重新应用当前框集合。
    if ( !m_ctx.isMarqueeSelectionDirty && !forceFullSync ) return;
    // 本轮开始消费脏标记，后续空框或无效投影早退也不会反复重算。
    // 这些早退保留当前实体选择，显式清空由独立选择入口负责。
    m_ctx.isMarqueeSelectionDirty = false;
    if ( m_ctx.marqueeBoxes.empty() ) return;

    const auto* cache =
        m_ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    // 按每个框自己的相机准备屏幕数据，后续候选重复使用这些结果。
    // 没有有效框时不先清除选中集合，避免缺少视口时丢失选择。
    const auto preparedBoxes = prepareMarqueeBoxes(m_ctx, cache);
    if ( preparedBoxes.empty() ) return;

    auto mode = m_ctx.lastConfig.settings.selectionMode;
    // 普通替换与强制同步先清旧选择，再建立框的并集。
    // 叠加模式保留旧集合，仅把新命中的候选加入。
    if ( forceFullSync || !m_ctx.marqueeIsAdditive ) {
        clearSelectedEntityFlags(m_ctx);
    }

    // 两个 Registry 的候选带领域汇入同一列表，实体号本身不具有全局唯一性。
    // 先做时间粗筛，再按框选模式对具体物件屏幕几何精筛。
    std::vector<MarqueeSelectionCandidate> candidates;
    collectMarqueeSelectionCandidates(m_ctx, preparedBoxes, candidates);
    collectMarqueeSampleCandidates(m_ctx, preparedBoxes, candidates);

    for ( const auto& candidate : candidates ) {
        bool isSelectedInAny = false;
        if ( candidate.kind == ChartObjectKind::PlayerNote ||
             candidate.kind == ChartObjectKind::DraftNote ) {
            if ( !m_ctx.noteRegistry.valid(candidate.entity) ||
                 !m_ctx.noteRegistry.all_of<NoteComponent>(candidate.entity) ) {
                continue;
            }

            const auto& note =
                m_ctx.noteRegistry.get<const NoteComponent>(candidate.entity);
            // 候选索引不承诺当前模式下可编辑，这里再次检查业务条件。
            // 子实体不独立选中，折线选择由父对象几何入口处理。
            if ( !SessionUtils::isNoteEditable(note,
                                               m_ctx.lastConfig.settings) ||
                 note.m_isSubNote ) {
                continue;
            }
            // 多个框按并集处理，命中一个就停止扫描后续框。
            // 严格或相交规则由几何助手决定，不在这里重复计算包围框。
            for ( const auto& box : preparedBoxes ) {
                if ( noteMatchesSelection(note, box.screen, box.rect, mode) ) {
                    isSelectedInAny = true;
                    break;
                }
            }
            if ( !isSelectedInAny ) continue;
            setChartObjectSelected(
                m_ctx, candidate.kind, candidate.entity, true);
            continue;
        }

        // 采样领域重新使用自己的 Registry 校验，避免同号音符被误选。
        // 候选收集后仍保持有效性检查，不假设所有组件永久存在。
        if ( !m_ctx.sampleRegistry.valid(candidate.entity) ||
             !m_ctx.sampleRegistry.all_of<SampleComponent>(candidate.entity) ) {
            continue;
        }
        const auto& sample =
            m_ctx.sampleRegistry.get<const SampleComponent>(candidate.entity);
        for ( const auto& box : preparedBoxes ) {
            // 精筛阶段继续排除 Preview 采样，不能仅依赖粗筛来源。
            // 多视口框共存时，采样只允许由实际支持它的视口框命中。
            if ( box.box.cameraId == "Preview" ||
                 box.box.cameraId == "PreviewCanvas" ) {
                continue;
            }
            if ( sampleMatchesSelection(sample, box.screen, box.rect, mode) ) {
                isSelectedInAny = true;
                break;
            }
        }
        if ( !isSelectedInAny ) continue;
        setChartObjectSelected(
            m_ctx, ChartObjectKind::AudioSample, candidate.entity, true);
    }
}

}  // namespace MMM::Logic
