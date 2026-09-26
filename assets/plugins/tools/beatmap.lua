-- 谱面工具用统一领域模型读取，再由原格式写出器生成可编辑文本预览。
-- IMD 是二进制格式，因此专用参数在保存前写回模型。
-- 闭包只存界面状态；谱面对象由宿主持有，脚本不直接操作 C++ 指针。
-- 输入文件始终只读，输出必须经过独立文件选择器指定目标路径。
-- 四种格式的数据模型并非完全同构，写出失败会由目标格式写出器报告。
-- 文本预览提供最终逐字节编辑能力，保存时不会覆盖人工修改。
-- 二进制 IMD 只开放可明确表达的字段，不把字节流伪装成文本。
-- 点击重新加载插件会清空闭包状态，用户应先保存已编辑的预览。
local state = {
    -- 当前输入与目标格式分离，防止覆盖原谱面。
    input = "", output = "", format = ".osu", info = nil,
    -- 详细数据可以很长，默认只显示基础信息与物件计数。
    show_details = false, preview = "", preview_ready = false,
    -- IMD 参数以原始文本保存，用户可以在无效中间值上继续编辑。
    imd_length = "", imd_bpm = "", imd_note_parameter = "0",
    message = "请选择 .mmm、.mc、.osu 或 .imd 谱面。",
}

local formats = { ".mmm", ".mc", ".osu", ".imd" }
-- 这些字段是四种格式共用的编辑入口；来源专属字段保留在 details 中。
-- 具体目标格式能否表达每一字段，由真实写出器在生成预览时检查。
-- 专辑字段进入跨格式模型；旧格式若没有对应字段，仍应以预览为准。
-- 内部名称与歌曲标题分开，避免把文件名错误覆盖为显示标题。
local editable = {
    { key = "name", label = "内部名称" },
    { key = "title", label = "歌曲标题" },
    { key = "title_unicode", label = "Unicode 标题" },
    { key = "artist", label = "艺术家" },
    { key = "artist_unicode", label = "Unicode 艺术家" },
    { key = "album", label = "专辑" },
    { key = "author", label = "谱师/作者" },
    { key = "version", label = "难度版本" },
}

