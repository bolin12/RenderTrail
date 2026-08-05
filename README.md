# RenderTrail

RenderTrail 是一个集成在 Unreal Editor 中的 RenderDoc 单像素溯源插件。它从选中的 `P1` 像素出发，整理最终写入、关键 Pass、Pipeline、Shader、资源生产者以及证据链断点，帮助缩短渲染问题的调用排查链条。

RenderDoc Replay 运行在插件拥有的独立 Worker 进程中；编辑器负责截帧入口、像素交互、结构化结果和 Agent 对话，不会在 Unreal Editor 进程内直接执行 Replay API。

## 主要能力

- 从 Level Editor 工具栏截取或恢复 `.rdc`。
- 一次只选择并分析一个像素，避免跨像素证据混合。
- 展示 Before/After 颜色、末端写入、显著上游候选和颜色差值。
- 整理 Pass、固定 Pipeline、Shader Reflection、DebugPixel 和资源 producer 证据。
- 用“已确认、候选、断点”区分证据强度，不虚构缺失的坐标映射、材质、Mesh 或 Actor 归属。
- Agent 只总结已经收集的有界证据，不自行选择或猜测 RenderDoc 事件。

## 当前支持范围

当前开发和验证目标是 **Unreal Engine 5.8、Windows、D3D12**。RenderDoc 路径由使用者在本机维护。Vulkan、其他 RHI 以及 UE 5.5–5.7 暂不作为当前版本的兼容目标，后续逐步支持。

## 快速开始

1. 在项目中启用 `RenderTrail` 和 Epic `RenderDocPlugin`，构建项目的 Editor Target 与 `RenderTrailReplayWorker`。
2. 在 Level Editor 顶部工具栏点击 **RenderTrail**；插件会恢复最近的完整截帧，或截取下一帧。
3. 等待预览载入，点击画面选择 `P1`，再点击 **分析当前像素**。
4. 在右侧查看结构化结论和技术证据；需要语义整理时，在底部向 Agent 提问。

`.rdc` 保存在 `Saved/RenderDocCaptures`。RenderTrail 只在旁边维护较小的 `.rendertrail.json` 上下文文件，不复制原始截帧。

## 文档

- [开发文档、架构、诊断配置与变更记录](Docs/DEVELOPMENT.md)
- Agent Prompt：`Config/RenderTrailAgentPrompt.ini`
- 完整诊断默认配置：`Config/RenderTrailDiagnostics.ini`

当前版本为 Beta，插件清单版本为 `0.4.0`。
