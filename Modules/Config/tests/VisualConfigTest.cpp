#include "config/EditorConfig.h"

#include "log/colorful-log.h"

#include <cmath>
#include <nlohmann/json.hpp>

namespace
{
// 每个用例同时覆盖当前格式往返与对应旧配置回退，避免迁移规则失去保护。

/// @brief 使用小容差比较视觉配置中的单精度数值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(float lhs, float rhs)
{
    // 固定绝对容差适合本测试中的归一化比例和小范围配置常量。
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 验证当前分拍线显示模式与自动范围能够完整往返。
/// @return 当前格式往返无损时返回 true。
/// @note 同时检查旧 drawBeatLines 兼容字段继续写出可见状态。
/// @details 输入采用 NearCursor，确保测试经过三态枚举的非默认分支。
/// 恢复后分别检查模式和两段比例，避免只验证 JSON 表面字段存在。
/// 旧布尔镜像必须为 true，因为 NearCursor 仍属于可显示状态。
bool testBeatLineDisplayModeRoundTrip()
{
    // 所有对象保留局部生命周期，测试之间不存在共享视觉配置状态。
    // 使用非默认模式和非默认比例，防止测试仅验证构造默认值。
    MMM::Config::VisualConfig source;
    source.beatLineDisplayMode = MMM::Config::BeatLineDisplayMode::NearCursor;
    source.beatLineCursorVisibleRatio = 0.27F;
    source.beatLineCursorFadeRatio    = 0.31F;

    // 通过 ADL 完成真实序列化与反序列化链路。
    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    // 三态、可见核心、淡出区域及旧布尔字段必须共同满足契约。
    if ( restored.beatLineDisplayMode !=
             MMM::Config::BeatLineDisplayMode::NearCursor ||
         !near(restored.beatLineCursorVisibleRatio, 0.27F) ||
         !near(restored.beatLineCursorFadeRatio, 0.31F) ||
         !encoded.value("drawBeatLines", false) ) {
        XERROR("Beat line display mode did not survive JSON round trip");
        // 聚合条件失败时使用单一职责消息，便于定位该兼容组。
        return false;
    }
    return true;
}

/// @brief 验证旧版 drawBeatLines 布尔值能够迁移为三态显示模式。
/// @return 旧版开关的开启与关闭语义均保持时返回 true。
/// @note 旧格式无法表达 NearCursor，因此只映射到 Always 和 Hidden。
/// @details 用例不提供新字段，确保读取逻辑真实进入旧字段迁移分支。
/// true 与 false 分别映射到可表达的两端状态，不推导自动显示模式。
/// 两个对象独立恢复，避免默认构造或前次读取掩盖迁移结果。
bool testLegacyDrawBeatLinesMigration()
{
    // 当前三态字段完全缺失，避免新字段优先级干扰旧值映射。
    // 两个最小 JSON 分别覆盖旧布尔字段的全部状态空间。
    const nlohmann::json hiddenJson{ { "drawBeatLines", false } };
    const nlohmann::json visibleJson{ { "drawBeatLines", true } };
    // 分别反序列化，避免同一对象前次状态影响另一分支。
    const auto hidden  = hiddenJson.get<MMM::Config::VisualConfig>();
    const auto visible = visibleJson.get<MMM::Config::VisualConfig>();
    // false 必须完全隐藏，true 必须保持旧版始终显示行为。
    if ( hidden.beatLineDisplayMode !=
             MMM::Config::BeatLineDisplayMode::Hidden ||
         visible.beatLineDisplayMode !=
             MMM::Config::BeatLineDisplayMode::Always ) {
        XERROR("Legacy drawBeatLines value was not migrated");
        // 迁移失败会直接影响旧用户的分拍线可见性。
        return false;
    }
    return true;
}

/// @brief 验证自动显示比例在读取配置时被限制到工具栏允许范围。
/// @return 过小和过大的比例均被正确限制时返回 true。
/// @note 输入同时触碰两个方向的边界，覆盖独立夹取规则。
/// @details 可见比例输入零，期望提升到最小的 0.05。
/// 淡出比例输入一，期望降低到最大的 0.40。
/// 显式提供 NearCursor 使这两个比例具有实际配置语境。
bool testBeatLineAutoRatioClamping()
{
    // 用例输入是普通 JSON 数字，覆盖 nlohmann 到 float 的实际转换。
    // 可见比例给出下界外值，淡出比例给出上界外值。
    const nlohmann::json json{
        { "beatLineDisplayMode", "NearCursor" },
        { "beatLineCursorVisibleRatio", 0.0F },
        { "beatLineCursorFadeRatio", 1.0F },
    };
    // 只测试读取净化，不依赖先经过生产写出路径。
    const auto config = json.get<MMM::Config::VisualConfig>();
    // 期望值与设置界面允许范围的精确两端一致。
    if ( !near(config.beatLineCursorVisibleRatio, 0.05F) ||
         !near(config.beatLineCursorFadeRatio, 0.40F) ) {
        XERROR("Beat line auto display ratios escaped supported bounds");
        // 两个比例共用一条错误消息，因为它们组成同一淡入淡出模型。
        return false;
    }
    return true;
}

/// @brief 验证悬浮检视分拍线单侧延伸比例能够持久化并限制到允许范围。
/// @return 当前值往返、旧配置默认值和上下界限制均正确时返回 true。
/// @note 用例覆盖有效值、缺失值、负值和超过一的值四种输入。
/// @details 有效值 0.75 应原样往返，证明净化不会压平合法自定义。
/// 空对象应恢复 0.5，保持功能引入前的视觉长度。
/// 两个越界对象应精确落到归一化区间的零和一。
bool testHoverSubdivisionLineExtensionRatioConfig()
{
    // 所有比较使用 near，避免浮点 JSON 转换细节造成误报。
    // 选择四分之三比例验证非默认有效值不会被错误夹取。
    MMM::Config::VisualConfig source;
    source.hoverSubdivisionLineExtensionRatio = 0.75F;

    // 当前格式先往返，再额外构造三个绕过写出净化的读取输入。
    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::VisualConfig>();
    // 两个越界夹具分别验证零和一的归一化边界。
    const auto belowMinimum =
        nlohmann::json{ { "hoverSubdivisionLineExtensionRatio", -0.4F } }
            .get<MMM::Config::VisualConfig>();
    const auto aboveMaximum =
        nlohmann::json{ { "hoverSubdivisionLineExtensionRatio", 1.8F } }
            .get<MMM::Config::VisualConfig>();
    // 旧配置默认二分之一，保证升级后悬浮辅助线长度不突变。
    if ( !near(restored.hoverSubdivisionLineExtensionRatio, 0.75F) ||
         !near(legacy.hoverSubdivisionLineExtensionRatio, 0.5F) ||
         !near(belowMinimum.hoverSubdivisionLineExtensionRatio, 0.0F) ||
         !near(aboveMaximum.hoverSubdivisionLineExtensionRatio, 1.0F) ) {
        XERROR("Hover subdivision line extension ratio was not normalized");
        // 任一分支失败都说明同一字段兼容或净化契约破坏。
        return false;
    }
    return true;
}

/// @brief 验证预览区默认隐藏分拍线并继续显示 Timing 线。
/// @return 默认构造和缺省 JSON 均使用相同的安全显示状态时返回 true。
/// @note 同时比较类型默认和反序列化默认，防止两处规则漂移。
/// @details 直接构造覆盖编译期成员默认，空 JSON 覆盖持久化回退。
/// 两条路径都应减少分拍线噪声，同时保留 Timing 结构提示。
/// 用例不提供其他预览字段，确保断言只受默认策略影响。
bool testPreviewAreaLineDefaults()
{
    // 仅观察两个开关，不让其他预览区默认值扩大用例职责。
    // defaults 代表直接构造路径，restored 代表旧配置缺少字段的路径。
    const MMM::Config::PreviewAreaConfig defaults;
    const auto                           restored =
        nlohmann::json::object().get<MMM::Config::PreviewAreaConfig>();
    // 分拍线应关闭以减少预览噪声，Timing 线必须保留结构提示。
    if ( defaults.drawBeatLines || restored.drawBeatLines ||
         !defaults.drawTimingLines || !restored.drawTimingLines ) {
        XERROR("Preview area line defaults were not preserved");
        // 任意路径不一致都会导致首次启动和升级用户体验不同。
        return false;
    }
    return true;
}

/// @brief 验证玩家物件绑定音效标签能够显式关闭且缺省配置默认开启。
/// @return 当前格式往返关闭且缺省 JSON 保持开启时返回 true。
/// @note encoded 也用于确认 false 不会因默认值优化而从 JSON 省略。
/// @details source 采用与默认相反的关闭值，能发现写出遗漏。
/// restored 验证当前格式，legacy 验证功能引入前的空配置。
/// 三个观察点共同保护标签可见性的向前和向后兼容。
bool testBoundSampleLabelConfigRoundTrip()
{
    // 该字段只有布尔语义，无需构造皮肤或音效资源。
    // 选择与默认值相反的 false，验证显式关闭可以持久化。
    MMM::Config::VisualConfig source;
    source.showBoundSampleLabels = false;

    // 当前格式和空旧格式分别走同一 from_json 入口。
    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::VisualConfig>();
    // 写出 false、往返 false 与缺失 true 三个条件共同定义兼容行为。
    if ( encoded.value("showBoundSampleLabels", true) ||
         restored.showBoundSampleLabels || !legacy.showBoundSampleLabels ) {
        XERROR("Bound sample label config did not preserve compatibility");
        // 失败会导致用户关闭设置丢失或旧配置标签意外消失。
        return false;
    }
    return true;
}

/// @brief 验证交互拾取包围盒横纵缩放能够持久化并限制到调试界面范围。
/// @return 往返、缺省值和上下界限制均正确时返回 true。
/// @note 横纵轴使用不同有效值和相反越界方向，覆盖独立处理。
/// @details 当前格式以 2.5 和 0.75 验证两个轴不会串位。
/// 空配置必须恢复公开 DEFAULT 常量而非复制的测试数值。
/// 越界配置分别命中 MIN 和 MAX 常量，覆盖读取端夹取。
bool testInteractionHitboxScaleConfig()
{
    // 公共边界常量同时作为生产 UI 和本测试的唯一约束来源。
    // X 放大而 Y 缩小，验证两个字段不会在序列化时互换。
    MMM::Config::VisualConfig source;
    source.interactionHitboxScaleX = 2.5F;
    source.interactionHitboxScaleY = 0.75F;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::VisualConfig>();
    // 直接读取零和超大值，确认净化不依赖设置界面预先约束。
    const auto clamped = nlohmann::json{ { "interactionHitboxScaleX", 0.0F },
                                         { "interactionHitboxScaleY", 8.0F } }
                             .get<MMM::Config::VisualConfig>();
    // 当前值、空配置默认和两端夹取一次性检查完整字段契约。
    if ( !near(restored.interactionHitboxScaleX, 2.5F) ||
         !near(restored.interactionHitboxScaleY, 0.75F) ||
         !near(legacy.interactionHitboxScaleX,
               MMM::Config::VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE) ||
         !near(legacy.interactionHitboxScaleY,
               MMM::Config::VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE) ||
         !near(clamped.interactionHitboxScaleX,
               MMM::Config::VisualConfig::MIN_INTERACTION_HITBOX_SCALE) ||
         !near(clamped.interactionHitboxScaleY,
               MMM::Config::VisualConfig::MAX_INTERACTION_HITBOX_SCALE) ) {
        XERROR("Interaction hitbox scales escaped supported bounds");
        // 精确错误归属于配置范围，而非实际拾取系统行为。
        return false;
    }
    return true;
}

/// @brief 验证非 Hold 打击特效时长能够持久化并兼容旧配置。
/// @return 当前值往返无损、旧配置使用默认值且越界值被限制时返回 true。
/// @note 测试不渲染特效，只验证消费前的持久化边界。
/// @details 合法时长 0.48 应原样恢复，避免范围校正误伤有效值。
/// 空配置使用公开默认常量，兼容功能加入前的用户文件。
/// 零和八秒分别命中公开最小值与最大值，覆盖两个夹取方向。
bool testNonHoldHitEffectDurationConfig()
{
    // 合法值、默认值和边界值都在同一精度口径下比较。
    // 使用范围内非默认时长证明合法值不会被重置。
    MMM::Config::VisualConfig source;
    source.nonHoldHitEffectDuration = 0.48F;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::VisualConfig>();
    // 两个读取夹具分别低于和高于产品允许时长。
    const auto tooShort = nlohmann::json{ { "nonHoldHitEffectDuration", 0.0F } }
                              .get<MMM::Config::VisualConfig>();
    const auto tooLong  = nlohmann::json{ { "nonHoldHitEffectDuration", 8.0F } }
                              .get<MMM::Config::VisualConfig>();
    // 默认值和最小最大常量直接来自类型，避免测试复制实现数值。
    if ( !near(restored.nonHoldHitEffectDuration, 0.48F) ||
         !near(
             legacy.nonHoldHitEffectDuration,
             MMM::Config::VisualConfig::DEFAULT_NON_HOLD_HIT_EFFECT_DURATION) ||
         !near(tooShort.nonHoldHitEffectDuration,
               MMM::Config::VisualConfig::MIN_NON_HOLD_HIT_EFFECT_DURATION) ||
         !near(tooLong.nonHoldHitEffectDuration,
               MMM::Config::VisualConfig::MAX_NON_HOLD_HIT_EFFECT_DURATION) ) {
        XERROR("Non-Hold hit effect duration did not preserve safe bounds");
        // 该失败只说明配置契约，不扩展为动画时序测试。
        return false;
    }
    return true;
}

/// @brief 验证折线编辑开关可持久化且旧配置保持现有完整编辑行为。
/// @return 关闭状态往返不变且缺失字段默认开启时返回 true。
/// @note 使用 EditorSettings 根对象覆盖实际持久化字段位置。
/// @details 当前格式明确写出 false，证明用户禁用选择不会丢失。
/// 空旧配置恢复 true，保持功能引入前已经可用的折线编辑能力。
/// 用例不关联 professionalMode，保护两个设置的独立性。
bool testPolylineEditingConfigRoundTrip()
{
    // 显式关闭与当前默认开启相反，可发现字段遗漏。
    MMM::Config::EditorSettings source;
    source.enablePolylineEditing = false;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    // 空对象模拟功能引入前的真实旧用户配置。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // JSON 值、恢复值和旧默认三层共同验证写读兼容。
    if ( encoded.value("enablePolylineEditing", true) ||
         restored.enablePolylineEditing || !legacy.enablePolylineEditing ) {
        XERROR("Polyline editing config did not preserve compatibility");
        // 测试不进入编辑器行为层，仅确认开关状态。
        return false;
    }
    return true;
}

/// @brief 验证工具栏悬浮滚轮开关可持久化且旧配置默认关闭。
/// @return 开启状态往返不变且缺失字段保持关闭时返回 true。
/// @note 选择 true 验证新增且默认关闭的字段确实被写出。
/// @details 当前格式检查 JSON 原值和恢复成员两个观察点。
/// 空配置仍关闭该交互，避免升级后滚轮操作突然改变工具栏数值。
/// 该设置与普通滚动速度无关，因此不构造其他交互字段。
bool testToolbarValueWheelAdjustmentRoundTrip()
{
    // 当前设置开启，空旧配置仍必须保持关闭以避免误操作。
    MMM::Config::EditorSettings source;
    source.enableToolbarValueWheelAdjustment = true;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    // legacy 与 restored 使用独立对象，避免默认构造状态相互影响。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 写出、往返和缺失默认值形成该布尔字段的完整状态验证。
    if ( !encoded.value("enableToolbarValueWheelAdjustment", false) ||
         !restored.enableToolbarValueWheelAdjustment ||
         legacy.enableToolbarValueWheelAdjustment ) {
        XERROR("Toolbar hover-wheel config did not preserve disabled default");
        // 失败可能导致旧用户在悬浮时意外调整数值。
        return false;
    }
    return true;
}

/// @brief 验证 BMS 编辑开关可持久化且旧配置默认显示 BGM 轨道。
/// @return 关闭状态往返不变且缺失字段默认开启时返回 true。
/// @note 字段位于 EditorSettings，不依赖当前打开谱面类型。
/// @details source 选择关闭状态，确认 false 仍被完整持久化。
/// legacy 为空对象，必须恢复开启以延续旧版 BGM 轨道可编辑行为。
/// 用例不加载谱面，避免把配置契约与格式检测耦合。
bool testBmsEditingConfigRoundTrip()
{
    // 使用非默认关闭状态确认用户可以持久禁用 BMS 编辑能力。
    MMM::Config::EditorSettings source;
    source.enableBmsEditing = false;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    // 空旧配置应获得当前默认开启，保留原有 BGM 轨道行为。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 同时检查原始 JSON，防止写出函数遗漏 false 字段。
    if ( encoded.value("enableBmsEditing", true) || restored.enableBmsEditing ||
         !legacy.enableBmsEditing ) {
        XERROR("BMS editing config did not preserve compatibility");
        // 用例不加载 BMS 文件，避免把格式行为混入配置测试。
        return false;
    }
    return true;
}

/// @brief 验证共用专业模式往返、旧时间线配置迁移及独立编辑开关。
/// @return 专业模式迁移正确且不会覆盖 BMS、折线编辑偏好时返回 true。
/// @note false 和 true 均执行，以覆盖迁移条件两端而非单一默认值。
/// @details 当前格式只应写 professionalMode，不再产生两个废弃字段。
/// 旧 timelineProfessionalMode 仍能恢复，且新字段同时存在时优先。
/// 独立编辑开关刻意取反，证明迁移不会把专业模式扩散到它们。
bool testProfessionalModeConfigMigration()
{
    // 循环两种布尔值，确保迁移不是仅对启用状态特判。
    for ( const bool enabled : { false, true } ) {
        // 两个独立编辑开关刻意取反，验证专业模式不会覆盖它们。
        MMM::Config::EditorSettings source;
        source.professionalMode      = enabled;
        source.enableBmsEditing      = !enabled;
        source.enablePolylineEditing = !enabled;
        const nlohmann::json encoded = source;
        // 当前格式、旧字段格式和新旧字段同时存在三种输入分别恢复。
        const auto restored = encoded.get<MMM::Config::EditorSettings>();
        const auto legacy   = nlohmann::json{
            { "timelineProfessionalMode", enabled },
            { "enableDraftLanes", !enabled }
        }.get<MMM::Config::EditorSettings>();
        // 新字段必须优先于值相反的旧字段，保证升级后的显式选择稳定。
        const auto explicitSetting = nlohmann::json{
            { "professionalMode", enabled },
            { "timelineProfessionalMode", !enabled }
        }.get<MMM::Config::EditorSettings>();
        // 当前写出禁止继续产生旧字段，读取兼容只保留在迁移入口。
        if ( encoded.at("professionalMode") != enabled ||
             encoded.contains("timelineProfessionalMode") ||
             encoded.contains("enableDraftLanes") ||
             restored.professionalMode != enabled ||
             legacy.professionalMode != enabled ||
             explicitSetting.professionalMode != enabled ||
             restored.enableBmsEditing != !enabled ||
             restored.enablePolylineEditing != !enabled ) {
            XERROR(
                "Global professional mode migration or independent editing "
                "preferences failed");
            return false;
        }
    }
    // 循环之外额外比较直接构造和空 JSON 的默认状态。
    const auto defaults =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    return !MMM::Config::EditorSettings{}.professionalMode &&
           !defaults.professionalMode && defaults.enableBmsEditing &&
           defaults.enablePolylineEditing;
}

/// @brief 验证禁止垂直移动设置可持久化且旧配置保持自由拖动。
/// @return 开启状态往返不变且缺失字段默认关闭时返回 true。
/// @note 用例只覆盖偏好，不模拟物件拖拽交互。
/// @details 当前值采用 true，确保非默认值不会在写出时被忽略。
/// 恢复对象必须继续禁止垂直拖动，而空旧配置必须允许拖动。
/// 三层检查共同保护新增限制不会反向改变旧用户操作方式。
bool testVerticalObjectDragConfigRoundTrip()
{
    // true 与默认 false 相反，能发现字段写出或读取遗漏。
    MMM::Config::EditorSettings source;
    source.disableVerticalObjectDrag = true;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    // 空配置代表功能加入前允许自由垂直拖动的行为。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 写出值、恢复值和缺失默认需同时保持一致。
    if ( !encoded.value("disableVerticalObjectDrag", false) ||
         !restored.disableVerticalObjectDrag ||
         legacy.disableVerticalObjectDrag ) {
        XERROR("Vertical object drag config did not preserve compatibility");
        // 错误消息聚焦持久化，不暗示拖拽系统本身已经执行。
        return false;
    }
    return true;
}

/// @brief 验证协作视野绘制三态可持久化且旧配置保持原有填充效果。
/// @return 三种稳定文本、缺失字段和非法值均按兼容规则恢复时返回 true。
/// @note 用例不建立网络连接，模式仅作为本地视觉配置验证。
/// @details TrackEdge 走当前格式往返，Outline 走单字段读取。
/// 空配置和未知字符串都必须回退 Filled，保持远端视野明显可见。
/// 用例覆盖三个合法枚举的输出或输入语义及非法值回退。
bool testCollaborationViewportRenderModeRoundTrip()
{
    // 当前往返选用 TrackEdge，另以最小 JSON 覆盖 Outline。
    MMM::Config::EditorSettings source;
    source.collaborationViewportRenderMode =
        MMM::Config::CollaborationViewportRenderMode::TrackEdge;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           outline =
        nlohmann::json{ { "collaborationViewportRenderMode", "Outline" } }
            .get<MMM::Config::EditorSettings>();
    // 空配置和未知字符串都应回退 Filled，避免远端视野被弱化。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    const auto invalid =
        nlohmann::json{ { "collaborationViewportRenderMode", "Unknown" } }
            .get<MMM::Config::EditorSettings>();

    // 写出标识和四种读取结果共同覆盖枚举持久化状态空间。
    if ( encoded.value("collaborationViewportRenderMode", std::string()) !=
             "TrackEdge" ||
         restored.collaborationViewportRenderMode !=
             MMM::Config::CollaborationViewportRenderMode::TrackEdge ||
         outline.collaborationViewportRenderMode !=
             MMM::Config::CollaborationViewportRenderMode::Outline ||
         legacy.collaborationViewportRenderMode !=
             MMM::Config::CollaborationViewportRenderMode::Filled ||
         invalid.collaborationViewportRenderMode !=
             MMM::Config::CollaborationViewportRenderMode::Filled ) {
        XERROR(
            "Collaboration viewport render mode did not preserve "
            "compatibility");
        return false;
    }
    return true;
}

/// @brief 验证批量音量编辑快捷键可持久化且旧配置默认不占用键位。
/// @return 自定义组合键往返无损且缺失字段保持禁用时返回 true。
/// @note 同时检查全部修饰键，避免只比较主键而漏掉组合语义。
/// @details 自定义绑定启用 Ctrl+Shift，同时明确关闭 Alt 和 Super。
/// 空配置的 legacy 绑定应禁用且不保存任何主键。
/// 该用例保护新增动作不会与旧用户快捷键集合发生隐式冲突。
bool testSelectedVolumeShortcutRoundTrip()
{
    // 自定义为 Ctrl+Shift+U，与默认空绑定明显区分。
    MMM::Config::EditorSettings source;
    source.shortcutConfig.editSelectedVolume =
        MMM::Config::ShortcutBinding{ true, "U", true, true, false, false };

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    // 空配置用于验证新增动作不会占用旧用户的任何键位。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 取得引用便于逐项表达绑定契约，不复制配置对象。
    const auto& binding       = restored.shortcutConfig.editSelectedVolume;
    const auto& legacyBinding = legacy.shortcutConfig.editSelectedVolume;
    // 当前组合必须无损，旧绑定则必须禁用且主键为空。
    if ( !binding.enabled || binding.key != "U" || !binding.ctrl ||
         !binding.shift || binding.alt || binding.super ||
         legacyBinding.enabled || !legacyBinding.key.empty() ) {
        XERROR("Selected volume shortcut did not preserve compatibility");
        // 单一错误表示该动作的完整组合状态不符合预期。
        return false;
    }
    return true;
}

/// @brief 验证添加选中物件批注快捷键可持久化并迁移旧默认组合键。
/// @return 自定义组合键往返无损且缺失或旧默认字段恢复为 Ctrl+R 时返回 true。
/// @note 旧 Ctrl+Alt+A 会与新增动作冲突，因此需要专门迁移。
/// @details 当前自定义 Ctrl+Shift+N 必须原样保留，不参与迁移。
/// 缺失绑定和精确旧默认绑定都应恢复当前 Ctrl+R 默认值。
/// 对主键及四种修饰键逐项断言，防止旧 Alt 状态残留。
bool testSelectedAnnotationShortcutRoundTrip()
{
    // 自定义 Ctrl+Shift+N 不应被识别为待迁移的旧默认组合。
    MMM::Config::EditorSettings source;
    source.shortcutConfig.addSelectedAnnotation =
        MMM::Config::ShortcutBinding{ true, "N", true, true, false, false };

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 显式构造历史 Ctrl+Alt+A，覆盖冲突修复的精确识别路径。
    nlohmann::json oldDefaultJson;
    oldDefaultJson["shortcutConfig"]["addSelectedAnnotation"] =
        MMM::Config::ShortcutBinding{ true, "A", true, false, true, false };
    // 当前、缺失和旧默认三类配置分别产生独立恢复对象。
    const auto  migrated = oldDefaultJson.get<MMM::Config::EditorSettings>();
    const auto& binding  = restored.shortcutConfig.addSelectedAnnotation;
    const auto& legacyBinding   = legacy.shortcutConfig.addSelectedAnnotation;
    const auto& migratedBinding = migrated.shortcutConfig.addSelectedAnnotation;
    // 自定义值保持不变，后两种输入都应得到无 Alt 的 Ctrl+R。
    if ( !binding.enabled || binding.key != "N" || !binding.ctrl ||
         !binding.shift || binding.alt || binding.super ||
         !legacyBinding.enabled || legacyBinding.key != "R" ||
         !legacyBinding.ctrl || legacyBinding.shift || legacyBinding.alt ||
         legacyBinding.super || !migratedBinding.enabled ||
         migratedBinding.key != "R" || !migratedBinding.ctrl ||
         migratedBinding.shift || migratedBinding.alt ||
         migratedBinding.super ) {
        XERROR("Selected annotation shortcut did not preserve compatibility");
        // 修饰键逐项检查可防止迁移仅替换主键而残留 Alt。
        return false;
    }
    return true;
}

/// @brief 验证批注详情显示开关可持久化且旧配置默认关闭。
/// @return 开关往返无损且缺失字段保持关闭时返回 true。
/// @note 使用 true 作为当前值，确保默认关闭字段会被实际写入。
/// @details restored 代表用户主动开启后的当前配置恢复路径。
/// legacy 代表字段加入前的空对象，继续保持紧凑显示。
/// 两种输入共同覆盖该布尔开关的完整兼容边界。
bool testAnnotationDetailVisibilityRoundTrip()
{
    // 显式开启代表用户希望批注详情持续显示。
    MMM::Config::EditorSettings source;
    source.showAnnotationDetails = true;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    // 空旧配置仍应关闭详情，保持界面原有紧凑状态。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 往返与缺失默认分别覆盖该二值字段的两种期望状态。
    if ( !restored.showAnnotationDetails || legacy.showAnnotationDetails ) {
        XERROR("Annotation detail visibility did not preserve compatibility");
        // 不检查窗口实际内容，保持测试职责在配置层。
        return false;
    }
    return true;
}

/// @brief 验证播放切换快捷键可持久化且旧配置保持空格默认值。
/// @return 自定义组合键往返无损且缺失字段恢复为空格时返回 true。
/// @note 自定义组合包含 Ctrl 和 Alt，覆盖多个修饰键的持久化。
/// @details 当前 Ctrl+Alt+P 必须保留启用状态且不附加 Shift 或 Super。
/// 空配置应恢复无修饰 Space，维持历史播放切换习惯。
/// 用例逐项比较而不调用冲突 helper，直接验证序列化结果。
bool testPlaybackShortcutRoundTrip()
{
    // Ctrl+Alt+P 与默认无修饰 Space 足以发现字段互换或遗漏。
    MMM::Config::EditorSettings source;
    source.shortcutConfig.togglePlayback =
        MMM::Config::ShortcutBinding{ true, "P", true, false, true, false };

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    // 空配置模拟快捷键可配置功能引入前的播放默认行为。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 分别取得当前与旧绑定，逐项比较启用、主键及四个修饰键。
    const auto& binding       = restored.shortcutConfig.togglePlayback;
    const auto& legacyBinding = legacy.shortcutConfig.togglePlayback;
    // 自定义绑定必须无损，默认绑定必须保持单独空格键。
    if ( !binding.enabled || binding.key != "P" || !binding.ctrl ||
         binding.shift || !binding.alt || binding.super ||
         !legacyBinding.enabled || legacyBinding.key != "Space" ||
         legacyBinding.ctrl || legacyBinding.shift || legacyBinding.alt ||
         legacyBinding.super ) {
        XERROR("Playback shortcut did not preserve compatibility");
        // 错误只归属于配置，不触发真实播放或音频初始化。
        return false;
    }
    return true;
}

/// @brief 验证快捷键冲突仅匹配完全相同的有效按键组合。
/// @return 相同组合冲突，禁用、空键位和不同修饰键均不冲突时返回 true。
/// @note 直接测试纯值 helper，不依赖 JSON 往返。
/// @details base 与 same 提供正例，shifted 提供修饰键差异反例。
/// disabled 和 empty 提供两种不可执行绑定反例。
/// 所有比较使用同一主键，确保结果确实来自状态和修饰键规则。
bool testShortcutConflictDetection()
{
    // 使用局部别名使夹具专注于六元绑定值。
    using MMM::Config::ShortcutBinding;
    // base 与 same 完全相同，是唯一应报告冲突的一对。
    const ShortcutBinding base{ true, "P", true, false, false, false };
    const ShortcutBinding same{ true, "P", true, false, false, false };
    // shifted 只改变一个修饰键，用于验证精确组合比较。
    const ShortcutBinding shifted{ true, "P", true, true, false, false };
    // disabled 和 empty 验证不可执行绑定永远不参与冲突。
    const ShortcutBinding disabled{ false, "P", true, false, false, false };
    const ShortcutBinding empty{ true, "", true, false, false, false };

    // 四个断言覆盖相同、不同、禁用和不完整绑定。
    if ( !MMM::Config::shortcutBindingsConflict(base, same) ||
         MMM::Config::shortcutBindingsConflict(base, shifted) ||
         MMM::Config::shortcutBindingsConflict(base, disabled) ||
         MMM::Config::shortcutBindingsConflict(base, empty) ) {
        XERROR("Shortcut conflict detection did not match exact bindings");
        // 失败表示设置页冲突提示可能产生误报或漏报。
        return false;
    }
    return true;
}

/// @brief 验证布局菜单的物件与背景复位仅影响各自管理的配置。
/// @return 两组字段恢复应用默认值且背景电平图等无关字段保持不变时返回 true。
/// @note 复位边界比具体 UI 操作更重要，因此直接调用配置方法。
/// @details 第一阶段复位音符缩放、特效、标签、填充和调色方案。
/// 第二阶段复位背景图及分拍线外观，但保留背景频谱参数。
/// 两阶段互相保留对方字段，验证配置方法的职责隔离。
bool testRenderingDefaultsReset()
{
    // 先把物件渲染字段设为明显非默认值。
    MMM::Config::EditorConfig config;
    config.visual.noteScaleX               = 2.4F;
    config.visual.noteScaleY               = 0.7F;
    config.visual.nonHoldHitEffectDuration = 0.76F;
    config.visual.showBoundSampleLabels    = false;
    config.visual.noteFillMode = MMM::Config::BackgroundFillMode::Center;
    // 调色方案名称属于物件渲染复位范围，需要随缩放和填充共同恢复。
    config.settings.defaultColorPaletteSchemeName = "Custom";
    // 背景字段也设为非默认，用于证明物件复位不会越界修改背景。
    config.visual.background.fillMode =
        MMM::Config::BackgroundFillMode::Stretch;
    config.visual.background.opaque_ratio            = 0.2F;
    config.visual.background.darken_ratio            = 0.1F;
    config.visual.beatLineAlpha                      = 0.2F;
    config.visual.hoverSubdivisionLineExtensionRatio = 0.9F;
    // 背景频谱参数不属于背景静态图复位范围，应在两轮操作后继续保留。
    config.visual.background.spectrum.bandCount = 64;
    config.visual.background.spectrum.opacity   = 0.8F;

    // 第一轮只执行物件渲染复位，并以全新默认配置为基准比较。
    config.resetNoteRenderingToDefaults();
    const MMM::Config::EditorConfig defaults;
    // 被管理字段必须恢复，背景 fillMode 必须仍保留用户设置。
    if ( !near(config.visual.noteScaleX, defaults.visual.noteScaleX) ||
         !near(config.visual.noteScaleY, defaults.visual.noteScaleY) ||
         !near(config.visual.nonHoldHitEffectDuration,
               defaults.visual.nonHoldHitEffectDuration) ||
         config.visual.showBoundSampleLabels !=
             defaults.visual.showBoundSampleLabels ||
         config.visual.noteFillMode != defaults.visual.noteFillMode ||
         config.settings.defaultColorPaletteSchemeName !=
             defaults.settings.defaultColorPaletteSchemeName ||
         config.visual.background.fillMode !=
             MMM::Config::BackgroundFillMode::Stretch ) {
        XERROR("Note rendering reset escaped its configuration boundary");
        // 失败说明复位遗漏或越过职责边界。
        return false;
    }

    // 第二轮恢复背景图与分拍线外观，但仍不应重置频谱分析参数。
    config.resetBackgroundRenderingToDefaults();
    // 静态背景字段恢复默认，频段数量与透明度继续保持自定义值。
    if ( config.visual.background.fillMode !=
             defaults.visual.background.fillMode ||
         !near(config.visual.background.opaque_ratio,
               defaults.visual.background.opaque_ratio) ||
         !near(config.visual.background.darken_ratio,
               defaults.visual.background.darken_ratio) ||
         !near(config.visual.beatLineAlpha, defaults.visual.beatLineAlpha) ||
         !near(config.visual.hoverSubdivisionLineExtensionRatio,
               defaults.visual.hoverSubdivisionLineExtensionRatio) ||
         config.visual.background.spectrum.bandCount != 64 ||
         !near(config.visual.background.spectrum.opacity, 0.8F) ) {
        XERROR("Background rendering reset escaped its configuration boundary");
        // 该断言保护设置菜单两个复位按钮的独立职责。
        return false;
    }
    return true;
}

/// @brief 验证背景频谱配置能够完整往返。
/// @return 当前格式往返无损时返回 true。
/// @note 同时覆盖频谱分析字段和迁移后的画布组件布局字段。
/// @details 频段、几何、透明度、双声道颜色和输入来源均使用非默认值。
/// 组件显隐、锚点和尺寸也独立设置，覆盖新权威布局来源。
/// 恢复后同时验证旧 enabled 镜像与新组件 visible 保持一致。
bool testBackgroundSpectrumRoundTrip()
{
    // 通过引用分别配置频谱数据和作为权威显隐来源的组件布局。
    MMM::Config::VisualConfig source;
    auto&                     spectrum = source.background.spectrum;
    auto& placement = source.canvasComponents.backgroundSpectrum;
    // 使用非默认锚点与尺寸，确认组件布局不会被旧字段迁移覆盖。
    placement.visible       = true;
    placement.anchorX       = 0.37F;
    placement.anchorY       = 0.42F;
    placement.fontSizeRatio = 0.08F;
    // 频段和比例均选择合法非默认值，便于识别字段错位。
    spectrum.bandCount     = 48;
    spectrum.widthRatio    = 0.72F;
    spectrum.heightRatio   = 0.44F;
    spectrum.baselineRatio = 0.83F;
    spectrum.opacity       = 0.27F;
    // 左右颜色采用不同序列，覆盖四通道及声道映射。
    spectrum.leftBarColor      = { 0.12F, 0.24F, 0.36F, 0.48F };
    spectrum.rightBarColor     = { 0.51F, 0.62F, 0.73F, 0.84F };
    spectrum.includeHitEffects = true;

    // 当前格式写出会把组件 visible 同步到旧 background.spectrum.enabled。
    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto&          result   = restored.background.spectrum;
    const auto& resultPlacement = restored.canvasComponents.backgroundSpectrum;
    // 分析字段、颜色端点和组件布局一次性验证完整往返。
    if ( !result.enabled || result.bandCount != 48 ||
         !near(result.widthRatio, 0.72F) || !near(result.heightRatio, 0.44F) ||
         !near(result.baselineRatio, 0.83F) || !near(result.opacity, 0.27F) ||
         !near(result.leftBarColor[0], 0.12F) ||
         !near(result.leftBarColor[3], 0.48F) ||
         !near(result.rightBarColor[0], 0.51F) ||
         !near(result.rightBarColor[3], 0.84F) || !result.includeHitEffects ||
         !resultPlacement.visible || !near(resultPlacement.anchorX, 0.37F) ||
         !near(resultPlacement.anchorY, 0.42F) ||
         !near(resultPlacement.fontSizeRatio, 0.08F) ) {
        XERROR("Background spectrum config did not survive JSON round trip");
        // 失败可能来自新权威字段或旧兼容镜像，统一归入该配置组。
        return false;
    }
    return true;
}

/// @brief 验证旧版背景频谱显隐和底边位置迁移到画布组件布局。
/// @return 旧字段生成可见组件且保持原始垂直位置时返回 true。
/// @note 旧基线减去半高应得到新组件中心锚点。
/// @details JSON 刻意不含 canvasComponents.backgroundSpectrum 新字段。
/// enabled=true 应迁移为组件 visible，并继续同步回旧内存字段。
/// includeHitEffects 缺失时保持默认关闭，不受显隐迁移影响。
bool testLegacyBackgroundSpectrumMigration()
{
    // 最小旧格式只提供 enabled、高度和底边基线三个迁移输入。
    const nlohmann::json json{
        { "background",
          { { "spectrum",
              { { "enabled", true },
                { "heightRatio", 0.4F },
                { "baselineRatio", 0.9F } } } } },
    };
    // 不提供 canvasComponents.backgroundSpectrum，确保触发旧迁移分支。
    const auto  config    = json.get<MMM::Config::VisualConfig>();
    const auto& placement = config.canvasComponents.backgroundSpectrum;
    // 新旧显隐必须一致，中心锚点为 0.9 - 0.4 / 2 = 0.7。
    if ( !placement.visible || !config.background.spectrum.enabled ||
         config.background.spectrum.includeHitEffects ||
         !near(placement.anchorY, 0.7F) ) {
        XERROR(
            "Legacy background spectrum migration did not use safe defaults");
        return false;
    }
    return true;
}

/// @brief 验证背景频谱配置在读取时被限制到设置菜单允许范围。
/// @return 所有越界字段均被正确限制时返回 true。
/// @note 左右颜色覆盖高低越界和合法中间值。
/// @details 频段数量低于最小值，几何和透明度覆盖两端越界。
/// 每个颜色数组同时包含合法值和需夹取的负值、超一值。
/// 断言逐通道展开，能定位左右声道或分量处理错位。
bool testBackgroundSpectrumClamping()
{
    // 直接构造越界 JSON，避免生产序列化掩盖读取端净化缺陷。
    const nlohmann::json json{
        { "background",
          { { "spectrum",
              { { "bandCount", 2 },
                { "widthRatio", 0.0F },
                { "heightRatio", 2.0F },
                { "baselineRatio", -1.0F },
                { "opacity", 3.0F },
                // 两组颜色交错越界方向，验证逐分量独立夹取。
                { "leftBarColor", { -1.0F, 0.25F, 2.0F, 0.75F } },
                { "rightBarColor", { 1.5F, -0.5F, 0.5F, 2.0F } } } } } }
    };
    // 通过完整 VisualConfig 路径恢复，覆盖 background 子对象组合。
    const auto  config   = json.get<MMM::Config::VisualConfig>();
    const auto& spectrum = config.background.spectrum;
    // 数量、几何、透明度和八个颜色通道都必须落到各自边界。
    if ( spectrum.bandCount != MMM::Config::BACKGROUND_SPECTRUM_MIN_BANDS ||
         !near(spectrum.widthRatio, 0.10F) ||
         !near(spectrum.heightRatio, 1.0F) ||
         !near(spectrum.baselineRatio, 0.05F) ||
         !near(spectrum.opacity, 1.0F) ||
         !near(spectrum.leftBarColor[0], 0.0F) ||
         !near(spectrum.leftBarColor[1], 0.25F) ||
         !near(spectrum.leftBarColor[2], 1.0F) ||
         !near(spectrum.leftBarColor[3], 0.75F) ||
         !near(spectrum.rightBarColor[0], 1.0F) ||
         !near(spectrum.rightBarColor[1], 0.0F) ||
         !near(spectrum.rightBarColor[2], 0.5F) ||
         !near(spectrum.rightBarColor[3], 1.0F) ) {
        XERROR("Background spectrum config escaped supported bounds");
        // 断言使用公开最小频段常量，避免实现和测试数值漂移。
        return false;
    }
    return true;
}

/// @brief 验证不同 Key 数的轨道、判定线与组件布局独立保存。
/// @return 独立编辑、旧配置继承和 JSON 往返均正确时返回 true。
/// @note 同时验证辅助区域可选覆盖、应用快照和损坏字段回退。
/// @details 四键和七键使用互异布局，五键查询用于验证根级回退。
/// 当前格式往返后再物化七键快照，检查三个根级字段整体更新。
/// 旧格式与损坏辅助区输入分别覆盖迁移和逐字段拒绝规则。
bool testKeyCountLayoutIsolationAndMigration()
{
    // 根级布局作为未定制键数和新专用布局的继承模板。
    MMM::Config::VisualConfig source;
    source.trackLayout.left                    = 0.11F;
    source.judgeline_pos                       = 0.81F;
    source.canvasComponents.beatNumber.anchorX = 0.13F;

    // 四键布局修改主轨道及三类辅助区域，形成完整定制样本。
    auto& fourTrackLayout = source.editableTrackLayoutForKeyCount(4);
    fourTrackLayout.left  = 0.21F;
    fourTrackLayout.right = 0.61F;
    // 三类辅助区域均应按 Key 数独立保存横向覆盖值。
    fourTrackLayout.draftLanes.right = 0.18F;
    fourTrackLayout.draftLanes.width = 0.07F;
    fourTrackLayout.annotation.left  = 0.64F;
    fourTrackLayout.annotation.width = 0.045F;
    fourTrackLayout.bgmLanes.left    = 0.72F;
    fourTrackLayout.bgmLanes.width   = 0.09F;
    // 判定线和组件布局通过各自可编辑入口写入同一四键快照。
    source.editableJudgmentLinePositionForKeyCount(4) = 0.74F;
    auto& fourComponents = source.editableCanvasComponentsForKeyCount(4);
    fourComponents.beatNumber.visible = true;
    fourComponents.beatNumber.anchorX = 0.24F;

    // 七键布局采用不同边界且不设置辅助区，用于检查键数隔离。
    auto& sevenTrackLayout = source.editableTrackLayoutForKeyCount(7);
    sevenTrackLayout.left  = 0.31F;
    sevenTrackLayout.right = 0.91F;
    source.editableJudgmentLinePositionForKeyCount(7) = 0.88F;
    auto& sevenComponents = source.editableCanvasComponentsForKeyCount(7);
    sevenComponents.beatNumber.visible = false;
    sevenComponents.beatNumber.anchorX = 0.67F;

    // 往返后分别查询四键、七键和未定制五键三种解析路径。
    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto&          restoredFourTrack = restored.trackLayoutForKeyCount(4);
    const auto& restoredSevenTrack         = restored.trackLayoutForKeyCount(7);
    const auto& restoredLegacyTrack        = restored.trackLayoutForKeyCount(5);
    const auto& restoredFourComponents =
        restored.canvasComponentsForKeyCount(4);
    const auto& restoredSevenComponents =
        restored.canvasComponentsForKeyCount(7);
    const auto& restoredLegacyComponents =
        restored.canvasComponentsForKeyCount(5);

    // applyKeyCountLayout 应把七键快照整体复制到根级兼容字段。
    auto materialized = restored;
    materialized.applyKeyCountLayout(7);
    // 旧格式没有 keyCountLayouts，所有键数查询必须回退根级布局。
    const auto legacy =
        nlohmann::json{
            { "trackLayout",
              { { "left", 0.17F },
                { "top", 0.05F },
                { "right", 0.77F },
                { "bottom", 0.95F } } },
            { "judgeline_pos", 0.79F },
            { "canvasComponents",
              { { "beatNumber", { { "anchorX", 0.29F } } } } },
        }
            .get<MMM::Config::VisualConfig>();
    // 损坏辅助区覆盖覆盖类型错误、数组、空值和非正宽度。
    const auto malformed =
        nlohmann::json{
            { "draftLanes",
              { { "left", "bad" }, { "right", "bad" }, { "width", -0.1F } } },
            { "annotation", nlohmann::json::array() },
            { "bgmLanes", { { "left", nullptr }, { "width", 0.0F } } },
        }
            .get<MMM::Config::TrackLayout>();

    // 首段验证集合数量、四键主轨道以及每类辅助覆盖的精确值。
    if ( source.keyCountLayouts.size() != 2U ||
         restored.keyCountLayouts.size() != 2U ||
         !near(restoredFourTrack.left, 0.21F) ||
         !near(restoredFourTrack.right, 0.61F) ||
         restoredFourTrack.draftLanes.left ||
         !restoredFourTrack.draftLanes.right ||
         !near(*restoredFourTrack.draftLanes.right, 0.18F) ||
         !restoredFourTrack.draftLanes.width ||
         !near(*restoredFourTrack.draftLanes.width, 0.07F) ||
         !restoredFourTrack.annotation.left ||
         !near(*restoredFourTrack.annotation.left, 0.64F) ||
         !restoredFourTrack.annotation.width ||
         !near(*restoredFourTrack.annotation.width, 0.045F) ||
         !restoredFourTrack.bgmLanes.left ||
         !near(*restoredFourTrack.bgmLanes.left, 0.72F) ||
         !restoredFourTrack.bgmLanes.width ||
         !near(*restoredFourTrack.bgmLanes.width, 0.09F) ||
         // 七键未定制辅助区必须保持空，不能继承四键覆盖。
         restoredSevenTrack.draftLanes.left ||
         restoredSevenTrack.draftLanes.right ||
         restoredSevenTrack.annotation.width ||
         restoredSevenTrack.bgmLanes.left ||
         !near(restoredSevenTrack.left, 0.31F) ||
         !near(restoredSevenTrack.right, 0.91F) ||
         !near(restoredLegacyTrack.left, 0.11F) ||
         // 三种键数的判定线分别来自四键、七键和根级布局。
         !near(restored.judgmentLinePositionForKeyCount(4), 0.74F) ||
         !near(restored.judgmentLinePositionForKeyCount(7), 0.88F) ||
         !near(restored.judgmentLinePositionForKeyCount(5), 0.81F) ||
         // 组件显隐和锚点也必须按键数隔离并为五键继承根值。
         !restoredFourComponents.beatNumber.visible ||
         !near(restoredFourComponents.beatNumber.anchorX, 0.24F) ||
         restoredSevenComponents.beatNumber.visible ||
         !near(restoredSevenComponents.beatNumber.anchorX, 0.67F) ||
         !near(restoredLegacyComponents.beatNumber.anchorX, 0.13F) ||
         // 物化后的三个根级字段应全部来自同一七键快照。
         !near(materialized.trackLayout.left, 0.31F) ||
         !near(materialized.judgeline_pos, 0.88F) ||
         !near(materialized.canvasComponents.beatNumber.anchorX, 0.67F) ||
         // 旧配置不凭空创建专用布局，其辅助覆盖也保持全部为空。
         !legacy.keyCountLayouts.empty() ||
         legacy.trackLayout.draftLanes.left ||
         legacy.trackLayout.draftLanes.right ||
         legacy.trackLayout.draftLanes.width ||
         legacy.trackLayout.annotation.left ||
         legacy.trackLayout.annotation.width ||
         legacy.trackLayout.bgmLanes.left ||
         // 非法可选字段统一回退空值，不保留无法渲染的半有效状态。
         legacy.trackLayout.bgmLanes.width || malformed.draftLanes.left ||
         malformed.draftLanes.right || malformed.draftLanes.width ||
         malformed.annotation.left || malformed.annotation.width ||
         malformed.bgmLanes.left || malformed.bgmLanes.width ||
         // 任意键数查询旧配置都应返回根轨道、判定线和组件布局。
         !near(legacy.trackLayoutForKeyCount(4).left, 0.17F) ||
         !near(legacy.judgmentLinePositionForKeyCount(7), 0.79F) ||
         !near(legacy.canvasComponentsForKeyCount(9).beatNumber.anchorX,
               0.29F) ) {
        XERROR("Key-count layouts were shared or legacy migration failed");
        // 聚合错误覆盖同一布局隔离职责，具体条件可由调试器快速定位。
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行视觉配置兼容性与默认值测试。
/// @return 全部测试通过时返回 0。
/// @note 采用短路组合，首个失败用例已负责输出具体日志。
/// @details 测试全部使用内存 JSON，不访问个人配置、项目或资源目录。
/// 每个子用例负责输出自身错误，main 仅汇总为进程退出码。
/// 执行顺序按基础字段、编辑设置、复位、频谱和布局复杂度排列。
int main()
{
    // 用例顺序先覆盖视觉字段，再覆盖 EditorSettings 和复杂布局集合。
    return testBeatLineDisplayModeRoundTrip() &&
                   testLegacyDrawBeatLinesMigration() &&
                   testBeatLineAutoRatioClamping() &&
                   testHoverSubdivisionLineExtensionRatioConfig() &&
                   testPreviewAreaLineDefaults() &&
                   testBoundSampleLabelConfigRoundTrip() &&
                   testInteractionHitboxScaleConfig() &&
                   testNonHoldHitEffectDurationConfig() &&
                   testPolylineEditingConfigRoundTrip() &&
                   testToolbarValueWheelAdjustmentRoundTrip() &&
                   testBmsEditingConfigRoundTrip() &&
                   testProfessionalModeConfigMigration() &&
                   testVerticalObjectDragConfigRoundTrip() &&
                   testCollaborationViewportRenderModeRoundTrip() &&
                   testSelectedVolumeShortcutRoundTrip() &&
                   testSelectedAnnotationShortcutRoundTrip() &&
                   testAnnotationDetailVisibilityRoundTrip() &&
                   testPlaybackShortcutRoundTrip() &&
                   testShortcutConflictDetection() &&
                   // 复位与背景频谱测试在基础字段兼容确认后执行。
                   testRenderingDefaultsReset() &&
                   testBackgroundSpectrumRoundTrip() &&
                   testLegacyBackgroundSpectrumMigration() &&
                   testBackgroundSpectrumClamping() &&
                   // Key 数隔离是最复杂场景，最后执行便于定位前置失败。
                   testKeyCountLayoutIsolationAndMigration()
               ? 0
               : 1;
}
