package top.xiang233.musicmapmaker;

import org.libsdl.app.SDLActivity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.net.Uri;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.GeneralSecurityException;
import java.security.cert.X509Certificate;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;
import android.util.Base64;

/**
 * Android 仅提供 Activity 外壳，现有编辑器界面和业务逻辑仍由 C++ 实现。
 * SDL Java 实现来自构建使用的同一上游源码目录，不在仓库复制维护。
 */
public final class MainActivity extends SDLActivity {
    /** 权限引导只属于当前 Activity，每次恢复重新核验系统实际授权。 */
    private FileAccessPrompt fileAccessPrompt;
    /** 只在 Java UI 线程修改；私有资源下载完成前不显示共享文件授权引导。 */
    private boolean editorResourcesReady;

    /** 返回回调只在 Activity 生命周期内存活，旧版系统不解析 API 33 类型。 */
    private Object backNavigationCallback;
    /** 鼠标监听器随 Activity 注册和释放，不依赖原生窗口是否已经构造。 */
    private InputDevices inputDevices;
    /** 键盘观察者只报告遮挡，SDL 仍负责文本与焦点。 */
    private SoftKeyboard softKeyboard;
    /** 低频发布设备能力，不能在此切换原生方案或直接访问 ImGui。 */
    static native void updateMouseConnection(boolean connected);

    /** 返回只请求移动工作区导航，不能直接调用 finish 或退出原生进程。 */
    static native void requestNativeBack();

