#include "ui/imgui/menu/actions/MainMenuToolsActions.h"

#ifndef __ANDROID__

#    include "StemSeparationJob.h"
#    include "audio/AudioManager.h"
#    include "audio/AudioTimelineMixerNode.h"
#    include "audio/StemAudition.h"
#    include "config/Utf8Path.h"
#    include "runtime/AppThreadPool.h"
#    include "ui/imgui/menu/MainMenuTypes.h"
#    include "ui/utils/NativeFileDialog.h"
#    include "ui/utils/UIWidgetUtils.h"

#    include <ice/config/config.hpp>
#    include <ice/thread/ThreadPool.hpp>
#    include <imgui.h>

#    include <chrono>
#    include <cstddef>
#    include <cstdint>
#    include <expected>
#    include <future>
#    include <memory>
#    include <optional>
#    include <stop_token>
#    include <string>
#    include <string_view>
#    include <utility>
#    include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 从工具菜单管理一组独立试听文件。
/// @details 编辑器主谱面和试听工具各自持有播放头，只有输出设备共享。
/// 音轨列表、选择记录和异步任务均由当前动作实例持有。
/// 下一帧可从已经完成的文件任务安装新目录，旧目录一直保留到接入成功。
/// 工程轨道及存盘状态由原入口管理，试听的源文件可以在工程之外。
/// 工具关闭时让 in-flight 结果失效，任务捕获的文件路径仍由后台持有。
class OpenStemAuditionAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 工具随主窗口销毁时通知后台取消，不等待推理进程。
    /// @details 主窗口析构会先于应用文件池销毁，池内任务可收到停止请求。
    ~OpenStemAuditionAction() override { m_separationStop.request_stop(); }

    /// @brief 暴露固定的工具身份，标题语言不会影响分派。
    /// @warning UI 热路径，仅返回静态字符串。
    std::string_view operationId() const override
    {
        return "OpenStemAuditionAction";
    }

    /// @brief 打开试听面板，文件选择由作者在面板中触发。
    /// @details 快捷键与菜单使用相同窗口身份，重复触发只保留可见状态。
    /// @warning 仅 UI 线程改变窗口可见性，工作线程不访问此动作。
    void execute(MainMenuContext&, const MainMenuItemActivation&) override
    {
        // 每个动作实例只恢复一次配置，后续打开沿用当前环境。
        if ( !m_environment ) m_environment = loadStemSeparationEnvironment();
        m_open = true;
    }

    /// @brief 轮询后台准备结果，呈现当前轨道和共享播放头。
    /// @details 轮询在 Begin 之前发生，以便折叠时仍消费已经完成的载入。
    /// 解码没结束则继续使用现有试听。窗口关闭后也应收回异步结果。
    /// @warning 热路径仅在结果就绪时执行安装，平时不访问文件系统。
    /// @warning UI 热路径。只消费已就绪结果，禁止等待解码或资源池。
    void renderDeferred(MainMenuContext& context) override
    {
        using namespace std::chrono_literals;
        if ( m_load.valid() &&
             m_load.wait_for(0s) == std::future_status::ready ) {
            // 即使面板已关闭，也消费完成的任务，避免重新打开时安装过期结果。
            auto result = m_load.get();
            if ( m_open && !m_discardLoad ) install(std::move(result));
        }
        // 无需打开窗口即可清理已经完成且应丢弃的载入结果。
        if ( !m_open ) return;

        // 初次打开为模型清单和四轨混音台留出空间，之后尊重用户保存的布局。
        ImGui::SetNextWindowSize(
            ImVec2(680.0F * context.dpiScale, 580.0F * context.dpiScale),
            ImGuiCond_FirstUseEver);
        // Begin/End 总是配对。折叠只停止绘制，显式关闭才断开混音节点。
        if ( ImGui::Begin("声部分离###StemAudition", &m_open) ) {
            // 一个窗口同时只拥有一份准备请求，避免覆盖仍在用解码池的 future。
            ImGui::BeginDisabled(m_load.valid());
            drawSeparationControls();
            if ( FeedbackButton("导入已有分轨") ) loadFiles();
            ImGui::EndDisabled();

            // 即使保留上一组播放，也向作者说明正在准备新的输入。
            if ( m_load.valid() )
                ImGui::TextUnformatted(m_separating ? "正在运行模型并准备音轨…"
                                                    : "正在解码…");
            // 文件导入不拥有外部进程，取消按钮只在模型作业排队或运行时出现。
            if ( m_load.valid() && m_separating ) {
                if ( FeedbackButton("取消分离") )
                    m_separationStop.request_stop();
            }
            if ( !m_error.empty() ) ImGui::TextWrapped("%s", m_error.c_str());
            if ( m_playback ) drawTrackControls(context.dpiScale);
        }
        ImGui::End();

        // 无论是否折叠，关闭窗口都会从主输出移除试听，防止留下隐形声源。
        if ( !m_open ) {
            releasePlayback();
            m_discardLoad = true;
            m_separationStop.request_stop();
            m_error.clear();
        }
    }

