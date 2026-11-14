#include "common/LogicCommands.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/ActionController.h"
#include "logic/session/context/SessionContext.h"
#include <initializer_list>
#include <string>

namespace
{
/// @brief 比较公开剪贴板及来源身份，不读取或改写操作系统的真实剪贴板。
/// @details 非空快照来自正式剪切命令，空操作必须连来源和剪切属性一起保留。
/// 两个独立 Registry 中的实体可有相同数值，数量与内容均分别检查。
bool retainsMixedClipboard(MMM::Logic::EditorEngine&         engine,
                           const MMM::Logic::SessionContext& source)
{
    // 公开接口按值返回独立副本，比较不能修改待保护的全局载荷。
    // 检查不同种类的特征字段，避免仅凭总数掩盖 Note／Sample 互相覆盖。
    const auto notes   = engine.getClipboard(&source);
    const auto samples = engine.getSampleClipboard(&source);
    // 数量检查先于 front，错误实现应报告回归失败而不是越界终止测试。
    // 来源的 Cut 属性也要保留，旧载荷仍有后续粘贴时消费源物件的职责。
    return notes.size() == 1 && samples.size() == 1 &&
           notes.front().note.m_timestamp == 2 &&
           samples.front().sample.m_audioResourceId == "clipboard-test.wav" &&
           engine.isClipboardCutFrom(&source);
}
}  // namespace

