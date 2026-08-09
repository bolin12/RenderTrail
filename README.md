# RenderTrail

RenderTrail 是面向 Unreal Engine 编辑器的 RenderDoc 像素溯源插件。它从最终 RT 上的一个选点出发，收集 Pixel History、关键 Pass、Pipeline、Shader 和跨资源 producer 证据，并把最终颜色、几何归属与覆盖层分开呈现。

RenderDoc Replay 运行在隔离 Worker 中。编辑器负责 capture、选点、调度和展示；Worker 负责重放 `.rdc`，降低 Device Lost、Replay 崩溃和大 capture 内存峰值对编辑器的影响。

## 主要能力

- 捕获或恢复 `.rdc`，并保存 capture 元数据和权威 RenderDoc 最终 RT 预览。
- 保证展示图、点击坐标和最终目标 RT 使用同一资源视图与坐标映射。
- 一次“分析当前像素”自动执行有界深追，无需继续深追按钮。
- 分别建立最终颜色、几何/Mesh 归属和覆盖层因果 lane。
- 提取 Pixel History、before/after、Pipeline、Shader 和事件上下文证据。
- 合并 MSAA sample 与 adaptive footprint 的重复分支，同时保留实际查询和原始证据计数。
- 将完整请求、响应、调度、裁剪和错误保存到本地 full trace，便于核查遗漏。
- Agent 只整理已经收集到的证据；Agent 不可用时，本地确定性结果仍可工作。

## 快速开始

1. 在 Unreal Editor 中打开 RenderTrail 面板。
2. 新建 capture，或载入已有 `.rdc`。
3. 在 RenderDoc 权威最终 RT 图像上选择像素。
4. 点击“分析当前像素”，等待自动深追收束后查看三类因果 lane 和完整性状态。

Capture 默认保存到 `Saved/RenderTrailCaptures/`，单次分析 trace 保存到 `Saved/RenderTrailTraces/`。

## 支持范围

- Unreal Engine 5.8 Editor
- Windows
- D3D12
- RenderTrail Beta 0.4.x

插件为 Editor-only，不进入 Game、Client 或 Server 包。

## 文档导航

| 文档 | 内容 |
| --- | --- |
| [当前进展](Docs/STATUS.md) | 最新完成项、基线数据、验证结果和剩余缺口 |
| [像素溯源设计](Docs/PIXEL_TRACING.md) | 坐标、Pixel History、writer、深追、因果 lane 与预算 |
| [架构与构建](Docs/ARCHITECTURE.md) | 编辑器/Worker 边界、模块职责、capture 与文件布局 |
| [Agent 与结果 UI](Docs/AGENT_AND_UI.md) | 证据投影、提示词契约、结果页和隐私边界 |
| [Replay 诊断与性能](Docs/DIAGNOSTICS.md) | trace、日志、Fast Replay、DRED、内存和 Shader Debug |
| [开发文档索引](Docs/DEVELOPMENT.md) | 面向贡献者的阅读顺序、代码地图和验证入口 |
| [变更记录](Docs/CHANGELOG.md) | 按时间整理的开发历史 |

当前最关键的端到端验收项，请直接查看 [当前进展](Docs/STATUS.md)。