-- 每次读取新输入时重新计算候选格式，确保来源后缀永不出现在目标列表。
-- 保持固定格式顺序，使同一谱面重载后默认选择稳定。
-- 此函数只处理 UI 候选项；宿主保存入口会再次检查后缀。
local function alternatives(source)
    local output = {}
    -- 新数组避免把共享的 formats 表在插件动作中误改。
    for _, format in ipairs(formats) do
        if format ~= source then output[#output + 1] = format end
    end
    return output
end

-- 使用真正的格式写出器生成文本，而不是复制输入文件或伪造 JSON。
-- 预览成功与否单独记录，元数据或目标格式变化后必须重新生成。
-- 宿主在配置目录临时区域写出再读回，预览不会覆盖目标路径。
-- 预览可能很长，逐字编辑需跳过控件树重建。
-- 临时预览由宿主清理；脚本只接收完整最终文本。
local function refresh_preview(api)
    state.preview_ready = false
    state.preview = ""
    -- 没有目标文件名时写出器无法判断目标格式。
    if state.output == "" then
        state.message = "请先选择输出路径。"
        return
    end
    local result = api.beatmap_preview(state.output)
    -- 兼容性失败会带 error，不能继续展示上一次成功生成的文本。
    if result.error then
        state.message = result.error
        return
    end
    state.preview_ready = true
    if result.binary then
        -- IMD 只展示结构化参数；二进制字节不进入文本编辑框。
        state.message = "IMD 为二进制格式；请调整专用参数后保存。"
    else
        -- 这是最终写出文本，用户之后可以逐字节修改再直接保存。
        state.preview = result.text or ""
        state.message = "已由真实写出器生成最终文本；可直接编辑后保存。"
    end
end

-- IMD 没有可编辑文本；每次保存前都要把输入控件值写回结构化模型。
-- 参数依次应用，任一失败立即停下，避免报告成功但写出部分旧值。
-- mapLength 使用非负 int32 毫秒数，不能依赖 Lua 浮点近似。
-- 首 BPM 必须对应真实 Timing 点，只有参考 BPM 不足以写出目标节奏。
local function apply_imd(api)
    local bpm = tonumber(state.imd_bpm)
    if not bpm then return "BPM 必须是数字。" end
    local error = api.beatmap_set_property("imd", "mapLength", state.imd_length)
    -- 首个 BPM 点是 IMD 的主要节奏参考；没有 BPM 点会返回错误。
    if error == "" then error = api.beatmap_set_imd_first_bpm(bpm) end
    if error == "" then
        -- 这里只统一调整普通 Note 的 Parameter，不改 Hold/Flick 的语义。
        error = api.beatmap_set_imd_note_parameter(state.imd_note_parameter)
    end
    return error
end

return {
    type = "tool",
    id = "mmm.beatmap_converter",
    name = "谱面格式转换",

    build = function(api)
        -- build 在加载和用户动作后产生声明式控件，普通帧由 C++ 缓存绘制。
        -- 尚未读取输入时只提供选择按钮和状态，避免无效的保存操作。
        local widgets = {
            { type = "button", id = "select_input", label = "选择输入谱面" },
            { type = "text", id = "input_path", label = state.input },
        }
        if state.info then
            -- info 是宿主读取时返回的快照；编辑后同步更新此表与宿主模型。
            -- 来源格式由输入后缀固定，不能将 .mc 属性解释成 .osu 属性。
            local info = state.info
            -- 基础信息优先展示，便于先判断轨数、速度与谱面长度。
            widgets[#widgets + 1] = {
                type = "text", id = "summary",
                label = string.format(
                    "来源：%s | 玩家轨：%d | BGM 轨：%d | BPM：%.3f | 长度：%.3f ms",
                    info.format or "", info.track_count or 0,
                    info.bgm_track_count or 0, info.bpm or 0,
                    info.map_length or 0),
            }
            -- 元数据字段统一用输入框；字段 ID 前缀区分按钮动作。
            for _, field in ipairs(editable) do
                widgets[#widgets + 1] = {
                    type = "input", id = "meta_" .. field.key,
                    label = field.label, value = info[field.key] or "",
                }
            end
            -- 特殊元数据含来源属性、背景、时间线和加载诊断，可能很长。
            -- 用户显式展开后才在窗口中绘制全文。
            widgets[#widgets + 1] = {
                type = "button", id = "toggle_details",
                label = state.show_details and "收起特殊元数据" or "显示特殊元数据",
            }
            if state.show_details then
                -- JSON 文本来自宿主真实模型，保持可审查且不要求脚本理解类型。
                -- 时间线与来源属性都显示，供用户判断目标格式可能丢失的信息。
                widgets[#widgets + 1] = {
                    type = "text", id = "details", label = info.details or "",
                }
            end
            -- 目标格式及路径放在修改入口后，让转换方向始终可见。
            widgets[#widgets + 1] = {
                type = "separator", id = "output_header", label = "输出转换",
            }
            widgets[#widgets + 1] = {
                -- 排除来源格式，避免把本工具误当作同格式的无损重写器。
                type = "combo", id = "output_format", label = "目标格式",
                value = state.format, choices = alternatives(info.format),
            }
            widgets[#widgets + 1] = {
                -- 用户选择输出位置，宿主按独立用途键记忆最近目录。
                type = "button", id = "select_output", label = "选择输出文件",
            }
            widgets[#widgets + 1] = {
                type = "text", id = "output_path", label = state.output,
            }
            if state.format == ".imd" then
                -- 二进制目标只开放写出器能够清楚表达的特定参数。
                -- 不尝试把任意字节转成文本，避免产出不可读的假预览。
                -- 应用参数只改内存模型，实际写盘必须点击保存。
                widgets[#widgets + 1] = {
                    type = "input", id = "imd_length", label = "IMD 声明长度（ms）",
                    value = state.imd_length,
                }
                widgets[#widgets + 1] = {
                    -- 首 BPM 与参考 BPM 不是同一字段，后端会修改 Timing 点。
                    type = "input", id = "imd_bpm", label = "首个 BPM 点",
                    value = state.imd_bpm,
                }
                widgets[#widgets + 1] = {
                    -- int32 值由宿主验证范围，不能在 Lua 浮点数中直接持久化。
                    type = "input", id = "imd_note_parameter",
                    label = "所有普通 Note 的 IMD Parameter",
                    value = state.imd_note_parameter,
                }
                widgets[#widgets + 1] = {
                    -- 可提前检查参数，也会在保存前再应用一次当前文本。
                    type = "button", id = "apply_imd", label = "应用 IMD 参数",
                }
            else
                -- 其它三种格式由真实写出器生成可现场修改的最终文本。
                -- 元数据变更会清除 ready，防止旧预览覆盖新模型。
                -- 刷新按钮让用户审查来源特殊字段在目标格式中的实际映射。
                widgets[#widgets + 1] = {
                    type = "button", id = "refresh_preview", label = "生成/刷新最终文本预览",
                }
                if state.preview_ready then
                    -- 大文本直接留在 C++ 输入缓存，逐字编辑不重建整棵控件树。
                    widgets[#widgets + 1] = {
                        type = "multiline", id = "preview", label = "最终文本",
                        value = state.preview,
                    }
                end
            end
            -- 保存时再次校验目标后缀；文本目标采用用户最后编辑的内容。
            widgets[#widgets + 1] = {
                type = "button", id = "save", label = "保存转换结果",
            }
        end
        -- 状态位于最下方，显示对话框取消、编码失败或保存成功的结果。
        widgets[#widgets + 1] = {
            type = "text", id = "status", label = state.message,
        }
        return widgets
    end,

    on_action = function(id, value, api)
        -- 文件访问只允许用户动作；build 只返回缓存声明，不触发读写。
        if id == "select_input" then
            -- 输入用途有独立最近目录；取消不会替换当前已载入的谱面。
            -- 宿主检查文件存在性和解析结果，脚本不猜测编码或读取字节。
            local path = api.pick_file("beatmap_input")
            if path == "" then return end
            local info = api.beatmap_read(path)
            -- 解析失败保留旧模型，用户可重新选择而不丢掉当前工作。
            if info.error then state.message = info.error; return end
            -- 新输入使旧目标及预览全部失效，防止误写到上次的文件。
            state.input, state.info = path, info
            state.output, state.preview, state.preview_ready = "", "", false
            state.format = alternatives(info.format)[1]
            -- IMD 参数从本次模型初始化，避免上一谱面的值污染转换。
            state.imd_length = tostring(math.floor((info.map_length or 0) + 0.5))
            state.imd_bpm = tostring(info.bpm or 0)
            state.message = "已读取谱面；选择另外一种格式输出。"
        elseif id == "toggle_details" then
            -- 展开只影响展示，不修改宿主 BeatMap。
            state.show_details = not state.show_details
        elseif id == "output_format" then
            -- 格式决定写出器和默认文件名，切换后必须重新选择目标。
            state.format = value
            state.output, state.preview, state.preview_ready = "", "", false
        elseif id == "select_output" then
            -- 文件选择器不强制后缀，所以脚本再核对用户实际输入的名字。
            -- 目标文件名不改变来源格式；来源始终属于已加载的模型。
            local path = api.save_file("beatmap_output", "converted" .. state.format)
            if path ~= "" then
                -- 下拉选择与真正的文件后缀保持一致，避免预览一种格式却保存另一种。
                local suffix = path:match("(%.[^./\\]+)$")
                if not suffix or suffix:lower() ~= state.format then
                    -- 错误时保留旧输出路径；下一次选择仍可以继续。
                    state.message = "输出文件后缀必须与目标格式一致：" .. state.format
                else
                    -- 路径确认后立即生成预览，用户看到的就是目标格式最终文本。
                    state.output = path
                    refresh_preview(api)
                end
            end
        elseif id == "refresh_preview" then
            -- 仅显式动作执行序列化，输入框逐字编辑不会重复写临时文件。
            refresh_preview(api)
        elseif id == "preview" then
            -- C++ 输入缓存已经更新，返回 false 避免每字重建整份大文本。
            -- 保存使用 state.preview，不再次调用写出器丢掉人工改动。
            state.preview = value
            return false
        elseif id == "imd_length" then
            -- 编辑阶段保持原样，保存前统一进行 int32 毫秒值验证。
            state.imd_length = value
        elseif id == "imd_bpm" then
            -- 首 BPM 只在应用时触及宿主 Timing 列表。
            state.imd_bpm = value
        elseif id == "imd_note_parameter" then
            -- 原始字符串保留符号和暂时空值，避免焦点移动时自动格式化。
            state.imd_note_parameter = value
        elseif id == "apply_imd" then
            -- 用户可以提前验证结构化参数，保存时仍重复应用最新输入。
            local error = apply_imd(api)
            state.message = error == "" and "IMD 参数已应用。" or error
        elseif id == "save" then
            -- 未选择路径不能推断输出目录；绝不覆盖来源文件。
            -- 宿主还会校验支持格式和输出不得与输入同后缀的约束。
            if state.output == "" then state.message = "请先选择输出文件。"; return end
            if state.format == ".imd" then
                -- 先验证所有参数，再交给 IMD 二进制写出器。
                local error = apply_imd(api)
                if error ~= "" then state.message = error; return end
            end
            if state.format ~= ".imd" and not state.preview_ready then
                -- 元数据已改而预览仍旧时阻止保存，要求用户检查新文本。
                state.message = "请先生成最终文本预览。"
                return
            end
            local error = api.beatmap_save(state.output, state.preview)
            -- 文本格式逐字节保存预览，IMD 则由宿主从结构化模型写出。
            state.message = error == "" and "转换结果已保存。" or error
        elseif id:sub(1, 5) == "meta_" and state.info then
            -- 同时维护可见快照与宿主模型；失败时 ready 仍失效以避免旧文本保存。
            -- 通用字段只通过白名单入口修改，脚本不能直接写 C++ 对象内存。
            local field = id:sub(6)
            state.info[field] = value
            local error = api.beatmap_set_field(field, value)
            -- 后端数值字段若解析失败，会保留原模型值并返回原因。
            if error ~= "" then state.message = error end
            state.preview_ready = false
        end
    end,
}
