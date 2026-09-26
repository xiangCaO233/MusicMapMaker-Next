# Lua 工具插件开发指南

工具插件位于用户配置目录的 `plugins/tools/`，每个 `.lua` 文件对应一个独立 Lua
状态。应用启动时加载一次；在“工具 → 插件列表”中打开窗口，在“工具 → 重载插件”
中重新读取脚本。窗口使用插件 `id` 作为固定 ImGui 内部标识，标题改名不会丢失
停靠位置。

复制下面的最小示例为配置目录中的 `plugins/tools/hello.lua`，再点击“工具 →
重载插件”，即可在“工具 → 插件列表”的“工具插件”区域打开。内置插件的源码
位于仓库 [`assets/plugins/tools/`](../../assets/plugins/tools/)；构建时嵌入程序，
不会覆盖配置目录里的用户脚本。插件目前没有版本协商字段；升级宿主时应检查
本指南中的 API，并保持已发布的 `id` 不变，以保留窗口停靠和最近目录状态。

## 最小示例

```lua
local state = { name = "" }

return {
    type = "tool",
    id = "example.hello",
    name = "示例工具",

    build = function(api)
        return {
            { type = "text", id = "help", label = "输入名称后点击按钮。" },
            { type = "input", id = "name", label = "名称", value = state.name },
            { type = "button", id = "greet", label = "显示问候" },
            { type = "text", id = "result", label = state.message or "" },
        }
    end,

    on_action = function(id, value, api)
        if id == "name" then
            state.name = value
        elseif id == "greet" then
            state.message = "你好，" .. state.name
        end
    end,
}
```

入口脚本必须返回表。`type` 固定为 `tool`；`id` 长度为 1–128，只允许 ASCII
字母、数字、`.`、`_`、`-`，发布后应保持稳定；`name` 是非空窗口标题。
`build(api)` 返回顺序控件数组，`on_action(id, value, api)` 在用户改变控件或
点击按钮后调用。两个函数都必须存在。每次动作结束后宿主重新调用 `build`，
然后把结果缓存为 C++ 控件；普通帧只绘制缓存，不执行 Lua。

插件加载时仅开放 Lua 的 `base`、`table`、`string`、`math` 库。不开放 `io`、
`os`、`package`；文件访问通过明确的宿主 API 完成。各插件使用独立 Lua
状态，不能通过全局变量通信。成功加载后，回调错误或控件声明错误会显示在
插件窗口中；入口语法错误、清单字段无效或重复 `id` 会在工具列表保留一条
不可打开的错误项，显示脚本路径及失败原因。
插件脚本不应依赖执行顺序，或把 `api` 保存到后台线程。

## 控件声明

每个控件都要有插件内唯一的非空 `id`。`label` 是可见文本，`value` 是字符串。

| `type` | 用途 | `value` / `choices` |
| --- | --- | --- |
| `text` | 自动换行的只读文字 | `label` 是正文 |
| `separator` | 带标题分隔线 | `label` 是标题 |
| `button` | 带统一悬浮和点击反馈的按钮 | 点击时 `value` 为空字符串 |
| `input` | 单行文本输入 | `value` 为当前文本，编辑时按需扩容 |
| `multiline` | 多行文本输入 | `value` 为当前文本，编辑时按需扩容 |
| `checkbox` | 布尔开关 | `value` 为 `"true"` 或 `"false"` |
| `combo` | 下拉选择 | `value` 为当前项，`choices` 为字符串数组 |
| `image` | 展示最近一次 `audio_probe` 读取的专辑封面 | 上传中显示 `label` |
| `audio_progress` | 绘制当前插件后台导出的实时进度条 | `label` 是进度提示文本 |

输入控件的 `on_action` 会在内容变化时调用，插件应立即把新值保存到自身
`state`，供下一次 `build` 返回。长文本输入会随用户编辑按需增长；插件应避免
每次按键都重新生成整份大文本。`on_action` 返回 `false` 时宿主保留当前 C++
控件树而不再次调用 `build`，适合最终文本预览的现场编辑。返回其他值或没有
返回值时会重新构造控件树。

`checkbox` 的动作值是字符串 `"true"` 或 `"false"`；`combo` 的动作值为所选
文本。`button` 的动作值为空字符串。每个控件通过 `id` 在窗口中隔离 ImGui
状态；同一插件不应在不同位置重复使用同一个 `id`。当前控件协议不提供直接
执行原始 ImGui 命令的入口；需要新增控件时应先在宿主定义可缓存的声明类型。

