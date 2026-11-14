package top.xiang233.musicmapmaker;

import android.app.Activity;
import android.graphics.Rect;
import android.os.Build;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.WindowInsets;
import android.view.WindowInsetsAnimation;
import java.util.List;

/**
 * @brief 仅观察本窗口键盘遮挡，SDL 继续拥有文本输入与输入法生命周期。
 * 零尺寸子 View 不参与绘制、焦点或触摸，不替换 SDL 安装的 Insets 监听器。
 * 所有字段只在 Activity 线程访问；JNI 只发布两个整数，不传递 View。
 */
final class SoftKeyboard extends View implements ViewTreeObserver.OnGlobalLayoutListener {
    /** 参考区域是 Activity 内容容器，系统状态栏不计作键盘。 */
    private final ViewGroup m_content;
    /** 复用坐标缓冲，键盘动画每帧不创建额外 Rect 或数组。 */
    private final Rect m_visible = new Rect();
    /** 坐标数组只由 UI 回调复用，不能传递给原生线程持有。 */
    private final int[] m_location = new int[2];
    /** 相同几何不跨 JNI 重复发布；关闭标志避免拆卸后的延迟事件。 */
    private int m_bottom = -1, m_height = -1;
    /** 拆卸后可能仍收到动画尾帧，此标志使它不能覆盖关闭快照。 */
    private boolean m_closed;
    /** 原生边界仅接收几何，不负责开启／关闭输入法。 */
    static native void publishOcclusion(int bottom, int height);

    /** SDL 初始化后安装被动观察子节点，不修改其 Surface 或输入视图。 */
    SoftKeyboard(Activity activity) {
        super(activity);
        m_content = activity.findViewById(android.R.id.content);
        setFocusable(false);
        setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        // 用零尺寸旁路观察，不为辅助节点扩大点击区域或改变 Surface 布局。
        // 这里保留父容器，因此 Activity 销毁时能准确移除同一节点。
        m_content.addView(this, new ViewGroup.LayoutParams(0, 0));
        m_content.getViewTreeObserver().addOnGlobalLayoutListener(this);
        // 新系统在键盘动画过程中也发布几何，避免只在动画结束后跳动布局。
        // 回调装在本观察节点，不覆盖根容器及 SDL 的回调。
        if (Build.VERSION.SDK_INT >= 30) Api30.observe(this);
        // 首次安装也请求快照，覆盖 Activity 创建时键盘已经可见的场景。
        requestApplyInsets();
    }

    /** 常规 Insets 分派仍原样向后传递，不能吞掉 SDL 所需的系统安全区。 */
    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        update(insets);
        return insets;
    }

    /** 旋转和旧系统可视框变化也重新计算，不使用固定键盘高度。 */
    @Override public void onGlobalLayout() { update(getRootWindowInsets()); }

    /** 仅在 UI 线程比较与发布；从内容容器坐标换算实际重叠区域。 */
    private void update(WindowInsets insets) {
        if (m_closed) return;
        // 每次从无遮挡开始计算，收起、旋转与外接键盘不能沿用上一帧高度。
        int height = m_content.getHeight(), bottom = 0;
        if (Build.VERSION.SDK_INT >= 30 && insets != null) {
            // IME 不可见时不使用导航栏高度，硬件键盘也不会压缩业务弹窗。
            // Android 新版支持 ADJUST_NOTHING 下单独报告 IME Insets。
            if (insets.isVisible(WindowInsets.Type.ime())) {
                View decor = getRootView();
                m_content.getLocationInWindow(m_location);
                // Insets 相对整个窗口；内容容器可能带有系统栏偏移。
                // 先换到同一窗口坐标系再取交集，不能直接减一个物理高度。
                int keyboardTop = decor.getHeight() - insets.getInsets(WindowInsets.Type.ime()).bottom;
                bottom = Math.max(0, m_location[1] + height - keyboardTop);
            }
        } else {
            // API 26～29 没有类型化 IME Insets；只认明显大于系统栏的可视框差。
            // 使用容器实际底边，避免把顶部状态栏或横屏刘海计入遮挡。
            m_content.getWindowVisibleDisplayFrame(m_visible);
            m_content.getLocationOnScreen(m_location);
            int overlap = m_location[1] + height - m_visible.bottom;
            if (overlap > height / 5) bottom = overlap;
        }
        // 动画与尺寸回调的顺序由系统决定，过渡帧也必须保持合法交集。
        bottom = Math.max(0, Math.min(height, bottom));
        // 原生以内容高度作分母，物理 DPI 和 ImGui framebuffer 缩放互不混淆。
        if (bottom != m_bottom || height != m_height) {
            m_bottom = bottom;
            m_height = height;
            publishOcclusion(bottom, height);
        }
    }

    /** 拆卸时只移除本观察者；旧 Activity 的尺寸不能污染新窗口。 */
    void close() {
        // 先封锁回调再拆卸，避免移除节点触发的布局事件重新发布旧尺寸。
        m_closed = true;
        m_content.getViewTreeObserver().removeOnGlobalLayoutListener(this);
        m_content.removeView(this);
        publishOcclusion(0, 0);
    }

    /** 新框架类型按版本惰性加载，旧系统仍可使用全局布局回退。 */
    private static final class Api30 {
        /** 动画回调不消费 Insets，父子视图继续按平台默认顺序分派。 */
        static void observe(SoftKeyboard view) {
            view.setWindowInsetsAnimationCallback(new WindowInsetsAnimation.Callback(
                    WindowInsetsAnimation.Callback.DISPATCH_MODE_CONTINUE_ON_SUBTREE) {
                @Override public WindowInsets onProgress(WindowInsets insets, List<WindowInsetsAnimation> running) {
                    // 只复制几何，禁止在动画帧里触发业务布局或开启输入法。
                    view.update(insets);
                    return insets;
                }
            });
        }
    }
}
