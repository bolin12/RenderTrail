# 像素溯源设计

本文说明 RenderTrail 如何从最终 RT 上的选中像素，追到颜色来源、几何归属和覆盖层证据。

## 用户流程

1. 载入或捕获 `.rdc`。
2. 显示与 RenderDoc 最终目标 RT 一致的权威图像。
3. 用户在图像上选中一个像素。
4. 点击“分析当前像素”。
5. 系统自动收集最终 Pixel History，并沿跨资源输入做有界深追。
6. UI 展示确定性事实、并行因果 lane、Pipeline/Shader 证据和所有裁剪边界。

不再提供“继续深追”按钮。一次分析必须自动追到 producer、reset、终端输入、明确失败或安全预算边界之一。

## 坐标不变量

选点成立的前提是展示图与 RenderDoc 最终 RT 表示同一个资源视图：

- 相同资源和 subresource；
- 相同 mip、array slice 和 sample 语义；
- 相同宽高和裁剪区域；
- 明确的 Y 轴方向、缩放和 letterbox 偏移；
- 点击坐标在映射后落到合法整数 texel。

Load 使用精确 texel。Sample、resolve、缩放和时域路径可能依赖 footprint；此时系统查询有限的自适应坐标和 sample，并把结果合并为一个分支，同时保留原始查询和映射置信度。

## 三类证据不能混为一条链

| 因果线 | 要回答的问题 | 典型证据 |
| --- | --- | --- |
| 最终颜色 | 哪些资源和 Pass 形成最终像素值 | Composite、Tonemap、Bloom、TSR、SceneColor producer |
| 几何归属 | 哪个可见表面覆盖该像素 | Depth writer、BasePass、drawcall、mesh marker |
| 覆盖层 | 是否有 outline、UI、debug overlay 等覆盖 | 独立颜色/遮罩资源及其 producer |

最终物理 writer 只回答“最后是谁写入目标 RT”。它通常是 Composite 或后处理，并不自动回答“哪个 Mesh 在这里”。Mesh 归属应从深度/可见性资源独立建立，然后与最终像素共享同一个屏幕坐标事实。

## 递归追踪模型

每个待追踪节点包含：

- trace purpose；
- consumer EID；
- 被读取的资源与 subresource；
- 映射后的坐标、sample 或 footprint；
- 已访问路径；
- 当前深度和预算。

对节点执行 Pixel History 后，分支必须落入下列状态之一：

| 状态 | 含义 |
| --- | --- |
| `producer` | 找到对当前读取有效的上游 writer |
| `reset` | 遇到真正清空相关值的边界 |
| `terminal` | 外部输入、常量、不可继续解释的资源入口 |
| `no_modification` | 查询成功，但该位置没有匹配修改 |
| `pruned` | 已达到资源、上下文、深度或时间预算 |
| `error` | Replay、映射或协议失败 |

`pruned`、`error` 和 `no_modification` 不能被渲染成“没有来源”。

## Writer 选择规则

- 颜色 lane 选择对目标颜色有效、且发生在 consumer 读取之前的 writer。
- 几何 lane 以深度或可见性资源为入口；仅 ClearStencil 的事件不是深度 writer。
- 真正的 Depth Clear 是 reset，而不是 Mesh producer。
- 覆盖 lane 单独处理 outline、mask、UI 或 debug overlay，避免污染颜色主链。
- 在同一事件、资源和语义下的 MSAA sample 或相邻 adaptive tap 可分组，但原始记录仍保留。

## 因果 lane 分组

`causalLanes` 按下列键归并：

`tracePurpose + consumer + resourceIndex + resourceName + producer/reset`

`resourceIndex` 是资源身份的权威字段，名称只用于展示。同名的输入、输出或历史纹理必须保留为不同节点；只有资源 ID 与事件关系都一致时才允许归并。

每条 UI/Agent 分支使用统一方向：

`consumer EID ← resource ← producer EID`

分组的目标是压缩重复 sample 和坐标，不是删除 Replay 证据。统计中必须区分：

- `evidenceRecordCount`：原始证据记录，包括 sample、tap 和裁剪边界；
- `queryRecordCount`：实际发给 RenderDoc 的 Pixel History 查询；
- grouped branch count：对人和模型展示的语义分支数。

## 最终颜色主路径

