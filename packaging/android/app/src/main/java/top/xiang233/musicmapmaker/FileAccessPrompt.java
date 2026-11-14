package top.xiang233.musicmapmaker;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Build;
import android.os.Environment;

/**
 * @brief 本机目录权限缺失时引导用户完成系统授权，不能把 SAF 当作跳过入口。
 * @details 系统页由用户手动操作，应用不调用 appops 或保存虚假的已授权标记。
 * 系统选择器的目录授权仍由 DocumentStorage 单独验证，二者不能互相替代。
 * Activity 每次恢复时重新核对权限，取消授权后不能把目录不可读隐藏成空列表。
 */
final class FileAccessPrompt {
    /** 当前 Activity 拥有的提示，不跨窗口重建保留引用。 */
    private final Activity activity;
    /** 用户明确点击后才进入系统页，不在权限检查时自动跳转。 */
    private final Runnable requestAccess;
    /** 同一窗口只显示一个提示，避免 onResume 与焦点回调叠加多个对话框。 */
    private AlertDialog dialog;

    /**
     * @brief Activity attach 后建立权限引导，不在构造阶段访问系统目录。
     * @param activity 当前外壳，销毁时随外壳释放。
     * @param requestAccess 当前包的系统授权页入口。
     */
    FileAccessPrompt(Activity activity, Runnable requestAccess) {
        this.activity = activity;
        this.requestAccess = requestAccess;
    }

    /**
     * @brief 启动及系统设置返回时核验，不使用上一次提示结果替代真实权限。
     * @warning 只在 Java UI 生命周期调用，不加入原生逐帧绘制或文件枚举热路径。
     * @details API 30 以下没有所有文件访问页，仍由既有 SAF 逐目录授权处理。
     */
    void showIfNeeded() {
        if (Build.VERSION.SDK_INT < 30 || activity.isFinishing() || activity.isDestroyed()) return;
        // 已授权时关闭可能仍在显示的提示；实际路径能力仍由文件操作再次检查。
        // 不能仅因“曾打开设置”就认为成功，用户可能取消或直接返回。
        if (Environment.isExternalStorageManager()) {
            if (dialog != null) dialog.dismiss();
            return;
        }
        if (dialog != null && dialog.isShowing()) return;
        // 缺失权限时不提供伪装可继续浏览本机目录的按钮，也不静默落入空目录。
        // 用户仍可通过系统 Home 离开应用；本对话框不会主动授予任何权限。
        dialog = new AlertDialog.Builder(activity)
                .setTitle("需要文件访问权限")
                .setMessage("请先在系统设置中授予本应用“所有文件访问”权限，以便浏览本机目录。使用系统文件选择器时，还需选择工程目录并点击“使用此文件夹／允许”完成目录授权。")
                .setPositiveButton("前往授权", (window, which) -> requestAccess.run())
                .setCancelable(false)
                .create();
        dialog.show();
    }
}
