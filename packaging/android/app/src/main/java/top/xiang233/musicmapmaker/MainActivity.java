package top.xiang233.musicmapmaker;

import org.libsdl.app.SDLActivity;
import android.content.Intent;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.net.Uri;
import java.nio.charset.StandardCharsets;

/**
 * Android 仅提供 Activity 外壳，现有编辑器界面和业务逻辑仍由 C++ 实现。
 * SDL Java 实现来自构建使用的同一上游源码目录，不在仓库复制维护。
 */
public final class MainActivity extends SDLActivity {
    /** 高刷偏好只作用于本应用窗口，与桌面渲染后端无关。 */
    private final HighRefreshRate highRefreshRate = new HighRefreshRate(this);

    /** SDL 完成恢复后再发布窗口和 Surface 的刷新率请求。 */
    @Override protected void onResume() {
        super.onResume();
        getWindow().getDecorView().post(highRefreshRate::apply);
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

    /** 只有低频文件写入调用此接口，平台错误必须传回 C++ 保存结果。 */
    public byte[] publishDocument(byte[] name) {
        // 原生 writer 已关闭文件，结果必须传回业务保存事务而非只在 Java 吞掉。
        String error = documents().publish(new String(name, StandardCharsets.UTF_8));
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
