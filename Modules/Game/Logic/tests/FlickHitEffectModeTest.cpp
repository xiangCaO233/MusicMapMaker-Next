#include "logic/ecs/system/HitFXSystem.h"

#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/VisualConfig.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace
{
/// @brief 测试直接驱动生产事件与 CPU 快照，不开启声卡或 Vulkan 设备。
using System = MMM::Logic::System::HitFXSystem;
/// @brief 三种覆盖模式使用生产枚举，不能用测试自定义数值替代。
using Mode = MMM::Config::FlickHitEffectMode;
/// @brief 保留完整角色集合，内部判定不能误用“非尾部”。
using Role = System::HitEvent::Role;

/// @brief 检查三种模式往返、旧配置默认值以及旧折线策略的退场。
/// @return 所有新字段独立保存且旧策略不再写出时为真。
/// @note 直接调用生产 ADL 序列化，不能只检查新建对象的成员默认值。
/// 缺失字段与非法枚举分别测试，二者采用同一个兼容回退但走不同入口。
/// 旧策略字符串仅作为历史输入，不要求新客户端保留被移除的类型。
/// 视觉与声音分别存储在 VisualConfig 与 SfxConfig，不能共享一个布尔字段。
bool testPersistence()
{
    // 非默认模式与关闭状态都必须往返，避免默认值掩盖序列化遗漏。
    for ( const auto mode :
          { Mode::HeadOnly, Mode::TailOnly, Mode::HeadToTail } ) {
        MMM::Config::VisualConfig visual;
        visual.flickHitEffectMode                 = mode;
        visual.enablePolylineInternalFlickEffects = false;
        const nlohmann::json encoded              = visual;
        // 读取完整视觉对象，覆盖字段名接入，不能只验证孤立枚举转换。
        const auto restored = encoded.get<MMM::Config::VisualConfig>();
        if ( restored.flickHitEffectMode != mode ||
             restored.enablePolylineInternalFlickEffects )
            return false;
    }
    // 旧配置缺失新字段，默认保留尾部特效与内部滑键声音、动画。
    // 空 JSON 对象与默认构造对象不同，必须实际经过读取入口。
    // 首部和全范围只由用户显式选择启用，不在升级时自动切换。
    const auto defaults =
        nlohmann::json::object().get<MMM::Config::VisualConfig>();
    if ( defaults.flickHitEffectMode != Mode::TailOnly ||
         !defaults.enablePolylineInternalFlickEffects )
        return false;
    // 枚举读取拒绝错误类型、未知字符串；不得依靠抛出异常恢复。
    for ( const nlohmann::json bad : { nlohmann::json(nullptr),
                                       nlohmann::json(42),
                                       nlohmann::json("future") } ) {
        // 这些输入不能产生无效枚举索引，也不能沿用上次解析的模式。
        if ( bad.get<Mode>() != Mode::TailOnly ) return false;
    }
    // 老策略不迁移成静音，读取后各物件恢复其实际类型的声音。
    // 四个旧值全部覆盖，防止只有 Exact 兼容而其他历史值仍残留分支。
    for ( const char* legacy :
          { "Exact", "InternalAsNormal", "OnlyTailExact", "AllAsNormal" } ) {
        const nlohmann::json old{ { "polylineStrategy", legacy } };
        auto                 sfx = old.get<MMM::Config::SfxConfig>();
        if ( !sfx.enablePolylineInternalFlickSfx ) return false;
        sfx.enablePolylineInternalFlickSfx = false;
        const nlohmann::json encoded       = sfx;
        // 再保存时删除废弃策略，声音开关不借用视觉配置存储。
        // contains 检查写出键集合，读回 false 检查新字段的实际值。
        // 两个断言不能合成一次默认值比较，否则可能漏掉无效持久化。
        if ( encoded.contains("polylineStrategy") ||
             encoded.get<MMM::Config::SfxConfig>()
                 .enablePolylineInternalFlickSfx )
            return false;
    }
    return true;
}

/// @brief 创建固定时间事件，轨道和角色由各场景自行覆盖。
/// @note 不绑定采样，所有纹理 UV 均由测试提供，不依赖 GPU 图集。
/// @return 零秒命中的玩家滑键，包含三个轨道并向右滑动。
/// duration 为复用到 Hold 用例准备，滑键自身仍使用非 Hold 视觉寿命。
/// role 与 isSubNote 必须成对覆盖，单独修改角色不能伪造折线节点。
System::HitEvent makeEvent()
{
    // 初始化所有必需字段，默认独立滑键从零号轨滑至二号轨。
    // 每个调用返回值对象，修改角色不会污染随后创建的用例。
    return { 0.0, MMM::NoteType::FLICK, Role::None, 3, 0, 2, 1.0, false };
}

/// @brief 只为期望类型的首帧注册 UV，选错序列时不能生成几何。
/// @param snapshot 当前用例独享的快照，不复用上一轮的纹理表。
/// @param type 期望的原始物件类型；内部滑键不能降级为单键序列。
/// @note 观察时间固定为零，不要求为后续动画帧注册额外纹理。
/// 合成正方形 UV 只提供宽高比，不模拟真实图像上传或采样结果。
void fillUvs(MMM::Logic::RenderSnapshot& snapshot,
             MMM::NoteType               type = MMM::NoteType::FLICK)
{
    auto&       skin = MMM::Config::SkinManager::instance();
    const char* key  = type == MMM::NoteType::FLICK  ? "note.effect.flick"
                       : type == MMM::NoteType::HOLD ? "note.effect.hold"
                                                     : "note.effect.note";
    const auto* seq  = skin.getEffectSequence(key);
    // 空序列不补造 ID，后续非空几何断言将明确失败。
    if ( seq && !seq->frames.empty() )
        snapshot.uvMap[seq->startId] = { 0, 0, 1, 1 };
}

/// @brief 单个覆盖用例的输入和手工期望端点。
struct LaneCase {
    /// @brief 玩家区或草稿区的原始起始轨道。
    int head;
    /// @brief 有符号滑动轨差，不以跨度绝对值代替。
    int offset;
    /// @brief 草稿区使用负索引并独立生成快照。
    bool draft;
    /// @brief 钳位后的期望头部局部轨道。
    int expectedHead;
    /// @brief 钳位后的期望尾部局部轨道。
    int expectedTail;
};

/// @brief 验证左右方向、零跨度、边界钳位和草稿区的真实快照位置。
/// @note 在同一个活动实例上切换模式，确认无需重放事件也能刷新范围。
/// @return 顶点数量、中心位置及画布隔离全部正确时为真。
/// 极端轨差只考察范围计算安全性，不代表允许创建越界谱面物件。
/// 草稿用负绝对索引表达，期望坐标仍为对应画布内部的零基索引。
/// 左向滑键的绘制顺序可以反转，覆盖集合必须与头尾闭区间一致。
bool testLaneModes()
{
    constexpr std::array<LaneCase, 7> cases{
        { { 0, 2, false, 0, 2 },
          { 3, -2, false, 3, 1 },
          { 2, 0, false, 2, 2 },
          { -4, 2, true, 0, 2 },
          { -1, -2, true, 3, 1 },
          { 3, std::numeric_limits<int>::max(), false, 3, 3 },
          { 1, std::numeric_limits<int>::min(), false, 1, 0 } }
    };
    for ( const auto& lane : cases ) {
        // 每组仅触发一次，再在同一个活动实例上依次应用三种视觉设置。
        // 若快照生成错误地清空活动状态，后两个模式会缺少顶点而失败。
        MMM::Config::EditorConfig config;
        // 极端轨差只检查视觉，避免把非法输入引入不相关 KPS 统计。
        config.visual.canvasComponents.kps.visible = false;
        auto event                                 = makeEvent();
        event.trackIndex                           = lane.head;
        event.trackOffset                          = lane.offset;
        event.isDraft                              = lane.draft;
        System system;
        system.update(0, { event }, 4, config);
        // 观察时间和触发时间相同，不能让寿命过期导致缺少几何的假阳性。
        for ( const auto mode :
              { Mode::HeadOnly, Mode::TailOnly, Mode::HeadToTail } ) {
            config.visual.flickHitEffectMode = mode;
            MMM::Logic::RenderSnapshot snapshot;
            fillUvs(snapshot);
            MMM::Logic::System::Batcher batcher(&snapshot);
            system.generateSnapshot(
                batcher, 0, config, 4, 300, 10, 0, 600, 100, lane.draft);
            // 期望范围在测试内明确给出，实际位置必须来自生产生成的四顶点。
            const int first =
                mode == Mode::HeadOnly ? lane.expectedHead
                : mode == Mode::TailOnly
                    ? lane.expectedTail
                    : std::min(lane.expectedHead, lane.expectedTail);
            const int last =
                mode == Mode::HeadToTail
                    ? std::max(lane.expectedHead, lane.expectedTail)
                    : first;
            // 先验证数量再取顶点，失败路径本身不能发生越界读取。
            // 每轨四顶点也能识别将整个跨度拉成一张宽纹理的错误实现。
            if ( snapshot.vertices.size() !=
                 static_cast<std::size_t>((last - first + 1) * 4) )
                return false;
            for ( int track = first; track <= last; ++track ) {
                float center = 0;
                // 取四顶点均值，兼容皮肤的独立倍率和固定/整轨两种布局。
                // 非零 leftX 验证坐标平移；只比较相对间距会遗漏整体偏移。
                // 容差只覆盖浮点缩放误差，远小于一个轨道的宽度。
                for ( int vertex = 0; vertex < 4; ++vertex )
                    center +=
                        snapshot.vertices[(track - first) * 4 + vertex].pos.x *
                        0.25f;
                if ( std::abs(center - (10 + (track + 0.5f) * 100)) > 0.001f )
                    return false;
            }
            // 同一事件不能泄漏到另一画布区域，即使局部轨道数相同。
            // 对照区域也提供有效纹理，空结果不能归因于 UV 缺失。
            // 使用独立快照，保证当前区域的旧顶点不会残留在对照结果里。
            MMM::Logic::RenderSnapshot other;
            fillUvs(other);
            MMM::Logic::System::Batcher otherBatcher(&other);
            system.generateSnapshot(
                otherBatcher, 0, config, 4, 300, 10, 0, 600, 100, !lane.draft);
            if ( !other.vertices.empty() ) return false;
        }
    }
    return true;
}

/// @brief 穷举身份、角色、类型和两个独立开关，严格保护折线头尾节点。
/// @note 声音验证生产预调度过滤器，不启动实际音频设备或模拟听感。
/// @return 仅严格内部滑键被关闭，其他组合全部保留时为真。
/// 独立物件故意搭配所有角色，验证只看 Role 而漏查 isSubNote 的错误实现。
/// 声音和视觉取值遍历笛卡尔积，尤其覆盖一开一关的交叉组合。
/// NOTE 和 HOLD 是反例，防止把“内部滑键”粗化成“所有内部节点”。
/// 活动实例二次生成快照时不再 update，专门验证实时隐藏路径。
bool testInternalSwitches()
{
    // 不复用可变事件，所有开关组合都从同一夹具重新构造。
    // 首尾反例与独立滑键反例同等重要，任意一个被抑制均属失败。
    for ( const bool sub : { false, true } )
        for ( const auto role :
              { Role::None, Role::Head, Role::Internal, Role::Tail } )
            for ( const auto type : { MMM::NoteType::NOTE,
                                      MMM::NoteType::FLICK,
                                      MMM::NoteType::HOLD } )
                for ( const bool sound : { false, true } )
                    for ( const bool effect : { false, true } ) {
                        auto event      = makeEvent();
                        event.isSubNote = sub;
                        event.role      = role;
                        event.type      = type;
                        // 三条件必须同时满足，None、Head、Tail
                        // 都不是严格内部节点。
                        const bool internal = sub && role == Role::Internal &&
                                              type == MMM::NoteType::FLICK;
                        MMM::Config::EditorConfig config;
                        config.settings.sfxConfig
                            .enablePolylineInternalFlickSfx = sound;
                        config.visual.enablePolylineInternalFlickEffects =
                            effect;
                        config.visual.flickHitEffectMode = Mode::HeadToTail;
                        // 分类函数是两个开关共用的契约；声音只受声音布尔值约束。
                        // 不检查声卡通道数量，避免设备初始化影响过滤测试。
                        if ( System::isPolylineInternalFlick(event) !=
                                 internal ||
                             System::shouldScheduleHitAudio(
                                 event, config.settings.sfxConfig) !=
                                 (!internal || sound) )
                            return false;
                        // 独立实例避免同轨道替换掩盖开关对不同角色的影响。
                        // 使用完整 update 而非直接写活动表，覆盖触发入口过滤。
                        System system;
                        system.update(0, { event }, 4, config);
                        MMM::Logic::RenderSnapshot snapshot;
                        fillUvs(snapshot, type);
                        MMM::Logic::System::Batcher batcher(&snapshot);
                        system.generateSnapshot(
                            batcher, 0, config, 4, 300, 0, 0, 600, 100);
                        // 视觉开启时，滑键三个轨道各一份，普通键与 Hold
                        // 始终只画一份。
                        const std::size_t expected =
                            internal && !effect            ? 0U
                            : type == MMM::NoteType::FLICK ? 12U
                                                           : 4U;
                        // 仅注册对应类型纹理，因此也排除旧普通特效策略残留。
                        // 音效关闭不能减少顶点，特效关闭不能改变音效过滤结果。
                        if ( snapshot.vertices.size() != expected )
                            return false;
                        // 已触发实例也必须响应实时关闭，不能等待旧特效寿命结束。
                        // 原本关闭的内部滑键仍应保持空结果，不因二次生成而复活。
                        // 首尾节点必须继续可见，不能用清空全部活动表代替角色过滤。
                        config.visual.enablePolylineInternalFlickEffects =
                            false;
                        MMM::Logic::RenderSnapshot hidden;
                        fillUvs(hidden, type);
                        MMM::Logic::System::Batcher hiddenBatcher(&hidden);
                        system.generateSnapshot(
                            hiddenBatcher, 0, config, 4, 300, 0, 0, 600, 100);
                        if ( hidden.vertices.size() !=
                             (internal ? 0U : expected) )
                            return false;
                    }
    return true;
}
}  // namespace

