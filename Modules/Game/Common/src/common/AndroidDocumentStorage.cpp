#include "common/platform/DocumentStorage.h"
#include <SDL3/SDL.h>
#include <jni.h>
#include <limits>

namespace MMM::Common
{
namespace
{
/// @brief JNI 借用资源按调用作用域释放，禁止跨原生线程保存 JNIEnv。
/// 每次调用从 SDL 取得当前线程环境，文件线程不缓存 UI 线程环境。
/// Activity 只使用 local reference，不取得系统生命周期所有权。
/// @details Activity 由 SDL 管理，本对象只持有本线程的 local reference。
class ActivityCall final
{
public:
    /// @brief SDL 为当前调用线程取得 JNI 环境，避免使用错误线程的 JNI 指针。
    /// @details 环境不可用时保留空对象，由外层通过返回值报告失败。
    /// @warning 不自行关闭 JVM 或为每帧创建独立 Java 线程。
    ActivityCall() : m_env(static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()))
    {
        // SDL 环境不可用时不创建 Java 引用，后续查找通过空类型失败。
        if ( !m_env ) return;
        // 取得当前 Activity 的本线程引用，不能复用旧 Surface 对应的 Java 对象。
        m_activity = static_cast<jobject>(SDL_GetAndroidActivity());
        if ( m_activity ) m_type = m_env->GetObjectClass(m_activity);
    }
    /// @brief 本调用结束即释放引用；不触碰 Java Activity 生命周期所有权。
    /// @details 挂起的平台异常在本边界清理，不能污染后续无关 JNI 调用。
    /// @warning C++ 不使用异常机制，业务必须检查显式错误结果。
    ~ActivityCall()
    {
        if ( !m_env ) return;
        if ( m_env->ExceptionCheck() ) m_env->ExceptionClear();
        // 先释放类型再释放实例，所有引用都只属于当前 native 调用。
        if ( m_type ) m_env->DeleteLocalRef(m_type);
        if ( m_activity ) m_env->DeleteLocalRef(m_activity);
    }
    /// @brief 查找项目自身 Activity 方法，不能在原生线程用 FindClass
    /// 猜测类加载器。
    /// @param signature 两侧共同维护的 JNI 方法签名。
    /// @return 不可用时返回空 ID，调用方禁止继续执行 Java 方法。
    jmethodID method(const char* name, const char* signature) const
    {
        // 使用实际子类查找继承链，保持 SDLActivity 自身的类加载器上下文。
        return m_type ? m_env->GetMethodID(m_type, name, signature) : nullptr;
    }
    /// @brief 跨 JNI 传 UTF-8 字节，避免 NewStringUTF 的 modified UTF-8
    /// 损坏文件名。
    /// @details 长度先检查 jsize 可表示范围，不允许截断后命中另一个文件。
    /// @return 返回 local reference，调用方必须在本次线程调用结束前释放。
    jbyteArray bytes(std::string_view text) const
    {
        if ( !m_type || text.size() > static_cast<std::size_t>(
                                          std::numeric_limits<jsize>::max()) )
            return nullptr;
        // 字节数组长度与原生字符串一致，不依赖 NUL 结尾或 JNI 特殊编码。
        auto result = m_env->NewByteArray(static_cast<jsize>(text.size()));
        if ( result )
            m_env->SetByteArrayRegion(
                result,
                0,
                static_cast<jsize>(text.size()),
                reinterpret_cast<const jbyte*>(text.data()));
        return result;
    }
    /// @brief 复制短结果后释放 Java 数组，帧间只保留原生值对象。
    /// @details null 是协议状态，不能当作成功选择了空路径。
    /// @warning 仅复制短诊断或路径，工程媒体不经 UI 帧传递。
    std::optional<std::string> take(jbyteArray data) const
    {
        if ( !data || m_env->ExceptionCheck() ) return std::nullopt;
        // 结果转为独立值后才能释放 JNI 数组，禁止让临时 Java 存储逃逸。
        std::string result(
            static_cast<std::size_t>(m_env->GetArrayLength(data)), '\0');
        m_env->GetByteArrayRegion(data,
                                  0,
                                  static_cast<jsize>(result.size()),
                                  reinterpret_cast<jbyte*>(result.data()));
        m_env->DeleteLocalRef(data);
        return result;
    }
    /// @brief 本线程环境由 SDL 管理，以下字段只在本次同步 JNI 调用中有效。
    JNIEnv* m_env = nullptr;
    /// @brief Activity 的 local reference，不能作为跨线程观察指针保存。
    jobject m_activity = nullptr;
    /// @brief 实际 Activity 类型的借用引用，方法声明位于项目 Java 外壳。
    jclass m_type = nullptr;
};
}  // namespace

