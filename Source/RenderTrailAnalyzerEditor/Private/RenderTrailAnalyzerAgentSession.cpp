#include "RenderTrailAnalyzerHomeInternal.h"

namespace UE::RenderTrail::Private
{
	FReply SAnalyzerHome::StartAgentAnalysis()
	{
		if (bAgentRunning)
		{
			SetAgentStatus(TEXT("Agent 正在运行，请等待当前有界循环完成。"));
			return FReply::Handled();
		}
		if (bReplaySynchronizationPending)
		{
			SetAgentStatus(TEXT("分析正在等待 Replay 同步；完成 ReplayController、目标 RT 和 Pixel History 后才能向 Agent 发送问题。"));
			return FReply::Handled();
		}
		if (!bSelectionConfirmed)
		{
			SetAgentStatus(TEXT("选点已变化，请先点击“分析当前像素”。"));
			return FReply::Handled();
		}
		if (Samples.ContainsByPredicate([](const FPixelSample& Sample)
		{
			return Sample.bPending;
		}))
		{
			SetAgentStatus(TEXT("Pixel History 尚未完成，请等待查询结束。"));
			return FReply::Handled();
		}
		const bool bHasReadyPoint = Samples.ContainsByPredicate([](const FPixelSample& Sample)
		{
			return !Sample.bPending && !Sample.bFailed;
		});
		if (!bHasReadyPoint)
		{
			SetAgentStatus(TEXT("请先选择并分析当前像素，再向 Agent 发送问题。"));
			return FReply::Handled();
		}
		if (Samples.ContainsByPredicate([](const FPixelSample& Sample)
		{
			return !Sample.bPending && !Sample.bFailed && !Sample.bEventSummaryComplete;
		}))
		{
			SetAgentStatus(TEXT("当前 Replay Worker 未返回完整 eventSummaries；已暂停 Agent，请先重新编译并重载 Worker。"));
			return FReply::Handled();
		}
		EnsureRelevantEventContexts();
		EnsureCandidateShaderDebug();
		if (HasPendingWorkerRequests())
		{
			bDeterministicForegroundCompletionReported = false;
			bAgentWaitingForDeterministicContexts = true;
			SetAgentStatus(FString::Printf(TEXT("正在等待自动深追收束（根事件=%d、Shader=%d、必需资源/sample=%d、其它资源/sample=%d、producer 上下文=%d）；全部已排队分支完成后再进行语义提炼。"),
				GetPendingCriticalContextCount(),
				PendingShaderDebugByRequest.Num(), GetPendingRequiredResourceHistoryCount(),
				GetPendingBackgroundResourceHistoryCount(), PendingEventContextIds.Num()));
			return FReply::Handled();
		}

		AgentMessages.Empty();
		AgentStep = 0;
		AgentPendingEventId.Reset();
		bAgentRunning = true;
		if (AgentRunButtonText.IsValid())
			AgentRunButtonText->SetText(FText::FromString(TEXT("处理中…")));
		if (HasPendingBackgroundDeterministicQueries())
		{
			SetAgentOutputText(FString::Printf(TEXT("自动深追已执行到安全上限；Agent 正在整理当前证据，并会保留 %d 个资源组（%d 个候选坐标）、%d 个 producer 上下文的截断边界。"),
				GetPendingBackgroundResourceHistoryCount() + GetBudgetDeferredResourceHistoryCount(),
				GetPendingBackgroundResourceHistoryCount() + BudgetDeferredResourcePixelHistoryRequests.Num(),
				DeferredEventContextIds.Num() + BudgetDeferredEventContextDepths.Num()));
		}
		else
		{
			SetAgentOutputText(TEXT("当前发现的确定性溯源已完成，Agent 只负责整理已收集的证据。"));
		}
		const FString PrefilterEvidence = BuildAgentPrefilterEvidence();
		const FAgentContextCoverageSelection AgentContextSelection = BuildAgentContextCoverageSelection();
		Diagnostics.WriteRecord(TEXT("agent_evidence_compaction"), FString::Printf(
			TEXT("chars=%d contextsIncluded=%d contextsTotal=%d pendingCriticalContexts=%d pendingRequiredResources=%d pendingBackgroundResources=%d budgetDeferredResources=%d budgetDeferredContexts=%d pendingShader=%d"),
			PrefilterEvidence.Len(), AgentContextSelection.DetailedEventIds.Num(), EventContexts.Num(),
			GetPendingCriticalContextCount(), GetPendingRequiredResourceHistoryCount(),
			GetPendingBackgroundResourceHistoryCount(), BudgetDeferredResourcePixelHistoryRequests.Num(),
			BudgetDeferredEventContextDepths.Num(), PendingShaderDebugByRequest.Num()));
		Diagnostics.WriteAgentLog(TEXT("RunStart"), FString::Printf(
			TEXT("Bounded agent run started. samples=%d, evidenceChars=%d, maxTurns=%d\nPREFILTERED_EVIDENCE\n%s"),
			Samples.Num(), PrefilterEvidence.Len(), MaxAgentSteps, *PrefilterEvidence), true);
		FString HumanRequest = AgentIntentTextBox.IsValid()
			? AgentIntentTextBox->GetText().ToString().TrimStartAndEnd()
			: FString();
		if (HumanRequest.IsEmpty())
		{
			HumanRequest = TEXT("请基于当前确定性证据，整理选中像素的最终形成原因、关键 Pass、Pipeline 和 Shader 证据。");
		}
		const FString UserMessage = FString(TEXT("HUMAN_REQUEST\n")) + HumanRequest
			+ TEXT("\n\nPREFILTERED_EVIDENCE\n") + PrefilterEvidence;
		Diagnostics.WriteAgentLog(TEXT("HumanRequest"), HumanRequest);
		AddAgentMessage(TEXT("system"), LoadRenderTrailAgentSystemPrompt());
		if (!LastAgentQuestion.IsEmpty() && !LastAgentAnswer.IsEmpty())
		{
			AddAgentMessage(TEXT("user"), FString(TEXT("PREVIOUS_QUESTION\n")) + LastAgentQuestion);
			AddAgentMessage(TEXT("assistant"), FString(TEXT("PREVIOUS_ANSWER\n")) + LastAgentAnswer);
		}
		AddAgentMessage(TEXT("user"), UserMessage);
		SendAgentTurn();
		return FReply::Handled();
	}

	FReply SAnalyzerHome::RunPrimaryAnalysis()
	{
		if (!bSelectionConfirmed)
		{
			return ConfirmPixelSelection();
		}
		if (bAgentRunning)
		{
			return StartAgentAnalysis();
		}
		if (Samples.ContainsByPredicate([](const FPixelSample& Sample)
		{
			return Sample.bPending;
		}))
		{
			SetAgentStatus(TEXT("规则分析尚未完成，请等待 Pixel History 查询结束。"));
			return FReply::Handled();
		}
		return StartAgentAnalysis();
	}

	void SAnalyzerHome::SendAgentTurn()
	{
		if (!bAgentRunning)
			return;
		if (AgentStep >= MaxAgentSteps)
		{
			FinishAgentWithError(TEXT("Agent 达到语义整理轮次上限但没有给出 finish；已停止，避免无界查询。"));
			return;
		}
		++AgentStep;
		if (AgentStep == MaxAgentSteps)
		{
			AddAgentMessage(TEXT("user"), TEXT("FINAL TURN: tools are now disabled. Return action=finish using only accumulated evidence and list every unresolved fact in unknowns."));
		}
		SendAgentBrokerCompletion();
	}

