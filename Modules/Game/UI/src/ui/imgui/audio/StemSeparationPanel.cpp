#include "StemSeparationPanel.h"

#ifndef __ANDROID__

#    include "StemModelCatalog.h"
#    include "StemSeparationJob.h"
#    include "audio/AudioManager.h"
#    include "audio/AudioTimelineMixerNode.h"
#    include "audio/StemAudition.h"
#    include "config/Utf8Path.h"
#    include "runtime/AppThreadPool.h"
#    include "ui/utils/NativeFileDialog.h"
#    include "ui/utils/UIWidgetUtils.h"

#    include <ice/config/config.hpp>
#    include <ice/thread/ThreadPool.hpp>
#    include <imgui.h>

#    include <array>
#    include <chrono>
#    include <cstddef>
#    include <cstdint>
#    include <expected>
#    include <filesystem>
#    include <future>
#    include <memory>
#    include <optional>
#    include <stop_token>
#    include <string>
#    include <utility>
#    include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 管理一个项目主音轨的分离与分轨试听。
/// @details 只接受控制器发布的主轨路径。分离结果是临时监听数据，
/// 不覆盖原主轨文件，也不更新工程资源清单。
class MainTrackStemPanel final : public StemSeparationPanel
{
public:
    /// @brief 停止作业并从音频总线撤下分轨试听。
    /// @details 作业持有自己的路径及停止令牌，释放控制器时无需等待。
    ~MainTrackStemPanel() override
    {
        m_separationStop.request_stop();
        m_deploymentStop.request_stop();
        releasePlayback();
    }

    /// @brief 绑定当前主轨，在同一控制器内绘制分离与混音。
    /// @warning UI 热路径。路径只在实际切换资源时更新；
    /// 后台结果仅在就绪时取回，窗口折叠不会阻塞进度。
    void render(const std::filesystem::path& root,
                const std::string& relativePath, float dpiScale) override
    {
        // 重命名或资源重载时，任何尚未发布的分轨都只属于旧输入。
        if ( root != m_root || relativePath != m_relativePath ) {
            // 停止信号可能晚于推理完成，因此还要标记本次结果不可用。
            m_separationStop.request_stop();
            m_discardLoad = true;
            releasePlayback();
            m_root         = root;
            m_relativePath = relativePath;
            m_error.clear();
        }
        // 收取结果先于绘制。收到新分轨的这一帧就能请求切换到混音台。
        pollJobs();
        const bool expanded = ImGui::CollapsingHeader("声部分离");
        if ( expanded ) {
            // 重新展开可读取其他音轨控制器新部署的模型。
            if ( !m_expanded ) restoreBindings();
            drawWorkspace(dpiScale);
        }
        m_expanded = expanded;
    }

private:
    /// @brief 展开面板时刷新可见绑定，首次选择一个可用模型。
    /// @warning 只在从折叠切换到展开时读配置和校验已有目录。
    void restoreBindings()
    {
        // 读取最新目录只刷新目录状态，不把用户手工选中的模型自动切走。
        m_bindings = readStemModelBindings();
        if ( !m_environment ) {
            // 自定义模型与目录中各项使用不同设置文件，旧 MSST 入口不做迁移。
            m_environment = loadStemSeparationEnvironment();
            // 绑定文件只保存路径。启动时仍要解析当前清单，以免目录搬迁后
            // 自动选中已经不能使用的模型。
            // 已绑定的首个模型可直接使用，手动导入仍保留单独运行设置。
            // 一处失效不会阻止其余模型恢复；先选首个有效条目。
            for ( std::size_t i = 0; i < m_bindings.size(); ++i ) {
                if ( m_bindings[i].empty() ) continue;
                auto bound =
                    bindStemModel(STEM_MODEL_CATALOG[i], m_bindings[i]);
                if ( bound ) {
                    // 启动时恢复绑定不写盘，其他窗口的设置保持原有顺序。
                    useBoundModel(i, std::move(*bound), false);
                    break;
                }
            }
        }
    }