/// @brief 取得平台检查权限后返回的共享目录，不缓存可被系统撤销的授权。
/// @return 仅完整支持本机访问的系统提供普通路径，其他情况保持 SAF 模式。
/// @warning 用户点击低频路径；不得由逐帧显示状态代码查询。
std::optional<std::string> localFileRoot()
{
    ActivityCall call;
    const auto   method = call.method("localFileRoot", "()[B");
    // 空返回包含未授权、旧平台与桥接失败，均不能扩大访问范围。
    if ( !method ) return std::nullopt;
    auto result = static_cast<jbyteArray>(
        call.m_env->CallObjectMethod(call.m_activity, method));
    return call.take(result);
}

/// @brief 进入用户可审查和撤销的系统授权页，原生线程不等待选择结果。
/// @return 只表示设置请求已投递，不代表系统已经授权。
/// @warning 只能由用户显式设置动作调用，不作为文件打开的隐式副作用。
bool requestLocalFileAccess()
{
    ActivityCall call;
    const auto   method = call.method("requestLocalFileAccess", "()V");
    // 缺少 Java 外壳时明确失败，不能用原始共享路径回退绕过权限检查。
    if ( !method ) return false;
    call.m_env->CallVoidMethod(call.m_activity, method);
    return !call.m_env->ExceptionCheck();
}

/// @brief 将系统选择请求交给 Java UI 线程，不等待用户授权。
/// @details 文件名在 Java 同步方法中复制，C++ 借用不跨越系统选择器。
/// @return 只表示请求已接受，不能据此报告工程已经打开。
bool beginDocumentSelection(int kind, std::string_view defaultName)
{
    ActivityCall call;
    const auto   method = call.method("beginDocumentSelection", "(I[B)Z");
    if ( !method ) return false;
    auto name = call.bytes(defaultName);
    // Java 返回仅表示请求入队；用户取消和复制错误由后续轮询单独返回。
    const bool accepted =
        call.m_env->CallBooleanMethod(call.m_activity, method, kind, name);
    // 方法已经同步消费名称，系统选择期间只持有 Java 自己复制的文本。
    if ( name ) call.m_env->DeleteLocalRef(name);
    return !call.m_env->ExceptionCheck() && accepted;
}

/// @brief 仅轮询已发布的 Java 短结果，工程复制在 Java 文件工作线程进行。
/// @details 无结果立即返回，完成后由 UI 线程执行业务回调。
/// @warning JNI 查询只读取原子结果槽，不等待文件门闩或 provider。
std::optional<std::string> takeDocumentSelection()
{
    ActivityCall call;
    const auto   method = call.method("takeDocumentSelection", "()[B");
    // 桥接失效必须结束等待状态，不能让 UI 永久保留在途选择槽。
    if ( !method )
        return std::string("ERROR\nAndroid document bridge unavailable");
    auto result = static_cast<jbyteArray>(
        call.m_env->CallObjectMethod(call.m_activity, method));
    if ( call.m_env->ExceptionCheck() )
        return std::string("ERROR\nAndroid document result failed");
    return call.take(result);
}

/// @brief writer 已成功关闭本地文件后，确认 provider 同样完成写入与内容校验。
/// @details 普通路径不会扩张为 SAF 权限；工作副本必须具有持久映射。
/// @warning 该同步调用只服务低频保存事务，禁止用于逐帧状态展示。
bool publishDocument(const std::filesystem::path& path, std::string& error)
{
    ActivityCall call;
    const auto   method = call.method("publishDocument", "([B)[B");
    // 没有发布能力就拒绝声明保存成功，私有工作副本保留供重试。
    if ( !method ) {
        error = "Android document bridge unavailable";
        return false;
    }
    // 路径使用标准 UTF-8 字节，与 Java StandardCharsets.UTF_8 严格配对。
    const auto utf8   = path.u8string();
    auto       name   = call.bytes(std::string_view(
        reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    auto       result = static_cast<jbyteArray>(
        call.m_env->CallObjectMethod(call.m_activity, method, name));
    if ( name ) call.m_env->DeleteLocalRef(name);
    // 平台异常在作用域退出时清理，当前保存明确报告失败。
    if ( call.m_env->ExceptionCheck() ) {
        error = "Android document publication failed";
        return false;
    }
    // null 表示普通路径或文档已完整发布；非空结果必须作为保存失败反馈给业务。
    const auto message = call.take(result);
    // 非空诊断即发布失败，包括权限撤回、外部冲突或读回校验不一致。
    if ( message ) {
        error = *message;
        return false;
    }
    return true;
}
}  // namespace MMM::Common
