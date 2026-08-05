# RenderTrail 开发文档

[返回 README](../README.md)

RenderTrail 是一个仅用于编辑器的 UE 插件。截帧、界面和模型配置运行在 Unreal Editor 中；RenderDoc Replay Controller 运行在独立的 Worker 进程中，因此 Replay 或驱动故障不会重置编辑器 GPU：

- `RenderTrailAnalyzerEditor`：编辑器内的 Nomad 标签页，包含预览、像素选择、因果卡片和 Agent 流程。
- `RenderTrailReplayWorker`：由插件拥有的 UBT Program 模块，包含 RenderDoc Replay API 适配器。编辑器通过管道启动生成的 `RenderTrailReplayWorker.exe`，自身不会链接或调用 Replay API。

Analyzer 内部按职责拆分：`RenderTrailAnalyzerEditorModule` 只负责模块生命周期、Nomad 标签页注册和 `OpenCapture` 转发；`RenderTrailAnalyzerHome` 负责编辑器面板状态及分析工作流编排；`RenderTrailAnalyzerImageView` 负责像素预览与选择；`RenderTrailAnalyzerResultView` 负责结构化结论、颜色、Pass/Pipeline/Shader 和证据链展示；`RenderTrailAnalyzerEvidence` 负责 Pixel History 数值化、事件聚合和因果图；`RenderTrailEvidenceFormatting` 将 Pipeline 与 Shader 调试 JSON 转成可读证据，但不补写缺失语义；`RenderTrailReplayWorkerClient` 独占 Worker 进程、管道和退出状态；`RenderTrailAgentClient` 独占模型 HTTP 请求生命周期和兼容响应解析；`RenderTrailAgentProtocol` 负责结构化 Action JSON 的严格解析与有界修复；`RenderTrailAnalyzerDiagnostics` 负责配置、会话日志和脱敏后的 Agent 诊断；`RenderTrailAnalyzerPrompt` 负责可覆盖 Prompt 的加载。Replay Worker 内部的资源生产者、Discard 和跨资源边界判定位于 `RenderTrailReplayEvidence`。

按照常规方式在项目或引擎中启用插件，然后编译普通 Editor Target。Level Editor 顶部工具栏的 Play 区域会显示一个 **RenderTrail** 图标，与 **Open Neural Rendering Lab** 位于同一区域。点击后，插件会优先恢复上一次编辑器运行遗留的、已经完成但尚未认领的 `.rdc`；如果没有可恢复文件，则按照 Epic 的 Alt+F12 截帧流程执行，但会显式传入当前聚焦的 Game Viewport 或活动 Level Editor Viewport 的原生窗口句柄。在 PIE 运行期间，即使点击工具栏暂时改变了 Slate 焦点，插件仍会保留 Game Viewport 作为截帧目标。插件等待文件大小稳定后写入 UE 上下文，然后在 Unreal Editor 中打开 RenderTrail Analyzer 标签页。

RenderDoc 截帧要求 Epic 的 `RenderDocPlugin` 已附加，并且当前 RHI 受支持。若只截取目标 Viewport，请关闭 **Project Settings > Plugins > RenderDoc > Capture all activity**；开启该选项会有意包含 Slate、所有 Viewport 以及编辑器窗口活动。如果一次截帧包含多个 Present/SwapBuffer 输出，Replay Worker 会选择有证据支持的最大输出，避免误选较小的通知窗口。

`.rdc` 由 RenderDoc 创建在 `Saved/RenderDocCaptures` 下；RenderTrail 只在旁边新增一个 `.rendertrail.json` UE 上下文文件，不复制体积较大的截帧。旧版位于 `Saved/RenderTrail/Captures` 下的截帧仍然受支持。

## 插件归属与构建布局

所有 Replay 实现代码和版本匹配的 RenderDoc SDK 均位于 `Plugins/RenderTrail` 下。唯一位于项目中的小型文件 `Source/Programs/RenderTrailReplayWorker.Target.cs`，只是构建隔离进程所需的标准 Unreal TargetRules 入口；插件不依赖批处理脚本、自定义暂存步骤或插件内预编译 Worker。

SDK 可以由插件、宿主项目、Engine 或 `RENDERTRAIL_RENDERDOC_ROOT` 环境变量提供。

## 像素检查流程

