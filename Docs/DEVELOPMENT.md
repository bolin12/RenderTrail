# RenderTrail 开发文档

[返回 README](../README.md)

RenderTrail 是一个仅用于编辑器的 UE 插件。截帧、界面和模型配置运行在 Unreal Editor 中；RenderDoc Replay Controller 运行在独立的 Worker 进程中，因此 Replay 或驱动故障不会重置编辑器 GPU：

- `RenderTrailAnalyzerEditor`：编辑器内的 Nomad 标签页，包含预览、像素选择、因果卡片和 Agent 流程。
- `RenderTrailReplayWorker`：由插件拥有的 UBT Program 模块，包含 RenderDoc Replay API 适配器。编辑器通过管道启动生成的 `RenderTrailReplayWorker.exe`，自身不会链接或调用 Replay API。

Analyzer 内部按职责拆分：`RenderTrailAnalyzerEditorModule` 只负责模块生命周期、Nomad 标签页注册和 `OpenCapture` 转发；`RenderTrailAnalyzerHome` 负责编辑器面板状态及分析工作流编排；`RenderTrailAnalyzerImageView` 负责像素预览与选择；`RenderTrailAnalyzerResultView` 负责结构化结论、颜色、Pass/Pipeline/Shader 和证据链展示；`RenderTrailAnalyzerEvidence` 负责 Pixel History 数值化、事件聚合和因果图；`RenderTrailEvidenceFormatting` 将 Pipeline 与 Shader 调试 JSON 转成可读证据，但不补写缺失语义；`RenderTrailReplayWorkerClient` 独占 Worker 进程、管道和退出状态；`RenderTrailAgentClient` 独占模型 HTTP 请求生命周期和兼容响应解析；`RenderTrailAgentProtocol` 负责结构化 Action JSON 的严格解析与有界修复；`RenderTrailAnalyzerDiagnostics` 负责配置、会话日志和脱敏后的 Agent 诊断；`RenderTrailAnalyzerPrompt` 负责可覆盖 Prompt 的加载。Replay Worker 内部的资源生产者、Discard 和跨资源边界判定位于 `RenderTrailReplayEvidence`。

按照常规方式在项目或引擎中启用插件，然后编译普通 Editor Target。Level Editor 顶部工具栏的 Play 区域会显示一个 **RenderTrail** 图标，与 **Open Neural Rendering Lab** 位于同一区域。点击后，插件会优先恢复上一次编辑器运行遗留的、已经完成但尚未认领的 `.rdc`；如果没有可恢复文件，则按照 Epic 的 Alt+F12 截帧流程执行，但会显式传入当前聚焦的 Game Viewport 或活动 Level Editor Viewport 的原生窗口句柄。在 PIE 运行期间，即使点击工具栏暂时改变了 Slate 焦点，插件仍会保留 Game Viewport 作为截帧目标。插件在同一次 Viewport 绘制后读回原生尺寸画面；等待 `.rdc` 文件大小稳定后，将该画面保存为像素精确预览并写入 UE 上下文，然后在 Unreal Editor 中打开 RenderTrail Analyzer 标签页。这个步骤不创建 RenderDoc ReplayController。

RenderDoc 截帧要求 Epic 的 `RenderDocPlugin` 已附加，并且当前 RHI 受支持。若只截取目标 Viewport，请关闭 **Project Settings > Plugins > RenderDoc > Capture all activity**；开启该选项会有意包含 Slate、所有 Viewport 以及编辑器窗口活动。如果一次截帧包含多个 Present/SwapBuffer 输出，Replay Worker 会优先选择尺寸与截帧 Viewport 元数据完全相同的最新 Present；没有这种候选时才回退到最新的最大 Present，避免误选较小的通知窗口。

`.rdc` 由 RenderDoc 创建在 `Saved/RenderDocCaptures` 下；RenderTrail 在旁边新增 `.rendertrail.json` UE 上下文，不复制体积较大的截帧。`Saved/Previews/<capture>.png` 保存 UE 原生 Viewport 读回，`Saved/Previews/<capture>.renderdoc.png` 保存 Worker 从所选最终目标纹理导出的权威 RT。两个文件从不互相覆盖；元数据中的 `previewPixelExact`、尺寸和路径只描述原生预览。旧版位于 `Saved/RenderTrail/Captures` 下的截帧仍然受支持。

## 插件归属与构建布局

所有 Replay 实现代码、Worker TargetRules 和版本匹配的 RenderDoc SDK 均位于 `Plugins/RenderTrail` 下。UBT 按插件 Target 的标准发现规则从 `Plugins/RenderTrail/Tests/RenderTrailReplayWorker.Target.cs` 构建隔离进程；插件不依赖项目级 Program 源码、批处理脚本、自定义暂存步骤或插件内预编译 Worker。

SDK 可以由插件、宿主项目、Engine 或 `RENDERTRAIL_RENDERDOC_ROOT` 环境变量提供。

## 像素检查流程

