#include <ice/core/MixBus.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/manage/dec/IDecoderFactory.hpp>
#include <ice/manage/dec/IDecoderInstance.hpp>
#include <ice/manage/dec/MediaInfo.hpp>
#include <ice/thread/ThreadPool.hpp>

#include <algorithm>
#include <cstddef>
#include <future>
#include <memory>
#include <string_view>

/**
 * @file AudioEngineAbiTest.cpp
 * @brief 验证主项目消费方头文件与预编译 ICE 库解释同一对象布局。
 *
 * 链接成功只能证明函数签名可解析，不能证明公开类的字段偏移一致。
 * ThreadPool 和 MixBus 在库内构造，再由消费方模板和内联函数访问。
 * AudioTrack 进一步按值保存 MediaInfo，MediaInfo 按值保存 AlbumArt。
 * 库更新后若复用旧业务对象，解码器指针可能被误读为字符串容量。
 * 这个场景曾在 Windows 启动加载皮肤音效时造成访问冲突。
 *
 * 内存工厂让测试不依赖文件、声卡、配置目录或操作系统编解码器。
 * 工厂返回确定的双声道八帧数据，并填写非空专辑标签。
 * 库内构造的 AudioTrack 须由消费方读取策略、路径和元信息。
 * num_frames 还会经过解码器指针访问，能直接暴露字段偏移错位。
 * 测试不能只调用 create：创建成功不代表后续消费方访问安全。
 *
 * 返回码区分线程池、混音缓冲、音轨帧数和公开字段四类失败。
 * 无外部输出文件，CTest 超时限定后台缓存任务的等待边界。
 */

namespace
{
/// @brief 提供不读取文件的固定八帧样本，用于检查消费方与 ICE 对象布局。
class AbiDecoderInstance final : public ice::IDecoderInstance
{
public:
    /// @brief 将游标限制在样本长度内，保持后续读取边界有效。
    bool seek(std::size_t pos) override
    {
        m_position = std::min(pos, std::size_t{ 8 });
        return true;
    }

    /// @brief 用固定非零值填充已请求的有效帧，避免依赖外部音频资源。
    std::size_t read(float** buffer, std::size_t chunkSize) override
    {
        const auto count = std::min(chunkSize, std::size_t{ 8 } - m_position);
        for ( std::size_t channel = 0; channel < m_format.channels;
              ++channel ) {
            std::fill_n(buffer[channel], count, 0.25F);
        }
        m_position += count;
        return count;
    }

    /// @brief 返回固定测试格式，避免从媒体文件读取格式信息。
    const ice::AudioDataFormat& get_source_format() const override
    {
        return m_format;
    }

    /// @brief 返回测试音频的确定帧数。
    std::size_t get_source_total_frames() const override { return 8; }

private:
    /// @brief 消费方与引擎双方都必须按此格式解释样本。
    ice::AudioDataFormat m_format{ 2, 48000 };
    /// @brief 当前读取游标，仅由测试线程与缓存准备任务串行访问。
    std::size_t m_position{ 0 };
};

/// @brief 用内存解码实例创建真实 AudioTrack，不让文件探测掩盖布局问题。
class AbiDecoderFactory final : public ice::IDecoderFactory
{
public:
    /// @brief 填充含专辑的媒体信息，使嵌套值类型参与对象布局。
    bool probe(std::string_view, ice::MediaInfo& info) const override
    {
        info.title       = "ABI sample";
        info.album       = "Layout sample";
        info.format      = { 2, 48000 };
        info.frame_count = 8;
        return true;
    }

    /// @brief 每条音轨使用独立游标，避免测试共享解码状态。
    std::unique_ptr<ice::IDecoderInstance> create_instance(
        std::string_view, const ice::AudioDataFormat&) const override
    {
        return std::make_unique<AbiDecoderInstance>();
    }
};
}  // namespace

/// @brief 用消费方头文件构造真实引擎对象，检查预编译依赖的启动契约。
/// @return 构造、任务执行和静音处理全部成功时返回零。
/// @details
/// 不初始化声卡、不读写项目；测试必须链接所选预编译库，不能补入私有实现。
/// @warning 仅供低频测试：线程池析构会等待工作线程，CTest 设置了独立超时。
int main()
{
    // 构造函数必须由库导出；旧头文件中的内联实现曾掩盖过归档缺符号。
    // 随后的模板入队从消费方编译，可同时检查线程池私有成员布局是否配套。
    ice::ThreadPool pool(4);
    auto            result = pool.enqueue([] { return 42; });
    if ( !result.valid() || result.get() != 42 ) return 1;

    // MixBus 构造会在库内准备缓冲区，正是启动崩溃曾发生的边界。
    // 栈上对象尺寸来自公开头文件，不能用库内工厂规避尺寸不一致。
    ice::MixBus bus;
    // 双声道的第二条地址能暴露声道指针表偏移错误，不能只检查首声道。
    const ice::AudioDataFormat format{ 2, 48000 };
    bus.prepare(format, 1024);
    ice::AudioBuffer buffer(format, 1024);
    if ( buffer.num_channels() != 2 || buffer.num_frames() != 1024 ) return 2;

    // 非零输入让空总线处理后的静音断言有意义，而不是只检查初始零值。
    // clear 的内联实现和 process 的库内实现必须以相同布局解释该对象。
    for ( std::size_t channel = 0; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0; frame < buffer.num_frames(); ++frame ) {
            buffer.raw_ptrs()[channel][frame] = 1.0f;
        }
    }
    bus.process(buffer);
    // 不使用 assert，发布配置也必须执行样本检查并返回明确失败码。
    for ( std::size_t channel = 0; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0; frame < buffer.num_frames(); ++frame ) {
            if ( buffer.raw_ptrs()[channel][frame] != 0.0f ) return 3;
        }
    }
    // 清零与析构也覆盖消费方内联资源释放，不能在检查完采样后直接退出进程。
    buffer.clear();

    // AudioTrack 在库内构造、在消费方内联读取，能暴露预编译布局错位。
    auto track = ice::AudioTrack::create("abi-memory",
                                         pool,
                                         std::make_shared<AbiDecoderFactory>(),
                                         ice::CachingStrategy::CACHY);
    if ( !track || track->num_frames() != 8 ) return 4;
    if ( track->cachingStrategy() != ice::CachingStrategy::CACHY ||
         track->path() != "abi-memory" ||
         track->get_media_info().album != "Layout sample" )
        return 5;
    return 0;
}
