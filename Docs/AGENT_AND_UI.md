# Agent 与结果 UI

Agent 的职责是整理已经收集到的 RenderDoc 证据，而不是替代 Replay、猜测资源来源或凭 Pass 名补全链条。

## 证据投影

一次 Agent 请求包含三种互补视图：

- 全局轻量索引：覆盖所有已收集 Event Context，防止重要事件因详细上下文上限而完全消失；
- 详细上下文：最多展开 12 个高价值事件的 Pipeline、Shader、资源和 Pixel History 细节；
- `causalLanes`：覆盖全部上下文的分组因果分支，区分颜色、几何和覆盖层。

详细投影是模型输入预算，不是 Replay 收集上限。被压缩的信息仍应在本地 full trace 中可查。

## 输出契约

结构化结果应包含：

- 最终物理 writer；
- `lanes[]`：每条 lane 的目标、结论、分支和证据引用；
- Pipeline 与 Shader 事实；
- 几何/Mesh 归属及其置信度；
- 未解析分支、裁剪原因和失败；
- 简短 `process[]`：只描述最终 RT 的直接写入顺序，最多四步。

因果步骤统一表达为：

`consumer EID ← resource ← producer EID`

Agent 不得把不同资源、不同 trace purpose 的事件串成一条看似连续的时间链。

## 提示词位置

默认提示词位于插件配置中，可由项目级配置覆盖。代码内保留最小回退提示词，用于外部配置缺失时维持协议，不应成为日常修改入口。

提示词必须明确：

- 最终 writer 与 Mesh 归属是不同问题；
- `evidenceRecordCount` 不等于实际查询数；
- `pruned`、`error` 和 `unknown` 不得写成否定事实；
- 只有确定性证据可以使用“最终由……形成”一类强措辞；
- 不上传 `.rdc` 或截图，不凭画面猜资产名。

## 结果页层次

结果 UI 按下列层次组织：

1. 顶部事实：最终物理 writer、lane 数、几何/Mesh、覆盖、Pipeline、Shader；
2. 因果 lane 卡片：最终颜色、几何归属、覆盖层分别展示；
3. 最终 RT 直接写入：只保留短时间序列；
4. 证据详情：Event、Pipeline、Shader、资源与 Pixel History；
5. 完整性与诊断：查询数、证据记录数、裁剪、错误和 full trace 路径。

Mesh 信息不能显示在 Composite 卡片内部，除非证据确实证明该 Composite draw 本身就是该 Mesh draw。

## Broker 配置

Agent 请求由显式的“分析当前像素”触发。插件可通过本地 Broker 或兼容的 Chat Completions 端点发送一次结构化请求。

典型配置项包括：

- Broker URL；
- 模型名；
- API key 对应的环境变量；
- 请求超时和最大输出；
- 项目级提示词覆盖。

密钥只从环境或受控配置读取，不写入 trace、日志或 `.rdc` 元数据。Broker 失败只影响语义整理，不能使已经完成的 RenderDoc 证据分析显示为失败。

## 发送时机与失败处理

- 等待必需的 event summary 和已调度高价值上下文完成后再发送 Agent 请求。
- 到达安全预算时可以发送，但必须携带明确的完整性和裁剪状态。
- JSON 不完整时可进行受限修复；无法恢复时使用本地确定性结果。
- Agent 文本不得覆盖或修改底层证据，只能引用和组织它。
- 用户应始终能够从结果页进入本地 full trace，核对模型没有展示的细节。

## 隐私边界

- 默认不向 Agent 上传 `.rdc`、截图或工程资产。
- 只发送完成任务所需的结构化证据字段。
- 日志应对密钥、认证头和用户路径中的敏感片段做脱敏。
- 用户禁用 Agent 时，Pixel History、Pipeline、Shader 和 lane UI 仍应完整工作。