1. 在 UE 中点击 Level Editor 顶部工具栏的 **RenderTrail** 图标。截帧完成并稳定后，编辑器内的 Analyzer 标签页会自动打开。
2. Analyzer 可以先显示 UE 原生预览或截帧内嵌缩略图，但这两类图只用于等待期间查看，不接受选点。隔离 Replay Worker 会立即打开截帧并导出权威 RenderDoc 最终 RT。
3. 使用鼠标滚轮缩放，按住鼠标中键或右键拖动平移。高倍率下，预览会从带过滤的缩略图采样切换为精确的源像素方格，叠加单像素边界，并高亮光标所在单元格，确保点击坐标没有歧义。
4. 状态显示“RenderDoc 最终 RT 已就绪”后才可点击像素。该点记为 `P1`，不会被预先认定为正确、正常或异常；再次点击会清除，点击其他位置会替换。此时显示图与 Pixel History 的目标纹理、宽高和坐标域一致。点击 **分析当前像素** 后读取 P1 的 Pixel History，并自动深追实际纹理访问足迹、MSAA sample、像素 writer 与 producer 上下文；规则分析完成后，可在底部填写问题并点击 **发送**。单纯选择像素不会启动查询或 Agent。
5. 右侧检查器默认打开 **结论与链条**：先显示简短结论、Before/After 颜色块、Pass/Pipeline/Shader 摘要，再用“已证明 / 候选 / 断点”节点展示像素形成链。完整事件、资源、Shader 调试和最近 24 个可追踪跳转放在独立的 **技术证据** 页。底部 Agent 输入始终绑定当前 P1；连续追问仅保留上一次问答作为指代理解上下文。点击 **清除** 会清除当前像素和报告。

为控制载荷大小，每个选中像素仅保留最新 256 条详细 Pixel History modification；但 Replay 模块还会针对完整 Pixel History 输出逐事件汇总 `eventSummaries`。技术证据会逐分支显示全部事件汇总，先列通过测试或改变资源值的 Draw，再列 rejected 事件及失败原因。如果该汇总缺失或不完整，Analyzer 会停止，而不会根据被截断的尾部证据生成因果结论。

确定性分析层采用聚焦的 writer-dominance 递归，默认最多 24 个 Event Context、64 条跨资源 Pixel History、10 跳。最终 RT 和每个资源位置的完整 `eventSummaries` 都保留为可见性历史。每个递归 Draw 都优先执行 DebugPixel：`Load(x,y)` 产生确认坐标；`Sample/SampleLevel(u,v)` 保留真实 UV 和有界 footprint，但同一 `(consumer, resource, mip, slice, sample, typeCast)` 的多条执行访问先合并为一个资源组，优先查询最接近消费位置的代表坐标，只有没有 writer 时才自适应扩展最多 4 个候选。`collapsedShaderAccessCount` 记录合并量，完整指令仍在 Shader Debug。颜色链选择最后改变值的 writer；geometry 链跳过 `ClearStencil`，只把通过且改变 Depth/Visibility 的 Draw 作为 owner，真正的 depth/visibility clear 输出 `geometry-reset-boundary`。Shader Debug 不可用时才从绑定纹理中按 color/geometry/overlay 语义选 2 个回退分支。

Agent 投影额外生成覆盖全部已收集上下文的 `causalLanes`。它按 `(tracePurpose, consumer, resource, producer/reset)` 合并重复 MSAA sample 与自适应坐标记录，同时分别保留 `groupedBranchCount` 和 `queryRecordCount`。结果 UI 将 `color`、`geometry`、`overlay` 显示为并行 DAG 分区；跨分区事件没有默认时间顺序，Mesh 归属不得挂在最终 Composite 事件下。

报告会明确区分最后物理写入者和显著颜色形成候选，并把 `tracePurpose=color`、`geometry`、`overlay` 分别解释为最终颜色形成、几何/可见性归属和编辑器覆盖层。SM_Cylinder 之类 Mesh 证据应从 SceneDepth/Stencil/VisBuffer/GPUScene、Primitive ID 或 UE 资产 Marker 中寻找，不能要求它必须成为最终 RGB 的直接父节点。绑定资源本身只算候选输入；执行过的 shader access、坐标映射和该位置的主导 writer 共同决定递归边。

`.rdc` 不包含现成的 UE 语义因果链。它保存 GPU 命令、资源、Pipeline、Shader 和可用 Marker，Pixel History 由 RenderDoc 回放时重建。RT 写入和资源使用通常可以推导；但精确采样坐标、材质、Mesh 或 Actor 在没有 Shader 调试、源码映射、ShaderMap 或 UE Marker 时可能不可恢复，此时 RenderTrail 会保留 `unknown` 或显式 `chainBreak`。

Analyzer 运行期间，`.rdc` 会保持在 Replay Worker 会话中打开，因此每次像素查询都会复用同一个 Replay 会话。Worker 提供目标资源元数据、任意纹理子资源的 sample-aware Pixel History、完整事件汇总和确定性的有界事件上下文前沿。Event Context 保留 shader binding、descriptor subresource 和 type-cast 信息，不再仅按 ResourceId 合并 Depth/Stencil 等别名。对于选中的末端写入者，如果 Replay 支持，Analyzer 还会自动请求 `DebugPixel`，收集指令/源码映射数量、嵌套常量与资源变量、执行状态样本、变量变化值、调用栈深度、执行标志，以及实际执行的纹理 Load/Sample/Gather 指令与当时可恢复的变量值。这些信息可以证明该点实际执行了什么，但不会假装总能从调试轨迹中恢复高层材质公式。

穿过 copy、resolve 或 resample 资源递归追踪生产者时，仍然受坐标映射证据约束。达到 64 条资源历史、24 个上下文、每事件 12 个不同资源组、每资源 4 个自适应坐标或 2 个回退分支的边界时，会以明确的 branch status 留在报告中。UI 分别显示“资源组”和“候选坐标”，不会把同一 blur 纹理上的几十个 texel 误称为几十个资源。

## 完整 Replay 诊断

插件维护结构化诊断会话，用于调查确定性链条中的缺失环节。`bEnabled`、Worker 协议和 Agent 诊断默认开启；`bFullEvidencePayload` 与 GPU crash 快照默认关闭。日常模式仍会把每个实际执行的聚焦查询及其响应写入 `Saved/Logs/RenderTrailDiagnostics/` 和 `Saved/RenderTrailTraces/`，完整 payload 模式只用于额外解除长 modification/Shader step 的序列化限制。

