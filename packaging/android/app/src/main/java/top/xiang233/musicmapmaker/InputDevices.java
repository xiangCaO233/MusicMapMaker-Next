package top.xiang233.musicmapmaker;

import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;

/**
 * @brief Activity 生命周期内跟踪实际鼠标与触摸板，不按最近一次触点猜测连接状态。
 * @details 设备枚举仅发生在初始化与系统通知时，原生每帧只读已发布快照。
 * 合成触摸、手写笔、只有键盘功能的设备不能解锁鼠标专用设置。
 */
final class InputDevices implements InputManager.InputDeviceListener {
    /** 系统服务只由 Android 主线程借用，不交给原生渲染或逻辑线程。 */
    private final InputManager manager;
    /** 主线程通知保证枚举、注册与注销顺序一致，不需要跨线程持有设备对象。 */
    private final Handler handler = new Handler(Looper.getMainLooper());

    /** @brief 注册后立即枚举，启动前已经连接的鼠标无需先移动才能识别。 */
    InputDevices(Context context) {
        manager = (InputManager) context.getSystemService(Context.INPUT_SERVICE);
        // 先注册再枚举，避免初始化窗口遗漏插拔通知。
        // SDLActivity 已完成库加载，发布只更新原生独立标量。
        if (manager != null) manager.registerInputDeviceListener(this, handler);
        refresh();
    }

    /** @brief Activity 销毁时解除监听，不能让旧实例继续发布设备状态。 */
    void close() {
        if (manager != null) manager.unregisterInputDeviceListener(this);
        // 下次 Activity 创建会重新枚举，不继承旧窗口的连接假设。
        MainActivity.updateMouseConnection(false);
    }

    /** @brief 只识别实际提供指针源的设备；虚拟键盘和触摸事件注入不算鼠标。 */
    private void refresh() {
        boolean connected = false;
        if (manager != null) {
            for (int id : manager.getInputDeviceIds()) {
                InputDevice device = manager.getInputDevice(id);
                // 通知与枚举之间设备可能已经移除，空条目必须忽略。
                // 使用完整 source 位掩码匹配，避免把所有 pointer 类设备都当鼠标。
                if (device != null && !device.isVirtual()
                        && (device.supportsSource(InputDevice.SOURCE_MOUSE)
                            || device.supportsSource(InputDevice.SOURCE_TOUCHPAD))) {
                    connected = true;
                    break;
                }
            }
        }
        // 多只鼠标只归约为一个能力值；拔掉其中一只不会误切回触控方案。
        // 原生不保存 InputDevice 或 Context，避免 Activity 生命周期泄漏。
        MainActivity.updateMouseConnection(connected);
    }

    /** @brief 新设备按当前完整集合判断，不能默认所有 USB 设备都是鼠标。 */
    @Override public void onInputDeviceAdded(int id) { refresh(); }
    /** @brief 移除后重新归约，仍连接的第二只鼠标继续提供指针能力。 */
    @Override public void onInputDeviceRemoved(int id) { refresh(); }
    /** @brief 设备 source 改变或重新启用同样需要刷新能力。 */
    @Override public void onInputDeviceChanged(int id) { refresh(); }
}
