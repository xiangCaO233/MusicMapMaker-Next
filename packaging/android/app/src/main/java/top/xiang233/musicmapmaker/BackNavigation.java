package top.xiang233.musicmapmaker;

import android.app.Activity;
import android.os.Build;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

/**
 * @brief 将 Android 返回导航接入原生工作区，不在 Java 线程直接关闭软件。
 * API 33 类型隔离在惰性加载的内部类中，API 28 仍走 Activity.onBackPressed。
 */
final class BackNavigation {
    // 回调以 Object 跨版本保存，避免旧系统主动解析新框架接口。
    // 实际类型只在 SDK 检查通过之后由 Api33 解包。
    /** 工具类没有可实例化状态，回调由 Activity 自己持有。 */
    private BackNavigation() {}

    /** 新系统注册预测性返回入口，旧系统只保留兼容回调。 */
    static Object register(Activity activity) {
        return Build.VERSION.SDK_INT >= 33 ? Api33.register(activity) : null;
    }

    /** 销毁时精确释放本次注册，不影响 SDL 或系统的其他回调。 */
    static void unregister(Activity activity, Object callback) {
        if (Build.VERSION.SDK_INT >= 33 && callback != null) Api33.unregister(activity, callback);
    }

    /** 只有受支持系统才加载本类型，避免旧版验证器解析缺失的框架接口。 */
    private static final class Api33 {
        /** 默认优先级允许 IME 先关闭键盘，应用再处理抽屉和退出确认。 */
        static Object register(Activity activity) {
            OnBackInvokedCallback callback = MainActivity::requestNativeBack;
            activity.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, callback);
            return callback;
        }

        /** 回调身份与注册时相同，禁止重新创建一个无法注销的 lambda。 */
        static void unregister(Activity activity, Object callback) {
            activity.getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback((OnBackInvokedCallback) callback);
        }
    }
}