1. 在 UE 中点击 Level Editor 顶部工具栏的 **RenderTrail** 图标。截帧完成并稳定后，编辑器内的 Analyzer 标签页会自动打开。
2. 等待最终目标预览出现，并确认状态显示 `Pixel History: yes`。
3. 使用鼠标滚轮缩放，按住鼠标中键或右键拖动平移。高倍率下，预览会从带过滤的缩略图采样切换为精确的源像素方格，叠加单像素边界，并高亮光标所在单元格，确保点击坐标没有歧义。
4. 点击一个希望解释的像素，该点记为 `P1`，不会被预先认定为正确、正常或异常。再次点击当前像素会将其清除；点击其他位置会直接替换当前点，旧点的证据不会与新点合并。点击 **分析当前像素** 确认 P1 并读取 Pixel History；规则分析完成后，可在底部填写问题并点击 **发送**。单纯选择像素不会启动查询或调用 Agent。
5. 右侧检查器默认打开 **结论与链条**：先显示简短结论、Before/After 颜色块、Pass/Pipeline/Shader 摘要，再用“已证明 / 候选 / 断点”节点展示像素形成链。完整事件、资源、Shader 调试和最近 24 个可追踪跳转放在独立的 **技术证据** 页。底部 Agent 输入始终绑定当前 P1；连续追问仅保留上一次问答作为指代理解上下文。点击 **清除** 会清除当前像素和报告。

为控制载荷大小，每个选中像素仅保留最新 256 条详细 Pixel History modification；但 Replay 模块还会针对完整 Pixel History 输出逐事件汇总 `eventSummaries`。如果该汇总缺失或不完整，Analyzer 会停止，而不会根据被截断的尾部证据生成因果结论。

确定性分析层会自动收集当前像素最多 24 个相关事件的 Pipeline、Shader 和资源上下文，优先收集最终 RT 上最多 10 个已确认写入者，并为每个事件最多展开 3 个有资源写入证据的 producer，反向深度最多 10 跳；这些上下文不是由 Agent 选择的。每次分析只处理一个像素，不执行跨点共同链或分叉归纳。输入与输出尺寸不一致时，精确的同坐标祖先追踪会停止，不会虚构连接。

报告会明确区分最后物理写入者和显著颜色形成候选。Analyzer 将 RenderDoc 的 Before/After 数值化并计算 `colorDeltaMax`、`colorDeltaL1`；小于一个标准 8-bit 量化步长的变化只作为继续上溯的启发式条件，不作为根因证明。跨资源部分以 `causalGraph` 输出 RT/资源候选、Pass/Event、Pipeline、Shader、producer 关系、坐标映射状态和 UE 归属状态。绑定资源只算候选输入；没有实际采样坐标和值时，`pixelTraceStatus` 会保持 blocked，不会声称该输入贡献了 P1。

`.rdc` 不包含现成的 UE 语义因果链。它保存 GPU 命令、资源、Pipeline、Shader 和可用 Marker，Pixel History 由 RenderDoc 回放时重建。RT 写入和资源使用通常可以推导；但精确采样坐标、材质、Mesh 或 Actor 在没有 Shader 调试、源码映射、ShaderMap 或 UE Marker 时可能不可恢复，此时 RenderTrail 会保留 `unknown` 或显式 `chainBreak`。

Analyzer 运行期间，`.rdc` 会保持在 Replay Worker 会话中打开，因此每次像素查询都会复用同一个 Replay 会话。Worker 提供 Pixel History、完整事件汇总和确定性的有界事件上下文前沿。对于选中的末端写入者，如果 Replay 支持，Analyzer 还会自动请求 `DebugPixel`，收集指令/源码映射数量、有界输入值和常量值、执行状态样本、变量变化值、调用栈深度及执行标志。这些信息可以证明该点实际执行了什么，但不会假装总能从调试轨迹中恢复高层材质公式。

穿过 copy、resolve 或 resample 资源递归追踪生产者时，仍然受坐标映射证据约束；无法证明映射时，会将该处报告为因果链边界。

## 完整 Replay 诊断

插件还维护一个独立的、不受常规证据限制的诊断会话，用于调查确定性链条中的缺失环节。插件默认配置 `Config/RenderTrailDiagnostics.ini` 会启用该功能，并在 `Saved/Logs/RenderTrailDiagnostics/` 下为每次截帧会话写入一个文件。

诊断文件记录完整的 Analyzer → Worker 请求、Worker 原始 stdout 和 stderr、完整 Pixel History/Event Context/DebugPixel 响应、发送给 Agent 的完整预筛选证据，以及模型请求和响应元数据。常规的 `RenderTrailAgent.log` 和 `gs.log` 仍然会有意限制大小。

制作可分发版本时，请在项目中创建 `Config/RenderTrailDiagnostics.ini` 覆盖插件默认值：

```ini
[RenderTrailDiagnostics]
bEnabled=false
bWorkerProtocol=false
bAgentTraffic=false
bFullEvidencePayload=false
```

当 `bFullEvidencePayload=true` 时，隔离 Worker 还会解除 Pixel History modification、绑定资源、Shader 变量/源码列表和 DebugPixel 步骤的常规证据限制，同时保留 65,536 个调试步骤的安全上限。该模式用于复现因果链断裂，不适合日常截帧。

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
