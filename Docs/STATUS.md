# RenderTrail 当前进展

最后更新：2026-08-10

本文只记录当前实现状态、最近一次验证证据和仍需验证的缺口。长期设计见 [像素溯源设计](PIXEL_TRACING.md)，历史变更见 [CHANGELOG](CHANGELOG.md)。

## 当前结论

RenderTrail 已从“只解释最终后处理写入”推进到“先计算一条值驱动的最终颜色主路径，再并列呈现几何归属、覆盖层和其他输入分支”。线索会并行发现和归类，但 RenderDoc Replay 请求仍按单 Worker 串行执行，以避免 Replay Controller 并发和显存压力。

当前最重要的行为是：

- 选点坐标以 RenderDoc 最终目标 RT 为权威，不再使用与最终 RT 尺寸或方向不一致的展示图坐标。
- “分析当前像素”自动执行有界深追，不再要求用户点击“继续深追”。
- UI 和 Agent 不再把 Depth/Mesh、TSR/Tonemap/Bloom、Outline 串成一条虚假的时间链。
- Mesh 归属作为独立的几何证据显示，不再挂在最终 Composite Pass 名下。
- 最终颜色主路径由本地确定性算法选择，Agent 不再生成或改写跨资源拓扑。
- 因果节点以 RenderDoc `resourceIndex` 区分；同名输入、输出纹理不会再形成虚假的自环。
- Compute/Copy/Draw producer 的权威写后值取 Pixel History `lastAfter/postMod`，不再把无代表性的 compute `shaderOutput` 零值当作结果。
- 全量调试记录保存在本地 trace 目录；Agent 只接收经过压缩、仍可审计的证据投影。

## 最新样本的因果链校正

样本目录：

`Saved/RenderTrailTraces/2026.08.10-00.01.49_capture_P1_1491_684_G2_20260810-000715`

旧结果只强调 E11083 Composite，并把 E10203 Compute 的 `shaderOutput = 0` 当成输出，因此看起来“没有继续往下追”。重新按资源 ID、实际采样值和 producer 写后值评估后，证据支持的颜色主路径为：

`E11083 Composite ← SelectionOutlineColor ← E10891 Composite ← Tonemap ← E10823 Tonemap ← TSR.Output ← E10203 Compute ← TSR.History.Color 边界`

- E11083 与 E10891 是保持选中像素值的传递/合成节点。
- E10823 把 TSR 输出映射为最终显示颜色，是已确认的值变化节点。
- E10203 的权威结果来自 `lastAfter/postMod = [0.1328125, 0.9765625, 0.123046875, 1]`，不能使用其零 `shaderOutput`。
- 当前真正未闭合的位置是 E10203 之前的 `TSR.History.Color` 映射边界，而不是 E11083 之后“没有 Mesh”。
- 几何归属仍作为旁路证据显示；它回答表面/Depth 所属，不伪装成颜色主路径的一环。

## 最近一次基线

基线目录：

`Saved/RenderTrailTraces/2026.08.09-23.32.52_capture_P1_1480_676_G2_20260809-233604`

选点 P1 为 `(1480, 676)`。最终物理写入者是 EID 10688 Composite；几何归属证据落到 EID 1427 WorldGrid/Floor。该次运行仍使用改造前的结果编排，因此最终文本混合了不同类型的线索；它用于衡量新分组逻辑，而不是代表新 UI 的最终输出。

| 指标 | 基线值 | 含义 |
| --- | ---: | --- |
| Event Context | 24 | 达到当前上下文安全上限 |
| Replay 请求 | 103 | 59 Pixel History、24 Event Context、20 Shader Debug |
| Pixel History | 59 | 1 次最终目标查询、58 次跨资源查询 |
| Shader Debug | 20 | 19 成功、1 失败 |
| Agent 详细上下文 | 12 / 24 | 详细展开有界，但轻量索引覆盖全部上下文 |
| 延后 producer 上下文 | 2 | 当前预算下未继续展开 |
| 延后资源组 | 0 | 已调度资源组没有被整体遗漏 |

将新因果分组算法应用到该次旧 trace 后，63 条资源分支记录被归并为 51 个分组分支和 3 条语义线：

| 因果线 | 分组分支 | 已识别 producer | 未解析 | 原始证据记录 |
| --- | ---: | ---: | ---: | ---: |
| 最终颜色 | 43 | 30 | 13 | 49 |
| 几何归属 | 5 | 4 | 1 | 5 |
| 覆盖层 | 3 | 3 | 0 | 9 |

这里的“证据记录数”包含 MSAA sample、相邻自适应采样和裁剪边界记录；“实际查询数”只统计真正发给 RenderDoc 的 Pixel History 请求。二者已在数据模型和 UI 中分开，避免再次把 63 条记录误报为 63 次查询。