/// @brief 空复制和空剪切不覆盖内存载荷、系统导出槽及状态提示。
/// @details 通过正式 ActionController 建立和操作剪贴板，不直接调用待测守卫。
/// 文件名仅作资源身份，不加载音频、不初始化图形上下文。
/// @return 非空复制仍可替换，空操作完全无副作用时返回零。
int main()
{
    using namespace MMM::Logic;
    SessionContext source;
    // 四条玩家轨使采样的绝对轨道 4 对应首个 BGM lane。
    // 不创建 BeatMap 或音频描述符，复制应只依赖现有领域组件。
    source.trackCount = 4;
    const auto note   = source.noteRegistry.create();
    // 选择通过正式组件表达，不能只填一个可能与注册表脱节的实体集合。
    source.noteRegistry.emplace<NoteComponent>(
        note, NoteComponent{ .m_timestamp = 2, .m_trackIndex = 1 });
    source.noteRegistry.emplace<InteractionComponent>(
        note, InteractionComponent{ .isSelected = true });
    const auto sample = source.sampleRegistry.create();
    // 两个注册表各自产生身份，测试不假设实体整数在两个领域间全局唯一。
    // 资源字符串无需存在于磁盘，复制只保留引用而不加载资源。
    source.sampleRegistry.emplace<SampleComponent>(
        sample,
        SampleComponent{ .m_timestamp       = 3,
                         .m_track           = 4,
                         .m_audioResourceId = "clipboard-test.wav" });
    source.sampleRegistry.emplace<InteractionComponent>(
        sample, InteractionComponent{ .isSelected = true });
    ActionController controller(source);
    auto&            engine = EditorEngine::instance();
    // 使用真实混合剪切载荷，能够发现空复制悄悄清掉原剪切身份的错误。
    // 系统导出只读取单槽队列，测试绝不触碰用户当前的系统文本。
    controller.handleCommand(CmdCut{});
    const auto originalText = engine.consumePendingSystemClipboardText();
    // 导出槽已被消费，接下来的空操作必须维持无请求，而非写入空协议文本。
    // 原剪切状态是独立基线，不能仅验证普通 Copy 的内容数量。
    if ( !originalText || !retainsMixedClipboard(engine, source) ) return 1;
    source.noteRegistry.get<InteractionComponent>(note).isSelected     = false;
    source.sampleRegistry.get<InteractionComponent>(sample).isSelected = false;
    source.lastActionMessage = "unchanged";
    // 特征状态消息在命令执行前设置，能捕获“复制 0 个”这类错误成功提示。
    // 同会话保留本地缓存，异会话保留全局来源；两者不能只修其中一个。
    // 空剪切复用复制实现，须防止它拿旧载荷重新标记一个错误的来源。
    SessionContext empty;
    empty.lastActionMessage = "empty-session";
    for ( auto* context : { &source, &empty } ) {
        // 空会话与有物件但全部未选中的会话都要保持剪贴板不变。
        // 先复制再剪切，覆盖 CmdCut 复用 CmdCopy 后误取旧缓存的执行顺序。
        ActionController action(*context);
        action.handleCommand(CmdCopy{});
        action.handleCommand(CmdCut{});
        if ( !retainsMixedClipboard(engine, source) ||
             engine.consumePendingSystemClipboardText() ) {
            XERROR(
                "Empty copy/cut replaced shared clipboard or queued system "
                "text");
            return 2;
        }
    }
    // 空操作不能显示“复制 0 个”，也不能清掉源实体原有的延迟剪切标记。
    // 异会话的空缓存应保持空，不能为了保护全局内容而复制进本地缓存。
    if ( source.clipboard.size() != 1 || source.sampleClipboard.size() != 1 ||
         source.lastActionMessage != "unchanged" ||
         empty.lastActionMessage != "empty-session" ||
         !empty.clipboard.empty() || !empty.sampleClipboard.empty() ||
         !source.noteRegistry.get<InteractionComponent>(note).isCut ||
         !source.sampleRegistry.get<InteractionComponent>(sample).isCut )
        return 3;
    // 有选中标志但缺少领域组件的实体，不是一个可以复制的物件。
    // 同时覆盖被当前配置隐藏的草稿；不能只检查选择集合是否非空。
    const auto orphan = empty.sampleRegistry.create();
    // 陈旧交互组件仍有选中标志，但缺少采样数据，不能被视为有效复制来源。
    empty.sampleRegistry.emplace<InteractionComponent>(
        orphan, InteractionComponent{ .isSelected = true });
    const auto draft = empty.noteRegistry.create();
    // 隐藏草稿保留真实 Note 数据；失效原因是编辑许可而非组件缺失。
    // 两种失效情况同时存在，守卫必须逐领域验证而非只统计 selected。
    empty.noteRegistry.emplace<NoteComponent>(
        draft,
        NoteComponent{
            .m_timestamp = 1, .m_trackIndex = -1, .m_isDraft = true });
    empty.noteRegistry.emplace<InteractionComponent>(
        draft, InteractionComponent{ .isSelected = true });
    empty.lastConfig.settings.professionalMode = false;
    // 关闭专业模式后已有选择不一定即时消失，命令执行时须重新检查许可。
    ActionController(empty).handleCommand(CmdCopy{});
    ActionController(empty).handleCommand(CmdCut{});
    if ( !retainsMixedClipboard(engine, source) ||
         engine.consumePendingSystemClipboardText() ||
         empty.sampleRegistry.get<InteractionComponent>(orphan).isCut )
        return 4;
    // 先排入一份非空导出，再空复制；旧请求必须完整保留，而非被取消或置空。
    // 先前记录的公开文本提供独立基线，不能仅检查 optional 仍然存在。
    source.noteRegistry.get<InteractionComponent>(note).isSelected = true;
    // 重新启用原选择可得到同一份公开文本，不用测试自行拼装协议内容。
    source.sampleRegistry.get<InteractionComponent>(sample).isSelected = true;
    controller.handleCommand(CmdCut{});
    source.noteRegistry.get<InteractionComponent>(note).isSelected     = false;
    source.sampleRegistry.get<InteractionComponent>(sample).isSelected = false;
    controller.handleCommand(CmdCopy{});
    // 旧请求必须逐字保留，存在一份不同的空载荷同样属于剪贴板被覆盖。
    if ( engine.consumePendingSystemClipboardText() != originalText ) return 5;
    // 对照接下来的有效复制，能够区分保留旧请求与永久屏蔽系统导出的错误方案。
    // 两类命令都执行到正式序列化边界，没有用测试替身伪造发布结果。
    // 只有采样被选中也属于有效复制，不能因音符选区为空而提前拒绝。
    // 成功操作应完整替换旧混合载荷，并从 Cut 恢复为普通 Copy 来源。
    source.sampleRegistry.get<InteractionComponent>(sample).isSelected = true;
    // 正向对照排除“禁用所有复制”的假修复，同时验证跨领域完整替换语义。
    // 只有有效选择才会排入新系统文本，此处必须与前面的无请求形成对照。
    controller.handleCommand(CmdCopy{});
    if ( !source.clipboard.empty() || source.sampleClipboard.size() != 1 ||
         !engine.getClipboard(&source).empty() ||
         engine.getSampleClipboard(&source).size() != 1 ||
         engine.isClipboardCutFrom(&source) ||
         !engine.consumePendingSystemClipboardText() )
        return 6;
    // 所有剪贴板来源均在本测试拥有的会话中，离开前清掉非拥有来源指针。
    // 不保存软件配置、不生成资源文件，其他测试使用独立配置目录。
    engine.clearClipboardForContext(&source);
    // 清理走公开来源接口，不直接访问剪贴板容器或销毁单例状态。
    return 0;
}