诊断文件记录完整的 Analyzer → Worker 请求、Worker 原始 stdout 和 stderr、完整 Pixel History/Event Context/DebugPixel 响应、发送给 Agent 的完整预筛选证据，以及模型请求和响应元数据。常规的 `RenderTrailAgent.log` 和 `gs.log` 仍然会有意限制大小。启用 GPU 故障诊断时，每次会话还会生成带 PID 和随机后缀的 `RenderTrailReplayWorker_*.log` 以及对应的 `*.renderdoc.log`，不会再依赖 RenderDoc 默认的按秒日志名，因而不会与 crash-handler 或 qrenderdoc 同秒启动的日志互相覆盖。

制作可分发版本时，请在项目中创建 `Config/RenderTrailDiagnostics.ini` 覆盖插件默认值：

```ini
[RenderTrailDiagnostics]
bEnabled=false
bWorkerProtocol=false
bAgentTraffic=false
bFullEvidencePayload=false
bGpuCrashDiagnostics=false
bFastReplay=true
bRenderDocDRED=false
```

当 `bFullEvidencePayload=true` 时，隔离 Worker 还会解除 Pixel History modification、绑定资源、Shader 变量/源码列表和 DebugPixel 步骤的常规证据限制，同时保留 65,536 个调试步骤的安全上限。该模式用于复现因果链断裂，不适合日常截帧。

Replay 加载和查询还会写入低开销的阶段计时。`progress` 行覆盖 RenderDoc DLL/运行时初始化、RDC 容器、可用 GPU、ReplayOptions、`OpenCapture` 总进度、Replay 元数据、`SetFrameEvent` 和预览导出；其中 `OpenCapture` 按当前 RenderDoc v1.45 的权重标记 debug resource、resource/chunk initialisation 和 frame event replay。`diagnostic` 行以 begin/end 记录 Pixel History、Event Context、DebugPixel、Action Index 与 Replay 关闭耗时。Analyzer 在加载或查询长时间没有完成时每 5 秒写入 `worker_wait`，Worker 自身的独立 watchdog 也每 5 秒写入总体进度、最后一次 progress callback 距今时间和进程内存；停止时写入 `worker_stop_complete`，明确区分正常退出和 1 秒等待后的强制终止。上述记录都会保存在对应会话的 `Saved/Logs/RenderTrailDiagnostics/` 文件中。

日常因果分析默认使用 `bFastReplay=true`，即把 RenderDoc `ReplayOptions.optimisation` 设为 `Fastest`。RenderDoc 对该级别的契约是：只启用不会造成无效或错误 Replay 的优化。当前 D3D12 实现的差异点是避免为 `DiscardResource` 和 `UNDEFINED` transition 额外写入 RenderDoc 的调试填充值；原始 Draw/Dispatch/Copy、资源绑定、Pixel History、Event Context 和 Shader Debug 都不会被关闭。RenderTrail 本来也禁止把 discard/未定义内容当作像素正向贡献证据，因此该优化不会缩短有效因果链。`ready` 消息、状态栏和诊断日志都会记录实际的 `Fastest` 或 `Balanced`。

如果需要对 RenderDoc 的未定义内容可视化做 A/B 对照，可在项目级 `Config/RenderTrailDiagnostics.ini` 中设置：

```ini
[RenderTrailDiagnostics]
bFastReplay=false
```

`bGpuCrashDiagnostics=true` 会在 `OpenCapture` 前后记录操作系统、TDR、相关 Unreal/RenderDoc 进程、DXGI 适配器 LUID、显存预算和当前 Worker 显存占用。失败后还会读取最近五分钟的 `nvlddmkm`、`Display` 与 `Microsoft-Windows-DxgKrnl` System Event XML，并把最后进度、Worker 日志和 RenderDoc 原始日志路径附到结构化错误。`bRenderDocDRED` 默认关闭：DRED Auto Breadcrumb 与 Page Fault 跟踪会增加 GPU 和系统内存开销，不应与每次正常因果 Replay 绑定。只有在稳定复现 `DXGI_ERROR_DEVICE_HUNG/REMOVED` 时才把它设为 `true`；该设置只修改 Worker 中的 RenderDoc 运行时，不调用 `RENDERDOC_SaveConfigSettings`，其它 GPU 快照与实时日志在 DRED 关闭时仍然保留。

为了兼容尚未重载新版 Editor 模块的会话，Worker 收到旧版已有的 `-RenderTrailFullDiagnostics`、但没有显式 GPU 开关时，仍会自动启用 GPU 快照与独立日志，但不会再隐式开启 DRED。新版 Analyzer 总会传入显式 Fast/Balanced 与 DRED enable/disable 参数，因此项目级 `bFastReplay` 和 `bRenderDocDRED` 覆盖不会被兼容回退改变。

隔离 Worker 必须重建 Replay 资源，无法与仍在运行的 Unreal Editor 共享其 D3D12 资源。对超过 1 GiB 的截帧，Fast Replay 和关闭 DRED 可以消除不必要的写入与诊断开销，但不能消除两套资源同时驻留导致的显存超额。如果日志仍显示 dedicated/shared GPU memory 逼近预算，保持因果能力的兜底方式是关闭占用该 GPU 的 qrenderdoc/其它实例，必要时关闭 Editor 后由 Worker 单独加载；不要强制改用不同厂商的 GPU，因为跨驱动 Replay 可能改变取证条件。