    /// @brief 消费部署和分离结果，保留当前主轨的有效输出。
    /// @warning UI 热路径。只轮询已提交任务，禁止等待后台工作。
    void pollJobs()
    {
        using namespace std::chrono_literals;
        if ( m_load.valid() &&
             m_load.wait_for(0s) == std::future_status::ready ) {
            // 任务提交后路径发生变化时只清空 future，不播放旧主轨。
            auto result = m_load.get();
            if ( !m_discardLoad ) install(std::move(result));
        }
        // 部署不依赖主轨路径。面板折叠时也可保存已就绪的模型。
        if ( m_deployment.valid() &&
             m_deployment.wait_for(0s) == std::future_status::ready ) {
            // 取走 future 后可重新发起任务。发布时使用提交那一刻的行号，
            // 目录表当前的选择不会改变持久化键。
            auto deployed = m_deployment.get();
            if ( deployed )
                useBoundModel(m_deployingIndex, std::move(*deployed), true);
            else
                // 错误留在分离页，用户可重试或改绑目录。
                m_error = deployed.error();
        }
    }

    /// @brief 文件任务返回的不可变目录，错误在 UI 线程显示。
    using LoadResult = std::expected<Audio::StemAudition, std::string>;

    /// @brief 以分离和混音两个工作区容纳模型选择及完整的推子布局。
    /// @details 尚未准备好音轨时只提供分离页。后台结果安装后切换一次，
    /// 已载入的音轨仍可在返回分离页时继续播放。
    /// 切页不接触播放节点的生命周期，暂停和停止由混音控件负责。
    /// @param dpiScale 当前界面缩放，用于保持混音推子物理尺寸一致。
    /// @warning UI 热路径，仅绘制已有状态，并按交互请求提交低频操作。
    void drawWorkspace(float dpiScale)
    {
        // 固定标签栏 ID，避免窗口显示语言改变时丢失交互状态。
        if ( !ImGui::BeginTabBar("StemWorkspace") ) return;
        // 分离页始终可用，因此播放上一组的同时也可准备下一组。
        // 启动新一轮分离前，用户仍可返回已有混音调整各声部。
        if ( ImGui::BeginTabItem("分离") ) {
            drawSetup();
            ImGui::EndTabItem();
        }
        // 本次结果可用时请求切换。页签下一帧才展开时需保留请求。
        // 混音页签取决于分轨播放节点是否已构建，模型清单本身没有可听数据。
        const auto mixerFlags = m_showMixer ? ImGuiTabItemFlags_SetSelected
                                            : ImGuiTabItemFlags_None;
        if ( m_playback &&
             ImGui::BeginTabItem("混音台", nullptr, mixerFlags) ) {
            m_showMixer = false;
            ImGui::Spacing();
            drawTrackControls(dpiScale);
            if ( !m_error.empty() ) ImGui::TextWrapped("%s", m_error.c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    /// @brief 排列模型、音源及当前作业，不在混音区重复显示部署控件。
    /// @details 同一窗口一次只准备一个任务。运行时可取消当前部署或推理。
    /// 部署结果只参与模型选择，不更换本次分离的来源。
    /// @warning UI 热路径，仅在用户操作时提交命令。
    void drawSetup()
    {
        ImGui::Spacing();
        // 后台任务持有已经冻结的输入，禁用操作可避免进度与新选择相互矛盾。
        // 取消按钮留在禁用区域外，无需中断播放即可结束文件任务。
        ImGui::BeginDisabled(m_load.valid() || m_deployment.valid());
        drawSeparationControls();
        ImGui::EndDisabled();

        // 安装和推理的取消信号独立，各自只影响当前作业。
        // 安装包含依赖解析和下载，不把单一阶段的字节数作为总进度。
        if ( m_deployment.valid() ) {
            ImGui::TextUnformatted("正在下载权重并安装独立环境…");
            ImGui::SameLine();
            if ( FeedbackSmallButton("取消部署") )
                m_deploymentStop.request_stop();
        }
        if ( m_load.valid() ) {
            ImGui::TextUnformatted("正在分离主音轨…");
            if ( FeedbackButton("取消分离") ) m_separationStop.request_stop();
        }
        // 统一在触发入口下方报告结果，失败时不清理已有效的模型或音轨。
        if ( !m_error.empty() ) ImGui::TextWrapped("%s", m_error.c_str());
    }

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

    /// @brief 切换到目录里的模型，必要时写入该模型的长期绑定。
    /// @details 外部 MSST 入口配置单独维护。目录模型总是使用其自身解释器。
    /// @param index 目录稳定顺序中的模型位置。
    /// @param bound 已冻结的模型计划、解释器和目录。
    /// @param persist 首次绑定或部署成功时写盘；切换既有条目时为 false。
    /// @warning 写盘仅发生在用户首次绑定或部署完成时。
    void useBoundModel(std::size_t index, BoundStemModel bound, bool persist)
    {
        // 环境与模型一起替换，保持下一次推理的清单和解释器归属一致。
        m_error.clear();
        m_model            = std::move(bound.model);
        m_selectedCatalog  = index;
        m_boundEnvironment = std::move(bound.environment);
        m_bindings[index]  = std::move(bound.directory);
        // 若配置目录不可写，保持本次会话可用，提示重启时会丢失新绑定。
        if ( persist && !saveStemModelBinding(STEM_MODEL_CATALOG[index],
                                              m_bindings[index]) ) {
            m_error = "模型已就绪，但无法保存目录绑定。";
        }
    }

    /// @brief 弹出目录选择器，交由对应流程处理已部署或待部署的位置。
    /// @param index 目录行号，在原生选择器打开期间保持稳定。
    /// @param deploy 为 true 时选择部署父目录，否则定位完整模型目录。
    /// 文件池拿到目录后的错误通过部署结果回传，选择失败则直接显示。
    /// @warning 用户点击触发，选择器可等待用户输入。
    void chooseModelDirectory(std::size_t index, bool deploy)
    {
        // 部署与绑定均使用目录选择器，模型清单的路径结构由后端管理。
        nfdu8char_t* path   = nullptr;
        const auto   status = NativeFileDialog::pickFolder(&path, nullptr);
        // 用户取消直接返回，原绑定及当前混音均保留。
        if ( status != NFD_OKAY || !path ) {
            if ( status == NFD_ERROR ) m_error = "无法打开目录选择器。";
            return;
        }
        // 原生缓冲区在选择完成后由 NFD 释放；工作池拿到自己的 UTF-8 副本。
        std::string selected(path);
        NFD_FreePathU8(path);
        if ( !deploy ) {
            // 绑定只核对目录文件，不重装依赖也不执行用户环境中的 Python。
            auto bound = bindStemModel(STEM_MODEL_CATALOG[index], selected);
            if ( bound )
                useBoundModel(index, std::move(*bound), true);
            else
                m_error = bound.error();
            return;
        }
        // 选择器可能长时间打开，只在确认路径后才查找文件执行器。
        auto* pool = Runtime::AppThreadPool::instance().getFileThreadPool();
        if ( !pool ) {
            m_error = "文件后台服务尚未就绪。";
            return;
        }
        // 保存行索引用于完成后显示和绑定，后台只捕获值与稳定条目。
        m_deployingIndex = index;
        m_deploymentStop = std::stop_source{};
        m_error.clear();
        // 不让文件线程拿到窗口引用，窗口销毁只需发布停止信号。
        m_deployment = pool->enqueue([entry  = STEM_MODEL_CATALOG[index],
                                      parent = std::move(selected),
                                      stop   = m_deploymentStop.get_token()] {
            return deployStemModel(entry, parent, stop);
        });
        // 停机阶段可以拒绝投递，空 future 不能驱动部署进度。
        if ( !m_deployment.valid() ) m_error = "无法提交部署任务。";
    }

    /// @brief 绘制目录表，按模型显示专长、已绑定状态和可用操作。
    /// @details 目录条目和绑定位置按固定顺序对应，长路径通过悬停查看。
    /// @warning 每帧只绘制缓存数据；点击时才解析新路径或提交后台任务。
    void drawCatalog()
    {
        // 优先把可拉伸列宽留给声部说明，部署按钮与说明文本分列。
        constexpr auto flags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
        // 目录增长时只扩展滚动内容，给主轨控制器保留操作和播放器空间。
        // 以字体行距限定内容高度，维持 DPI 切换后的空间分配。
        const auto tableHeight = 13.0F * ImGui::GetTextLineHeightWithSpacing();
        if ( !ImGui::BeginTable(
                 "StemModelCatalog", 4, flags, { 0.0F, tableHeight }) )
            return;
        ImGui::TableSetupColumn(
            "模型", ImGuiTableColumnFlags_WidthStretch, 1.35F);
        ImGui::TableSetupColumn(
            "擅长声部", ImGuiTableColumnFlags_WidthStretch, 2.3F);
        ImGui::TableSetupColumn(
            "环境", ImGuiTableColumnFlags_WidthStretch, 0.65F);
        ImGui::TableSetupColumn(
            "操作", ImGuiTableColumnFlags_WidthStretch, 1.45F);
        // 表头和内容共用列定义，缩放时仍可对齐模型状态与按钮。
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        // 窄控制器内按可用列宽堆叠操作，避免绑定入口被右侧 Dock 裁掉。
        // 取自当前 DPI 与皮肤字体计算，避免用固定像素阈值。
        const float buttonSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float inlineButtonWidth =
            ImGui::CalcTextSize("部署").x + ImGui::CalcTextSize("绑定已有").x +
            4.0F * ImGui::GetStyle().FramePadding.x + buttonSpacing;
        for ( std::size_t index = 0; index < STEM_MODEL_CATALOG.size();
              ++index ) {
            const auto& entry = STEM_MODEL_CATALOG[index];
            // 紧凑标签按目录语义换行，完整型号始终在悬停和当前模型中可见。
            const char* label =
                entry.catalogName ? entry.catalogName : entry.name;
            // 渲染不探测目录是否还在。选择已有绑定时才重新验证，
            // 已挂载磁盘暂时离线也不会拖慢每帧模型表。
            const bool bound = !m_bindings[index].empty();
            // 同名按钮按模型 ID 隔离，条目在将来增删或重排也不会串状态。
            ImGui::PushID(entry.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if ( bound ) {
                if ( FeedbackSelectable(label, m_selectedCatalog == index) ) {
                    // 重新解析用户可能在 MMM 之外更新的模型清单。
                    auto model = bindStemModel(entry, m_bindings[index]);
                    if ( model )
                        useBoundModel(index, std::move(*model), false);
                    else
                        m_error = model.error();
                }
            } else {
                // 未部署条目只提供目录操作，不能成为当前分离目标。
                ImGui::TextUnformatted(label);
            }
            if ( entry.catalogName && ImGui::IsItemHovered() )
                ImGui::SetTooltip("%s", entry.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", entry.stems);
            // 说明作为声部名称的辅助层，但仍需在深色主题和行底色上可读。
            auto descriptionColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            descriptionColor.w *= 0.8F;
            ImGui::PushStyleColor(ImGuiCol_Text, descriptionColor);
            ImGui::TextWrapped("%s", entry.strengths);
            ImGui::PopStyleColor();
            // 状态只是持久化绑定的存在性，运行时校验交给实际选择动作。
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(bound ? "已绑定" : "未部署");
            // 长路径不挤占模型说明，悬停即可核对存在哪个目录。
            if ( bound && ImGui::IsItemHovered() ) {
                ImGui::SetTooltip("%s", m_bindings[index].c_str());
            }
            ImGui::TableSetColumnIndex(3);
            // 使用列宽判断排版，同一工具在浮窗宽屏可并排，窄 Dock 则上下排列。
            const bool inlineButtons =
                ImGui::GetContentRegionAvail().x >= inlineButtonWidth;
            // 保留部署入口以便更换磁盘；部署程序会拒绝覆盖已有目录。
            if ( FeedbackSmallButton("部署") )
                chooseModelDirectory(index, true);
            // 已部署到别处时直接重新绑定，跳过下载和安装。
            if ( inlineButtons ) ImGui::SameLine(0.0F, buttonSpacing);
            if ( FeedbackSmallButton("绑定已有") )
                chooseModelDirectory(index, false);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    /// @brief 展示高级外部模型导入选项，兼容已有的用户自定义 MSST 工作流。
    /// @details 后端配置仅应用于自定义清单，与内置条目环境独立。
    /// @warning 渲染热路径，只有按钮事件和实际配置变更才触发文件操作。
    void drawExternalModelControls()
    {
        // 自定义引擎保留对旧配置的编辑入口，目录模型的环境随各自绑定管理。
        auto& environment = *m_environment;
        // 自定义清单允许多模型协作。沿用公开包格式，不要求迁到目录部署。
        if ( FeedbackButton("选择外部模型") ) {
            const nfdu8filteritem_t filter{ "MMM model package", "json" };
            if ( const auto file = pickFile(&filter); !file.empty() ) {
                auto model = readStemSeparationModel(file);
                if ( model ) {
                    // 清掉目录模型的运行环境，手工清单使用此区域的配置。
                    m_model = std::move(*model);
                    m_selectedCatalog.reset();
                    m_boundEnvironment.reset();
                    m_error.clear();
                } else {
                    m_error = model.error();
                }
            }
        }
        // 按交互批量提交，展开配置区域本身不写盘。
        bool changed = false;
        if ( FeedbackButton("选择 Python") ) {
            if ( auto file = pickFile(); !file.empty() ) {
                environment.python = std::move(file);
                changed            = true;
            }
        }
        ImGui::TextWrapped("%s", environment.python.c_str());
        // 自定义运行入口只接受脚本。目录模型的脚本由安装器选择。
        if ( FeedbackButton("选择 MSST inference.py") ) {
            const nfdu8filteritem_t filter{ "Python", "py" };
            if ( auto file = pickFile(&filter); !file.empty() ) {
                environment.backend = std::move(file);
                changed             = true;
            }
        }
        if ( !environment.backend.empty() )
            ImGui::TextWrapped("%s", environment.backend.c_str());
        // 旧 MSST 配置允许显式指定设备。目录内置权重使用独立 CPU 环境。
        changed |= FeedbackCheckbox("使用 CPU", &environment.cpu);
        if ( changed && !saveStemSeparationEnvironment(environment) ) {
            m_error = "无法保存分离运行环境。";
        }
    }

    /// @brief 按目录、音源和作业的顺序组织分离面板。
    /// @details 是否可分离由当前模型、音源及有效后端共同决定。
    /// @warning 只显示缓存状态；分离及模型部署均提交后台执行。
    void drawSeparationControls()
    {
        // 目录是默认入口。已知模型的声部在表格中呈现，无需再重复一份列表。
        if ( ImGui::CollapsingHeader("模型目录",
                                     ImGuiTreeNodeFlags_DefaultOpen) ) {
            drawCatalog();
        }
        // 运行时解释器只属于高级配置，普通目录模型绑定后即可使用。
        if ( ImGui::CollapsingHeader("高级 · 自定义模型") )
            drawExternalModelControls();

        ImGui::Spacing();
        if ( m_model ) {
            ImGui::TextWrapped("当前模型  %s", m_model->name.c_str());
            // 自定义清单没有对应的目录行，才从解析结果展示声部名。
            if ( !m_selectedCatalog ) {
                for ( const auto& stem : m_model->stems )
                    ImGui::BulletText("%s", stem.c_str());
            }
        } else {
            ImGui::TextDisabled("请部署或绑定一个模型。");
        }
        // 音源已经由宿主窗口的资源身份确定，界面只负责选择模型。
        // 解析模型包时可能还没设置旧的 MSST 入口，避免提交无后端的请求。
        const auto& environment =
            m_boundEnvironment ? *m_boundEnvironment : *m_environment;
        ImGui::BeginDisabled(!m_model || environment.backend.empty());
        if ( FeedbackButton("分离主音轨") ) startSeparation();
        ImGui::EndDisabled();
    }

    /// @brief 固定本次模型和音源快照，启动后台推理和解码。
    /// @pre 控制器关联有效的项目主轨，且模型已解析。
    /// @details 每个主轨控制器一次只提交一项分离，结果就绪后才接入混音。
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
        // 新请求只能由分离页发起，上一组音轨在结果到达时才切换。
        // 在提交时才组合根与相对路径，取消或重命名不改后台已持有的路径。
        // 工作线程只持有值快照和应用级池指针，不观察 UI 的后续选择。
        // 每个任务获得新的停止状态，上一任务的取消不会污染下一次启动。
        m_separationStop = std::stop_source{};
        m_load           = files->enqueue(
            [model  = *m_model,
             source = Config::pathToUtf8(m_root /
                                         Config::utf8ToPath(m_relativePath)),
             environment =
                 m_boundEnvironment ? *m_boundEnvironment : *m_environment,
             stop = m_separationStop.get_token(),
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

    /// @brief 成功准备新节点后才替换上一组试听。
    /// @param result 文件任务返回的目录或可展示错误。
    /// @details 播放节点和目录使用同一批 PCM，清单与轨道选择只在成功后替换。
    /// 失败时原播放头和原组合仍有效。其他主轨控制器可能正在试听，
    /// 新节点只在用户按下播放时占用共享输出。
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
                Config::pathToUtf8(Config::utf8ToPath(track.id).stem()));
            ids.push_back(track.id);
        }
        // 默认选择全开；作业只生成数据，播放仍由用户明确触发。
        auto playback = result->makePlayback(ids, BLOCK_FRAMES);
        if ( !playback ) {
            m_error = "无法准备分轨播放。";
            return;
        }
        // 先撤下本面板的旧节点；总线上其他控制器的试听继续。
        releasePlayback();
        // 在同一帧同步替换标签和状态，混音页不会绘制目录的半成品。
        // 顺序提交目录与节点。未安装过节点时旧目录为空，释放逻辑相同。
        m_catalog  = std::move(*result);
        m_playback = std::move(*playback);
        m_labels   = std::move(labels);
        m_enabled.assign(m_labels.size(), true);
        // 独听索引属于当前目录，更换声部后重新回到全轨试听。
        m_solo.reset();
        // 每次载入使用新轨道的电平，旧推子不按位置错误套到不同声部。
        m_gains.assign(m_labels.size(), 1.0F);
        // 仅在新结果安装成功后切页，失败时留在当前页面提示重试。
        m_showMixer = true;
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
            } else if ( Audio::AudioManager::instance().attachStemAudition(
                            m_playback) ) {
                // 分轨共享一个输出槽。主动播放既可开始新组，也可取回其他
                // 控制器播放时被切走的输出，attach 会先停掉前一个节点。
                m_error.clear();
                // 已结束的声源重新播放时从第一帧开始。普通暂停仍恢复原位置。
                if ( node.finished() ) node.seek(0);
                node.play();
            } else {
                m_error = "无法接入音频输出。";
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
    /// 解绑按节点身份判断，关闭未播放的控制器不会中断其他音轨的试听。
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
        m_showMixer         = false;
    }

    /// @brief 试听暂存容量。较大的回调输出由节点分段填充。
    /// @details 固定上限避免随着歌曲长短调整实时暂存。
    static constexpr std::size_t BLOCK_FRAMES = 1024;
    /// @brief 上一帧区域已展开，用于按交互刷新模型绑定。
    bool m_expanded = false;
    /// @brief 已提交的文件任务；仅在 ready 后消费其值。
    /// @details 销毁 future 不取消解码，任务也不捕获本对象。
    std::future<LoadResult> m_load;
    /// @brief 当前主轨变化后丢弃尚未返回的旧轨结果。
    /// @details 旧任务仍需消费后才允许用新主轨启动分离。
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
    /// @brief 构造本次主轨路径的项目根，与资源路径共同判断来源是否变化。
    std::filesystem::path m_root;
    /// @brief 控制器主轨在当前项目中的相对路径。
    std::string m_relativePath;
    /// @brief 从本机设置恢复解释器和推理入口，首次打开时读取一次。
    std::optional<StemSeparationEnvironment> m_environment;
    /// @brief 已选用目录模型的独立解释器，空值表示使用自定义模型设置。
    /// @details 环境与模型在 UI 线程成对替换，提交推理时一起快照。
    std::optional<StemSeparationEnvironment> m_boundEnvironment;
    /// @brief 当前选中的目录行，可空以兼容用户导入模型。
    std::optional<std::size_t> m_selectedCatalog;
    /// @brief 按模型目录顺序记录长期绑定位置。
    /// @details 本机配置以稳定 ID 序列化，UI 顺序不会成为磁盘格式。
    std::array<std::string, STEM_MODEL_CATALOG.size()> m_bindings;
    /// @brief 正在部署的目录行，完成后将结果存至该行。
    /// @details 与当前选择独立，只有部署 future 有效时参与绘制与发布。
    std::size_t m_deployingIndex = 0;
    /// @brief 文件任务的部署结果，渲染路径只轮询是否完成。
    std::future<std::expected<BoundStemModel, std::string>> m_deployment;
    /// @brief 部署停止信号与分离任务独立，避免跨作业取消。
    /// @details 每次提交前替换，已排队任务只持有对应代次的 token。
    std::stop_source m_deploymentStop;
    /// @brief 新混音结果到达后切换页签。呈现后交还页签控制权。
    /// @details 在页签首次获选后清空，避免覆盖用户后续的鼠标选择。
    bool m_showMixer = false;
    /// @brief 本次推理取消信号，工作线程持有自身 token。
    std::stop_source m_separationStop;
    /// @brief 目录索引对应推子位置，关轨和独听不改变设定。
    std::vector<float> m_gains;
    /// @brief 临时独听通道索引，空值播放已启用的组合。
    std::optional<std::size_t> m_solo;
};
}  // namespace

/// @brief 创建控制器独占的桌面声部分离区域。
std::unique_ptr<StemSeparationPanel> createStemSeparationPanel()
{
    return std::make_unique<MainTrackStemPanel>();
}
}  // namespace MMM::UI

#endif