## 文件选择与最近目录

`api.pick_file(purpose)` 打开输入文件选择器；
`api.save_file(purpose, suggested_name)` 打开输出文件选择器。返回 UTF-8
绝对路径；用户取消时返回空字符串。只能在 `on_action` 中调用，在 `build`
或加载期调用会返回空字符串。`purpose` 是插件自己决定的非空用途键，例如
`"audio_input"`、`"audio_output"`，同一插件的不同用途不会互相覆盖。

每个插件的用途键和最近目录保存在用户配置目录
`plugin-state/<plugin-id>.json` 中。状态按插件 `id` 隔离，重载和应用重启后继续
使用；只有用户成功选择文件时才写磁盘。插件的 `id` 改变时会得到一份新的
最近目录状态。

```lua
on_action = function(id, value, api)
    if id == "choose_input" then
        local path = api.pick_file("audio_input")
        if path ~= "" then state.input = path end
    elseif id == "choose_output" then
        local path = api.save_file("audio_output", "result.flac")
        if path ~= "" then state.output = path end
    end
end
```

## 帧时间与耗时工作

插件的 `build` 只在加载或用户动作后执行，普通帧没有 Lua/C++ 回调开销。
文件选择器会在用户点击后阻塞当前 UI 线程；音频解码、转码和谱面批量写出等
长任务应由宿主后台服务执行，不能放进 `build` 或任何逐帧绘制代码。窗口
`ImGui::Begin`/`End` 由宿主配对，插件不能直接破坏 ImGui 栈。

LuaJIT 边界的合成测试（系统 LuaJIT 2.1、200000 测量帧）得到：一次空
C++→Lua 调用约 `0.008 µs/帧`；一次 C++→Lua 加 40 次空 Lua→C 调用约
`0.294 µs/帧`。这仅测调用边界，不包含真实 ImGui 控件、纹理、文件 I/O
或音频处理；不能把它当作整帧性能结论。声明式缓存使常态绘制与该边界开销
无关。

与宿主通信的所有 `api` 函数在 UI 线程由用户动作触发。`audio_export` 只提交
后台任务，其进度可由 `audio_progress` 控件逐帧读取，最终结果通过
`audio_status` 返回；谱面读取、预览生成和保存目前是
同步低频操作，大文件操作期间窗口可能短暂停顿。插件应把昂贵调用放在按钮
动作中，避免每次输入字符都重复完整读取或序列化。状态以 Lua 闭包保存；
“重载插件”会重新执行脚本并丢弃这份内存状态。

## 音频宿主 API

内置实现可见于 [`assets/plugins/tools/audio.lua`](../../assets/plugins/tools/audio.lua)。
读取和导出操作都只能在 `on_action` 中发起。

| 调用 | 返回值 | 说明 |
| --- | --- | --- |
| `api.audio_probe(path)` | 表 | 读取标题、艺术家、专辑、时长、总帧数、声道、采样率、码率与封面存在状态；失败时返回 `error`。封面像素在低频资源准备阶段上传，随后由 `image` 控件显示。 |
| `api.audio_export(options)` | 错误字符串 | 空字符串表示后台任务已提交；`options` 包含 `input`、`output`、`speed`、`pitch_semitones`、`sample_rate`、`bitrate`。非空字符串解释拒绝原因。 |
| `api.audio_status()` | 表 | `state` 为 `idle`、`running`、`success` 或 `error`；运行时带 `progress`，结束时带 `error`、`frames`、`duration`。 |

`audio_export` 的变调单位是半音，`0` 保留原音高，支持范围为 `-48` 到
`+48`。输出容器和编码器由文件扩展名选择，是否支持某个扩展名取决于本机
FFmpeg 构建。`sample_rate` 是目标 Hz，`bitrate` 是目标 bit/s；均为非负整数，
`0` 使用编码器默认值。显式采样率必须被目标编码器精确支持；无损或 PCM
格式不接受显式目标码率。后台任务不访问 Lua 或 ImGui；脚本可在下一次用户
操作后调用 `audio_status` 查看结果。如果安装的是尚未更新该接口的 ICE
预编译包，设置非零采样率或码率会返回明确错误；不会假装应用了参数。
当前 macOS 预编译库仍需在原生 Runner 上重建，因此 macOS 上这两个非零参数
也会返回上述错误，默认参数的音频导出仍可使用。

