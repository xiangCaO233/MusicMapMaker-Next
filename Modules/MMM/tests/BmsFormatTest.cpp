#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
/// @brief 明确验证导入后的音乐时间，不用 writer 的计算作为期望。
bool verify(const MMM::BeatMap& map)
{
    // 两条同通道 BGM 必须共存，不能被后出现的行覆盖。
    // 转盘计入六轨布局，不能把五颗普通键误当作五轨谱面。
    // 普通 Note 和 Hold 分容器断言，避免长条拆成两个头也通过。
    if ( map.m_baseMapMetadata.track_count != 6 ||
         map.m_audioSamples.size() != 2 || map.m_noteData.notes.size() != 2 ||
         map.m_noteData.holds.size() != 1 )
        return false;
    // 第二拍变速后，第四拍在 1.5 秒；STOP 不改变音乐坐标。
    if ( std::abs(map.m_noteData.notes.back().m_timestamp - 1500) > 1e-5 )
        return false;
    // 长条跨越 BPM 变更，持续时间必须逐段积分。
    // STOP 位于长条起点，但不应被算进音乐时间长度。
    const auto& hold = map.m_noteData.holds.front();
    if ( std::abs(hold.m_timestamp - 500) > 1e-5 ||
         std::abs(hold.m_duration - 750) > 1e-5 )
        return false;
    // WAV 名称必须进入显式 Note 绑定，原生保存后仍能用于 Key 音。
    if ( !hold.m_sampleBinding ||
         hold.m_sampleBinding->m_audioResourceId != "key.wav" )
        return false;
    // 既检查参数也检查事件数量，防止重复生成同一暂停。
    // 只比较绝对期望，避免读写器共享同一错误公式时互相掩盖。
    int stops = 0;
    for ( const auto& timing : map.m_timings ) {
        if ( timing.m_timingEffect != MMM::TimingEffect::STOP ) continue;
        ++stops;
        // 24 单位为半拍；120 BPM 时应等待 0.25 秒。
        if ( std::abs(timing.m_timestamp - 500) > 1e-5 ||
             std::abs(timing.m_timingEffectParameter - .25) > 1e-8 )
            return false;
    }
    return stops == 1;
}
}  // namespace

