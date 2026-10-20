package top.xiang233.musicmapmaker;

import org.libsdl.app.SDLActivity;

/**
 * Android 仅提供 Activity 外壳，现有编辑器界面和业务逻辑仍由 C++ 实现。
 * SDL Java 实现来自构建使用的同一上游源码目录，不在仓库复制维护。
 */
public final class MainActivity extends SDLActivity {
    /** 先加载唯一的 SDL3，再加载包含业务模块的原生入口。 */
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
