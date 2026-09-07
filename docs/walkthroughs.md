# 演练主题与分支流程

## 入口和目录

“帮助 → 欢迎”打开独立欢迎标签页。首页仅展示演练主题卡片，不放新建、打开项目等启动操作，也不在画布叠加入口。
底部“启动时显示欢迎页”默认开启，保存到用户设置 `showWelcomeOnStartup`；关闭标签页不改变该偏好，关闭自动显示后仍可从帮助菜单手动打开。
点击卡片后在同一标签页展示内嵌演练，通过顶部“返回欢迎页”回到主题目录。主题正文采用窄版居中排版，分支通过圆形进度标记和细描边展开卡片展示，每次展开一个分支。
关闭窗口或返回首页不影响业务事件计数，重新进入继续显示已有进度。

- 内置主题源：`assets/walkthroughs/open-project.json`，构建时嵌入程序，避免安装路径影响基础教程。
- 示例资源：`assets/walkthroughs/canonrock/`。当前示例包含音频和两张图片，不包含谱面或谱包；谱面拖放和谱包拖放分支需要自行准备对应文件。
- 构建同步后的资源：用户配置根的 `assets/walkthroughs/`。不要在受管资源目录直接编辑项目，应先复制到自己的练习目录。
- 自定义主题：用户配置根的 `walkthroughs/*.json`，下次启动读取。使用不同的主题 ID，不覆盖内置主题。
- 学习进度：用户配置根的 `walkthrough-progress.json`，独立于项目、`user_config.json` 和 `imgui.ini`。

## 最小声明

```json
{
  "id": "my.open-project",
  "version": 1,
  "title": {"zh_cn": "打开项目练习", "en_us": "Open a project"},
  "description": "下面的操作分支可以独立学习。",
  "completion": "any",
  "branches": [
    {
      "id": "folder",
      "title": "文件夹拖放",
      "steps": [
        {
          "id": "prepare",
          "title": "准备目录",
          "body": "在系统文件管理器中找到项目目录。"
        },
        {
          "id": "drop",
          "title": "拖入目录",
          "body": "关闭模态窗口，将一个文件夹拖到软件中。",
          "requires": ["prepare"],
          "signals": ["project.folder_drop.ready"],
          "match": "all"
        }
      ]
    }
  ]
}
```

标题、简介和正文可写字符串或以语言标识为键的对象。正文使用现有 Markdown 渲染器；语言缺失时优先回退 `en_us`。

`completion` 定义主题目标：`any` 表示任一分支完成即可，`all` 表示所有分支完成。
目标完成不隐藏其他分支；进度条仍显示所有步骤的学习覆盖率。

`requires` 引用同一主题内的步骤 ID，允许跨分支引用；仅约束自动完成和操作按钮，不限制阅读或手动“已了解”。
`signals` 非空时才会自动完成，`match` 可选 `any` 或 `all`。
各步骤的“已了解”独立生效，不模拟业务操作、不修改项目。

加载时检查重复 ID、缺失引用和循环依赖。单主题最多 32 个分支、128 个步骤、1 MiB JSON。
稳定步骤 ID 用于保留升级后的学习记录；修改正文或递增内容版本不会清空进度。

## 业务信号与操作

| 信号 | 判定时机 |
| --- | --- |
| `project.menu.picker` | 文件菜单唤起目录选择器 |
| `project.menu.ready` | 对应菜单请求已打开项目 |
| `project.shortcut.picker` | Ctrl+O 唤起目录选择器 |
| `project.shortcut.ready` | 对应快捷键请求已打开项目 |
| `project.folder_drop.ready` | 全局文件夹拖放请求已打开项目 |
| `project.beatmap_drop.ready` | 画布拖放请求已打开项目，且指定谱面成功创建会话 |
| `project.package_drop.ready` | 拖放谱包已作为临时只读项目打开 |

业务层只提供 `ProjectOpenOrigin` 与 `ProjectOpenInteractionEvent`，不引用主题或步骤 ID。
入口信息随项目切换请求传递，包括等待旧画布关闭的阶段，最终由逻辑层发布结果。
取消、解析失败及未创建谱面会话不产生相应成功信号。
选择器打开只完成“唤起选择器”步骤，不代表项目打开成功。

步骤的可选 `action` 字段引用可信 C++ 操作注册表。当前注册 `open_folder`，用于直接唤起目录选择器。
该操作不冒充文件菜单或快捷键操作，因此不会替这两条入口分支自动计数。
未知操作显示为禁用；JSON 和 Markdown 均不能执行任意脚本或系统命令。

## 维护边界

- `WalkthroughModel`：数据校验和纯状态归约，不依赖 ImGui。
- `WalkthroughService`：目录、操作注册、跨线程事件队列和进度文件；由 UIManager 持有，不属于窗口。
- `WelcomeView`：主题卡片目录、返回导航、停靠和启动显示偏好。
- `WalkthroughPage`：欢迎页中的演练正文，不创建独立窗口。
- `ProjectDropRouter`：主窗口文件夹/谱包拖放入口，不依赖演练是否打开。
- 磁盘扫描只发生在目录加载；进度保存只发生在进度变化后。损坏进度文件保留，不自动覆盖。
- 系统拖放一次只处理一个项目入口；模态窗口或文件操作占用期间不打开新项目。

验证入口：`WelcomeViewTest`、`WalkthroughTest`、`ProjectOpenOriginTest`，测试输出使用构建目录的 `test_output/`。
