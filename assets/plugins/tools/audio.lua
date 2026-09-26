-- 内置音频工具通过声明式控件与宿主音频服务通信。
-- 所有磁盘操作只在 on_action 中由用户明确触发。
-- 闭包状态只属于当前插件虚拟机，重载后重新初始化。
-- 路径始终由宿主文件选择器返回 UTF-8 字符串，脚本不自行打开文件。
-- 列表只展示宿主接收器已识别的常见后缀；实际可用性仍由 FFmpeg 构建决定。
-- .m4a 和 .aac 使用不同封装，不能只按编解码器名称合并成同一项。
-- .opus 与 .ogg 也需要保留独立后缀，接收器会据此选择不同音频编码。
-- lossy 仅表示是否展示目标码率；它不承诺具体编码器提供恒定码率。
-- 保存建议名和用户最终选择的路径必须使用同一张映射表。
local output_formats = {
    { label = "FLAC (.flac)", extension = ".flac", lossy = false },
    { label = "WAV (.wav)", extension = ".wav", lossy = false },
    { label = "MP3 (.mp3)", extension = ".mp3", lossy = true },
    { label = "Ogg (.ogg)", extension = ".ogg", lossy = true },
    { label = "AAC (.m4a)", extension = ".m4a", lossy = true },
    { label = "Opus (.opus)", extension = ".opus", lossy = true },
    { label = "AAC (.aac)", extension = ".aac", lossy = true },
}
local format_labels = {}
-- combo 协议返回可见文本，因此选项顺序和值都从格式表一次构造。
-- 不在每次 build 里重建该数组，避免插件控件缓存出现不同选择顺序。
for _, format in ipairs(output_formats) do
    format_labels[#format_labels + 1] = format.label
end

-- 显示文案只作选择值；扩展名是后端选择容器和编码器的依据。
-- 未知选择回退到初始无损格式，不能把任意文本拼进文件后缀。
-- 返回的格式表项在插件虚拟机生命周期内保持稳定。
local function selected_format(label)
    for _, format in ipairs(output_formats) do
        if format.label == label then return format end
    end
    return output_formats[1]
end

-- 文件选择器可能返回 Windows 或 POSIX 路径；只识别最后文件名中的后缀。
-- 大小写比较只作用于 ASCII 后缀，不改动用户选择的原始路径文本。
-- 未识别后缀保留为 nil，由调用方拒绝，而非猜测编码器。
local function format_from_path(path)
    local extension = path:match("%.[^%.\\/]+$")
    if not extension then return nil end
    extension = extension:lower()
    for _, format in ipairs(output_formats) do
        if format.extension == extension then return format end
    end
    return nil
end

local state = {
    -- 输入和输出用途分别记录最近目录，重复选择互不覆盖。
    input = "",
    output = "",
    -- 格式与目标后缀同步；改格式后清空旧路径以重新确认覆盖目标。
    format = output_formats[1].label,
    -- 文本输入便于原样保留用户编辑；提交时才转换成数值并验证。
    speed = "1.0",
    pitch = "0.0",
    -- 零使用后端默认值，不能把探测到的输入参数当作显式输出约束。
    sample_rate = "0",
    bitrate = "0",
    -- info 是一次媒体探测的快照，路径改变时由新的探测结果替换。
    info = nil,
    show_details = false,
    message = "请选择音频文件。",
}

-- 输入扩展名只用于解码器选择，输出扩展名决定容器及编码器。
-- 无损或 PCM 不接受目标码率，脚本导出时传零；切回有损格式保留原输入。
-- 源文件中的标签和封面目前只作展示，导出音频不会自动复制这些数据。
-- 最近目录由宿主按插件 ID 和用途写入配置目录，不保存在此闭包中。
-- 状态文字在任务完成时由宿主触发重建，不需要 Lua 定时器轮询。
-- 单个插件同时只运行一项导出，重复提交由宿主拒绝。
-- 不在脚本中保存后台任务对象，避免重载插件时留下悬空回调。

-- 探测失败也显示原始诊断，不把缺失字段当作成功读到的标签。
-- 时长是后端从帧数和时钟估算出的秒数；零表示无法确定。
local function describe(info)
    if not info then return "" end
    if info.error then return info.error end
    -- 标签允许为空；数值允许为零，不能用格式化错误中断窗口构建。
    return string.format(
        "标题：%s\n艺术家：%s\n专辑：%s\n时长：%.3f 秒\n声道：%d\n采样率：%d Hz\n码率：%d bit/s\n总帧数：%d\n封面：%s",
        info.title or "", info.artist or "", info.album or "",
        info.duration or 0, info.channels or 0, info.sample_rate or 0,
        info.bitrate or 0, info.frames or 0,
        info.cover_present and "已读取" or "无")
end

-- FFmpeg 的字典保留同名键与原始顺序；不能先转成 Lua 哈希表再展示。
-- 容器与每条流分开展示，避免把附图流误当作音频编码参数。
-- 缺失标签保留空章节，用户能区分没有标签与探测入口失败。
-- 行数组只在展开详情时构造，普通帧由宿主绘制缓存文本。
local function append_tags(lines, heading, tags)
    lines[#lines + 1] = heading
    if not tags or #tags == 0 then
        -- 保留章节标题，让没有标签的流也能与前后流明确分隔。
        lines[#lines + 1] = "  （无标签）"
        return
    end
    for _, tag in ipairs(tags) do
        -- 原值不做数值转换，日期、语言和自由文本均可原样查看。
        lines[#lines + 1] = string.format("  %s: %s", tag.key or "", tag.value or "")
    end
end

-- 详情只在用户展开后拼接，普通帧由宿主复用已声明的文本控件。
-- 容器时长与 ICE 估算时长分别展示，二者可能有不同精度。
-- 一条媒体可以同时含音频、视频、图片或额外流。
local function describe_details(info)
    if not info or info.error then return "" end
    if info.details_error then return info.details_error end
    -- 基础信息仍可用时只显示详情错误，不覆盖上方摘要及封面。
    local lines = {
        "容器：" .. (info.container_long_name or info.container or ""),
        string.format("容器时长：%.3f 秒", info.container_duration or 0),
    }
    append_tags(lines, "容器标签", info.format_tags)
    -- 逐流标签不与容器标签合并，避免相同键覆盖各自来源。
    for _, stream in ipairs(info.streams or {}) do
        -- 附图标志不代表像素上传完成，上方控件负责真实图片展示。
        -- 流序号保留 FFmpeg 原值，方便对应外部分析工具。
        lines[#lines + 1] = string.format(
            "流 #%d：%s / %s / %s bit/s / %s Hz / %s 声道 / %.3f 秒%s",
            stream.index or 0, stream.type or "", stream.codec or "",
            stream.bitrate or 0, stream.sample_rate or 0,
            stream.channels or 0, stream.duration or 0,
            stream.attached_picture and " / 附图" or "")
        append_tags(lines, "流标签", stream.tags)
    end
    return table.concat(lines, "\n")
end

return {
    type = "tool",
    id = "mmm.audio_transcoder",
    name = "音频信息与转码",

    build = function(api)
        -- build 仅在加载、用户动作和后台完成时执行；普通帧复用缓存控件。
        -- 后台进度另由 audio_progress 读取原子值，避免每帧进入 Lua。
        local status = api.audio_status()
        local status_text = state.message
        if status.state == "running" then
            -- 文本快照可用于没有进度控件的插件；本工具显示实时进度条。
            status_text = string.format("正在导出：%.1f%%", (status.progress or 0) * 100)
        elseif status.state == "success" then
            -- duration 是按输入时钟和送入编码器的帧数计算的导出时长。
            status_text = string.format("导出完成：%.3f 秒", status.duration or 0)
        elseif status.state == "error" then
            -- 编码器拒绝参数、文件打开失败等都沿同一错误通道呈现。
            status_text = "导出失败：" .. (status.error or "未知错误")
        end
        -- 输入信息与封面归为一组，输出参数在文件选择之后展示。
        local widgets = {
            { type = "button", id = "select_input", label = "选择输入音频" },
            { type = "text", id = "input_path", label = state.input },
            { type = "text", id = "media_info", label = describe(state.info) },
        }
        if state.info and state.info.cover_present then
            -- 图像纹理在宿主资源阶段上传，上传前先显示文字占位。
            -- 封面存在标志表示已经成功解码为可上传像素，不只检查附件标签。
            widgets[#widgets + 1] = {
                type = "image", id = "album_cover", label = "封面待上传或不可用"
            }
        end
        if state.info and not state.info.error then
            -- 基础摘要始终可见；较长的容器与逐流标签由用户按需展开。
            -- 展开状态只属于本插件，切换输出参数不会重新探测输入。
            -- 插件重载后闭包重置，不会持有关闭媒体的详情引用。
            widgets[#widgets + 1] = {
                type = "button", id = "toggle_details",
                label = state.show_details and "收起完整音频信息" or "显示完整音频信息",
            }
            if state.show_details then
                widgets[#widgets + 1] = {
                    type = "text", id = "media_details",
                    label = describe_details(state.info),
                }
            end
        end
        widgets[#widgets + 1] = { type = "separator", id = "export_section", label = "输出" }
        -- 先选格式，再生成对应的保存建议名；手动改后缀时反向同步此选择。
        -- 下拉项显示实际后缀，用户无需靠默认文件名推测可导出格式。
        -- 宿主仍会对编码器是否存在进行最终检查，界面不假定所有平台一致。
        widgets[#widgets + 1] = {
            type = "combo", id = "output_format", label = "输出格式",
            value = state.format, choices = format_labels,
        }
        widgets[#widgets + 1] = { type = "button", id = "select_output", label = "选择输出文件" }
        widgets[#widgets + 1] = { type = "text", id = "output_path", label = state.output }
        -- 宽面板中两列并排；窄面板自动纵排，标签始终位于输入框上方。
        -- 倍速与独立变调都由后台音频图处理，码率和采样率由编码器协商。
        -- 两组 row 只描述视觉排列；子控件 ID 仍直接到达 on_action。
        -- 最小列宽保证数字输入在侧栏中不会被压到无法阅读。
        widgets[#widgets + 1] = {
            type = "row", id = "speed_pitch", min_column_width = 230,
            children = {
                { type = "input", id = "speed", label = "倍速", value = state.speed },
                { type = "input", id = "pitch", label = "变调（半音）", value = state.pitch },
            },
        }
        -- 零值交给编码器默认协商，显式设置要求目标格式确实支持。
        -- 编码参数与播放参数分组，窗口横向空间不足时仍按业务顺序展示。
        -- 这里不复制参数值；重建控件树时直接读取当前闭包状态。
        local format = selected_format(state.format)
        -- 无损格式用文字替代输入框，明确当前填写的有损码率不会被使用。
        -- 保留闭包中的原码率，切回有损格式时不丢失用户刚输入的数值。
        widgets[#widgets + 1] = {
            type = "row", id = "encoding_options", min_column_width = 230,
            children = {
                { type = "input", id = "sample_rate", label = "输出采样率 Hz（0 为自动）", value = state.sample_rate },
                format.lossy
                    and { type = "input", id = "bitrate", label = "目标码率 bit/s（0 为默认）", value = state.bitrate }
                    or { type = "text", id = "lossless_bitrate", label = "无损格式不使用目标码率" },
            },
        }
        widgets[#widgets + 1] = { type = "button", id = "export", label = "开始导出" }
        if status.state == "running" then
            -- 进度由 C++ 缓存控件直接读取原子快照，无需每帧跨 Lua 边界。
            widgets[#widgets + 1] = { type = "audio_progress", id = "status", label = "正在导出" }
        else
            -- 完成状态恢复普通文本，实时条不再持有过期进度快照。
            widgets[#widgets + 1] = { type = "text", id = "status", label = status_text }
        end
        return widgets
    end,

    on_action = function(id, value, api)
        -- 只有明确点击按钮或修改参数才会到达这里；文件对话框可阻塞 UI。
        if id == "select_input" then
            local path = api.pick_file("audio_input")
            if path ~= "" then
                -- 成功选择后才替换旧探测；取消时保留上一份信息与参数。
                state.input = path
                state.info = api.audio_probe(path)
                -- 探测失败也保留路径，便于重新选择；状态文字必须报告真实结果。
                state.message = state.info.error or "已读取音频信息。"
            end
        elseif id == "output_format" then
            -- 旧路径只经过旧后缀的保存确认；切换格式后必须重新选择目标。
            -- 不能直接改写旧路径后缀，否则新路径若已存在会绕过保存确认。
            local format = selected_format(value)
            if state.format ~= format.label then
                state.format = format.label
                if state.output ~= "" then
                    state.output = ""
                    state.message = "输出格式已更改，请重新选择输出文件。"
                end
            end
        elseif id == "select_output" then
            -- 保存建议名和当前格式一致；手动输入其他已知后缀时尊重用户选择。
            -- 选择器返回的路径才是后端真实目标，不能只依赖下拉框显示值。
            local format = selected_format(state.format)
            local path = api.save_file("audio_output", "output" .. format.extension)
            if path ~= "" then
                local chosen = format_from_path(path)
                if chosen then
                    -- 用户手动改后缀时，码率控件也要随真正的目标编码切换。
                    state.output = path
                    state.format = chosen.label
                elseif path:match("%.[^%.\\/]+$") then
                    -- 未知后缀不能留着旧目标路径供误导出。
                    -- 后端虽然会报告错误，但这里先给出可操作的格式提示。
                    state.output = ""
                    state.message = "不支持该输出后缀；请选择列表中的音频格式。"
                else
                    -- 对话框尚未确认补后缀后的目标是否已有文件，必须重新选择。
                    state.output = ""
                    state.message = "请选择带 " .. format.extension .. " 后缀的输出文件。"
                end
            end
        elseif id == "toggle_details" then
            state.show_details = not state.show_details
        elseif id == "speed" then
            -- 编辑期间不启动 DSP；无效中间文本允许留在输入框内继续修正。
            state.speed = value
        elseif id == "pitch" then
            -- 半音值可以为负，负数表示降调；具体限制由宿主检查。
            state.pitch = value
        elseif id == "sample_rate" then
            -- 数值检查延后到提交，以免输入空字符串时反复弹错误。
            state.sample_rate = value
        elseif id == "bitrate" then
            -- bit/s 与常见的 kbit/s 不同，避免把 192 误认为 192 kbit/s。
            state.bitrate = value
        elseif id == "export" then
            -- Lua number 可能是小数；采样时钟和目标码率必须是整数。
            local speed, pitch = tonumber(state.speed), tonumber(state.pitch)
            local format = selected_format(state.format)
            local sample_rate = tonumber(state.sample_rate)
            local bitrate = 0
            -- 无损码率字段不可见，即使旧输入不是数字也不应阻止无损导出。
            -- 有损格式必须沿用真实用户值，不能把解析失败误当成默认零值。
            if format.lossy then bitrate = tonumber(state.bitrate) end
            if not speed or not pitch or not sample_rate or not bitrate or
                sample_rate < 0 or bitrate < 0 or
                sample_rate % 1 ~= 0 or bitrate % 1 ~= 0 then
                -- 允许 0 表示后端默认值；负数和小数没有有效编码器语义。
                state.message = "倍速、变调、采样率和码率必须是有效数字；后两项必须为非负整数。"
                return
            end
            if state.output == "" or format_from_path(state.output) ~= format then
                -- 防御外部脚本或保存对话框返回了与选择不一致的路径。
                state.message = "请选择与输出格式一致的目标文件。"
                return
            end
            -- 宿主再次验证数值范围、路径和编码器能力，脚本校验只负责输入体验。
            -- 返回空串仅表示任务进入队列，最终结果仍以 audio_status 为准。
            local error = api.audio_export({
                input = state.input, output = state.output,
                speed = speed, pitch_semitones = pitch,
                sample_rate = sample_rate, bitrate = bitrate,
            })
            state.message = error == "" and "正在准备导出..." or error
            -- 成功提交后控件树重建一次；后续进度由宿主 C++ 直接绘制。
        end
    end,
}