查询编排还会记录以下稳定阶段名，便于按 `requestId` 还原等待位置：`worker_request_queued/dispatched/completed` 区分 Analyzer 本地排队、实际写入 Worker 和总完成时间；`analysis_generation_changed`、`selection_state_cleared` 与 `worker_superseded_result_discarded` 记录换点清理边界；`event_context_queued/completed` 和 `shader_debug_queued/completed` 是模块边界；`trace_schedule_begin/end`、`trace_branch_queued/completed/deduplicated` 是资源分支；`agent_evidence_compaction`、`agent_completion_phase`、`ModelActionJsonRepair` 或 `ModelActionLocalFallback` 覆盖模型证据大小、结束原因和结构化恢复结果。Worker 的 `pixel_history.replay` 会记录 Replay key 的 cache hit/miss，`event_context.set_frame_event/collect` 与 `shader_debug.*` begin/end 可进一步区分卡在 RenderDoc 的哪一步。Event Context 和 Shader Debug 完成后不再强制恢复 FinalEvent；下一条依赖管线状态的请求会自行设置目标 Event。

选点标题下方会持续显示历史完整度：资源分支和 writer 上下文分别显示已返回、失败、查询中与安全上限截断数量。“分析当前像素”从一开始就使用 64 条跨资源 Pixel History、24 个 Event Context 的聚焦深追预算；不存在额外的继续深追按钮。Agent 只等待关键前沿，不会被非关键诊断队列拖住。

实时日志标题栏的 **释放 Replay** 只在 Worker 没有待处理请求且 Agent 未运行时可用。它关闭隔离 Worker、回收 Replay 的 GPU/系统内存，但不清除预览、像素证据、确定性报告或 Agent 上下文；日志记录 `manual_replay_release_begin/end`。释放后仍可阅读报告和询问已收集证据；需要重新分析像素时，点击 **分析当前像素** 按需重新载入 Replay并自动深追。

## Agent 语义整理

规则化像素分析完成后，紫色的 **Agent 语义整理** 卡片可以把有界证据转换为面向人的说明。

Agent System Prompt 已外置到标准 Unreal 风格的 INI 文件 `Plugins/RenderTrail/Config/RenderTrailAgentPrompt.ini`，每次运行 Agent 时都会重新加载。修改其中重复的 `+Line=` 条目后，可以直接再次运行 Agent，无需重新编译插件。若要提供项目级覆盖，请创建 `Config/RenderTrailAgentPrompt.ini`；项目文件优先于插件默认文件。旧版 `RenderTrailAgentPrompt.txt` 格式仍作为迁移回退方案。如果所有 Prompt 文件都无法读取，RenderTrail 会使用最小安全回退 Prompt，并向 Unreal 日志写入警告。

1. 在 Unreal Editor 中打开 **Project Settings > Plugins > RenderTrail Model Broker**。
2. 选择供应商预设（OpenAI、Google Gemini、OpenRouter、DeepSeek、Groq、自定义兼容服务或本地兼容服务），在需要时填写供应商 API Key；使用 DeepSeek V4 模型时可配置 **Enable Thinking**。Thinking 默认关闭，以保持有界 Agent 请求稳定。Base URL Override 留空时使用预设地址；自定义/本地供应商既可以填写 Base URL，也可以填写完整的 `/chat/completions` URL。
3. 点击 **Fetch Models**，在使用当前 Key 返回的实时模型列表中搜索并选择所需模型，例如 Flash 或 Pro 版本。选中的 ID 会复制到 **Model ID (Current / Manual)**；对于不提供模型发现接口的供应商，该字段仍可手动编辑。这些值按项目、按用户保存，不属于共享项目默认配置。当前编辑器进程还可以通过 `RENDERTRAIL_MODEL_ENDPOINT`、`RENDERTRAIL_MODEL_NAME` 和 `RENDERTRAIL_MODEL_API_KEY` 覆盖配置；不希望把 Key 写入 INI 文件时，优先使用环境变量。
4. 截取一帧，选择一个关注像素，然后点击 **分析**。这是一个显式操作：仅选择像素不会发送任何证据。
5. 结果卡片的分析边界止于选中像素。默认界面不再重复展示一篇长报告，而是直接消费 Agent 的结构化字段：一句结论、颜色变化、Pass/Pipeline/Shader 摘要、像素链和证据缺口；完整自然语言回答与原始报告默认折叠。固定 System Prompt 禁止继续推测 Blueprint、C++、玩法逻辑、状态机或其他上游根因。

该实现保留了 Coding Agent 循环中有用的边界，但没有引入通用 Coding Agent。RenderTrail 自行管理 Endpoint、模型和 Key 配置，每次有界整理只发送一次直接兼容 OpenAI Chat Completions 的请求；不需要 Unreal MCP Server 或 MCP Session。

像素证据提取与预算由 Analyzer 的工作流和证据组件负责，模块入口不参与具体分析。Agent 在一次有界请求中接收确定性事件链及已收集的上下文，仅执行语义总结；它不会选择要检查的 RenderDoc 事件。

只有当前 P1 返回完整的 `eventSummaries` 后，请求才会发送；否则 UI 会停止并提示重新构建或重载 Worker。请求内容包括规则结论、有界因果路径、项目/地图元数据、选中坐标、最终观测值、候选事件、带显式完整性标记的近期确定性事件链窗口，以及已经收集的事件上下文。

`.rdc`、预览图和完整事件树不会上传。Analyzer 标签页不会接收供应商 Endpoint 或 API Key。当 RenderDoc Marker 和当前 `.rendertrail.json` 都不包含 UE 资产或 PrimitiveComponent 映射时，Mesh 归属会明确保持为 `unknown`。