```lua
local error = api.audio_export({
    input = state.input,
    output = state.output,
    speed = 1.25,
    pitch_semitones = -2,
    sample_rate = 48000,
    bitrate = 192000,
})
if error ~= "" then state.message = error end
```

文件后缀控制实际编码格式，`bitrate` 只设置目标有损码率，不保证压缩器做到
逐秒恒定码率。输出文件不会携带输入音频中的图片或标签；`audio_probe` 的
封面只用于界面展示。导出失败时后端可能留下未完成的输出文件，插件不应将其
标记为可交付结果。`audio_status().frames` 是送入编码器的输入时钟帧数，
显式改变采样率后与容器内部的样本数不同；`duration` 由该输入时钟推算。

## 谱面宿主 API

内置实现可见于 [`assets/plugins/tools/beatmap.lua`](../../assets/plugins/tools/beatmap.lua)。
输入格式为 `.mmm`、`.mc`、`.osu`、`.imd`。`api.beatmap_read(path)` 返回
基本元数据、四类物件与 Timing 等计数，以及按来源列出的 `details` JSON
文本；其中包含谱面属性、背景设置、时间线点、物件的来源属性及加载诊断。
失败时返回 `error`。输出格式必须从输入格式以外的三种格式中选择。

| 调用 | 返回值 | 说明 |
| --- | --- | --- |
| `api.beatmap_set_field(key, value)` | 错误字符串 | 修改内部名称、标题、Unicode 标题、艺术家、Unicode 艺术家、专辑、作者、版本、长度或参考 BPM。 |
| `api.beatmap_set_property(source, key, value)` | 错误字符串 | 修改来源格式属性；`source` 为 `osu`、`malody` 或 `imd`。 |
| `api.beatmap_set_imd_first_bpm(number)` | 错误字符串 | 调整第一个 BPM Timing 点。 |
| `api.beatmap_set_imd_note_parameter(text)` | 错误字符串 | 为所有普通 Note 写入 int32 `Parameter`。 |
| `api.beatmap_preview(output_path)` | 表 | 调用真实写出器生成文本格式的 `text`；IMD 返回 `binary=true`。失败时返回 `error`。不会覆盖目标文件。 |
| `api.beatmap_save(output_path, text)` | 错误字符串 | 文本格式逐字节写入调用方调整后的预览；IMD 从当前模型写出二进制。 |

`beatmap_read` 成功时返回的表包含 `format`（带点的来源后缀）、`name`、
`title`、`title_unicode`、`artist`、`artist_unicode`、`album`、`author`、
`version`、`track_count`、`bgm_track_count`、`bpm`、`map_length`，以及
`details`（格式化 JSON 字符串）。失败时只需检查 `result.error`，此前成功
载入的谱面模型会保留。`details` 中的 `counts` 分别给出普通 Note、Hold、
Flick、折线、Timing、音频事件与注释数量；`source_properties`、
`note_source_properties` 和 `timing_points` 展示各来源格式的特殊字段，另有
`resource_paths`、`background` 与 `load_diagnostics`。折线子物件同样存于
Note/Hold/Flick 容器，其来源属性可出现在 `note_source_properties` 中。

文本预览生成于配置目录的临时区域，成功读取后会删除。内置工具在用户编辑
标题等元数据后要求重新生成预览；用户直接编辑预览文本时不会再次序列化，
点击保存会写出当前文本。IMD 的声明长度通过来源属性 `mapLength` 设置；
其隐式音频命名和采样绑定等可表达性约束由现有 IMD 写出器检查，失败会作为
错误返回，不会悄悄丢弃这些数据。内置工具在保存 IMD 时会再次应用当前输入
框中的参数，避免只修改控件文本却写出旧值。

`beatmap_set_field` 修改当前插件内的谱面模型，不会修改用户输入文件。修改后
应重新调用 `beatmap_preview`，使文本输出与模型一致。IMD 没有可编辑的最终
文本；应通过 `beatmap_set_property`、`beatmap_set_imd_first_bpm` 和
`beatmap_set_imd_note_parameter` 等结构化入口修改。输出路径后缀必须是输入
格式以外的三种之一，`beatmap_save` 会再次核对后缀。文本格式保存时直接写入
传入的 `text`，调用方必须保存并传入用户最后编辑的预览全文。