private:
    /// @brief 文件任务返回的不可变目录，错误在 UI 线程显示。
    using LoadResult = std::expected<Audio::StemAudition, std::string>;

    /// @brief 只在用户点击后调用原生选择器，接管 UTF-8 路径后释放 NFD 内存。
    /// @param filter 可选的文件类型限制，空值允许模型环境和音频的各平台扩展名。
    /// @return 已拷贝的路径，取消或对话框失败时返回空。
    /// @warning 选择对话框可等待用户，禁止从普通渲染帧无条件调用。
    std::string pickFile(const nfdu8filteritem_t* filter = nullptr)
    {
        nfdu8char_t* path = nullptr;
        const auto   status =
            NativeFileDialog::openFile(&path, filter, filter ? 1 : 0, nullptr);
        if ( status != NFD_OKAY || !path ) {
            if ( status == NFD_ERROR ) m_error = "无法打开文件选择器。";
            return {};
        }
        // NFD 使用自己的分配方式。复制之后立即使用对应的释放 API。
        std::string selected(path);
        NFD_FreePathU8(path);
        return selected;
    }

    /// @brief 呈现外部模型包、运行环境与声部预览。
    /// @details 路径只在对应按钮按下时更新。每次任务使用提交时的选择。
    /// 模型选择和运行配置分离，因此模型可以随项目更换而不影响环境。
    /// 该面板与旧音轨使用不同对象，选择新来源不改变实时混音。
    /// 运行环境保存到用户配置，只允许明确的文件选择更新。
    /// @warning 普通帧只绘制已缓存的名字；文件解析仅发生在确认选择后。
    void drawSeparationControls()
    {
        // 模型选择只解析描述，不触发权重加载或本地进程。
        if ( FeedbackButton("选择外部模型") ) {
            const nfdu8filteritem_t filter{ "MMM model package", "json" };
            if ( const auto file = pickFile(&filter); !file.empty() ) {
                auto model = readStemSeparationModel(file);
                // 选择失败时保留上一份有效模型，便于修正本地文件。
                if ( model ) {
                    m_model = std::move(*model);
                    m_error.clear();
                } else {
                    m_error = model.error();
                }
            }
        }
        // 当前曲目与模型包独立，可以在同一曲目上对照多个模型。
        ImGui::SameLine();
        if ( FeedbackButton("选择待分离音频") ) {
            // 取消选择保持已显示的源音频，可继续按当前配置分离。
            if ( auto file = pickFile(); !file.empty() )
                m_source = std::move(file);
        }
        // 声部按照模型包声明排序。名称不会用作资源路径或动态可执行参数。
        if ( m_model ) {
            ImGui::Text("模型：%s", m_model->name.c_str());
            // 在加载权重前显示实际会请求的声部，允许作者确认计划。
            for ( const auto& stem : m_model->stems ) {
                ImGui::BulletText("%s", stem.c_str());
            }
        }
        // 原曲路径可能很长，用软换行维持控制面板宽度。
        if ( !m_source.empty() ) ImGui::TextWrapped("%s", m_source.c_str());
        // 环境独立于模型权重；换模型或换歌可以复用同一 Python 和推理入口。
        auto& environment = *m_environment;
        if ( ImGui::CollapsingHeader("分离运行环境") ) {
            // 只有配置值在当前帧改变时才写盘。展开面板本身不会触发 I/O。
            bool changed = false;
            // 同一模型运行时允许换解释器，更便于切换虚拟环境。
            if ( FeedbackButton("选择模型环境的 Python") ) {
                if ( auto file = pickFile(); !file.empty() ) {
                    environment.python = std::move(file);
                    changed            = true;
                }
            }
            ImGui::TextWrapped("%s", environment.python.c_str());
            // 推理脚本仅从本机文件选择器接受，模型包本身只引用权重。
            if ( FeedbackButton("选择 MSST inference.py") ) {
                const nfdu8filteritem_t filter{ "Python", "py" };
                if ( auto file = pickFile(&filter); !file.empty() ) {
                    environment.backend = std::move(file);
                    changed             = true;
                }
            }
            // 首次设置时在禁用分离按钮前说明需要的本机组件。
            if ( environment.backend.empty() )
                ImGui::TextDisabled("请先设置 MSST 推理入口。");
            else
                ImGui::TextWrapped("%s", environment.backend.c_str());
            // CPU 选项只改变新任务的设备选择，已有结果仍可继续试听。
            changed |= FeedbackCheckbox("使用 CPU", &environment.cpu);
            // 保存错误可以提示，但当前会话仍能使用刚选择的运行环境。
            if ( changed && !saveStemSeparationEnvironment(environment) ) {
                m_error = "无法保存分离运行环境。";
            }
        }
        ImGui::BeginDisabled(!m_model || m_source.empty() ||
                             environment.backend.empty());
        // 从按钮点击进入任务提交，本次推理使用全部声明的目标声部。
        if ( FeedbackButton("分离音轨") ) startSeparation();
        ImGui::EndDisabled();
    }

    /// @brief 固定本次模型和音源快照，启动后台推理和解码。
    /// @pre 已解析模型描述且选择本机后端及源音频。
    /// @details 分离与正常导入共享一个 future，界面一次只排入一个装载任务。
    /// @warning 点击触发。后台作业不借用窗口对象，关闭窗口仅丢弃待发布结果。
    void startSeparation()
    {
        auto& runtime = Runtime::AppThreadPool::instance();
        auto* files   = runtime.getFileThreadPool();
        auto* decode  = runtime.get();
        if ( !files || !decode ) {
            m_error = "音频后台服务尚未就绪。";
            return;
        }
        m_error.clear();
        m_discardLoad = false;
        // 工作线程只持有值快照和应用级池指针，不观察 UI 的后续选择。
        m_separating = true;
        // 每个任务获得新的停止状态，上一任务的取消不会污染下一次启动。
        m_separationStop = std::stop_source{};
        m_load           = files->enqueue([model       = *m_model,
                                           source      = m_source,
                                           environment = *m_environment,
                                           stop        = m_separationStop.get_token(),
                                           decode] {
            // 工作进程生成完一组结果后才进入解码，解码池不持有界面状态。
            return runStemSeparation(model,
                                     source,
                                     environment.python,
                                     environment.backend,
                                     environment.cpu,
                                     *decode,
                                     stop);
        });
        if ( !m_load.valid() ) m_error = "无法提交分离任务。";
    }

    /// @brief 启动低频文件选择，再将完整解码交给文件池。
    /// @details 文件列表来自桌面原生多选结果，取消不替换任何状态。
    /// UTF-8 完整路径作为目录 ID，显示时才取文件名。
    /// @warning 只有点击选择按钮才进入本函数，系统对话框可同步等待用户。
    void loadFiles()
    {
        std::vector<std::string> paths;
        // 文件选择失败与用户取消有不同反馈。取消时保留原试听状态。
        const auto status =
            NativeFileDialog::openFiles(paths, nullptr, 0, nullptr);
        if ( status != NFD_OKAY ) {
            if ( status == NFD_ERROR ) m_error = "无法打开音轨选择器。";
            return;
        }
        // 在打开选择器之后取线程池，避免长时间等用户选择时保留观察指针。
        auto& runtime = Runtime::AppThreadPool::instance();
        auto* files   = runtime.getFileThreadPool();
        auto* decode  = runtime.get();
        // 文件线程等待解码，必须确认两个执行域都已初始化。
        if ( !files || !decode ) {
            m_error = "音频解码服务尚未就绪。";
            return;
        }
        // 工作者只捕获所选文件与应用线程池观察指针。应用先关闭文件池，
        // 再关闭解码池，因此等待解码时拥有有效的执行器。
        m_error.clear();
        m_discardLoad = false;
        m_separating  = false;
        m_load =
            files->enqueue([paths = std::move(paths), decode]() -> LoadResult {
                auto tracks =
                    Audio::StemAudition::loadAlignedFiles(paths, *decode);
                if ( !tracks ) return std::unexpected("无法解码导入的音轨。");
                return std::move(*tracks);
            });
        // 线程池在退出阶段可以拒绝投递，此时没有后续结果可轮询。
        if ( !m_load.valid() ) m_error = "无法提交音轨解码任务。";
    }

    /// @brief 成功准备并接入新节点后才替换上一组试听。
    /// @param result 文件任务返回的目录或可展示错误。
    /// @details 播放节点和目录使用同一批 PCM，清单与轨道选择只在成功后替换。
    /// 失败时原播放头和原组合仍有效。
    /// @warning 控制线程低频发布。音频回调中不能构造或销毁调度。
    void install(LoadResult result)
    {
        // 文件级错误先于可见状态变更，失败的局部结果始终不会接入设备。
        if ( !result ) {
            m_error = result.error();
            return;
        }
        // 选择和显示文本分别保存，绘制时无需创建文件名或读取文件系统。
        // 完整路径留在目录内部，只有曲名参与窗口布局。
        // 显示名可以重复，实际选择始终用完整路径查找。
        std::vector<std::string> labels;
        std::vector<std::string> ids;
        for ( const auto& track : result->tracks() ) {
            labels.push_back(
                Config::pathToUtf8(Config::utf8ToPath(track.id).filename()));
            ids.push_back(track.id);
        }
        // 默认选择全开；作业只生成数据，播放仍由用户明确触发。
        auto playback = result->makePlayback(ids, BLOCK_FRAMES);
        if ( !playback ||
             !Audio::AudioManager::instance().attachStemAudition(*playback) ) {
            m_error = "无法接入音频输出。";
            return;
        }
        // 新节点初始停止，与旧节点短暂同在总线也不会额外发声。
        releasePlayback();
        // 顺序提交目录与节点。未安装过节点时旧目录为空，释放逻辑相同。
        m_catalog  = std::move(*result);
        m_playback = std::move(*playback);
        m_labels   = std::move(labels);
        m_enabled.assign(m_labels.size(), true);
        // 独听索引属于当前目录，更换声部后重新回到全轨试听。
        m_solo.reset();
        // 每次载入使用新轨道的电平，旧推子不按位置错误套到不同声部。
        m_gains.assign(m_labels.size(), 1.0F);
        m_error.clear();
    }

    /// @brief 绘制单独的试听传输与每轨启停控件。
    /// @details 传输按钮发往公共播放节点，设备在音频块边界应用。
    /// 按钮意图读取请求态，进度读取已发布的实际播放位置。
    /// @warning UI 热路径。仅在交互改变选择时建调度，不读取文件。
    /// @param dpiScale 当前窗口 DPI 比例，统一推子和滚动区域的物理尺寸。
    void drawTrackControls(float dpiScale)
    {
        // 生命周期受当前渲染调用保护，使用引用而不复制节点所有权。
        auto& node = *m_playback;
        // 请求状态可早于设备反馈，按钮仍应对短时间内的重复点击作出响应。
        // 该状态只驱动传输按钮，不把后台分离的进度当作播放状态。
        const auto state = node.requestedState();
        if ( FeedbackButton(
                 state == Audio::AudioTimelinePlaybackState::Playing &&
                         !node.finished()
                     ? "暂停"
                     : "播放") ) {
            if ( state == Audio::AudioTimelinePlaybackState::Playing &&
                 !node.finished() ) {
                node.pause();
            } else {
                // 已结束的声源重新播放时从第一帧开始。普通暂停仍恢复原位置。
                if ( node.finished() ) node.seek(0);
                node.play();
            }
        }
        // 停止清空播放位置。暂停按钮则保留原帧。
        ImGui::SameLine();
        if ( FeedbackButton("停止") ) node.stop();
        // UI 展示秒，设备使用采样帧。转换使用与解码同一个内部采样率。
        const double rate = ice::ICEConfig::internal_format.samplerate;
        const double zero = 0.0;
        // 试听零点由载入的混音组决定，未选中的通道不会改变持续时间。
        const double endSeconds =
            static_cast<double>(m_catalog->endFrame()) / rate;
        // 进度上限来自最长轨道，勾选较短音轨不会收缩进度范围。
        double position = static_cast<double>(node.positionFrame()) / rate;
        if ( ImGui::SliderScalar("播放位置",
                                 ImGuiDataType_Double,
                                 &position,
                                 &zero,
                                 &endSeconds,
                                 "%.3f s",
                                 ImGuiSliderFlags_AlwaysClamp) ) {
            // 只把当前用户的拖动送入邮箱，普通进度刷新不会反向触发定位。
            node.seek(static_cast<Audio::AudioTimelineFrame>(position * rate));
        }
        // 仅在值变化后计算 ID 列表，平时保留当前调度与共享缓存。
        bool changed = false;
        // 每条通道独立维护推子。静音与独听只影响选轨，不重置推子值。
        // 横向排布给各通道同一种控制结构。长音轨列表仍可滚动。
        // 子区域覆盖推子和启停按钮，滚动时仍可看到共享时间轴。
        ImGui::BeginChild("StemMixer",
                          ImVec2(0, 230 * dpiScale),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for ( std::size_t index = 0; index < m_labels.size(); ++index ) {
            // 同一文件名可能来自不同目录，固定的索引作用域隔离控件状态。
            ImGui::PushID(static_cast<int>(index));
            // vector<bool> 的位引用不能借给 ImGui，提交时用实际 bool。
            // 推子、启用和独听共享同一个通道作用域，避免标签重复冲突。
            ImGui::BeginGroup();
            // 显示名按普通文本输出，文件名中的 ImGui 分隔符不参与控件 ID。
            ImGui::TextUnformatted(m_labels[index].c_str());
            changed |=
                ImGui::VSliderFloat("##gain",
                                    ImVec2(46 * dpiScale, 120 * dpiScale),
                                    &m_gains[index],
                                    0.0F,
                                    1.0F,
                                    "%.2f");
            // 使用临时 bool 保存复选框结果，变更后才发布整套轨道选择。
            bool enabled = m_enabled[index];
            if ( FeedbackCheckbox("启用", &enabled) ) {
                m_enabled[index] = enabled;
                changed          = true;
            }
            ImGui::SameLine();
            if ( FeedbackSmallButton(m_solo == index ? "退出独听" : "独听") ) {
                // 独听是临时监听状态。退出后恢复原先的静音组合及推子位置。
                if ( m_solo == index )
                    m_solo.reset();
                else
                    m_solo = index;
                changed = true;
            }
            // 组宽由标签和控制按钮共同决定，下一轨接在完整列之后。
            ImGui::EndGroup();
            ImGui::PopID();
            // 末尾不延长水平布局，子窗口自行计算滚动宽度。
            if ( index + 1 < m_labels.size() ) ImGui::SameLine();
        }
        // 无论子区域是否被裁剪，EndChild 均须与 BeginChild 配对。
        ImGui::EndChild();
        // 单帧即使修改多个控件，也只发布一次最新的完整组合。
        if ( changed ) {
            // 整批发布选择，共享播放头在下一个音频块继续推进。
            // 所有推子值仍按目录传入。静音轨重新启用时可恢复其先前电平。
            std::vector<std::string> selected;
            // 目录与控件共享装载顺序，无需逐帧匹配完整路径。
            const auto tracks = m_catalog->tracks();
            for ( std::size_t index = 0; index < tracks.size(); ++index ) {
                // 单人监听时暂时忽略启用位，退出后按保存的启用位恢复。
                if ( m_solo ? *m_solo == index : m_enabled[index] ) {
                    selected.push_back(tracks[index].id);
                }
            }
            if ( auto generation = m_catalog->select(
                     node, selected, BLOCK_FRAMES, m_gains) ) {
                // 后续切换可覆盖尚未生效的选择，只跟踪最新一代。
                m_pendingGeneration = *generation;
            } else {
                m_error = "无法切换试听音轨。";
            }
        }
        // 只在存在发布请求时观察代次，确认音频线程接管后再回收退役调度。
        // 若窗口折叠，旧状态会暂留至恢复绘制或下一次关闭。
        if ( m_pendingGeneration != 0 ) {
            static_cast<void>(node.reclaimRetiredSchedules());
            // 时钟快照跨字段一致，竞争时下一帧再观察，无须同步等待。
            const auto clock = node.clockSnapshot();
            if ( clock.valid &&
                 clock.scheduleGeneration >= m_pendingGeneration ) {
                // 发布代次确认后再扫一次，处理两次观察间音频线程的退役。
                static_cast<void>(node.reclaimRetiredSchedules());
                m_pendingGeneration = 0;
            }
        }
    }

    /// @brief 静音并移除节点，再释放界面持有的目录和播放引用。
    /// @details 节点调度在总线快照中拥有轨道生命周期，目录可独立释放。
    /// 失效的代次也随目录清空，避免新节点代次从头计数时误判。
    /// @warning 只能从 UI 控制线程调用。节点退役遵循引擎现有总线规则。
    void releasePlayback()
    {
        if ( m_playback ) {
            // 先停止再解绑，旧总线快照即使存留一块也能收到停止命令。
            m_playback->stop();
            Audio::AudioManager::instance().detachStemAudition(m_playback);
            m_playback.reset();
        }
        m_catalog.reset();
        m_pendingGeneration = 0;
    }

    /// @brief 试听暂存容量。较大的回调输出由节点分段填充。
    /// @details 固定上限避免随着歌曲长短调整实时暂存。
    static constexpr std::size_t BLOCK_FRAMES = 1024;
    /// @brief 窗口显示标记由 ImGui 关闭控件与菜单共同维护。
    /// @details 关闭不会改动当前工程、谱面、BGM 设置或文件。
    bool m_open = false;
    /// @brief 已提交的文件任务；仅在 ready 后消费其值。
    /// @details 销毁 future 不取消解码，任务也不捕获本对象。
    std::future<LoadResult> m_load;
    /// @brief 关闭面板使尚未返回的文件操作结果失效。
    /// @details 即使重开窗口，也要等旧请求被消费后才接受新文件。
    bool m_discardLoad = false;
    /// @brief 等待音频块接纳的最新选择代次。
    /// @details 零表示没有需要确认的调度，按钮静止时无需访问退役栈。
    std::uint64_t m_pendingGeneration = 0;
    /// @brief 当前试听可用的声部目录。
    /// @details 目录与节点各自保有轨道缓存。拆图与目录的释放顺序无关。
    std::optional<Audio::StemAudition> m_catalog;
    /// @brief 工具与主混音总线共享节点，回调结束前持续有效。
    /// @warning 热路径借用引用，仅在安装或释放时变更共享所有权。
    std::shared_ptr<Audio::AudioTimelineMixerNode> m_playback;
    /// @brief 打开时计算的可见文件名。
    /// @details 与目录同序，切换选择时用索引关联，不用可见名称做身份。
    std::vector<std::string> m_labels;
    /// @brief 当前组合，按目录顺序排列。
    /// @details 使用本地 UI 状态，不从实时节点反向查询每个轨道。
    std::vector<bool> m_enabled;
    /// @brief 最近一次操作失败的提示，可包含后台返回的本地错误。
    /// @details 拥有完整文本，避免后台 future 销毁后仍持有借用字符串。
    std::string m_error;
    /// @brief 经过校验的外部模型及声部定义。
    std::optional<StemSeparationModel> m_model;
    /// @brief 当前待分离音频所在路径，仅任务快照持有副本。
    std::string m_source;
    /// @brief 从本机设置恢复解释器和推理入口，首次打开时读取一次。
    std::optional<StemSeparationEnvironment> m_environment;
    /// @brief 区分文件导入与模型推理的等待状态。
    bool m_separating = false;
    /// @brief 本次推理取消信号，工作线程持有自身 token。
    std::stop_source m_separationStop;
    /// @brief 目录索引对应推子位置，关轨和独听不改变设定。
    std::vector<float> m_gains;
    /// @brief 临时独听通道索引，空值播放已启用的组合。
    std::optional<std::size_t> m_solo;
};
}  // namespace

/// @brief 注册桌面多音轨文件试听入口。
std::unique_ptr<IMainMenuItemActionHandler> createOpenStemAuditionAction()
{
    return std::make_unique<OpenStemAuditionAction>();
}
}  // namespace MMM::UI

#endif