截帧命令不会启动任何模型服务。如果配置的 RenderTrail Endpoint 不可用，只有紫色语义整理阶段会失败；预览、Pixel History 和确定性因果卡片仍可继续使用。

## 开发日志

### 2026-08-09 — 因果覆盖式 Agent 证据选择

- 移除详细 Event Context 按“关键事件、浅层优先、同层新 EID 优先”的扁平 Top-N。Analyzer 现在构建覆盖所有已收集上下文的 `deterministicEventContextIndex`，显式保存 confirmed writer/consumer 边、Pixel History 结果、语义角色和详细选择状态。
- 12 个详细上下文名额按因果覆盖分配：末端关键事件、确认 Pixel Writer、资产 Marker、BasePass/PrePass/GBuffer、Nanite、Depth、链条断点和深层前沿都拥有保留机会；未选上下文仍在轻量索引和本地全量文件中可见。
- 每个详细上下文中的绑定资源、Provenance 和 Pixel History 不再按插入顺序截取，改为覆盖 SceneColor、Depth、GBuffer、Nanite VisBuffer、GPUScene Primitive/Instance、Material、确认坐标、实际 writer 和失败边界。
- resource producer 调度在每个 consumer 内先按上述取证语义排序，再展开最多 16 个；剩余 producer 的准确 Event ID 会登记为安全边界，不再静默丢弃。
- 新增 `RenderTrail.Analyzer.Agent.ContextCausalCoverage` 自动化测试，验证深层 BasePass 资产 writer、Nanite writer 和显式断点不会被较新的浅层后处理噪声挤出详细预算。

### 2026-08-09 — D3D12 Device Lost 取证日志

- 每次 Replay 会话使用 PID 与随机后缀生成独立的 Analyzer、Worker 和 RenderDoc 日志，避免 RenderDoc Worker、crash-handler 或 qrenderdoc 同秒启动时覆盖默认日志。
- Worker 在 `OpenCapture` 前后记录操作系统、TDR、相关进程、DXGI 适配器/LUID/显存预算、RenderDoc GPU 枚举、驱动版本和实际 ReplayOptions；独立 watchdog 每 5 秒记录总体进度、progress callback 停滞时间和内存。
- GPU Replay 失败时先落盘结果码、最后阶段和 DRED 状态，再查询 DXGI 快照及最近五分钟的 NVIDIA/Display/DxgKrnl System Event XML，避免驱动恢复期间的次级查询挡住核心错误。
- GPU 故障诊断模式在 Worker 内临时开启 RenderDoc DRED，但不保存全局设置。RenderDoc 实时写入唯一的 `*.renderdoc.live.log`；正常关闭前读取原始字节并保留为 `*.renderdoc.log`，确保 `DXGI_ERROR_DEVICE_HUNG` 返回后 breadcrumb/page-fault 日志不被析构删除。
- Analyzer 记录 Worker PID、退出码、最后阶段与独立日志路径；结构化 `open_capture` 错误也直接携带最后加载阶段和两份诊断日志位置。

### 2026-08-06 — MSAA 逐 sample 因果编排

- Worker 协议升级到 v3；Ready 响应新增最终目标的资源索引、格式和真实 sample 数，避免把 Pass Marker 中的 `MSAA=4` 误当作最终 RT 属性。
- `pixel_history` 支持指定任意纹理资源、mip、slice、sample、type cast 和 `beforeEventId`；Analyzer 从 consumer 的绑定输入自动分叉查询，并从真实像素 writer 继续递归。
- Event Context 保存 shader binding 与 descriptor 子资源；同一 D32S8 资源的 Depth/Stencil binding 不再因 ResourceId 相同而被合并。
- DebugPixel 证据新增实际执行的纹理访问指令、嵌套变量值和资源变量；当 Load 坐标与查询像素吻合时，映射可升级为 `confirmed-executed-values`。
- 每个空 sample、失败查询、writer/context 上限和总查询预算都会以结构化 branch status 保留，避免报告把未展开误写成“没有更早原因”。
- “分析当前像素”自动查询最终写入者、候选 Shader Debug、各直接输入、sample 1+、实际过滤足迹及递归 producer；Agent 等待自动深追收束，安全上限之外的分支以预算边界保留。
- Analyzer 新增请求排队/完成、关键/后台余量、Event Context、Shader Debug、资源分支与模型阶段日志；Depth/Stencil 等完全相同的底层资源/sample 查询会合并并保留 binding aliases。
- Agent 证据改为有界紧凑上下文，不再重复携带完整 Shader Trace 和资源历史；模型多写不匹配的 `]` 时执行有界 closer 修复，仍无法解析则展示本地确定性结果，不再把原始 JSON 当作用户错误输出。

### 2026-08-05 — 原生预览与按需 Replay

- 截帧阶段直接读回同一 Viewport 的原生尺寸画面并保存到 `Saved/Previews`；Analyzer 不再为了显示可选点画面而等待 Replay Worker 冷启动。
- `.rendertrail.json` 新增可选的预览路径、尺寸和 `previewPixelExact` 契约；旧截帧中的 RenderDoc 内嵌窗口缩略图不会被误当作最终 RT 像素坐标。
- Analyzer 命中像素精确预览后延迟创建 Replay；只有点击 **分析当前像素** 才启动 Worker，预览和 P1 在加载期间保持可用。
- Worker 在完整 Replay 就绪前不会用低分辨率 RenderDoc 缩略图覆盖原生预览，完成后再输出精确目标 RT。
- 恢复插件内 `RenderTrailReplayWorker` TargetRules，并改为可独立重建的单体后台可执行文件；它仍是插件内部故障隔离边界，不是独立 Analyzer 产品。