	void SAnalyzerHome::SendAgentBrokerCompletion()
	{
		const URenderTrailOwnedModelSettings* Settings = GetDefault<URenderTrailOwnedModelSettings>();
		const FString EndpointOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("RENDERTRAIL_MODEL_ENDPOINT")).TrimStartAndEnd();
		const FString ModelOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("RENDERTRAIL_MODEL_NAME")).TrimStartAndEnd();
		const FString ApiKeyOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("RENDERTRAIL_MODEL_API_KEY")).TrimStartAndEnd();
		const FString AgentBrokerUrl = EndpointOverride.IsEmpty()
			? Settings->GetChatCompletionsUrl()
			: URenderTrailOwnedModelSettings::MakeChatCompletionsUrl(EndpointOverride);
		const FString Model = ModelOverride.IsEmpty() ? Settings->Model.TrimStartAndEnd() : ModelOverride;
		const FString ApiKey = ApiKeyOverride.IsEmpty() ? Settings->ApiKey.TrimStartAndEnd() : ApiKeyOverride;
		if (AgentBrokerUrl.IsEmpty() || Model.IsEmpty())
		{
			FinishAgentWithError(TEXT("RenderTrail Model Broker is not configured. Set Base URL and Model in Project Settings > Plugins > RenderTrail Model Broker."));
			return;
		}

		const int32 MaxTokens = FMath::Clamp(Settings->MaxOutputTokens, 128, 8192);
		const bool bDeepSeekV4 = Model.StartsWith(TEXT("deepseek-v4-"), ESearchCase::IgnoreCase);
		TSharedRef<FJsonObject> DirectLog = MakeShared<FJsonObject>();
		DirectLog->SetNumberField(TEXT("turn"), AgentStep);
		DirectLog->SetNumberField(TEXT("messageCount"), AgentMessages.Num());
		DirectLog->SetStringField(TEXT("url"), AgentBrokerUrl);
		DirectLog->SetStringField(TEXT("model"), Model);
		DirectLog->SetNumberField(TEXT("maxOutputTokens"), MaxTokens);
		DirectLog->SetStringField(TEXT("thinking"), bDeepSeekV4 ? (Settings->bEnableThinking ? TEXT("enabled") : TEXT("disabled")) : TEXT("not-applicable"));
		DirectLog->SetArrayField(TEXT("messages"), AgentMessages);
		Diagnostics.WriteAgentLog(TEXT("ModelTurnRequest"), SerializeJson(DirectLog));

		FRenderTrailAgentRequest Request;
		Request.Endpoint = AgentBrokerUrl;
		Request.Model = Model;
		Request.ApiKey = ApiKey;
		Request.Messages = AgentMessages;
		Request.MaxOutputTokens = MaxTokens;
		Request.bIncludeThinking = bDeepSeekV4;
		Request.bEnableThinking = Settings->bEnableThinking;
		const TWeakPtr<SAnalyzerHome> WeakHome = SharedThis(this);
		AgentClient->Submit(Request, [WeakHome](FRenderTrailAgentResponse&& Response)
		{
			if (const TSharedPtr<SAnalyzerHome> Pinned = WeakHome.Pin())
			{
				Pinned->HandleAgentBrokerResponse(MoveTemp(Response));
			}
		});
		SetAgentStatus(FString::Printf(TEXT("Agent turn %d/%d - RenderTrail Model Broker completing..."), AgentStep, MaxAgentSteps));
		return;

	}

	void SAnalyzerHome::HandleAgentBrokerResponse(FRenderTrailAgentResponse&& Response)
	{
		if (!bAgentRunning)
			return;
		Diagnostics.WriteAgentLog(TEXT("ModelTurnHttpResponse"), FString::Printf(TEXT("http=%d body=%s"),
			Response.HttpStatus, *Response.RawBody));
		if (!Response.IsSuccess())
		{
			FinishAgentWithError(Response.Error);
			return;
		}
		Diagnostics.WriteAgentLog(TEXT("ModelTurnParsedJson"), FString::Printf(
			TEXT("finishReason=%s contentChars=%d reasoningChars=%d content=%s"),
			*Response.FinishReason, Response.Content.Len(), Response.ReasoningContent.Len(), *Response.Content));
		Diagnostics.WriteRecord(TEXT("agent_completion_phase"), FString::Printf(
			TEXT("turn=%d finishReason=%s contentChars=%d reasoningChars=%d likelyTruncated=%s"),
			AgentStep, *Response.FinishReason, Response.Content.Len(), Response.ReasoningContent.Len(),
			Response.FinishReason.Equals(TEXT("length"), ESearchCase::IgnoreCase) ? TEXT("true") : TEXT("false")));
		HandleAgentAction(Response.Content, Response.ReasoningContent);
	}

	bool SAnalyzerHome::NormalizeAnswerOnlyAgentObject(const TSharedPtr<FJsonObject>& Action)
	{
		if (!Action.IsValid())
		{
			return false;
		}
		FString Answer;
		if (!Action->TryGetStringField(TEXT("answer"), Answer) || Answer.IsEmpty())
		{
			return false;
		}

		Action->SetStringField(TEXT("action"), TEXT("finish"));
		FString HumanRequest;
		if (!Action->TryGetStringField(TEXT("humanRequest"), HumanRequest) || HumanRequest.IsEmpty())
		{
			HumanRequest = AgentIntentTextBox.IsValid()
				? AgentIntentTextBox->GetText().ToString().TrimStartAndEnd()
				: TEXT("基于当前确定性证据整理选中像素的形成原因。");
			Action->SetStringField(TEXT("humanRequest"), HumanRequest);
		}

		if (!Action->HasField(TEXT("points")))
		{
			TArray<TSharedPtr<FJsonValue>> Points;
			for (const FPixelSample& Sample : Samples)
			{
				const TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
				Point->SetStringField(TEXT("sample"), FString::Printf(TEXT("P%d"), Points.Num() + 1));
				Point->SetStringField(TEXT("coordinate"), FString::Printf(TEXT("(%d,%d)"), Sample.Pixel.X, Sample.Pixel.Y));
				Point->SetStringField(TEXT("finalColor"), Sample.Modifications.IsEmpty()
					? TEXT("unknown") : Sample.Modifications.Last().After);
				Point->SetStringField(TEXT("formation"), Sample.Modifications.IsEmpty()
					? TEXT("Pixel History 没有可用于补全直接形成过程的修改记录。")
					: FString::Printf(TEXT("EID %u · %s；after=%s"), Sample.Modifications.Last().EventId,
						*Sample.Modifications.Last().Action, *Sample.Modifications.Last().After));
				Points.Add(MakeShared<FJsonValueObject>(Point));
			}
			Action->SetArrayField(TEXT("points"), MoveTemp(Points));
		}

		if (!Action->HasField(TEXT("influence")))
		{
			const TSharedRef<FJsonObject> Influence = MakeShared<FJsonObject>();
			if (LastCandidate.IsSet())
			{
				const FEventEvidence& Event = LastCandidate->Event;
				Influence->SetNumberField(TEXT("eventId"), Event.EventId);
				Influence->SetStringField(TEXT("type"), Event.ActionKind.IsEmpty() ? TEXT("unknown") : Event.ActionKind);
				Influence->SetStringField(TEXT("name"), Event.Action);
				Influence->SetStringField(TEXT("effect"), DescribeEventResult(Event));
				Influence->SetStringField(TEXT("evidence"), FString::Printf(TEXT("确定性 Pixel History 候选事件 EID %u；模型未返回独立 influence 字段。"), Event.EventId));
			}
			else
			{
				Influence->SetNumberField(TEXT("eventId"), 0);
				Influence->SetStringField(TEXT("type"), TEXT("unknown"));
				Influence->SetStringField(TEXT("name"), TEXT("unknown"));
				Influence->SetStringField(TEXT("effect"), TEXT("模型未返回结构化直接影响字段。"));
				Influence->SetStringField(TEXT("evidence"), TEXT("需要使用确定性 Pixel History 结果补全。"));
			}
			Action->SetObjectField(TEXT("influence"), Influence);
		}

		if (!Action->HasField(TEXT("shaders")))
		{
			TArray<TSharedPtr<FJsonValue>> Shaders;
			if (LastCandidate.IsSet())
			{
				const uint32 EventId = LastCandidate->Event.EventId;
				const FEventContextEvidence* Context = EventContexts.Find(EventId);
				const TSharedRef<FJsonObject> Shader = MakeShared<FJsonObject>();
				Shader->SetNumberField(TEXT("eventId"), EventId);
				Shader->SetStringField(TEXT("stage"), Context ? Context->ShaderStage : TEXT("unknown"));
				Shader->SetStringField(TEXT("name"), Context && !Context->ShaderEntry.IsEmpty() ? Context->ShaderEntry : TEXT("unknown"));
				Shader->SetStringField(TEXT("effect"), TEXT("仅补全模型遗漏的结构化字段；不对 Shader 算法作额外推断。"));
				Shader->SetStringField(TEXT("evidence"), Context ? FString::Printf(TEXT("pipeline.shaderEntry=%s"), *Context->ShaderEntry) : TEXT("没有对应的确定性事件上下文。"));
				Shaders.Add(MakeShared<FJsonValueObject>(Shader));
			}
			Action->SetArrayField(TEXT("shaders"), MoveTemp(Shaders));
		}

		if (!Action->HasField(TEXT("mesh")))
		{
			const TSharedRef<FJsonObject> Mesh = MakeShared<FJsonObject>();
			Mesh->SetStringField(TEXT("name"), TEXT("unknown"));
			Mesh->SetStringField(TEXT("evidence"), TEXT("模型未返回 Mesh 归属字段；当前证据不足以建立 UE 对象映射。"));
			Action->SetObjectField(TEXT("mesh"), Mesh);
		}
		if (!Action->HasField(TEXT("lanes")))
		{
			TArray<TSharedPtr<FJsonValue>> Lanes;
			for (const FCausalLaneEvidence& EvidenceLane : BuildCausalLaneEvidence(EventContexts, EventContextDepths))
			{
				const TSharedRef<FJsonObject> Lane = MakeShared<FJsonObject>();
				Lane->SetStringField(TEXT("kind"), EvidenceLane.TracePurpose);
				Lane->SetStringField(TEXT("status"), EvidenceLane.UnresolvedBoundaryCount > 0 ? TEXT("partial") : TEXT("confirmed"));
				Lane->SetStringField(TEXT("summary"), FString::Printf(
					TEXT("本地确定性索引：%d 个 producer、%d 个 reset、%d 个未完成边界。"),
					EvidenceLane.ConfirmedProducerCount, EvidenceLane.ResetBoundaryCount,
					EvidenceLane.UnresolvedBoundaryCount));
				TArray<TSharedPtr<FJsonValue>> EmptySteps;
				Lane->SetArrayField(TEXT("steps"), MoveTemp(EmptySteps));
				Lanes.Add(MakeShared<FJsonValueObject>(Lane));
			}
			Action->SetArrayField(TEXT("lanes"), MoveTemp(Lanes));
		}
		if (!Action->HasField(TEXT("process")))
		{
			TArray<TSharedPtr<FJsonValue>> Process;
			Process.Add(MakeShared<FJsonValueString>(TEXT("最终画面 → RenderDoc Pixel History 确定性证据")));
			if (LastCandidate.IsSet())
			{
				Process.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("EID %u · %s"), LastCandidate->Event.EventId, *LastCandidate->Event.Action)));
			}
			Process.Add(MakeShared<FJsonValueString>(TEXT("模型回答已保留；缺失的结构化字段由本地证据补全。")));
			Action->SetArrayField(TEXT("process"), MoveTemp(Process));
		}
		if (!Action->HasField(TEXT("finding")))
		{
			Action->SetStringField(TEXT("finding"), Answer.Left(500));
		}
		if (!Action->HasField(TEXT("confidence")))
		{
			Action->SetStringField(TEXT("confidence"), TEXT("low"));
		}
		if (!Action->HasField(TEXT("unknowns")))
		{
			TArray<TSharedPtr<FJsonValue>> Unknowns;
			Unknowns.Add(MakeShared<FJsonValueString>(TEXT("模型没有返回完整的 finish 结构；已由 RenderTrail 本地确定性证据补全。")));
			Action->SetArrayField(TEXT("unknowns"), MoveTemp(Unknowns));
		}
		return true;
	}

	void SAnalyzerHome::HandleAgentAction(const FString& Content, const FString& ReasoningContent)
	{
		Diagnostics.WriteAgentLog(TEXT("ModelAction"), Content);
		FString Json;
		TSharedPtr<FJsonObject> Action;
		bool bRepairedJson = false;
		FString ParseError;
		FString ActionName;
		if (!AgentProtocol::TryParseActionJson(Content, Json, Action, bRepairedJson, &ParseError))
		{
			Diagnostics.WriteAgentLog(TEXT("ModelActionParseFailed"), FString::Printf(
				TEXT("turn=%d contentChars=%d extractedChars=%d error=%s"),
				AgentStep, Content.Len(), Json.Len(), *ParseError));
			if (AgentStep >= MaxAgentSteps)
			{
				TSharedRef<FJsonObject> Fallback = MakeShared<FJsonObject>();
				Fallback->SetStringField(TEXT("answer"), LastReportSummary.IsEmpty()
					? TEXT("模型结构化输出无效；已保留 RenderTrail 本地确定性报告。")
					: LastReportSummary + TEXT(" 模型结构化输出无效，本结果由本地确定性证据回退生成。"));
				NormalizeAnswerOnlyAgentObject(Fallback);
				TArray<TSharedPtr<FJsonValue>> Unknowns;
				Unknowns.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("模型返回内容无法解析为约定 JSON：%s"), *ParseError.Left(300))));
				if (HasPendingBackgroundDeterministicQueries())
				{
					Unknowns.Add(MakeShared<FJsonValueString>(HasPendingWorkerRequests()
						? TEXT("自动深层资源溯源仍在执行；完成后可再次运行语义整理。")
						: TEXT("自动深追已达到安全上限，仍有预算截断的资源/sample 或 producer 上下文。")));
				}
				Fallback->SetArrayField(TEXT("unknowns"), MoveTemp(Unknowns));
				Fallback->SetStringField(TEXT("finding"), LastReportSummary.IsEmpty()
					? TEXT("确定性报告可用；模型结构化整理失败。") : LastReportSummary);
				Diagnostics.WriteAgentLog(TEXT("ModelActionLocalFallback"),
					TEXT("Displayed a deterministic local finish object instead of exposing malformed model JSON."));
				DisplayAgentFinal(Fallback);
				return;
			}
			AddAgentMessage(TEXT("assistant"), Content, ReasoningContent);
			AddAgentMessage(TEXT("user"), TEXT("FORMAT_ERROR: return exactly one JSON action object. Do not use markdown."));
			SendAgentTurn();
			return;
		}
		if (!Action->TryGetStringField(TEXT("action"), ActionName))
		{
			if (!NormalizeAnswerOnlyAgentObject(Action))
			{
				FinishAgentWithError(TEXT("模型返回了 JSON，但缺少 action 且无法从 answer-only 结果恢复：") + Content.Left(800));
				return;
			}
			Diagnostics.WriteAgentLog(TEXT("ModelActionSchemaRepair"), TEXT("Model returned answer/unknowns without action; normalized to finish using deterministic local evidence."));
			ActionName = TEXT("finish");
		}
		if (bRepairedJson)
		{
			Diagnostics.WriteAgentLog(TEXT("ModelActionJsonRepair"), TEXT("Accepted the action after repairing a mismatched JSON bracket."));
		}

		if (ActionName == TEXT("finish"))
		{
			DisplayAgentFinal(Action.ToSharedRef());
			return;
		}
		FinishAgentWithError(TEXT("语义模型只负责整理确定性溯源证据，不允许自行追加事件查询。"));
	}

	void SAnalyzerHome::ResumeAgentAfterEventContext(uint32 EventId)
	{
		if (!bAgentRunning || !AgentPendingEventId.IsSet() || AgentPendingEventId.GetValue() != EventId)
			return;
		AgentPendingEventId.Reset();
		AddAgentMessage(TEXT("user"), TEXT("TOOL_RESULT\n") + BuildAgentEventObservation(EventId));
		SendAgentTurn();
	}

	TArray<FRenderTrailResultLane> SAnalyzerHome::BuildDeterministicResultLanes() const
	{
		const TArray<FCausalLaneEvidence> EvidenceLanes =
			BuildCausalLaneEvidence(EventContexts, EventContextDepths);
		TArray<FRenderTrailResultLane> Result;
		auto RoleText = [](const FString& Role)
		{
			if (Role == TEXT("value-changing-producer")) return FString(TEXT("改变数值的 producer"));
			if (Role == TEXT("pass-through-producer")) return FString(TEXT("无变化传递 producer"));
			if (Role == TEXT("neutral-input")) return FString(TEXT("本像素中性/零值输入"));
			if (Role == TEXT("geometry-owner")) return FString(TEXT("几何/可见性 owner"));
			if (Role == TEXT("external-history-boundary")) return FString(TEXT("捕获外历史边界"));
			if (Role == TEXT("consumer-read-write-output")) return FString(TEXT("consumer 的读写输出，不作为上游输入"));
			if (Role == TEXT("reset-boundary")) return FString(TEXT("ownership reset"));
			if (Role == TEXT("budget-boundary")) return FString(TEXT("预算边界"));
			return Role.IsEmpty() ? FString(TEXT("未分类边")) : Role;
		};
	auto MakeBranchNode = [&RoleText](const FCausalLaneBranchEvidence& Branch)
		{
			FRenderTrailResultNode Node;
			if (Branch.ProducerEventId == 0 && Branch.ResetBoundaryEventId == 0)
			{
				Node.State = ERenderTrailResultNodeState::Blocked;
			}
			else if (Branch.EdgeConfidence.StartsWith(TEXT("confirmed")))
			{
				Node.State = ERenderTrailResultNodeState::Confirmed;
			}
			else if (Branch.EdgeRole == TEXT("neutral-input")
				|| Branch.EdgeRole == TEXT("consumer-read-write-output"))
			{
				Node.State = ERenderTrailResultNodeState::Information;
			}
			else
			{
				Node.State = ERenderTrailResultNodeState::Candidate;
			}
			const FString ResourceIdentity = Branch.ResourceIndex != INDEX_NONE
				? FString::Printf(TEXT("%s [R%d]"), *Branch.ResourceName, Branch.ResourceIndex)
				: Branch.ResourceName;
			if (Branch.ProducerEventId > 0)
			{
				Node.Title = FString::Printf(TEXT("EID %u ← %s ← EID %u"),
					Branch.ConsumerEventId, *ResourceIdentity, Branch.ProducerEventId);
			}
			else if (Branch.ResetBoundaryEventId > 0)
			{
				Node.Title = FString::Printf(TEXT("EID %u ← %s ← reset EID %u"),
					Branch.ConsumerEventId, *ResourceIdentity, Branch.ResetBoundaryEventId);
			}
			else
			{
				Node.Title = FString::Printf(TEXT("EID %u ← %s ← 边界"),
					Branch.ConsumerEventId, *ResourceIdentity);
			}
			Node.Subtitle = FString::Printf(TEXT("%s · confidence=%s · mapping=%s · status=%s"),
				*RoleText(Branch.EdgeRole),
				Branch.EdgeConfidence.IsEmpty() ? TEXT("unknown") : *Branch.EdgeConfidence,
				Branch.MappingConfidence.IsEmpty() ? TEXT("unknown") : *Branch.MappingConfidence,
				Branch.BranchStatus.IsEmpty() ? TEXT("unknown") : *Branch.BranchStatus);
			if (!Branch.ExecutedSampleValue.IsEmpty())
			{
				Node.Subtitle += TEXT("\n执行采样值：") + Branch.ExecutedSampleValue;
			}
			if (!Branch.ProducerWrittenValue.IsEmpty())
			{
				Node.Subtitle += TEXT("\nproducer 写入值（lastAfter/postMod）：") + Branch.ProducerWrittenValue;
			}
			return Node;
		};
	auto BranchLine = [&RoleText](const FCausalLaneBranchEvidence& Branch)
		{
			const FString Source = Branch.ProducerEventId > 0
				? FString::Printf(TEXT("EID %u"), Branch.ProducerEventId)
				: (Branch.ResetBoundaryEventId > 0
					? FString::Printf(TEXT("reset EID %u"), Branch.ResetBoundaryEventId)
					: TEXT("边界"));
			return FString::Printf(TEXT("EID %u ← %s [R%d] ← %s；%s；confidence=%s；mapping=%s；sample=%s；written=%s\n"),
				Branch.ConsumerEventId, *Branch.ResourceName, Branch.ResourceIndex, *Source,
				*RoleText(Branch.EdgeRole),
				Branch.EdgeConfidence.IsEmpty() ? TEXT("unknown") : *Branch.EdgeConfidence,
				Branch.MappingConfidence.IsEmpty() ? TEXT("unknown") : *Branch.MappingConfidence,
				Branch.ExecutedSampleValue.IsEmpty() ? TEXT("unknown") : *Branch.ExecutedSampleValue,
				Branch.ProducerWrittenValue.IsEmpty() ? TEXT("unknown") : *Branch.ProducerWrittenValue);
		};

		const uint32 RootEventId = LastCandidate.IsSet() ? LastCandidate->Event.EventId : 0;
		const FPrimaryCausalPathEvidence PrimaryPath = BuildPrimaryColorPathEvidence(
			EvidenceLanes, EventContexts, RootEventId);
		if (RootEventId > 0)
		{
			FRenderTrailResultLane PrimaryLane;
			PrimaryLane.Kind = TEXT("primary-color-path");
			PrimaryLane.Title = TEXT("最终颜色主形成路径");
			PrimaryLane.Summary = FString::Printf(
				TEXT("从最终物理写入 EID %u 沿已执行颜色读取反向连接；当前在 %s 收束。辅助 Bloom、曝光、LUT、覆盖与几何分支在下方独立展示。"),
				RootEventId, PrimaryPath.StopReason.IsEmpty() ? TEXT("unknown") : *PrimaryPath.StopReason);
			for (const FCausalLaneBranchEvidence& Branch : PrimaryPath.Branches)
			{
				PrimaryLane.Nodes.Add(MakeBranchNode(Branch));
			}
			if (PrimaryLane.Nodes.IsEmpty())
			{
				FRenderTrailResultNode Boundary;
				Boundary.State = ERenderTrailResultNodeState::Blocked;
				Boundary.Title = FString::Printf(TEXT("EID %u ← 未找到可连接的已执行颜色输入"), RootEventId);
				Boundary.Subtitle = PrimaryPath.StopReason;
				PrimaryLane.Nodes.Add(MoveTemp(Boundary));
			}
			Result.Add(MoveTemp(PrimaryLane));
		}

		for (const FCausalLaneEvidence& EvidenceLane : EvidenceLanes)
		{
			FRenderTrailResultLane Lane;
			Lane.Kind = EvidenceLane.TracePurpose;
			Lane.Title = EvidenceLane.TracePurpose == TEXT("geometry")
				? TEXT("几何 / 可见性归属线")
				: (EvidenceLane.TracePurpose == TEXT("overlay")
					? TEXT("编辑器覆盖层线") : TEXT("颜色形成线"));
			TArray<const FCausalLaneBranchEvidence*> OrderedBranches;
			for (const FCausalLaneBranchEvidence& Branch : EvidenceLane.Branches)
			{
				OrderedBranches.Add(&Branch);
			}
			OrderedBranches.Sort([&PrimaryPath](const FCausalLaneBranchEvidence& A,
				const FCausalLaneBranchEvidence& B)
			{
				auto Score = [&PrimaryPath](const FCausalLaneBranchEvidence& Branch)
				{
					int32 Value = 0;
					Value += PrimaryPath.Branches.ContainsByPredicate([&Branch](const FCausalLaneBranchEvidence& PathBranch)
					{
						return PathBranch.ConsumerEventId == Branch.ConsumerEventId
							&& PathBranch.ResourceIndex == Branch.ResourceIndex
							&& PathBranch.ProducerEventId == Branch.ProducerEventId;
					}) ? 1000 : 0;
					Value += Branch.EdgeConfidence.StartsWith(TEXT("confirmed")) ? 160
						: (Branch.EdgeConfidence.StartsWith(TEXT("strong")) ? 100 : 0);
					Value += Branch.ProducerEventId > 0 ? 80 : 0;
					Value -= Branch.EdgeRole == TEXT("neutral-input") ? 120 : 0;
					Value -= Branch.EdgeRole == TEXT("consumer-read-write-output") ? 220 : 0;
					Value -= Branch.ResourceName.Contains(TEXT("BlackDummy"), ESearchCase::IgnoreCase) ? 160 : 0;
					Value -= Branch.ReverseDepth * 3;
					return Value;
				};
				const int32 AScore = Score(A);
				const int32 BScore = Score(B);
				if (AScore != BScore) return AScore > BScore;
				if (A.ConsumerEventId != B.ConsumerEventId) return A.ConsumerEventId > B.ConsumerEventId;
				return A.ResourceIndex < B.ResourceIndex;
			});
			const int32 DisplayedBranches = FMath::Min(OrderedBranches.Num(), MaxDisplayedResultLaneBranches);
			Lane.Summary = FString::Printf(
				TEXT("%d 个去重分支：%d 个 producer、%d 个 reset、%d 个未完成边界；%d 条实际 Pixel History。优先显示 %d/%d，其余可在卡片内展开。%s"),
				EvidenceLane.Branches.Num(), EvidenceLane.ConfirmedProducerCount,
				EvidenceLane.ResetBoundaryCount, EvidenceLane.UnresolvedBoundaryCount,
				EvidenceLane.QueryRecordCount,
				DisplayedBranches, EvidenceLane.Branches.Num(),
				EvidenceLane.TracePurpose == TEXT("geometry")
					? TEXT("该线证明几何归属，不等同于最终 RGB 贡献。")
					: TEXT("producer 边证明资源结构依赖；最终像素数学贡献仍以映射与 Shader 证据为准。"));

			for (int32 BranchIndex = 0; BranchIndex < DisplayedBranches; ++BranchIndex)
			{
				Lane.Nodes.Add(MakeBranchNode(*OrderedBranches[BranchIndex]));
			}
			for (const FCausalLaneBranchEvidence* Branch : OrderedBranches)
			{
				Lane.FullEvidenceText += BranchLine(*Branch);
			}
			Lane.FullEvidenceTitle = FString::Printf(TEXT("查看全部 %d 个确定性分支"), OrderedBranches.Num());
			Result.Add(MoveTemp(Lane));
		}
		return Result;
	}

	void SAnalyzerHome::DisplayAgentFinal(const TSharedRef<FJsonObject>& Final)
	{
		FString PointsText;
		const TArray<TSharedPtr<FJsonValue>>* PointValues = nullptr;
		if (Final->TryGetArrayField(TEXT("points"), PointValues) && PointValues)
		{
			for (const TSharedPtr<FJsonValue>& PointValue : *PointValues)
			{
				const TSharedPtr<FJsonObject> Point = PointValue.IsValid() ? PointValue->AsObject() : nullptr;
				if (!Point.IsValid())
				{
					continue;
				}
				FString Sample = TEXT("P?");
				FString Coordinate = TEXT("unknown");
				FString FinalColor = TEXT("unknown");
				FString Formation = TEXT("证据不足");
				Point->TryGetStringField(TEXT("sample"), Sample);
				Point->TryGetStringField(TEXT("coordinate"), Coordinate);
				Point->TryGetStringField(TEXT("finalColor"), FinalColor);
				Point->TryGetStringField(TEXT("formation"), Formation);
				PointsText += FString::Printf(TEXT("%s · %s\n最终颜色：%s\n直接形成：%s\n\n"),
					*Sample, *Coordinate, *FinalColor, *Formation);
			}
		}

		FString TargetSample = TEXT("selected");
		FString TargetCoordinate = TEXT("unknown");
		FString PixelColor = TEXT("unknown");
		const TSharedPtr<FJsonObject>* TargetPixel = nullptr;
		if (Final->TryGetObjectField(TEXT("targetPixel"), TargetPixel) && TargetPixel && TargetPixel->IsValid())
		{
			(*TargetPixel)->TryGetStringField(TEXT("sample"), TargetSample);
			(*TargetPixel)->TryGetStringField(TEXT("coordinate"), TargetCoordinate);
			(*TargetPixel)->TryGetStringField(TEXT("finalColor"), PixelColor);
		}
		else
		{
			// Backward compatibility with results produced by the pre-0.3.1 prompt.
			Final->TryGetStringField(TEXT("pixelColor"), PixelColor);
		}
		if (!Samples.IsEmpty())
		{
			if (TargetSample == TEXT("selected"))
			{
				TargetSample = TEXT("P1");
			}
			if (TargetCoordinate == TEXT("unknown"))
			{
				TargetCoordinate = FString::Printf(TEXT("(%d,%d)"), Samples[0].Pixel.X, Samples[0].Pixel.Y);
			}
		}
		if ((PixelColor.IsEmpty() || PixelColor == TEXT("unknown")) && LastCandidate.IsSet())
		{
			PixelColor = LastCandidate->Event.After;
		}
		if (PointsText.IsEmpty())
		{
			PointsText = FString::Printf(TEXT("%s · %s\n最终颜色：%s\n"),
				*TargetSample, *TargetCoordinate, *PixelColor);
		}

		FString InfluenceType = TEXT("unknown");
		FString InfluenceName = TEXT("unknown");
		FString InfluenceEffect = TEXT("证据不足");
		FString InfluenceEvidence = TEXT("没有返回直接影响证据");
		uint32 InfluenceEventId = 0;
		const TSharedPtr<FJsonObject>* Influence = nullptr;
		if (Final->TryGetObjectField(TEXT("influence"), Influence) && Influence && Influence->IsValid())
		{
			double EventNumber = 0.0;
			if ((*Influence)->TryGetNumberField(TEXT("eventId"), EventNumber) && EventNumber > 0.0)
				InfluenceEventId = static_cast<uint32>(EventNumber);
			(*Influence)->TryGetStringField(TEXT("type"), InfluenceType);
			(*Influence)->TryGetStringField(TEXT("name"), InfluenceName);
			(*Influence)->TryGetStringField(TEXT("effect"), InfluenceEffect);
			(*Influence)->TryGetStringField(TEXT("evidence"), InfluenceEvidence);
		}

		FString ShaderText;
		FString PrimaryShaderName;
		const TArray<TSharedPtr<FJsonValue>>* Shaders = nullptr;
		if (Final->TryGetArrayField(TEXT("shaders"), Shaders) && Shaders)
		{
			int32 ShaderIndex = 0;
			for (const TSharedPtr<FJsonValue>& ShaderValue : *Shaders)
			{
				const TSharedPtr<FJsonObject> Shader = ShaderValue.IsValid() ? ShaderValue->AsObject() : nullptr;
				if (!Shader.IsValid())
					continue;
				double EventNumber = 0.0;
				FString Stage = TEXT("unknown");
				FString Name = TEXT("unknown");
				FString Effect = TEXT("证据未说明");
				FString Evidence = TEXT("未提供");
				Shader->TryGetNumberField(TEXT("eventId"), EventNumber);
				Shader->TryGetStringField(TEXT("stage"), Stage);
				Shader->TryGetStringField(TEXT("name"), Name);
				Shader->TryGetStringField(TEXT("effect"), Effect);
				Shader->TryGetStringField(TEXT("evidence"), Evidence);
				if (PrimaryShaderName.IsEmpty() && !Name.IsEmpty() && !Name.Equals(TEXT("unknown"), ESearchCase::IgnoreCase))
				{
					PrimaryShaderName = FString::Printf(TEXT("%s · %s"), *Stage, *Name);
				}
				ShaderText += FString::Printf(
					TEXT("%d. EID %u · [%s] %s\n   作用：%s\n   依据：%s\n"),
					++ShaderIndex,
					static_cast<uint32>(FMath::Max(0.0, EventNumber)), *Stage, *Name, *Effect, *Evidence);
			}
		}
		if (ShaderText.IsEmpty())
			ShaderText = TEXT("没有足够证据确认 Shader 名称\n");

		FString MeshName = TEXT("unknown");
		FString MeshEvidence = TEXT("没有可归属到 UE Mesh 的证据");
		const TSharedPtr<FJsonObject>* Mesh = nullptr;
		if (Final->TryGetObjectField(TEXT("mesh"), Mesh) && Mesh && Mesh->IsValid())
		{
			(*Mesh)->TryGetStringField(TEXT("name"), MeshName);
			(*Mesh)->TryGetStringField(TEXT("evidence"), MeshEvidence);
		}
		FString Finding = TEXT("没有生成结论");
		FString Confidence = TEXT("low");
		Final->TryGetStringField(TEXT("finding"), Finding);
		Final->TryGetStringField(TEXT("confidence"), Confidence);
		FString RequestedQuestion;
		Final->TryGetStringField(TEXT("humanRequest"), RequestedQuestion);
		if (RequestedQuestion.IsEmpty() && AgentIntentTextBox.IsValid())
		{
			RequestedQuestion = AgentIntentTextBox->GetText().ToString().TrimStartAndEnd();
		}
		if (RequestedQuestion.IsEmpty())
		{
			RequestedQuestion = TEXT("请基于当前确定性证据整理选中像素的形成原因。");
		}
		FString Answer;
		Final->TryGetStringField(TEXT("answer"), Answer);
		if (Answer.IsEmpty())
		{
			Answer = Finding;
		}

		FString ProcessText;
		const TArray<TSharedPtr<FJsonValue>>* Process = nullptr;
		if (Final->TryGetArrayField(TEXT("process"), Process) && Process)
		{
			for (int32 Index = 0; Index < Process->Num(); ++Index)
				ProcessText += FString::Printf(TEXT("%d. %s\n"), Index + 1, *(*Process)[Index]->AsString());
		}
		FString AgentLaneSummaryText;
		const TArray<TSharedPtr<FJsonValue>>* AgentLanes = nullptr;
		if (Final->TryGetArrayField(TEXT("lanes"), AgentLanes) && AgentLanes)
		{
			for (const TSharedPtr<FJsonValue>& LaneValue : *AgentLanes)
			{
				const TSharedPtr<FJsonObject> Lane = LaneValue.IsValid() ? LaneValue->AsObject() : nullptr;
				if (!Lane.IsValid()) continue;
				FString Kind = TEXT("unknown");
				FString Status = TEXT("unknown");
				FString LaneSummary;
				Lane->TryGetStringField(TEXT("kind"), Kind);
				Lane->TryGetStringField(TEXT("status"), Status);
				Lane->TryGetStringField(TEXT("summary"), LaneSummary);
				AgentLaneSummaryText += FString::Printf(TEXT("[%s · %s] %s\n"), *Kind, *Status, *LaneSummary);
			}
		}
		const TArray<FRenderTrailResultLane> DeterministicResultLanes = BuildDeterministicResultLanes();
		// The final-RT process is also deterministic. Model-provided process text may summarize
		// language, but it must not introduce cross-resource or cross-lane edges.
		ProcessText.Empty();
		if (LastCandidate.IsSet())
		{
			const FEventEvidence& FinalWriter = LastCandidate->Event;
			ProcessText = FString::Printf(TEXT("1. EID %u · %s：最终目标物理写入；before=%s；after=%s；change=%s\n"),
				FinalWriter.EventId, *CompactMarkerPath(FinalWriter.MarkerPath),
				FinalWriter.Before.IsEmpty() ? TEXT("unknown") : *FinalWriter.Before,
				FinalWriter.After.IsEmpty() ? TEXT("unknown") : *FinalWriter.After,
				*ClassifyColorDelta(FinalWriter));
		}
		FString DeterministicLaneText;
		for (const FRenderTrailResultLane& Lane : DeterministicResultLanes)
		{
			DeterministicLaneText += FString::Printf(TEXT("[%s] %s\n"), *Lane.Title, *Lane.Summary);
			for (const FRenderTrailResultNode& Node : Lane.Nodes)
			{
				DeterministicLaneText += TEXT("  • ") + Node.Title;
				if (!Node.Subtitle.IsEmpty()) DeterministicLaneText += TEXT("；") + Node.Subtitle.Replace(TEXT("\n"), TEXT("；"));
				DeterministicLaneText += TEXT("\n");
			}
		}
		FString UnknownText;
		const TArray<TSharedPtr<FJsonValue>>* Unknowns = nullptr;
		if (Final->TryGetArrayField(TEXT("unknowns"), Unknowns) && Unknowns)
		{
			for (const TSharedPtr<FJsonValue>& Unknown : *Unknowns)
				UnknownText += FString::Printf(TEXT("• %s\n"), *Unknown->AsString());
		}

		// Promote deterministic topology and values above model prose. The Agent may summarize
		// these facts, but it cannot redefine the final writer, primary path, mesh owner, or gaps.
		const TArray<FCausalLaneEvidence> DeterministicEvidenceLanes =
			BuildCausalLaneEvidence(EventContexts, EventContextDepths);
		const uint32 DeterministicRootEventId = LastCandidate.IsSet() ? LastCandidate->Event.EventId : 0;
		const FPrimaryCausalPathEvidence DeterministicPrimaryPath = BuildPrimaryColorPathEvidence(
			DeterministicEvidenceLanes, EventContexts, DeterministicRootEventId);
		FString PrimaryPathText;
		for (const FCausalLaneBranchEvidence& Branch : DeterministicPrimaryPath.Branches)
		{
			if (PrimaryPathText.IsEmpty())
			{
				PrimaryPathText = FString::Printf(TEXT("EID %u"), Branch.ConsumerEventId);
			}
			PrimaryPathText += FString::Printf(TEXT(" ← %s [R%d] ← "), *Branch.ResourceName, Branch.ResourceIndex);
			PrimaryPathText += Branch.ProducerEventId > 0
				? FString::Printf(TEXT("EID %u"), Branch.ProducerEventId)
				: TEXT("边界");
		}
		const FCausalLaneBranchEvidence* FirstTransform = DeterministicPrimaryPath.Branches.FindByPredicate(
			[](const FCausalLaneBranchEvidence& Branch)
			{
				return Branch.EdgeRole == TEXT("value-changing-producer") && Branch.ProducerEventId > 0;
			});
		if (LastCandidate.IsSet())
		{
			const FEventEvidence& FinalWriter = LastCandidate->Event;
			InfluenceEventId = FinalWriter.EventId;
			InfluenceType = FinalWriter.ActionKind;
			InfluenceName = CompactMarkerPath(FinalWriter.MarkerPath);
			if (InfluenceName.IsEmpty()) InfluenceName = FinalWriter.Action;
			const bool bNoFinalChange = ClassifyColorDelta(FinalWriter) == TEXT("no-change");
			InfluenceEffect = bNoFinalChange
				? TEXT("最终物理写入/传递，但该事件没有改变 P1 数值")
				: TEXT("最终物理写入并改变了 P1 数值");
			InfluenceEvidence = FString::Printf(TEXT("Pixel History before=%s；after=%s；change=%s"),
				FinalWriter.Before.IsEmpty() ? TEXT("unknown") : *FinalWriter.Before,
				FinalWriter.After.IsEmpty() ? TEXT("unknown") : *FinalWriter.After,
				*ClassifyColorDelta(FinalWriter));
			if (PixelColor.IsEmpty() || PixelColor == TEXT("unknown")) PixelColor = FinalWriter.After;
			Answer = FString::Printf(TEXT("最终物理写入是 EID %u，但它%s。颜色主路径已由执行采样值连接为 %s；当前真实断点是 %s。"),
				FinalWriter.EventId, bNoFinalChange ? TEXT("只做无变化传递") : TEXT("改变了目标值"),
				PrimaryPathText.IsEmpty() ? TEXT("未建立") : *PrimaryPathText,
				DeterministicPrimaryPath.StopReason.IsEmpty() ? TEXT("unknown") : *DeterministicPrimaryPath.StopReason);
			Finding = FirstTransform
				? FString::Printf(TEXT("最终颜色经 EID %u 物理写入，首个已确认的数值变化 producer 为 EID %u；主路径在 %s 收束。"),
					FinalWriter.EventId, FirstTransform->ProducerEventId, *DeterministicPrimaryPath.StopReason)
				: FString::Printf(TEXT("最终颜色物理写入为 EID %u；未建立已确认的数值变化主路径。"), FinalWriter.EventId);
		}
		PointsText = FString::Printf(TEXT("%s · %s\n最终颜色：%s\n确定性主路径：%s\n"),
			*TargetSample, *TargetCoordinate, *PixelColor,
			PrimaryPathText.IsEmpty() ? TEXT("未建立") : *PrimaryPathText);
		Confidence = DeterministicPrimaryPath.Branches.IsEmpty() ? TEXT("low")
			: (DeterministicPrimaryPath.bReachedExplicitBoundary ? TEXT("medium") : TEXT("high"));

		const FCausalLaneBranchEvidence* GeometryOwner = nullptr;
		for (const FCausalLaneEvidence& Lane : DeterministicEvidenceLanes)
		{
			if (Lane.TracePurpose != TEXT("geometry")) continue;
			for (const FCausalLaneBranchEvidence& Branch : Lane.Branches)
			{
				if (Branch.EdgeRole == TEXT("geometry-owner") && Branch.ProducerEventId > 0
					&& (!GeometryOwner || Branch.ReverseDepth < GeometryOwner->ReverseDepth))
				{
					GeometryOwner = &Branch;
				}
			}
		}
		if (GeometryOwner)
		{
			if (const FEventContextEvidence* GeometryContext = EventContexts.Find(GeometryOwner->ProducerEventId))
			{
				MeshName = CompactMarkerPath(GeometryContext->MarkerPath);
				if (MeshName.IsEmpty()) MeshName = GeometryContext->Action;
				MeshEvidence = FString::Printf(TEXT("geometry lane：EID %u ← %s [R%d] ← EID %u；mapping=%s；独立于最终 RGB"),
					GeometryOwner->ConsumerEventId, *GeometryOwner->ResourceName, GeometryOwner->ResourceIndex,
					GeometryOwner->ProducerEventId, *GeometryOwner->MappingConfidence);
			}
		}

		UnknownText.Empty();
		if (DeterministicPrimaryPath.bReachedExplicitBoundary && !DeterministicPrimaryPath.Branches.IsEmpty())
		{
			const FCausalLaneBranchEvidence& Boundary = DeterministicPrimaryPath.Branches.Last();
			UnknownText += FString::Printf(TEXT("• 主路径断点：EID %u ← %s [R%d]；role=%s；status=%s\n"),
				Boundary.ConsumerEventId, *Boundary.ResourceName, Boundary.ResourceIndex,
				*Boundary.EdgeRole, *Boundary.BranchStatus);
		}
		if (!BudgetDeferredEventContextDepths.IsEmpty())
		{
			UnknownText += FString::Printf(TEXT("• %d 个非主路径 producer 上下文达到安全预算；底层 full trace 保留具体 EID。\n"),
				BudgetDeferredEventContextDepths.Num());
		}

		const FString InfluenceHeading = InfluenceEventId > 0
			? FString::Printf(TEXT("EID %u · %s"), InfluenceEventId, *InfluenceName)
			: InfluenceName;
		FString Output;
		Output += TEXT("本次分析诉求\n");
		Output += RequestedQuestion;
		Output += TEXT("\n\n针对性回答\n");
		Output += Answer;
		Output += TEXT("\n\n");
		Output += TEXT("关注像素\n");
		Output += PointsText;
		Output += TEXT("\n结论\n");
		Output += Finding;
		Output += TEXT("\n\n直接影响\n");
		Output += InfluenceHeading;
		Output += FString::Printf(TEXT("\n类型：%s\n作用：%s\n依据：%s\n"),
			*InfluenceType, *InfluenceEffect, *InfluenceEvidence);
		Output += FString::Printf(TEXT("\n几何 / Mesh 归属（独立于最终 Composite）\n%s\n依据：%s\n"),
			*MeshName, *MeshEvidence);
		Output += TEXT("\nPipeline 状态\n");
		if (InfluenceEventId > 0)
		{
			if (const FEventContextEvidence* Context = EventContexts.Find(InfluenceEventId))
			{
				Output += EvidenceFormatting::FormatPipelineState(Context->PipelineState);
				Output += FString::Printf(TEXT("- Shader 绑定：%s entry %s；debuggable=%s；source symbols=%s\n"),
					*Context->ShaderStage,
					Context->ShaderEntry.IsEmpty() ? TEXT("unknown") : *Context->ShaderEntry,
					Context->bShaderDebuggable ? TEXT("yes") : TEXT("no"),
					Context->bSourceDebugInfo ? TEXT("yes") : TEXT("no"));
				Output += FString::Printf(TEXT("- Shader 反射：encoding=%s；inputSig=%d；outputSig=%d；constantBlocks=%d；samplers=%d；RO=%d；RW=%d\n"),
					Context->ShaderEncoding.IsEmpty() ? TEXT("unknown") : *Context->ShaderEncoding,
					Context->ShaderInputSignatureCount, Context->ShaderOutputSignatureCount,
					Context->ShaderConstantBlockCount, Context->ShaderSamplerCount,
					Context->ShaderReadOnlyResourceCount, Context->ShaderReadWriteResourceCount);
				if (Context->ShaderDebugTrace.IsValid())
				{
					Output += EvidenceFormatting::FormatShaderDebugTrace(Context->ShaderDebugTrace);
				}
				else
				{
					Output += TEXT("- Shader 算法：");
					Output += (Context->bShaderDebuggable && Context->bSourceDebugInfo)
						? TEXT("有源码/调试信息，但当前结果没有执行指令级追踪。\n")
						: TEXT("当前无法从入口名和资源绑定推断具体数学算法。\n");
				}
			}
			else
			{
				Output += TEXT("- 尚未加载该事件的 Pipeline 状态；当前结论只使用 Pixel History。\n");
			}
		}
		else
		{
			Output += TEXT("- 没有确定的影响事件，未展开 Pipeline 状态。\n");
		}
		Output += TEXT("\nShader\n");
		Output += ShaderText;
		if (!DeterministicLaneText.IsEmpty())
		{
			Output += TEXT("\n确定性主路径与并行因果线索（拓扑由 Replay 证据生成）\n");
			Output += DeterministicLaneText;
		}
		if (!AgentLaneSummaryText.IsEmpty())
		{
			Output += TEXT("\nAgent 语义摘要（不参与拓扑）\n");
			Output += AgentLaneSummaryText;
		}
		if (!ProcessText.IsEmpty())
		{
			Output += TEXT("\n最终 RT 直接过程\n");
			Output += ProcessText;
		}
		Output += FString::Printf(TEXT("\n置信度：%s"), *Confidence);
		if (!UnknownText.IsEmpty())
		{
			Output += TEXT("\n\n未知项\n");
			Output += UnknownText;
		}
		Output += TEXT("\n分析范围\n仅覆盖所选像素及其直接 GPU 因果链；未追踪 Blueprint、C++ 或游戏逻辑上游。");

		const FEventContextEvidence* InfluenceContext = InfluenceEventId > 0
			? EventContexts.Find(InfluenceEventId)
			: nullptr;
		FString PipelineText;
		FString ShaderEvidenceText = ShaderText;
		if (InfluenceContext)
		{
			PipelineText += EvidenceFormatting::FormatPipelineState(InfluenceContext->PipelineState);
			PipelineText += FString::Printf(
				TEXT("Shader 绑定：%s entry %s\n可调试：%s · 源码符号：%s\nShader 反射：encoding=%s · inputSig=%d · outputSig=%d · constantBlocks=%d · samplers=%d · RO=%d · RW=%d\n"),
				*InfluenceContext->ShaderStage,
				InfluenceContext->ShaderEntry.IsEmpty() ? TEXT("unknown") : *InfluenceContext->ShaderEntry,
				InfluenceContext->bShaderDebuggable ? TEXT("yes") : TEXT("no"),
				InfluenceContext->bSourceDebugInfo ? TEXT("yes") : TEXT("no"),
				InfluenceContext->ShaderEncoding.IsEmpty() ? TEXT("unknown") : *InfluenceContext->ShaderEncoding,
				InfluenceContext->ShaderInputSignatureCount, InfluenceContext->ShaderOutputSignatureCount,
				InfluenceContext->ShaderConstantBlockCount, InfluenceContext->ShaderSamplerCount,
				InfluenceContext->ShaderReadOnlyResourceCount, InfluenceContext->ShaderReadWriteResourceCount);
			if (InfluenceContext->ShaderDebugTrace.IsValid())
			{
				ShaderEvidenceText += TEXT("\n\n执行跟踪：\n");
				ShaderEvidenceText += EvidenceFormatting::FormatShaderDebugTrace(InfluenceContext->ShaderDebugTrace);
			}
		}
		else
		{
			PipelineText = TEXT("尚未加载该事件的 Pipeline 状态；当前结论只使用 Pixel History。\n");
		}
		if (PipelineText.IsEmpty())
		{
			PipelineText = TEXT("没有确定的影响事件，未展开 Pipeline 状态。\n");
		}
		if (ShaderEvidenceText.IsEmpty())
		{
			ShaderEvidenceText = TEXT("没有足够证据确认 Shader。\n");
		}

		FRenderTrailAgentResultViewModel ViewModel;
		ViewModel.Question = RequestedQuestion;
		ViewModel.Finding = Finding;
		ViewModel.Answer = Answer;
		ViewModel.PixelLabel = FString::Printf(TEXT("%s · %s"), *TargetSample, *TargetCoordinate);
		ViewModel.FinalColor = PixelColor;
		ViewModel.Confidence = Confidence;
		ViewModel.Lanes = DeterministicResultLanes;
		ViewModel.ProcessText = ProcessText;
		ViewModel.PipelineText = PipelineText;
		ViewModel.ShaderText = ShaderEvidenceText;
		ViewModel.UnknownText = UnknownText;
		ViewModel.RawReport = Output;

		FRenderTrailResultFact PassFact;
		PassFact.Label = TEXT("最终物理写入");
		PassFact.Value = InfluenceHeading;
		ViewModel.Facts.Add(MoveTemp(PassFact));
		FRenderTrailResultFact LaneFact;
		LaneFact.Label = TEXT("并行追踪线索");
		LaneFact.Value = FString::Printf(TEXT("1 条最终颜色主路径 + %d 条并行证据线"),
			FMath::Max(0, ViewModel.Lanes.Num() - 1));
		ViewModel.Facts.Add(MoveTemp(LaneFact));
		FRenderTrailResultFact MeshFact;
		MeshFact.Label = TEXT("几何 / Mesh 归属");
		MeshFact.Value = MeshName;
		ViewModel.Facts.Add(MoveTemp(MeshFact));
		FRenderTrailResultFact CoverageFact;
		CoverageFact.Label = TEXT("确定性覆盖");
		CoverageFact.Value = FString::Printf(TEXT("%d contexts · %d Pixel History · %d producer 边界"),
			EventContexts.Num(), ResourcePixelHistoryQueriesSubmitted + Samples.Num(),
			BudgetDeferredEventContextDepths.Num());
		ViewModel.Facts.Add(MoveTemp(CoverageFact));
		FRenderTrailResultFact PipelineFact;
		PipelineFact.Label = TEXT("Pipeline");
		PipelineFact.Value = InfluenceContext
			? EvidenceFormatting::FormatPipelineCompactSummary(InfluenceContext->PipelineState)
			: TEXT("未采集");
		ViewModel.Facts.Add(MoveTemp(PipelineFact));
		FRenderTrailResultFact ShaderFact;
		ShaderFact.Label = TEXT("Shader");
		ShaderFact.Value = PrimaryShaderName.IsEmpty()
			? (InfluenceContext && !InfluenceContext->ShaderEntry.IsEmpty() ? InfluenceContext->ShaderEntry : TEXT("unknown"))
			: PrimaryShaderName;
		ViewModel.Facts.Add(MoveTemp(ShaderFact));

		if (LastCandidate.IsSet())
		{
			const FEventEvidence& FinalWriter = LastCandidate->Event;
			FRenderTrailResultNode FinalNode;
			FinalNode.State = ERenderTrailResultNodeState::Confirmed;
			FinalNode.Title = FString::Printf(TEXT("最终写入 · EID %u · %s"), FinalWriter.EventId, *FinalWriter.Action);
			FinalNode.Subtitle = FString::Printf(TEXT("%s · Δmax=%.6g · %s"),
				*ClassifySemantics(FinalWriter), FinalWriter.ColorDeltaMax, *ClassifyColorDelta(FinalWriter));
			ViewModel.Chain.Add(MoveTemp(FinalNode));
			if (FinalWriter.BeforeValue.bValid && FinalWriter.AfterValue.bValid)
			{
				ViewModel.bHasColorTransition = true;
				ViewModel.BeforeColor = FLinearColor(
					static_cast<float>(FMath::Clamp(FinalWriter.BeforeValue.R, 0.0, 1.0)),
					static_cast<float>(FMath::Clamp(FinalWriter.BeforeValue.G, 0.0, 1.0)),
					static_cast<float>(FMath::Clamp(FinalWriter.BeforeValue.B, 0.0, 1.0)), 1.0f);
				ViewModel.AfterColor = FLinearColor(
					static_cast<float>(FMath::Clamp(FinalWriter.AfterValue.R, 0.0, 1.0)),
					static_cast<float>(FMath::Clamp(FinalWriter.AfterValue.G, 0.0, 1.0)),
					static_cast<float>(FMath::Clamp(FinalWriter.AfterValue.B, 0.0, 1.0)), 1.0f);
				ViewModel.BeforeColorText = FString::Printf(TEXT("Before\n%.3f  %.3f  %.3f"),
					FinalWriter.BeforeValue.R, FinalWriter.BeforeValue.G, FinalWriter.BeforeValue.B);
				ViewModel.AfterColorText = FString::Printf(TEXT("After\n%.3f  %.3f  %.3f"),
					FinalWriter.AfterValue.R, FinalWriter.AfterValue.G, FinalWriter.AfterValue.B);
				ViewModel.ColorDeltaText = FString::Printf(TEXT("%s · Δmax %.6g · ΔL1 %.6g"),
					*ClassifyColorDelta(FinalWriter), FinalWriter.ColorDeltaMax, FinalWriter.ColorDeltaL1);
			}
		}
		else if (InfluenceEventId > 0)
		{
			FRenderTrailResultNode FinalNode;
			FinalNode.State = ERenderTrailResultNodeState::Confirmed;
			FinalNode.Title = FString::Printf(TEXT("直接影响 · EID %u · %s"), InfluenceEventId, *InfluenceName);
			FinalNode.Subtitle = InfluenceEvidence;
			ViewModel.Chain.Add(MoveTemp(FinalNode));
		}

		if (LastSignificantCandidate.IsSet())
		{
			const FEventEvidence& Significant = LastSignificantCandidate->Event;
			FRenderTrailResultNode CandidateNode;
			CandidateNode.State = ERenderTrailResultNodeState::Candidate;
			CandidateNode.Title = FString::Printf(TEXT("显著形成候选 · EID %u · %s"), Significant.EventId, *Significant.Action);
			CandidateNode.Subtitle = FString::Printf(TEXT("Δmax=%.6g；仍需证明跨资源的像素贡献"), Significant.ColorDeltaMax);
			ViewModel.Chain.Add(MoveTemp(CandidateNode));
			if (!UnknownText.IsEmpty())
			{
				FRenderTrailResultNode BoundaryNode;
				BoundaryNode.State = ERenderTrailResultNodeState::Blocked;
				BoundaryNode.Title = TEXT("跨资源像素链在此停止");
				BoundaryNode.Subtitle = TEXT("producer、坐标映射或 UE 归属至少有一项尚未证明；具体缺口见下方。");
				ViewModel.Chain.Add(MoveTemp(BoundaryNode));
			}
		}
		else
		{
			FRenderTrailResultNode BoundaryNode;
			BoundaryNode.State = ERenderTrailResultNodeState::Blocked;
			BoundaryNode.Title = TEXT("显著上游写入尚未确认");
			BoundaryNode.Subtitle = UnknownText.IsEmpty()
				? TEXT("资源 producer 关系不能单独证明 P1 的采样坐标和值。")
				: TEXT("具体缺口见下方“证据缺口”。");
			ViewModel.Chain.Add(MoveTemp(BoundaryNode));
		}

		if (AgentResultView.IsValid())
		{
			AgentResultView->SetAgentResult(ViewModel);
		}
		bAgentResultDisplayed = true;
		LastAgentQuestion = RequestedQuestion;
		LastAgentAnswer = Answer;
		Diagnostics.WriteAgentLog(TEXT("RunComplete"), Output);
		if (AgentOutputText.IsValid())
		{
			AgentOutputText->SetText(FText::FromString(TEXT("回答已完成，并按“结论 → 最终 RT 写入 → 颜色/几何/覆盖层并行线索 → 缺口”分层显示。可修改问题后再次发送。")));
		}
		bAgentRunning = false;
		bAgentWaitingForDeterministicContexts = false;
		AgentPendingEventId.Reset();
		if (AgentRunButtonText.IsValid())
			AgentRunButtonText->SetText(FText::FromString(TEXT("重新发送")));
		SetAgentStatus(FString::Printf(TEXT("完成 · %d 个模型轮次 · %d 条并行线索 · 只使用所选像素的有界证据%s"), AgentStep,
			ViewModel.Lanes.Num(),
			HasPendingBackgroundDeterministicQueries() ? TEXT(" · 自动深追达到安全上限") : TEXT(" · 自动深追已收束")));
	}

	void SAnalyzerHome::FinishAgentWithError(const FString& Error)
	{
		Diagnostics.WriteAgentLog(TEXT("RunError"), Error);
		bAgentRunning = false;
		bAgentWaitingForDeterministicContexts = false;
		if (AgentClient.IsValid())
		{
			AgentClient->Cancel();
		}
		AgentPendingEventId.Reset();
		if (AgentRunButtonText.IsValid())
			AgentRunButtonText->SetText(FText::FromString(TEXT("重新发送")));
		SetAgentStatus(Error);
	}

	void SAnalyzerHome::CancelAgentRun()
	{
		bAgentRunning = false;
		bAgentWaitingForDeterministicContexts = false;
		if (AgentClient.IsValid())
		{
			AgentClient->Cancel();
		}
		AgentPendingEventId.Reset();
		AgentMessages.Empty();
		if (AgentRunButtonText.IsValid())
			AgentRunButtonText->SetText(FText::FromString(TEXT("发送")));
	}
}