`primaryColorPath` 不是把所有颜色输入按事件号串起来，而是从最终物理 writer 开始，逐 hop 选择最能解释当前值的已执行输入。每个候选边至少记录：

- consumer/producer EID 与资源 ID；
- shader binding 和 read/read-write 访问语义；
- Shader Debug 实际 sample 值；
- producer 的 before、shader output 和权威 written value；
- producer 是否改变该位置的值；
- 边角色、置信度和停止边界。

主路径选择优先采用实际执行读取且 sample 值与 producer 写后值匹配的边；中性零输入、consumer 自身 read-write 输出和自环不会成为向上游推进的主边。颜色、几何和覆盖旁支仍完整保留，不能因为未进入主路径就删除。

边角色至少包括：

| 角色 | 含义 |
| --- | --- |
| `value-changing-producer` | producer 在该位置产生了不同的写后值 |
| `pass-through-producer` | producer 被执行并写入，但该位置值保持不变 |
| `neutral-input` | 已执行读取，sample 值为零或对本次输出中性 |
| `consumer-read-write-output` | 当前 consumer 自己的读写输出，不是上游输入 |
| `geometry-owner` | 独立的 Depth/可见性几何归属证据 |
| `external-history-boundary` | 已到跨帧或外部 history，当前 capture 内不能唯一闭合 |
| `budget-boundary` / `unresolved-boundary` | 因预算或证据不足而停止，不能表述成“没有来源” |

对于 Draw、Copy 和 Compute，Pixel History 的 `lastAfter/postMod` 是资源写后值。`shaderOutput` 只表示 API 能提供的 shader 输出字段；尤其对 Compute，它可能是零或无代表性，不能覆盖实际资源值。

Shader Debug 若证明某条纹理采样指令已执行，并给出 UV、导数/footprint 与 sample 结果，即可构成强执行读取证据。只有在宣称“唯一精确上游 texel”时，才要求精确整数坐标；bounded footprint 不应被降级成“未证明读取”。

## 当前安全预算

默认策略会随版本调整，UI 和 trace 文件中的实际值始终高于文档中的示例。当前重点约束为：

| 预算 | 当前策略 |
| --- | --- |
| 详细 Pixel History modifications | 每次查询最多 256，event summary 保留完整计数 |
| Event Context | 聚焦模式最多 24 |
| 跨资源 Pixel History | 聚焦模式最多 64 |
| 递归深度 | 最多 10 hop |
| 单事件候选资源 | 最多 12 |
| Agent 详细上下文 | 最多 12，另带完整轻量索引 |

安全上限必须以“已收束但被预算裁剪”的方式显示，同时报告待处理 producer 和资源组数量。

## Pipeline、Shader 与 Mesh 证据强度

证据强度从高到低大致为：

1. Pixel History 证明该 draw 在该像素通过测试并写入目标；
2. Shader Debug 证明具体输入、控制流或输出值；
3. Pipeline state 证明绑定的 shader、资源、深度/混合状态；
4. event marker、资源名或 mesh 名支持候选归属；
5. 仅根据 Pass 名或画面外观推测。

RenderTrail 必须把“确定”“结构性支持”“候选”和“未知”分开。Marker 中出现 `SM_Cylinder` 可以成为强候选，但若没有 draw/资源绑定证据，不能把它提升为精确 Actor 或材质实例证明。

## 全量调试输出

每次分析目录保存：

- Worker 请求与响应；
- Pixel History 摘要与完整 modifications；
- Event Context、Pipeline 和 Shader Debug 结果；
- 调度、去重、裁剪和错误记录；
- Agent 证据投影与响应；
- 最终结构化快照和 JSONL 阶段日志。

UI 负责给出可读摘要，本地 full trace 负责证明是否遗漏。两者不能用同一个过度压缩的 payload 替代。

## 已知边界

- RDC 只能证明 capture 中实际存在、且 RenderDoc 能重放的事实。
- 某些 compute、temporal 或自定义采样路径不能唯一反推出一个上游 texel，只能给出 bounded footprint。
- Pixel History 很昂贵，特别是 MSAA、大尺寸资源和复杂 D3D12 capture；串行 Replay 是稳定性选择。
- Shader 调试信息不足时，系统能建立资源和 writer 因果关系，但不能声称已经复原 shader 内部全部数学贡献。