    /** SDL 初始化之后接管系统返回手势，保留平台 IME 的更高优先级处理。 */
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        backNavigationCallback = BackNavigation.register(this);
        inputDevices = new InputDevices(this);
        softKeyboard = new SoftKeyboard(this);
        // 只建立引导对象，显示与返回核验统一交给 onResume，避免启动时重复弹窗。
        fileAccessPrompt = new FileAccessPrompt(this, this::requestLocalFileAccess);
    }

    /** Activity 销毁时精确解除自己的回调，SDL 生命周期仍由基类负责。 */
    @Override protected void onDestroy() {
        if (softKeyboard != null) softKeyboard.close();
        if (inputDevices != null) inputDevices.close();
        BackNavigation.unregister(this, backNavigationCallback);
        super.onDestroy();
    }

    /** 兼容三键导航及旧系统，统一进入移动端的独立退出确认。 */
    @Override public void onBackPressed() { requestNativeBack(); }

    /**
     * @brief 为原生 TLS 导出本应用默认信任管理器接受的根证书。
     * @return 仅含公开证书的 PEM 字节；没有可用信任管理器时返回空数组。
     * @throws GeneralSecurityException 平台证书读取失败，由 JNI 边界转换为失败结果。
     * @warning 仅原生启动阶段调用，不在 UI 帧内遍历系统证书。
     */
    public byte[] systemCaCertificates() throws GeneralSecurityException {
        // null 使用平台默认信任配置，避免硬编码 Android 版本相关的 APEX 文件路径。
        TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        factory.init((KeyStore) null);
        StringBuilder pem = new StringBuilder();
        // 只导出默认管理器已经接受的信任锚，不把远端站点证书提升为根证书。
        for (TrustManager manager : factory.getTrustManagers()) {
            if (!(manager instanceof X509TrustManager)) continue;
            // 证书禁用与平台根证书更新在下次进程启动时重新取样。
            // 不访问用户私钥，也不要求存储权限或额外网络下载。
            for (X509Certificate certificate : ((X509TrustManager) manager).getAcceptedIssuers()) {
                pem.append("-----BEGIN CERTIFICATE-----\n");
                pem.append(Base64.encodeToString(certificate.getEncoded(), Base64.NO_WRAP));
                pem.append("\n-----END CERTIFICATE-----\n");
            }
        }
        // PEM 为纯 ASCII，通过 byte[] 传递而不依赖 JNI modified UTF-8。
        return pem.toString().getBytes(StandardCharsets.US_ASCII);
    }

    /** 高刷偏好只作用于本应用窗口，与桌面渲染后端无关。 */
    private final HighRefreshRate highRefreshRate = new HighRefreshRate(this);

    /** SDL 完成恢复后再发布窗口和 Surface 的刷新率请求。 */
    @Override protected void onResume() {
        super.onResume();
        getWindow().getDecorView().post(highRefreshRate::apply);
        // 系统设置返回也必须重新核验；未授权时不能继续显示不可读的空目录。
        getWindow().getDecorView().post(() -> {
            if (editorResourcesReady) fileAccessPrompt.showIfNeeded();
        });
    }

    /**
     * @brief 原生资源同步完成且正式窗口建立后，才允许提示工程目录权限。
     * @warning JNI 从 SDL 线程调用；状态与弹窗都投递到 Java UI 线程处理。
     */
    public void notifyEditorResourcesReady() {
        // 下载只访问应用私有目录，不应被不相关的共享存储授权弹窗盖住。
        // Activity 后续恢复继续沿用 onResume 的授权核验，不重复建立引导对象。
        runOnUiThread(() -> {
            editorResourcesReady = true;
            fileAccessPrompt.showIfNeeded();
        });
    }

    /** 系统文件选择器返回时可能替换 Surface，重新应用当前显示能力。 */
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) highRefreshRate.apply();
    }

    /** SAF 持久授权与工作副本只属于当前包，结果不直接跨线程调用业务 UI。 */
    private DocumentStorage documents;

    /** Android 在 Activity attach 后才能解析内部目录，不在构造阶段访问 Context。 */
    private synchronized DocumentStorage documents() {
        if (documents == null) documents = new DocumentStorage(this);
        return documents;
    }

    /** 原生线程只投递选择请求，系统交互由 Java UI 线程处理。 */
    public boolean beginDocumentSelection(int kind, byte[] name) {
        String filename = new String(name, StandardCharsets.UTF_8);
        // 请求在 SDL 线程到达；Activity 只能由 Java UI 线程打开系统界面。
        runOnUiThread(() -> documents().begin(kind, filename));
        return true;
    }

    /** 返回值为短 UTF-8 载荷，尚未完成时为 null，不能阻塞 SDL 生命周期。 */
    public byte[] takeDocumentSelection() { return documents().takeResult(); }

    /** 显示信息由文件线程准备；SDL 帧只取已经发布的短结果，不等待 provider。 */
    public byte[] documentPresentation(byte[] path) {
        return documents().presentation(new String(path, StandardCharsets.UTF_8));
    }

    /** 只有低频文件写入调用此接口，平台错误必须传回 C++ 保存结果。 */
    public byte[] publishDocument(byte[] name) {
        // 原生 writer 已关闭文件，结果必须传回业务保存事务而非只在 Java 吞掉。
        String error = documents().publish(new String(name, StandardCharsets.UTF_8));
        return error == null ? null : error.getBytes(StandardCharsets.UTF_8);
    }

    /** 转换失败不能留下像成功包一样的零字节占位文件，完整工作副本保持可恢复。 */
    public byte[] discardUnwrittenDocument(byte[] name) {
        String error = documents().discardUnwritten(new String(name, StandardCharsets.UTF_8));
        return error == null ? null : error.getBytes(StandardCharsets.UTF_8);
    }

    /** 先路由自己的请求码，SDL 原有请求仍交给其基类处理。 */
    @Override protected void onActivityResult(int request, int result, Intent data) {
        // 不拦截 SDL 自己的权限与 Activity 请求，避免破坏上游生命周期。
        if (!documents().accept(request, result, data)) super.onActivityResult(request, result, data);
    }

    /**
     * @brief 兼容模式只在用户已授予所有文件访问后提供共享存储根。
     * @return 未授权或 API 不支持时为 null，调用方继续使用 SAF。
     * @warning 仅在打开文件选择器的低频操作查询，不在每帧轮询系统权限。
     */
    public byte[] localFileRoot() {
        // 不能把 Manifest 声明当作已授权；设置页可能被用户取消或撤销。
        if (Build.VERSION.SDK_INT < 30 || !Environment.isExternalStorageManager()) return null;
        return Environment.getExternalStorageDirectory().getAbsolutePath().getBytes(StandardCharsets.UTF_8);
    }

    /**
     * @brief 由显式设置按钮打开本包的授权页，权限结果由系统和用户决定。
     * @details 不调用 appops 或修改其他系统文件提供器的权限。
     * @warning Java UI 线程只启动系统页，不等待用户操作或阻塞 SDL 生命周期。
     */
    public void requestLocalFileAccess() {
        runOnUiThread(() -> {
            if (Build.VERSION.SDK_INT < 30) return;
            // 即使已授权也允许打开设置，用户可在同一入口撤回兼容访问。
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        });
    }

    /** 先加载唯一的 SDL3，再加载包含业务模块的原生入口。 */
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
