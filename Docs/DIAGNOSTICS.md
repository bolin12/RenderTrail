# Replay 诊断与性能

本文集中说明 RenderTrail 的 trace、日志、Replay 模式、GPU 崩溃诊断和 Shader Debug 配置。

## 默认诊断策略

日常分析保留阶段耗时、请求摘要、完整性计数和错误。完整证据 payload 与 GPU crash 诊断默认关闭，避免日志膨胀和额外 Replay 成本。

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

项目排障时可按需开启，不建议把高成本开关作为发行默认值。

## Trace 内容

每次分析在 `Saved/RenderTrailTraces/<analysis-id>/` 下生成独立目录。按配置可包含：

- Editor 与 Worker 阶段 JSONL；
- Worker 协议请求/响应；
- Pixel History、Event Context 和 Shader Debug payload；
- Agent 证据投影、响应和 JSON 修复记录；
- 完整结构化分析快照；
- GPU crash/DRED 相关日志引用。

完整 payload 模式下，单次 Pixel History 最多保留 65,536 个 debug step；正常 UI 展示仍使用更小的有界摘要。

## 稳定阶段名

编辑器侧常用阶段：

- `worker_request_queued`
- `worker_request_dispatched`
- `worker_request_completed`
- `analysis_generation_changed`
- `selection_state_cleared`
- `worker_superseded_result_discarded`
- `event_context_queued`
- `event_context_completed`
- `shader_debug_queued`
- `shader_debug_completed`
- `trace_schedule_begin`
- `trace_schedule_end`
- `trace_branch_queued`
- `trace_branch_completed`
- `trace_branch_deduplicated`
- `agent_evidence_compaction`
- `agent_completion_phase`
- `ModelActionJsonRepair`
- `ModelActionLocalFallback`

Worker 侧重点阶段：

- `pixel_history.replay`
- `event_context.set_frame_event`
- `event_context.collect`
- `shader_debug.*`

这些名称用于对比不同版本的耗时，不应频繁改名。新增字段优先于替换已有阶段名。

## Fast Replay 与完整诊断

Fast Replay 是默认交互模式，目标是减少重复打开 capture、无关资源枚举和不必要的高成本 payload。它不会跳过用户选中像素所需的 Pixel History。

需要调查 Replay 稳定性或证据缺口时，可切换 Balanced/完整诊断：

- 保存 Worker 协议和完整证据；
- 延长 watchdog 并记录心跳；
- 开启更细的 Replay 阶段耗时；
- 必要时启用 DRED 或 RenderDoc 相关设备丢失诊断。

“更快”与“证据更少”不是同一概念。可以压缩重复 sample、缓存上下文和延迟 Shader Debug，但不能在没有状态说明时静默丢弃 producer 分支。

## Worker 心跳与超时

- Worker 在长 Replay 阶段周期性发送心跳。
- watchdog 超时表示 Worker 无响应或 Replay 卡住，不表示像素没有修改历史。
- 新分析 generation 会使旧结果失效；旧 Worker 回包应记录为 superseded 并丢弃。
- Device Lost、协议错误、进程退出和用户取消必须使用不同错误类型。

## D3D12 Device Lost

当 RenderDoc Replay 触发 D3D12 Device Lost 时，优先收集：

- Worker 最后一个已完成阶段；
- 当前 EID、资源、subresource、sample 与坐标；
- RenderDoc replay 日志；
- Windows Event Log 中对应时间窗口；
- 可选 DRED breadcrumbs 和 page fault 信息。

GPU crash 诊断可能改变性能和日志量，只在复现问题时开启。失败分支应保留在 full trace 中，并在 UI 标记为 error。

## 大 Capture 与内存

超过 1 GiB 的 `.rdc` 可能在解压、资源枚举和 Shader Debug 时产生显著内存峰值。建议：

- 保持单 Worker、单 Replay Controller；
- 复用已打开 capture，避免为每个分支重新初始化；
- 优先 Event Summary，再按价值加载详细上下文；
- 对 Shader Debug 做按需调度；
- 记录进程 working set 与阶段耗时，区分 I/O、Replay 和模型耗时。

## Shader Debug 配置

为了让 RenderDoc 获得可调试 shader，项目通常需要在 `DefaultEngine.ini` 中启用 shader symbols、调试名和额外数据，并按需要关闭相关优化。修改后需要重启编辑器并重新编译 shader。

生成信息通常位于 `Saved/ShaderDebugInfo/`。插件附带的 RenderDoc 1.45 SDK/运行时引用位于 `ThirdParty` 布局中；升级 RenderDoc 时应同时验证协议、Pixel History 和 Shader Debug 行为。

## Release Replay

发行模式应：

- 关闭完整 payload、Agent traffic 和 GPU crash 诊断；
- 保留阶段摘要、完整性计数、用户可理解的错误和 trace 标识；
- 不记录密钥或认证头；
- 保持 Pixel History、因果 lane 和本地确定性结果可用；
- 允许开发者通过配置临时升级诊断级别。