/// @brief 生成独立夹具并验证 BMS、MMM 两条往返路径及失败边界。
int main(int argc, char** argv)
{
    // 输出目录由 CTest 传入构建树，测试不会写入只读资源目录。
    // 独立命令运行同样必须明确提供输出位置。
    if ( argc != 2 && argc != 3 ) return 1;
    const std::filesystem::path root(argv[1]);
    std::error_code             error;
    std::filesystem::create_directories(root, error);
    if ( error ) return 2;
    // 每轮重写生成夹具，不能依赖上一次导出留下的内容。
    // WAV 名称只用于模型绑定验证，不需要音频解码或真实设备。
    const auto source = root / "source.bms";
    {
        // 定义有意放在通道之后，验证两阶段声明解析。
        std::ofstream file(source);
        file << "#TITLE STOP test\n#BPM 120\n#00011:0100\n#00111:01\n"
                "#00051:00010001\n#00009:00010000\n#00008:00000200\n"
                "#00001:01\n#00001:02\n#WAV01 key.wav\n#WAV02 bgm.wav\n"
                "#STOP01 24\n#BPM02 240\n";
    }
    // 公共入口负责扩展名分派，此处不直接调用格式私有 helper。
    // 从而同时验证编辑器加载路径能够到达新的读取器。
    auto map = MMM::BeatMap::loadFromFile(source);
    if ( !verify(map) ) return 3;
    // 两条实际 writer 都经过公共分派，能发现后缀或序列化遗漏。
    for ( const auto* extension : { ".mmm", ".bms" } ) {
        const auto target = root / (std::string("roundtrip") + extension);
        // 原生格式须保存秒数，BMS 须反算 1/48 拍定义。
        // 两种重载结果都与同一人工算出的期望比较。
        if ( !map.saveToFile(target) ) return 4;
        if ( !verify(MMM::BeatMap::loadFromFile(target)) ) return 5;
    }
    // 拒绝导出也属于用户数据完整性契约。
    // 若 writer 默默忽略 STOP，文件能打开但节奏和音频语义已经改变。
    // 失败必须通过公共 saveToFile 返回，不只写一条诊断日志。
    // 相邻两次原生/BMS 保存已经证明当前模型本身有效。
    // 因而该断言专门检查目标格式能力，而非一般路径失败。
    // 不支持 STOP 的外部格式必须拒绝，不能悄悄变成 SV。
    if ( map.saveToFile(root / "unsupported.osu") ) return 6;
    {
        std::ofstream file(root / "lnobj.bms");
        file << "#BPM 120\n#LNOBJ ZZ\n#WAV01 key.wav\n#00011:010000ZZ\n";
    }
    // LNOBJ 尾部不产生额外 Note，绑定仍来自开始点。
    // 夹具的最后一个对象位于第四格，即三拍而非四拍。
    auto lnobj = MMM::BeatMap::loadFromFile(root / "lnobj.bms");
    if ( !lnobj.m_noteData.notes.empty() ||
         lnobj.m_noteData.holds.size() != 1 ||
         lnobj.m_noteData.holds.front().m_duration != 1500 )
        return 7;
    // 随机分支不能被无条件拼接；现阶段明确失败比导入错误谱面更安全。
    {
        std::ofstream file(root / "random.bms");
        file << "#BPM 120\n#RANDOM 2\n#IF 1\n#00011:01\n#ENDIF\n";
    }
    if ( !MMM::BeatMap::loadFromFile(root / "random.bms")
              .m_noteData.notes.empty() )
        return 8;
    // 测试只证明模型转换和持久化，设备播放由 Audio 回归单独验证。
    // 失败返回码保留阶段身份，便于无 UI 的 CI 定位。
    // 变长小节后接直接十六进制 BPM，DP 的第二侧偏移不能丢失。
    // 第二小节从第八拍开始，即 120 BPM 下四秒。
    {
        std::ofstream file(root / "double.bme");
        file << "#BPM 120\n#00002:2\n#00018:01\n#00129:01\n#00103:F0\n";
    }
    // 拍号不是固定毫秒槽位，02 改变小节累计起点后仍须正确积分。
    // F0 按十六进制表示 240，不能当作资源编号查声明。
    // 轨道十五是第二侧第七键，验证预扫描确实识别了双侧七键。
    // 初始物件也在同一最终布局中，不因后续出现第二侧而迁移错位。
    auto dual = MMM::BeatMap::loadFromFile(root / "double.bme");
    if ( dual.m_baseMapMetadata.track_count != 16 ||
         dual.m_noteData.notes.size() != 2 ||
         dual.m_noteData.notes.back().m_track != 15 ||
         std::abs(dual.m_noteData.notes.back().m_timestamp - 4000) > 1e-6 )
        return 9;
    // PMS 的 25 通道对应第九键，不是双人布局的第十三键。
    // 大写后缀与小写一样走相同读取协议。
    {
        std::ofstream file(root / "nine.PMS");
        file << "#BPM 120\n#00025:01\n";
    }
    auto pms = MMM::BeatMap::loadFromFile(root / "nine.PMS");
    if ( pms.m_baseMapMetadata.track_count != 9 ||
         pms.m_noteData.notes.size() != 1 ||
         pms.m_noteData.notes.front().m_track != 8 )
        return 10;
    // 日文旧谱面的 CP932 名称在模型中必须变成 UTF-8，包含音频文件名。
    // 使用字节夹具避免编译器源字符集改变测试输入。
    {
        std::ofstream file(root / "cp932.bms", std::ios::binary);
        file << "#BPM 120\n#TITLE \x83\x65\x83\x58\x83\x67\n"
                "#WAV01 \x83\x65\x83\x58\x83\x67.wav\n#00011:01\n";
    }
    // 读取成功不能只看标题，音效文件名错误会导致编辑器静音。
    // 因此同一夹具检查模型中的显式绑定路径，保持字符转换与资源定位一致。
    // 期望为源码 UTF-8 字面量，不调用生产解码函数生成期望值。
    auto encoded = MMM::BeatMap::loadFromFile(root / "cp932.bms");
    if ( encoded.m_baseMapMetadata.title != "テスト" ||
         encoded.m_noteData.notes.size() != 1 ||
         !encoded.m_noteData.notes.front().m_sampleBinding ||
         encoded.m_noteData.notes.front().m_sampleBinding->m_audioResourceId !=
             "テスト.wav" )
        return 11;
    // 四颗普通键转标准五键加转盘：空转盘位于零，原物件顺移一轨。
    // 毫秒量化位置、非默认增益和视觉滚速共同覆盖外部谱面的常见导出路径。
    auto keyboard = MMM::BeatMap::loadFromFile(root / "source.bms");
    keyboard.m_baseMapMetadata.track_count = 4;
    // 普通键补位夹具不沿用原六轨长条，避免来源键数与物件轨号互相矛盾。
    // 原来的长条往返已由 verify 覆盖，这里专门检查四轨转换边界。
    keyboard.m_noteData.holds.clear();
    keyboard.m_noteData.notes.front().m_track     = 0;
    keyboard.m_noteData.notes.front().m_timestamp = 628;
    keyboard.m_noteData.notes.back().m_track      = 3;
    keyboard.m_noteData.notes.back().m_timestamp  = 1234;
    // 非默认音量必须来自真实绑定，空绑定无法触发事件增益兼容分支。
    // 自动采样另设不同增益，防止 writer 只处理其中一种音频来源。
    keyboard.m_noteData.notes.front().m_sampleBinding->m_volume = 0.4F;
    keyboard.m_audioSamples.front().m_volume                    = 0.5F;
    // 新增视觉时间线不应影响四分音符积分，也不应使整个导出失败。
    // STOP 和 BPM 仍沿用原夹具，用于检查降级不会误删可支持的效果。
    MMM::Timing visual;
    visual.m_timingEffect          = MMM::TimingEffect::SCROLL;
    visual.m_timingEffectParameter = 1.25;
    keyboard.m_timings.push_back(visual);
    // 目标名称独立于 roundtrip 夹具，失败不会被旧文件掩盖。
    // saveToFile 的成功和后续重新导入必须同时满足，不能只检查磁盘存在。
    if ( !keyboard.saveToFile(root / "keyboard.bms") ) return 12;
    // 读取器仍按标准通道判断布局，验证结果不依赖私有键数元数据。
    // 首、末轨分别覆盖补转盘和右侧保留空键两个边界。
    const auto converted = MMM::BeatMap::loadFromFile(root / "keyboard.bms");
    // 重载标准布局而不是私有扩展，且两个音符均保留在普通键上。
    // 导出不能把修改写回来源模型的键数、轨号或增益。
    if ( converted.m_baseMapMetadata.track_count != 6 ||
         converted.m_noteData.notes.size() != 2 ||
         converted.m_noteData.notes.front().m_track != 1 ||
         converted.m_noteData.notes.back().m_track != 4 ||
         std::abs(converted.m_noteData.notes.front().m_timestamp - 628) >
             0.01 ||
         std::abs(converted.m_noteData.notes.back().m_timestamp - 1234) >
             0.01 ||
         keyboard.m_noteData.notes.front().m_sampleBinding->m_volume != 0.4F ||
         keyboard.m_baseMapMetadata.track_count != 4 )
        return 13;
    // 可选外部谱面探针用于用户现场复测，标准 CTest 不依赖本机目录。
    // 产物仍全部写入调用方给出的构建输出目录。
    if ( argc == 3 ) {
        // 现场输入使用公共格式分派，覆盖 OSU 载入而非手工重建近似模型。
        // 源文件只读，输出固定为 external.bms，避免覆盖用户已保存的谱面。
        // 预期轨号来自原布局加一个空转盘，与生产通道计算独立比较。
        const auto external = MMM::BeatMap::loadFromFile(argv[2]);
        if ( !external.saveToFile(root / "external.bms") ) return 14;
        const auto result = MMM::BeatMap::loadFromFile(root / "external.bms");
        if ( result.m_noteData.notes.size() !=
                 external.m_noteData.notes.size() ||
             result.m_noteData.holds.size() !=
                 external.m_noteData.holds.size() )
            return 15;
        // 音符数量相同还不足以证明转换正确，逐项核对键序和时间。
        // 红线自身的分数近似可能逐段累积，因此现场探针允许四分之一毫秒。
        // 该容差远小于原 OSU 整数毫秒，不能把明显吸附错误当作转换成功。
        // Hold 尾部也必须检查，避免只保留头部而丢失持续长度。
        // 测试读取源模型而不修改它，所有误差均与原始导入结果比较。
        // 同刻多轨对象经分数近似后可能重新排序，不能按容器下标比较。
        // 使用轨号和时间共同匹配，避免把和弦内部换序误判为丢失长条。
        // 两侧数量已经独立核对，匹配同时检查每个来源对象的目标位置。
        const auto matches = [](const auto& sourceNotes,
                                const auto& targetNotes) {
            for ( const auto& sourceNote : sourceNotes ) {
                const auto target = std::find_if(
                    targetNotes.begin(),
                    targetNotes.end(),
                    [&](const auto& note) {
                        return note.m_track == sourceNote.m_track + 1 &&
                               std::abs(note.m_timestamp -
                                        sourceNote.m_timestamp) <= 0.25;
                    });
                if ( target == targetNotes.end() ) return false;
                // Note 没有持续时长，Hold 则额外检查尾部；共用同一匹配标准。
                if constexpr ( requires { sourceNote.m_duration; } ) {
                    if ( std::abs(target->m_duration - sourceNote.m_duration) >
                         0.25 )
                        return false;
                }
            }
            return true;
        };
        if ( !matches(external.m_noteData.notes, result.m_noteData.notes) ||
             !matches(external.m_noteData.holds, result.m_noteData.holds) )
            return 16;
    }
    XINFO("BMS import/export and native STOP persistence passed");
    return 0;
}
