# RenderTrail 开发文档索引

本页是开发入口，不再混放当前状态、设计细节、诊断说明和逐日开发日志。

## 建议阅读顺序

1. [STATUS](STATUS.md)：先确认当前版本已经完成什么、最近验证了什么、还缺什么。
2. [PIXEL_TRACING](PIXEL_TRACING.md)：理解像素坐标、writer 选择、自动深追和三类因果 lane。
3. [ARCHITECTURE](ARCHITECTURE.md)：理解 Editor/Worker 进程边界、模块职责和产物布局。
4. 根据改动范围阅读 [AGENT_AND_UI](AGENT_AND_UI.md) 或 [DIAGNOSTICS](DIAGNOSTICS.md)。
5. 需要追溯设计演进时再阅读 [CHANGELOG](CHANGELOG.md)。

## 按主题查找

| 你要处理的问题 | 首选文档 |
| --- | --- |
| 当前实现是否已经修复、是否需要重启验证 | [STATUS](STATUS.md) |
| 展示图和 RenderDoc 最终 RT 坐标是否一致 | [PIXEL_TRACING](PIXEL_TRACING.md#坐标不变量) |
| 为什么最终 Composite 不能代表 Mesh 来源 | [PIXEL_TRACING](PIXEL_TRACING.md#三类证据不能混为一条链) |
| 自动深追为何停止、是否遗漏 producer | [PIXEL_TRACING](PIXEL_TRACING.md#递归追踪模型) |
| Pixel History 为什么慢、如何查看全量记录 | [DIAGNOSTICS](DIAGNOSTICS.md) |
| Agent 为什么只看到部分详细上下文 | [AGENT_AND_UI](AGENT_AND_UI.md#证据投影) |
| UI、提示词和输出 schema 如何配合 | [AGENT_AND_UI](AGENT_AND_UI.md) |
| Worker 崩溃或 D3D12 Device Lost | [DIAGNOSTICS](DIAGNOSTICS.md#d3d12-device-lost) |
| 历史上某项能力何时加入 | [CHANGELOG](CHANGELOG.md) |

## 代码地图

| 代码区域 | 主要职责 |
| --- | --- |
| `RenderTrailAnalyzerHome` | 会话状态、自动深追调度、证据汇总和 Agent 触发 |
| `RenderTrailAnalyzerImageView` | 权威 RT 展示、坐标变换和选点 |
| `RenderTrailAnalyzerEvidence` | writer 规则、分支状态和因果 lane 分组 |
| `RenderTrailAnalyzerResultView` | 顶部事实、lane 卡片和完整性 UI |
| `RenderTrailReplayWorkerClient` | Worker 生命周期、协议、心跳和超时 |
| `RenderTrailAgentClient` / `RenderTrailAgentProtocol` | 语义请求和结构化响应 |
| `RenderTrailAnalyzerDiagnostics` | trace、阶段耗时和可选完整 payload |
| Worker `RenderTrailReplayEvidence` | Pixel History、Event Context、Pipeline 和 Shader Replay |
| `RenderTrailTracePolicyTests` | writer、分组、预算和终端状态策略测试 |

更完整的模块边界见 [ARCHITECTURE](ARCHITECTURE.md#模块职责)。

## 构建与验证入口

- 插件是 UE 5.8 Editor-only 模块。
- Replay Worker Target 位于 `Tests/RenderTrailReplayWorker.Target.cs`。
- 修改 writer、分支或 lane 策略后，至少运行 `RenderTrail.Analyzer.Trace.FocusedPolicy`。
- 修改 UI、提示词或模块 DLL 后，应重启 Unreal Editor，再对固定 capture/固定像素进行端到端对比。
- 修改坐标或 preview 逻辑后，必须同时核对 RenderDoc 最终 RT 的资源、尺寸、方向和选中 texel。
- 性能结论应根据 full trace 的 Replay 阶段耗时得出，不能只根据 Agent 总等待时间判断。

## 文档维护规则

- `STATUS.md` 只保留当前事实、最新基线、待验证事项；完成或失效的进展移入 CHANGELOG。
- 主题文档描述长期设计，不复制每日进展。
- `CHANGELOG.md` 只追加历史，不作为当前行为的权威来源。
- README 只提供产品概览、快速开始和文档导航。
- 数字预算若可能随配置变化，应标明“当前默认”或引导读者查看 trace 中的实际值。
- 任何“没有来源”的结论都必须区分 `no_modification`、`pruned`、`error` 和真正的 terminal input。

## 外部规格

早期设计规格位于：

`../../../RENDERTRAIL_RENDERDOC_DEBUG_AGENT_SPEC_2026-08-01.md`

该规格用于理解背景；当前行为以代码、自动化测试、[STATUS](STATUS.md) 和实际 full trace 为准。
