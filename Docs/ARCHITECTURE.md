# RenderTrail 架构与构建

本文描述长期稳定的模块边界、进程模型和文件布局。当前实现进展见 [STATUS](STATUS.md)。

## 总体结构

RenderTrail 是 UE 5.8 Editor-only 插件。编辑器进程负责交互、调度和结果展示；隔离的 Replay Worker 负责打开 `.rdc`、执行 Pixel History、读取 Pipeline/Shader 状态以及生成结构化证据。

```text
Unreal Editor
  ├─ Capture / 选点 / UI
  ├─ Trace 调度与预算
  ├─ 证据归并与 Agent 投影
  └─ Worker Client
          │ 单请求协议
          ▼
RenderTrailReplayWorker
  ├─ RenderDoc Replay Controller
  ├─ Pixel History / Event Context
  ├─ Pipeline / Shader Debug
  └─ 结构化响应与诊断日志
```

Worker 隔离的目的不是并行压榨 RenderDoc，而是把潜在的 D3D12 Device Lost、Replay 崩溃和大 capture 内存压力与编辑器进程隔开。一个 Worker 同时只处理一个 in-flight 请求；新一代分析会使旧结果失效并清理相应状态。

## 模块职责

| 模块或类 | 主要职责 |
| --- | --- |
| `RenderTrailAnalyzerEditorModule` | 插件入口、命令和工具栏注册 |
| `RenderTrailAnalyzerHome` | 分析会话、状态机、调度、证据汇总和 Agent 请求 |
| `RenderTrailAnalyzerImageView` | 权威预览、坐标变换和选点交互 |
| `RenderTrailAnalyzerResultView` | 事实摘要、因果 lane 和诊断结果展示 |
| `RenderTrailAnalyzerEvidence` | writer 选择、因果分组和结构化证据模型 |
| `RenderTrailEvidenceFormatting` | 稳定、可审计的文本与 JSON 格式化 |
| `RenderTrailReplayWorkerClient` | Worker 生命周期、协议、心跳和 watchdog |
| `RenderTrailAgentClient` | Agent Broker / Chat Completions 调用 |
| `RenderTrailAgentProtocol` | Agent 请求和响应契约 |
| `RenderTrailAnalyzerDiagnostics` | 阶段日志、trace 文件和可选 GPU 诊断 |
| `RenderTrailAnalyzerPrompt` | 外部提示词加载与回退提示词 |
| Worker `RenderTrailReplayEvidence` | RenderDoc Replay、Pixel History、上下文和 Shader 证据采集 |

## Capture 生命周期

1. 插件通过 RenderDocPlugin 对目标 viewport 发起 capture。
2. capture 使用精确 viewport handle，并在需要时开启 Capture All Activity。
3. 从候选 Present 中选择与目标 viewport 对应的最终输出。
4. 保存 `.rdc`、capture 元数据和两类预览。
5. 用户选点后，编辑器向隔离 Worker 发起有界证据查询。

最终 RT 的资源、尺寸、sample 和坐标原点必须作为一个整体记录。后续所有像素坐标映射都从该权威入口开始，不能从任意缩略图反推。

## 构建布局

- 插件模块只面向 Editor Target，不进入 Game、Client 或 Server 包。
- Replay Worker Target 位于 `Tests/RenderTrailReplayWorker.Target.cs`。
- Worker 与编辑器插件共享协议结构，但不共享活动的 Replay Controller。
- RenderDoc SDK 查找顺序支持插件目录、项目目录、Engine 目录以及显式环境配置。
- Release Replay 关闭开发期完整 payload 和高成本诊断，仅保留用户可理解的错误和必要摘要。

## 文件与产物

| 内容 | 默认位置 |
| --- | --- |
| Capture | `Saved/RenderTrailCaptures/*.rdc` |
| Capture 元数据 | `Saved/RenderTrailCaptures/*.rendertrail.json` |
| 普通预览 | `Saved/RenderTrailCaptures/*.png` |
| RenderDoc 权威预览 | `Saved/RenderTrailCaptures/*.renderdoc.png` |
| 单次分析 trace | `Saved/RenderTrailTraces/<analysis-id>/` |
| Shader 调试信息 | `Saved/ShaderDebugInfo/` |

普通预览可用于浏览；选点和坐标校验必须以 RenderDoc 权威预览及其元数据为准。

## 并发与代际控制

- 语义上的颜色、几何、覆盖线索可同时入队和归类。
- RenderDoc Replay 请求由单 Worker 串行处理。
- 每次新分析递增 generation；旧 generation 的完成回调必须丢弃。
- 清空选点、切换 capture 或关闭面板时，应取消或失效旧状态，避免旧结果覆盖新 UI。
- watchdog 只负责识别 Worker 无响应；不能把超时等价为“该像素没有历史”。
