package top.xiang233.musicmapmaker;

import android.app.Activity;
import android.os.Build;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Set;

/**
 * 只为本 Activity 申请当前分辨率支持的最高刷新率，不修改系统全局显示设置。
 * Android 的物理显示模式与 Surface 帧率投票分别维护，两者都要跟随窗口恢复。
 * 请求只是应用偏好，最终选择仍由系统电源、温度和厂商策略决定。
 */
final class HighRefreshRate {
    /** Activity 由应用拥有，本辅助对象不持有其他应用窗口。 */
    private final Activity activity;
    /** 每个 SurfaceHolder 只安装一次回调，所有访问均发生在 Java UI 线程。 */
    private final Set<SurfaceHolder> attached = Collections.newSetFromMap(new IdentityHashMap<>());
    /** 当前窗口请求的帧率；不使用虚假的固定 144Hz 替代设备能力。 */
    private float requested;

    /** 在 Activity 创建后绑定，实际窗口操作延迟至 apply。 */
    HighRefreshRate(Activity activity) { this.activity = activity; }

    /** 在恢复和获得焦点时重新投票，防止 SDL 或系统重新创建 Surface 后丢失偏好。 */
    /**
     * @details 只枚举系统公开支持的当前分辨率模式，不调用厂商私有 API。
     * @warning 仅在 Java UI 线程恢复或焦点事件调用，禁止从绘制循环访问视图。
     */
    void apply() {
        Display display = activity.getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        Display.Mode best = current;
        // 只选择相同物理分辨率，避免刷新率请求意外改变渲染尺寸和资源需求。
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight()
                    && mode.getRefreshRate() > best.getRefreshRate()) best = mode;
        }
        // 多个模式同帧率时保持已选身份，避免无意义切换物理显示模式。
        requested = best.getRefreshRate();
        // Window 模式请求用于旧平台与模式选择，Surface 投票用于 Android 11 之后。
        WindowManager.LayoutParams attributes = activity.getWindow().getAttributes();
        attributes.preferredDisplayModeId = best.getModeId();
        attributes.preferredRefreshRate = requested;
        activity.getWindow().setAttributes(attributes);
        visit(activity.getWindow().getDecorView());
    }

    /** 沿本 Activity 的视图树定位实际绘制表面，不依赖 SDL 私有字段。 */
    private void visit(View view) {
        if (view instanceof SurfaceView) {
            SurfaceHolder holder = ((SurfaceView) view).getHolder();
            // Surface 可能晚于 onResume 创建，回调保证首次创建与后续替换都有投票。
            if (attached.add(holder)) holder.addCallback(new SurfaceHolder.Callback() {
                @Override public void surfaceCreated(SurfaceHolder value) { vote(value); }
                @Override public void surfaceChanged(SurfaceHolder value, int format, int width, int height) { vote(value); }
                @Override public void surfaceDestroyed(SurfaceHolder value) { /* 失效 Surface 不接受新投票。 */ }
            });
            // 已经创建的 Surface 不会重新触发 created，必须同时进行一次即时投票。
            vote(holder);
        }
        if (view instanceof ViewGroup) {
            // 只遍历当前应用的子树，不保存 View 的跨线程引用。
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); ++i) visit(group.getChildAt(i));
        }
    }

    /** 有效 Surface 才能投票；渲染更新频率继续由原项目配置控制。 */
    private void vote(SurfaceHolder holder) {
        // holder 可先于 Surface 存在；无效表面等待生命周期回调再投票。
        Surface surface = holder.getSurface();
        if (Build.VERSION.SDK_INT >= 30 && surface != null && surface.isValid()) {
            // DEFAULT 适合可变帧率的交互内容，不采用电影固定帧率的兼容模式。
            surface.setFrameRate(requested, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        }
    }
}
