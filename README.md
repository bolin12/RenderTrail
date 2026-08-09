# RenderTrail

RenderTrail 是一个集成在 Unreal Editor 中的 RenderDoc 单像素溯源插件。它从选中的 `P1` 像素出发，整理最终写入、关键 Pass、Pipeline、Shader、资源生产者以及证据链断点，帮助缩短渲染问题的调用排查链条。

RenderDoc Replay 运行在插件拥有的独立 Worker 进程中；编辑器负责截帧入口、像素交互、结构化结果和 Agent 对话，不会在 Unreal Editor 进程内直接执行 Replay API。

日常分析默认使用因果安全的 Fast Replay：保留有效 Draw/Dispatch/Copy、Pixel History 与 Shader Debug，只省略 RenderDoc 对 discard/未定义内容的额外调试填充；DRED 默认仅在复现 GPU device-lost 时手动开启。两项策略都可在 `Config/RenderTrailDiagnostics.ini` 中切回 Balanced/DRED 诊断模式。

## 主要能力

- 从 Level Editor 工具栏截取或恢复 `.rdc`。
- 一次只选择并分析一个像素，避免跨像素证据混合。
- UE 原生预览与 RenderDoc 最终 RT 分文件保存；只有 Worker 导出的权威最终 RT 允许选点，显示坐标与 Pixel History 查询坐标始终属于同一纹理。
- 展示 Before/After 颜色、末端写入、显著上游候选和颜色差值。
- 整理 Pass、固定 Pipeline、Shader Reflection、DebugPixel 和资源 producer 证据。
- 自动把证据拆成最终颜色链、几何/可见性归属链和编辑器覆盖层链；Shader Debug 返回实际执行的 `Load(x,y)` 时使用确认坐标，返回 `Sample(u,v)` 时保留完整执行 UV，但同一 consumer/resource 的多 tap 会合并成一个结构依赖，并仅在代表坐标没有 writer 时自适应扩展最多 4 个 footprint 坐标。
- Agent 与结果 UI 使用三个并行 `causalLanes`（`color`、`geometry`、`overlay`），不再把 Depth/Mesh、TSR/Tonemap/Bloom 和 Editor Outline 串成一条伪时间线；同一 producer 边上的 MSAA sample 和自适应查询会在展示层去重，原始 Pixel History 数仍单独保留。
- 技术证据逐分支列出 Worker 返回的完整 Pixel History 事件汇总，保留所有 passed/rejected 事件；递归时只选择最后一个真正改变资源值的 `selectedWriterEventId`，没有变化证据时才回退到最后一个 passed writer，避免把“可见历史”错误扩成大量并列因果父节点。
- 在选点旁显示资源/sample 与 producer 上下文的历史完整度；“分析当前像素”会自动执行深追并去重已完成查询，只有达到安全上限的分支才作为截断边界保留。
- 每次选点分析都会在 `Saved/RenderTrailTraces/` 下实时追加 `trace-records.jsonl`（全部 Worker 请求/响应），并在收束时生成 `full-trace.json`（完整目标 Pixel History、全部已收集 Event Context、跨资源 Pixel History、Pipeline、Shader Debug、失败项与预算边界）。工具栏的“打开全量追踪”可直接定位该目录；Agent 回答只是有界摘要，不再被当作全量输出。
- Agent 证据采用“全上下文轻量因果索引 + 12 个覆盖式详细上下文”：详细名额强制覆盖末端关键事件、确认 Pixel Writer、资产 Marker、BasePass/PrePass/GBuffer、Nanite、Depth 和链条断点，不再按浅层/EID 新旧做扁平 Top-N；资源历史、绑定资源与 Provenance 也采用相同的语义覆盖策略。
- 分析结束后可用“释放 Replay”回收隔离 Worker 的 GPU/系统内存，同时保留预览、已收集证据和报告。
- Analyzer 对 Replay Worker 保持单条在途请求，其余查询留在有界的本地队列；替换 P1 时会清除旧点尚未发送的查询和派生证据，旧点唯一的在途请求只收尾并丢弃结果，不会污染或阻塞新点状态。
- 用“已确认、候选、断点”区分证据强度，不虚构缺失的坐标映射、材质、Mesh 或 Actor 归属。
- Agent 只总结已经收集的有界证据，不自行选择或猜测 RenderDoc 事件。

## 当前支持范围

当前开发和验证目标是 **Unreal Engine 5.8、Windows、D3D12**。RenderDoc 路径由使用者在本机维护。Vulkan、其他 RHI 以及 UE 5.5–5.7 暂不作为当前版本的兼容目标，后续逐步支持。

## 快速开始

1. 在项目中启用 `RenderTrail` 和 Epic `RenderDocPlugin`，构建项目的 Editor Target 与 `RenderTrailReplayWorker`。
2. 在 Level Editor 顶部工具栏点击 **RenderTrail**；插件会恢复最近的完整截帧，或截取下一帧。UE 原生预览可在等待期间显示，但暂不接受选点；隔离 Worker 会立即载入 Replay 并导出 RenderDoc 最终 RT。
3. 状态显示“RenderDoc 最终 RT 已就绪”后，点击画面选择 `P1`，再点击 **分析当前像素**。
4. 在右侧查看结构化结论和技术证据；需要语义整理时，在底部向 Agent 提问。

`.rdc` 保存在 `Saved/RenderDocCaptures`。RenderTrail 不复制原始截帧；`Saved/Previews/<capture>.png` 保存 UE 原生预览，`Saved/Previews/<capture>.renderdoc.png` 独立保存权威 RenderDoc 最终 RT，二者不会相互覆盖。

## 文档

- [开发文档、架构、诊断配置与变更记录](Docs/DEVELOPMENT.md)
- Agent Prompt：`Config/RenderTrailAgentPrompt.ini`
- 完整诊断默认配置：`Config/RenderTrailDiagnostics.ini`

当前版本为 Beta，插件清单版本为 `0.4.0`。

点击“分析当前像素”后，确定性溯源会自动完成整条聚焦链，不需要“继续深追”。它从最终 RT 的主导 writer 开始，在每一跳优先执行 DebugPixel，并把同一资源上的 blur/kernel 多 tap 合并成一个结构查询；一个代表坐标没有 writer 时才自适应尝试剩余 footprint。颜色链选择最后改变值的 writer；geometry 链跳过只清 Stencil 的事件，选择真正通过并改变 Depth/Visibility 的 Draw，真正清 Depth/Visibility 时记录 ownership reset。到 BasePass、PrePass、GBuffer、VisBuffer、Nanite Raster 或明确 UE 资产 Draw 时，保留 Pipeline/Shader/Primitive 证据并停止向系统常量扩散。默认边界为 10 跳、24 个 Event Context、64 条跨资源 Pixel History；每个事件最多查询 12 个不同资源组，Shader Debug 不可用时只保留 2 个高价值回退分支。所有执行访问和查询仍写入全量追踪。