/// @brief 从真实皮肤加载配置后验证纯 CPU 播放逻辑。
/// @param argv 皮肤根和翻译根；不读取用户当前皮肤或个人配置。
/// @return 任一持久化、覆盖范围或内部开关断言失败时返回非零。
/// @note 内置资源路径由 CTest 提供，缺参数明确失败而不猜测工作目录。
/// 测试不写源码资源目录，不生成图片，不改变用户皮肤配置文件。
/// 每次加载皮肤后都重新查询序列，不跨加载保留帧指针或纹理 ID。
int main(int argc, char* argv[])
{
    if ( argc != 3 || !testPersistence() ) return 1;
    // 短路失败不会把尚未执行的其他皮肤算成通过；日志给出失败皮肤名称。
    // 四套皮肤包含不同混合与缩放设置，覆盖模式不能依赖单一纹理尺寸。
    // 资源只用于序列分配，真实 GPU 合成与音频听感仍需交互验收。
    for ( const char* name : { "mmm-default", "ivm", "rm", "rm-old" } ) {
        auto&             skin = MMM::Config::SkinManager::instance();
        const std::string path =
            std::string(argv[1]) + "/" + name + "/skin.lua";
        if ( !skin.loadSkin(path, argv[2]) || !testLaneModes() ||
             !testInternalSwitches() ) {
            XERROR(
                "Flick effect mode or strict internal switch test failed: {}",
                name);
            return 1;
        }
    }
    return 0;
}
