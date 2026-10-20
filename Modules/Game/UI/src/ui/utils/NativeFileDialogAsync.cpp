#include "log/colorful-log.h"
#include "ui/utils/NativeFileDialog.h"
#include <utility>
#ifdef __ANDROID__
#    include "common/platform/DocumentStorage.h"
#endif

namespace MMM::UI::NativeFileDialog
{
namespace
{
#ifdef __ANDROID__
/// @brief 只由 SDL UI 线程创建和消费，一次系统选择对应唯一回调。
DialogCompletion pendingCompletion;

/// @brief 发布请求前先保存回调，Java 文件线程不直接访问任何视图。
/// @details 同一 Activity 只允许一个在途选择，重复请求不覆盖旧回调。
/// @warning 回调拥有者必须活到选择完成，不能捕获当前帧 context。
/// @param kind 请求种类只描述授权范围，不在原生端拼接 content URI。
/// @param name 建议名称在 JNI 入口复制，不借用跨帧调用者字符串。
void beginAsync(int kind, const char* name, DialogCompletion completion)
{
    // 旧回调拥有唯一结果槽，拒绝新请求仍保留正在执行的文档授权。
    if ( pendingCompletion ) {
        XWARN("Another Android document selection is pending");
        completion(NFD_ERROR, {});
        return;
    }
    // 启动方法只投递 Java UI 事件，不等待授权、资源复制或 Activity 恢复。
    pendingCompletion = std::move(completion);
    if ( !Common::beginDocumentSelection(kind, name ? name : "") ) {
        auto failed       = std::move(pendingCompletion);
        pendingCompletion = nullptr;
        failed(NFD_ERROR, {});
    }
}
#else
/// @brief 桌面同步 NFD 的所有权在回调前转为普通字符串，保持原平台交互顺序。
/// @details 成功、取消和失败都在原 UI 线程内完成，不额外启动桌面工作线程。
/// @warning NFD 分配只在这里回收，业务回调不得再次释放路径。
/// @param result 保留 NFD 的成功、取消和错误区分，不把空路径视为成功。
/// @param completion 必须在释放 NFD 资源之后调用，回调可以发起新的选择。
void completeDesktop(nfdresult_t result, nfdu8char_t* path,
                     DialogCompletion completion)
{
    const std::string selected = path ? path : "";
    if ( path ) NFD_FreePathU8(path);
    completion(result, selected);
}
#endif
}  // namespace

/// @brief 轮询只触碰已经发布的短载荷，不在帧循环执行文件复制。
/// @details 首个换行分隔状态与载荷，取消不携带可供加载的路径。
/// @details 请求槽先清空再回调，允许业务失败后从同一入口立即重试。
/// @warning 每帧 UI 入口：无请求时立即返回；Java 查询只读取原子结果槽，不等待
/// I/O。
/// @details 协议中的首个换行划分状态与路径，后续换行仍属于文件名。
/// @details 错误诊断写日志后交回原调用者，由其保留现有错误提示和脏位。
void pollAsyncDialogs()
{
#ifdef __ANDROID__
    if ( !pendingCompletion ) return;
    // Java 只返回短原子结果，不会在这里等待工程复制或系统对话框。
    const auto result = Common::takeDocumentSelection();
    if ( !result ) return;
    auto completion = std::move(pendingCompletion);
    // 小对象优化下移动 std::function 不保证源槽为空，必须显式释放在途标志。
    pendingCompletion = nullptr;
    // 路径按值保存，解析后不再引用 JNI 数组或 provider 的临时文本。
    const auto separator = result->find('\n');
    const auto status    = result->substr(0, separator);
    const auto path      = separator == std::string::npos
                               ? std::string{}
                               : result->substr(separator + 1);
    // 完成槽先清空，再执行用户回调，允许失败提示或后续请求按原 UI 顺序继续。
    if ( status == "OK" )
        completion(NFD_OKAY, path);
    else if ( status == "CANCEL" )
        completion(NFD_CANCEL, {});
    else {
        XERROR("Android document selection failed: {}", path);
        completion(NFD_ERROR, {});
    }
#endif
}

/// @brief Android 使用 OPEN_DOCUMENT，桌面保留原生 NFD 过滤与默认目录。
/// @param completion 结果在有效 UI 帧返回，不由 Java 工作线程调用。
/// @details Android 先完成工作副本复制，再交给现有格式和资源验证入口。
/// @details 桌面过滤器仍原样传递，Android 由加载器校验真实文件格式。
/// @warning 单文件授权不含兄弟媒体，打开完整工程必须使用目录选择。
void openFileAsync(const nfdu8filteritem_t* filters, nfdfiltersize_t count,
                   const char* directory, DialogCompletion completion)
{
#ifdef __ANDROID__
    // MIME 无法可靠表达所有谱面格式，Android 展示可打开文档，由业务验证内容。
    (void)filters;
    (void)count;
    (void)directory;
    beginAsync(0, nullptr, std::move(completion));
#else
    nfdu8char_t* path   = nullptr;
    const auto   result = openFile(&path, filters, count, directory);
    completeDesktop(result, path, std::move(completion));
#endif
}

/// @brief 创建授权保存目标，真正字节发布由 writer 成功后执行。
/// @details 系统创建占位文档不是谱面保存成功，不能提前清理编辑脏位。
/// @warning 请求参数只在启动阶段借用，平台方法会同步复制默认名称。
/// @param name 只建议最终文件名，系统 provider 决定实际目标身份。
/// @details 目标路径交回原业务保存流程，不能在此提前写入空谱面。
void saveFileAsync(const nfdu8filteritem_t* filters, nfdfiltersize_t count,
                   const char* directory, const char* name,
                   DialogCompletion completion)
{
#ifdef __ANDROID__
    (void)filters;
    (void)count;
    (void)directory;
    // 默认文件名只复制到 Java 请求，不能保存调用栈内 c_str 的借用指针。
    // 默认名称已由原逻辑按格式生成，选择后仍需走覆盖与兼容性流程。
    beginAsync(1, name, std::move(completion));
#else
    nfdu8char_t* path   = nullptr;
    const auto   result = saveFile(&path, filters, count, directory, name);
    completeDesktop(result, path, std::move(completion));
#endif
}

/// @brief 授权完整工程树并生成持久工作副本，保持音频与谱面的相对布局。
/// @details 目录授权与单文件授权不同，不能借单文件 URI 访问兄弟音频。
/// @warning 工作副本可能保留未保存编辑，不能作为可随意删除的缓存处理。
/// @param directory 桌面初始位置，Android 系统选择器不接受该普通路径。
/// @details 返回前完整复制相对目录结构，以复用现有音频和图片解析器。
void pickFolderAsync(const char* directory, DialogCompletion completion)
{
#ifdef __ANDROID__
    (void)directory;
    // 不传桌面默认目录冒充授权 URI，系统只显示当前可授权的树。
    beginAsync(2, nullptr, std::move(completion));
#else
    nfdu8char_t* path   = nullptr;
    const auto   result = pickFolder(&path, directory);
    completeDesktop(result, path, std::move(completion));
#endif
}
}  // namespace MMM::UI::NativeFileDialog
