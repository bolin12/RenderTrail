# RenderTrail 变更记录

本文保留按时间组织的开发历史。当前可用能力和剩余问题以 [STATUS](STATUS.md) 为准；这里的旧限制只代表当时版本。

## 2026-08-10

### 因果 lane、提示词与结果页重构

- 新增最终颜色、几何归属和覆盖层三类 `causalLanes`。
- 分组键统一为 trace purpose、consumer、resource 与 producer/reset，折叠重复 MSAA sample 和 adaptive tap。
- 将原始证据记录数、实际 Pixel History 查询数和分组分支数拆开统计。
- Agent 输出新增 `lanes[]`，`process[]` 缩减为最终 RT 的短直接写入序列。
- 结果页改为 lane 卡片与确定性事实摘要；Mesh 归属从 Composite 说明中移出。
- 新增独立的几何/Mesh 原始报告段落和完整性状态。
- 扩充 focused trace policy 自动化测试，并完成新编辑器模块链接验证。

## 2026-08-09

### 因果覆盖与筛选修正

- 自动深追改为围绕 writer-dominance 选择上下文和资源，不再依赖单一后处理链。
- 完整轻量事件索引覆盖所有已收集上下文，Agent 详细上下文保持有界。
- 记录 producer 上下文、资源组、预算裁剪和终端边界，避免“未展开”等价成“没有来源”。
- 几何 writer 选择忽略仅 ClearStencil 的事件；真正 Depth Clear 作为 reset。
- 增加 full trace JSONL、请求/响应快照和调度诊断。

### D3D12 Device Lost 诊断

- 增加 Worker 心跳、watchdog、Replay 阶段和 Windows 事件时间关联。
- 将超时、进程退出、协议错误和 Device Lost 分开报告。
- 增加可选 RenderDoc DRED/GPU crash 诊断开关。

## 2026-08-06

### MSAA 与逐 sample 追踪

- Pixel History 支持逐 sample 请求和记录。
- 相同事件/资源的 sample 证据可归并展示，同时保留底层记录。
- 改进 coverage、depth/stencil 和颜色 writer 的选择规则。

## 2026-08-05

### 原生预览与按需 Replay

- 增加 RenderDoc 原生最终 RT 预览，修正展示图与最终目标坐标不一致的问题。
- 普通预览和权威 RenderDoc 预览使用不同后缀保存。
- Load 使用精确 texel；Sample/resolve 路径增加有限 footprint 查询。
- Replay Controller 在分析期间复用，昂贵上下文按需加载。

### 因果图与模块重构

- 将证据选择、格式化、Worker 协议、Agent 协议、诊断和 UI 从 Home 类中逐步拆分。
- 资源追踪由单链演进为带 producer/reset/terminal 状态的分支图。
- 引入 generation 和 superseded result 丢弃机制，避免旧回调覆盖新选点。

### 分层 UI 与结构化结果

- 结果页区分最终 writer、关键 Pass、Pipeline、Shader、Mesh/几何和完整性。
- 增加本地确定性结果，在 Agent 失败时仍能展示 Replay 事实。
- 选点分析成为统一入口，逐步移除分散的手工深追操作。

### 单像素与完整诊断

- 增加单像素请求、Pixel History modifications 上限和完整 event summary。
- 增加 Worker 请求、响应、阶段耗时与 Agent traffic 的可选落盘。
- 增加 Fast Replay 配置，减少重复资源枚举和无关 payload。

## 2026-08-03

### 完整 Replay 诊断

- 建立每次分析独立 trace 目录和稳定阶段名。
- 增加 Worker 协议、Event Context、Shader Debug 和 Agent 证据快照。
- 增加大 capture、GPU crash 和 Release Replay 的配置边界。

## 2026-08-02

### 工具栏与显式选点

- 增加 RenderTrail 编辑器工具栏入口和主面板。
- 将分析目标绑定到显式选点，清空/更换 capture 时同步清理状态。
- 修正 Replay 启动、capture 路径、viewport handle 和多 Present 选择问题。

### Broker、分阶段 Replay 与确定性证据

- 接入可配置 Agent Broker / Chat Completions 请求。
- Agent 改为只整理结构化 RenderDoc 证据，不上传 `.rdc` 或图像。
- Replay 分为最终 Pixel History、上下文、Pipeline/Shader 和语义整理阶段。
- Agent 失败不再影响本地确定性分析。

### 隔离 Worker 与回调安全

- RenderDoc Replay 移出 Unreal Editor 进程，建立独立 Worker Target。
- 增加单 in-flight 请求、心跳、协议版本和进程退出处理。
- 使用弱引用、generation 和销毁期防护处理异步回调。

### Editor-only 分发

- 插件明确为 Editor-only，不进入 Game/Client/Server 包。
- 补齐插件/项目/Engine/环境配置下的 RenderDoc SDK 搜索。
- 增加 Release Replay 配置和日志脱敏要求。

## 2026-08-01

### Capture 与 Replay 基础

- 建立 `.rdc` capture、元数据和预览保存流程。
- 对接 RenderDocPlugin，并从目标 viewport 的 Present 中选择最终输出。
- 实现基础 Pixel History、Pipeline state 和 Shader 信息提取。

### 有界像素分析与 Agent 原型

- 引入 Pixel History modifications 上限和结构化摘要。
- 建立外部提示词、项目级覆盖和最小回退提示词。
- 明确 Agent 只做证据总结，不负责虚构缺失的 drawcall、shader 或资产归属。

## 外部设计资料

早期设计规格仍保留在项目根附近：

`../../../RENDERTRAIL_RENDERDOC_DEBUG_AGENT_SPEC_2026-08-01.md`

若规格与当前代码或 [STATUS](STATUS.md) 冲突，以当前实现和测试结果为准。