### 2026-08-05 — 像素因果图与证据模块化

- 将 Analyzer 的像素视图、证据模型/因果图和 Prompt 加载拆成独立模块；将 Worker 的资源生产者和生命周期判定拆到独立 Replay 取证模块。
- 区分 `finalWriter` 与 `significantWriterCandidate`，数值化 Before/After 并输出颜色差值；末端微调不会再被自动描述为主要形成原因。
- 新增有界 `causalGraph`：包含最终 RT 像素写入跳转、资源 producer 边、坐标映射状态、Shader/Pipeline 摘要、UE Material/Mesh/Actor 归属状态和链条断点。
- producer 判定改用 RenderDoc `GetUsage`；`Discard` 现在明确表示资源失效和因果链断点，不再被错误当作颜色生产者。
- `DebugPixel` 的已执行指令现在映射到有界反汇编窗口；它能作为实际执行证据，但没有源码映射时仍不等同于 UE 材质公式。

### 2026-08-05 — Analyzer 分层交互与结构化结果

- Analyzer 改为“左侧像素预览 + 右侧结论/证据检查器 + 底部常驻 Agent 输入”，去除右侧连续堆叠的长卡片流。
- 新增结构化结果视图：用 Before/After 色块和带文字状态的链条节点区分已证明写入、显著候选与证据断点；Pass、Pipeline、Shader 只保留紧凑摘要。
- Agent 的完整自然语言回答和原始可复制报告默认折叠，Prompt 同步限制 `answer` 避免重复结构化字段；同一 P1 的追问只携带上一轮问答作为语言上下文。
- 将原来集中在 `RenderTrailAnalyzerEditorModule.cpp` 的编辑器实现迁入独立 `RenderTrailAnalyzerHome`；模块入口现在只保留启动、关闭、标签页注册和打开捕获转发。结构化报告展示继续由独立的 `RenderTrailAnalyzerResultView` 承担。
- 继续将 Home 中的底层 I/O 拆出：Replay 进程/管道、Agent HTTP、Agent JSON 协议和诊断日志分别由独立组件拥有；Home 不再持有 `FProcHandle`、匿名管道或 `FHttpRequestPtr`。同时删除已被替代的 `#if 0` MCP 与编辑器内 Replay 旧实现，源码只保留当前架构。
- 将 Pipeline 固定功能状态与 Shader Debug Trace 的纯格式化逻辑移入 `RenderTrailEvidenceFormatting`，防止页面控制器逐渐演变成第二个证据模块。

### 2026-08-05 — 单像素分析与完整诊断修复

- Analyzer 改为单像素模式：界面只保留 `P1`；点击其他位置会替换当前点，旧点的 Pixel History、Event Context 和 Shader Debug 响应不会进入新点报告。
- 移除当前分析路径中的跨点共同锚点、覆盖率和分叉归纳，末端候选只针对当前像素计算。
- 修复完整诊断启动路径：每次截帧会初始化独立诊断会话，并在启用 `bFullEvidencePayload` 时向隔离 Worker 传入 `-RenderTrailFullDiagnostics`。

### 2026-08-03 — 完整 Replay 诊断

- 新增支持项目级覆盖的 `Config/RenderTrailDiagnostics.ini`。现在每次截帧会话都可以在 `Saved/Logs/RenderTrailDiagnostics/` 下写入完整诊断文件，其中包含原始 Worker 协议、stderr、完整确定性证据载荷和 Agent 流量。常规 Unreal/Agent 日志仍保持有界。
- 新增 Worker 完整证据模式，解除 Pixel History、绑定资源、Shader 变量/源码列表和 DebugPixel 轨迹数据的常规序列化限制，同时保留 65,536 步安全上限。

### 2026-08-01 — 截帧与离线 Replay 基础

- 将原始 OPC 原型和规格中的名称统一更改为 `RenderTrail`，覆盖插件、Unreal Program Target、构建脚本和文档。
- 最初的原型使用独立的 `RenderTrailAnalyzer` 和 `RenderTrailReplayWorker` Target；之后由编辑器内 Analyzer 模块取代，同时保留隔离 Worker 边界。
- 将直接调用 RenderDoc Application API `TriggerCapture` 的路径替换为显式的 Start/End Frame Capture 流程，与 Alt+F12 的 Viewport 选择和强制 Viewport 绘制保持一致，同时传入选中 Viewport 的原生窗口句柄，而不是进程活动窗口。插件会等待四次文件大小稳定采样后再打开截帧，不再把任意编辑器 Present 或警告弹窗误当作分析目标。
- 新增恢复已经完成但尚未认领的 `.rdc`。如果编辑器在截帧文件写完后崩溃，下次点击菜单时可以重新打开该文件，无需重复截帧。
- 大型 `.rdc` 保留在 `Saved/RenderDocCaptures`，只在旁边新增小型 `.rendertrail.json` 文件，避免在截帧交接时复制或移动数百 MB 数据。
- 新增长驻的进程外 Replay API Worker。`.rdc` 只加载一次，之后供所有像素查询复用；Replay API 故障与编辑器进程隔离。

### 2026-08-01 — 有界像素因果分析与语义整理

- 新增最终 Render Target 预览、像素坐标映射、Pixel History 查询、关注点采样、候选 Draw/Pass 选择和有界事件上下文查询。
- 将序列化证据限制为五个点、每个像素最新 256 条 modification、每个事件 16 个输入/输出资源以及 10 个展示用追踪跳转。JSON 会显式标记截断。
- 新增规则化因果卡片，分别展示已确认观测、嫌疑项和成本最低的下一项查询。被所有选中像素共同经过的全屏 Action 会视为共同锚点，不会自动当作根因。
- 新增像素局部 Agent 流程。历史版本由 Unreal MCP 管理供应商、模型和 Key 配置，Analyzer 管理固定 Prompt、证据预算、两个只读查询动作和四轮限制。
- 将截帧/恢复与检查合并为 Level Editor 顶部工具栏中的一个 **RenderTrail** 图标。

