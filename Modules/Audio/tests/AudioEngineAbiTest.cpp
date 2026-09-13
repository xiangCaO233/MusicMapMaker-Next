#include <ice/core/MixBus.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <ice/thread/ThreadPool.hpp>

#include <cstddef>
#include <future>

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
    return 0;
}