## 已完成的改进

### 1. 最终 RT 与选点一致性

- 展示图、点击坐标和 Replay 查询绑定到同一个 RenderDoc 最终目标资源。
- 原生 RenderDoc preview 与普通预览图分开保存，避免缩放图被误当作权威 RT。
- Load 使用精确 texel；Sample 路径根据 footprint 自适应查询并合并多 tap 结果。

### 2. 自动深追与可审计输出

- “分析当前像素”会自动追踪跨资源 producer，直到命中来源、明确边界或安全预算。
- 移除了依赖人工触发的继续深追流程。
- 每次分析保存请求、响应、调度、裁剪和错误信息，便于确认是没有证据、预算裁剪，还是 Replay 失败。
- 详细 Pixel History 有界展示，但完整 event summary 和轻量索引保留全局覆盖。

### 3. Writer 与资源筛选

- writer 选择按 trace purpose 区分颜色、几何和覆盖层。
- 几何链不会把仅 ClearStencil 的事件误当作深度来源；真正的深度 clear 会作为 reset 边界。
- MSAA sample 和相邻自适应坐标可折叠到同一因果分支，减少重复文本，不丢失原始证据计数。
- 每条分支显式记录已找到 producer、reset、终端输入、无修改、被预算裁剪或查询失败。

### 4. 因果线、提示词和 UI

- 新增 `causalLanes` 证据模型，按 `tracePurpose + consumer + resourceIndex + producer/reset` 分组。
- 新增本地确定性的 `primaryColorPath`；它按实际读取、采样值、producer 写后值与边界状态选择主路径。
- 每条边标记 `value-changing-producer`、`pass-through-producer`、`neutral-input`、`consumer-read-write-output` 或明确边界，并附置信度。
- Agent 仍可整理 lane 摘要，但不得输出拓扑步骤；最终主路径、顶部结论和最终 RT 直接写入均由本地证据生成。
- 原 `process[]` 只允许表示最终 RT 的直接写入顺序，最多保留四步，不再承担完整资源图职责。
- 结果页先显示最终颜色主路径，再显示颜色旁支、几何归属和覆盖层；每条 lane 可展开查看未进入摘要的全量分支。
- 原始报告新增“几何 / Mesh 归属（独立于最终 Composite）”段落。

## 验证状态

- 编辑器模块已完成编译和链接检查。
- 已生成并链接 `UnrealEditor-RenderTrailAnalyzerEditor-810010.dll`。
- `UnrealEditor.modules` 已指向上述模块。
- 自动化测试 `RenderTrail.Analyzer.Trace.FocusedPolicy` 通过，进程退出码为 0。

如果 Unreal Editor 在上述 DLL 生成前已经启动，需要重启编辑器后再做同一 capture 的端到端验证，否则看到的仍可能是旧 UI、旧提示词和旧结果编排。

## 仍需验证或保留的边界

- 需要在重启后的编辑器中，对同一 capture 重新分析一次，确认颜色主路径、旁路线索、顶部事实和 Agent 摘要均采用新结构。
- “producer 结构上参与了颜色形成”不等于“已经证明其精确 shader 数学贡献”；只有 Shader Debug 或明确的数据流证据才能给出更强结论。
- Compute/TSR 等路径的精确上游像素映射可能只能到候选 footprint，UI 必须保留映射置信度。
- Agent 详细上下文仍限制为 12 个；完整轻量上下文索引和 `causalLanes` 覆盖全部已收集上下文。
- RenderDoc 耗时仍主要来自 Pixel History Replay。因果分组减少的是重复展示和模型负担，不会伪造或跳过必要的 Replay 证据。
- 当前基线有 2 个 producer 上下文在安全预算处停止。状态必须明确显示为“预算裁剪”，不能显示成“无来源”。
- UE marker 或资源名可支持 Mesh 候选归属，但不能单独证明 Actor、材质实例或 ShaderMap 的精确身份。

## 下一次验收重点

1. 重启 Unreal Editor，确认加载 `810010` 或更新版本模块。
2. 对最新样本 P1 `(1491, 684)` 重跑“分析当前像素”。
3. 确认 UI 首先显示 E11083 到 E10203 的最终颜色主路径，并把 `TSR.History.Color` 标成真实边界。
4. 核对同名但不同 `resourceIndex` 的输入/输出资源不再显示成自环；几何/Mesh 只出现在独立旁路证据中。
5. 核对 UI 中“证据记录数”和“实际 Pixel History 查询数”不再混淆。
6. 打开本地 full trace，确认任何未展开分支都有 producer、reset、terminal、pruned 或 error 之一的明确状态。