### 2026-08-02 — 顶部工具栏入口

- 将截帧并检查操作从 **Tools** 菜单移到 Level Editor 顶部的 Play 工具栏，与 **Open Neural Rendering Lab** 使用的分组相邻。截图图标仍然是单击入口：优先恢复最新的完整截帧，否则截取下一帧，然后打开 Analyzer 标签页。

### 2026-08-02 — Replay 启动修复（已被后续实现取代）

- 修复从插件二进制目录启动 Worker 时出现的 `Replay Worker exited with code 3`。Analyzer 现在会传入项目 Unreal 二进制基础目录，使 Program 能够找到 Engine ICU 数据。
- Worker 查找遵循 UBT 标准的宿主项目/Engine `Binaries/<Platform>` 输出位置；插件不声明或暂存 Worker 可执行文件及 TBB DLL。
- Worker 启动、Ready 状态、结构化错误和提前退出现在都会记录到 Unreal Editor 日志中的 `LogRenderTrailAnalyzer`；结构化 `open_capture` 消息会保留在 UI 中，不再被通用退出码覆盖。

### 2026-08-02 — 显式确认像素选择

- 点击预览现在只会添加或移除有界关注点（`P1`–`P5`），不会立即启动后台查询。
- 新增 **确认选点并开始分析** 操作，接受任意非空点集，然后将所有选中像素一起提交给 Pixel History。这样可以保持选点编辑流畅，并使分析边界明确可见。
- **清空选点** 和 **确认选点并开始分析** 控件使用固定宽度，因此缩小 Analyzer 面板时，按钮不会横向拉伸填满整行。
- **运行语义筛选** 现在位于上述两个选择控件旁边；Agent 面板继续专注展示解释和状态输出。

### 2026-08-02 — UI 约束与 Broker 诊断

- 完整的 **Agent 语义整理** 区域现在始终可见。Broker 配置说明不再把主要的 **运行语义筛选** 操作隐藏在可展开帮助按钮之后。
- 继续支持预览缩放。鼠标右键和中键平移现在受到边界限制：较小图像保持居中，放大图像不能被完全拖出预览区域。
- 高倍率下使用保留的 BGRA 预览数据精确绘制逐像素四边形，不再使用 Slate 默认的双线性动态纹理采样。当像素格足够大时，会显示真实像素网格、悬停单元格轮廓和已选单元格轮廓；滚轮缩放以光标为中心，并支持更大的检查倍率。
- 将每轮语义输出请求从 1200 Token 提高到 2048 Token，并兼容 `choices[0].message.content` 的字符串和数组形式。
- 保留 MCP `content[].text` 兼容回退，并区分字段缺失与供应商实际返回空 Assistant Content。空响应会报告 `finish_reason`、Content 长度和 Reasoning 长度，不再显示具有误导性的通用 `structuredContent.content` 错误。
- 新增有界本地诊断。每次语义运行都会重置 `Saved/Logs/RenderTrailAgent.log`，其中记录预筛选证据、每一轮、解析后的 MCP/SSE 响应和最终结果。`Saved/Logs/gs.log` 记录 Broker 模型/结束元数据，以及最多 2400 个字符的 Assistant Content 预览。诊断中绝不会写入 API Key、预览图、完整事件树或 `.rdc`。
- 上述修改完成后，原始独立 Target 和 `gsEditor` 均成功构建；Analyzer 随后迁移到下文所述的 Editor 模块。

### 2026-08-02 — 仅编辑器分发与无标签多点证据

- 将完整 Analyzer Slate UI/Controller 从独立 Unreal Program 迁移到 `RenderTrailAnalyzerEditor` Editor 模块。截帧完成后会在 Unreal Editor 中打开或聚焦一个 Nomad 标签页。
- 移除 `RenderTrailAnalyzer` Program Target。Replay Worker 仍在进程外运行，因此 RenderDoc Replay、驱动或加载器故障不会终止编辑器。
- 将 Worker 实现和 Program Target 移入插件。源码分发现在遵循普通 UBT Target 生命周期，不需要项目级 Worker 源码、自定义暂存脚本或插件内预编译 Worker。
- 用五个无标签关注点（`P1`–`P5`）替换异常点/对照点模式。规则分析会比较共同事件后缀和各点链条分叉，不会假设任一选中点代表正确值。
- 更新 Agent 证据 Schema 和固定 Prompt，使用 `P1`–`P5`，生成逐点颜色/形成过程摘要；没有参考图或预期值时，不会虚构设计意图。

### 2026-08-02 — Agent HTTP 回调安全与输出预算

- 将所有 Agent Broker `ProcessRequest` 和 `CancelRequest` 调用延迟到下一次 Core Ticker。HTTP Manager 会在调用回调期间遍历自身请求数组；如果在该回调中启动下一轮或取消当前 SSE 请求，可能触发 UE 的 `Array has changed during ranged-for iteration` Ensure。
- 延迟请求在启动前会检查对象身份，因此取消操作或已经完成的轮次不会在下一个 Tick 复活过期请求。
- 将项目级默认及当前模型输出预算提高到 8192 Token。此前请求成功到达供应商，但在生成 4874 个 Reasoning 字符后，因为请求仅允许 2048 Token 而以 `finish_reason=length` 停止。

当前验证状态：原始故障已经成功到达 Unreal MCP 并完成一轮模型请求，但第二轮响应因 Content 为空或格式不兼容而停止。HTTP 重入保护、8192 Token 预算以及 120 秒 MCP/Broker 超时已经写入源码。需要重启 Unreal Editor 以加载新模块，然后再次运行语义整理；重启后的日志是最终端到端检查依据。

### 2026-08-02 — 统一分析操作、Marker 可见性与分发说明

- 将分析窗口中的三个操作按钮替换为一个固定大小的 **分析** 按钮。第一阶段确认选中点并执行有界规则查询；查询结束后，第二阶段启动 Agent。
- 移除价值较低的 **只做下一步** 卡片及其重复的下一步报告字段。
- 所有选中点的十字线/像素标记改为纯黑色。悬停单元格轮廓仍为黄色，以便区分当前光标位置。
- 将 Agent 和 Model Broker 的供应商请求总超时/空闲超时提高到 120 秒，避免长时间推理被 Unreal 默认约 30 秒的 HTTP 活动超时截断。
- 新增截帧加载计时反馈。打开 `.rdc` 时，Analyzer 状态行会刷新已用时间和当前 Replay 阶段；`LogRenderTrailAnalyzer` 会记录开始、Replay Ready、预览解码和总加载时长，便于诊断慢速截帧。
- 右侧的结论、因果路径、嫌疑项、Agent 输出/状态和技术证据改为只读可选文本。用户可以选中文字后按 Ctrl+C，或通过上下文菜单复制报告和错误详情。
- Model Broker 继续由 RenderTrail 插件拥有，并作为正确的分发边界：Analyzer 把有界证据直接发送给配置的供应商，Broker 管理供应商、Endpoint、模型和凭据。API Key 必须保留在用户本地。分发副本时，优先使用 `RENDERTRAIL_MODEL_*` 环境变量覆盖或操作系统密钥存储；不要把含用户 Key 的 `Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini` 打包进去。

详细设计和证据边界维护在 `../../../RENDERTRAIL_RENDERDOC_DEBUG_AGENT_SPEC_2026-08-01.md` 中。

### 2026-08-02 — RenderTrail 自有 Model Broker 与分阶段 Replay 加载

- 将 RenderTrail 自有模型设置移入插件较早加载的 `RenderTrailCore` 模块。设置区域仍位于 **Project Settings > Plugins > RenderTrail Model Broker**，包含 Endpoint、Model、API Key、Max Output Tokens 和 DeepSeek V4 Thinking 控件。这样可以避免编辑器加载 Capture 和 Analyzer 模块时发生较晚的 UHT 类注册。
- 移除 RenderTrail 对 Unreal MCP 插件的 Agent 调用依赖。Analyzer 直接发送兼容 OpenAI 的 Chat Completions 请求，并支持 `RENDERTRAIL_MODEL_ENDPOINT`、`RENDERTRAIL_MODEL_NAME` 和 `RENDERTRAIL_MODEL_API_KEY` 环境变量覆盖。项目仍可为其他编辑器/外部进程集成启用 `ModelContextProtocol`；RenderTrail 不会将其作为插件依赖加载，也不会启动 MCP Server 或 Session。
- 现有的用户级原型设置会从旧 Model Broker 区域迁移，且不会记录 API Key。项目分发仍应排除 `Saved/Config/WindowsEditor`。
- 编辑器内 Replay 模块会报告内部加载阶段，在预览 PNG 比 `.rdc` 更新时复用它，并将完整 Action 标签/路径索引延迟到真正请求 Pixel History 或 Event Context 时再构建。

### 2026-08-02 — 确定性因果证据扩展

- 即使详细 modification 载荷仅保留最新 256 条，Pixel History 现在也会输出完整的逐事件汇总；如果该汇总不可用，Analyzer 会拒绝声称因果链完整。
- Analyzer 会在语义输出前自动收集相关事件的上下文，包括 Pipeline State、Shader Reflection、资源输入/输出、Marker Path、精确 Action Flag，以及绑定资源的尽力而为前序生产者。它会明确标记同一资源的坐标映射尚未证明。如果受支持，还会为选中的写入者自动运行一次有界 `DebugPixel` Trace，并把确定性 Trace State 样本加入技术卡片和 Agent 证据。Agent 在一次仅语义请求中接收这些证据，不能选择额外的 RenderDoc 查询。

### 2026-08-02 — 完全由编辑器侧调度的隔离 Replay

- 编辑器内 Replay 实验在已经被 RenderDoc 注入的 Unreal Editor 中触发 `DXGI_ERROR_DEVICE_RESET` 后，恢复独立的 `RenderTrailReplayWorker` Program 边界。
- Analyzer 启动 Worker，并通过标准管道交换现有的结构化证据协议；Editor 进程绝不会调用 `RENDERDOC_InitialiseReplay` 或 `RENDERDOC_ShutdownReplay`。
- Worker 定义 `REPLAY_PROGRAM_MARKER`，每个进程只接受一个截帧，并仅在进程退出时关闭 RenderDoc，以符合 RenderDoc 的 Replay 生命周期约定。

## Shader 调试配置

项目在 `Config/DefaultEngine.ini` 中启用了 RenderTrail Shader 取证配置。它会生成 Shader Symbol 和名称、关闭优化、保留未使用的 Interpolator、跳过调试 Shader 压缩，并输出编译器调试产物。修改这些设置后需要重启编辑器；首次启动会触发大规模 Shader 重编译，`Saved/ShaderDebugInfo` 可能快速增长。

版本匹配的 RenderDoc v1.45 源码参考和 Replay Adapter 说明保存在 `ThirdParty/RenderDoc` 中。
