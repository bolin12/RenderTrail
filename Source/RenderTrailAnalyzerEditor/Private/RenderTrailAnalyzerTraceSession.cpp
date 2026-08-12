#include "RenderTrailAnalyzerHomeInternal.h"

namespace UE::RenderTrail::Private
{
	bool SAnalyzerHome::HasPendingWorkerRequests() const
	{
		return !PendingSampleByRequest.IsEmpty()
			|| !PendingEventContextByRequest.IsEmpty()
			|| !PendingShaderDebugByRequest.IsEmpty()
			|| !PendingResourcePixelHistoryByRequest.IsEmpty()
			|| !DispatchedWorkerRequestId.IsEmpty()
			|| !QueuedWorkerRequests.IsEmpty();
	}

	int32 SAnalyzerHome::GetPendingRequiredResourceHistoryCount() const
	{
		int32 Count = 0;
		for (const TPair<FString, FResourcePixelHistoryRequest>& Pair : PendingResourcePixelHistoryByRequest)
		{
			Count += Pair.Value.bRequiredForAgent ? 1 : 0;
		}
		return Count;
	}

	int32 SAnalyzerHome::GetPendingBackgroundResourceHistoryCount() const
	{
		int32 Count = PendingResourcePixelHistoryByRequest.Num() - GetPendingRequiredResourceHistoryCount();
		for (const TPair<uint32, int32>& Pair : DeferredResourceHistoryBranchCounts)
		{
			Count += Pair.Value;
		}
		return Count;
	}

	int32 SAnalyzerHome::GetDeferredBackgroundResourceHistoryCount() const
	{
		int32 Count = 0;
		for (const TPair<uint32, int32>& Pair : DeferredResourceHistoryBranchCounts)
		{
			Count += Pair.Value;
		}
		return Count;
	}

	int32 SAnalyzerHome::GetBudgetDeferredResourceHistoryCount() const
	{
		TSet<FString> Groups;
		for (const TPair<FString, FResourcePixelHistoryRequest>& Pair : BudgetDeferredResourcePixelHistoryRequests)
		{
			const FResourcePixelHistoryRequest& Request = Pair.Value;
			Groups.Add(FString::Printf(TEXT("%u:%d:%d:%d:%d:%d:%s"), Request.ConsumerEventId,
				Request.ResourceIndex, Request.Mip, Request.Slice, Request.Sample, Request.TypeCast,
				*Request.TracePurpose));
		}
		return Groups.Num();
	}

	int32 SAnalyzerHome::GetDiscoveredResourceHistoryCount() const
	{
		return ScheduledResourcePixelHistoryKeys.Num() + GetBudgetDeferredResourceHistoryCount()
			+ GetDeferredBackgroundResourceHistoryCount();
	}

	FString SAnalyzerHome::BuildHistoryCoverageText() const
	{
		if (!bSelectionConfirmed || (GetDiscoveredResourceHistoryCount() == 0 && EventContexts.IsEmpty()))
		{
			return TEXT("历史完整度：尚未开始资源/sample 深追");
		}
		const int32 Pending = PendingResourcePixelHistoryByRequest.Num();
		const int32 Completed = FMath::Max(0, ResourcePixelHistoryQueriesSubmitted - Pending);
		return FString::Printf(
			TEXT("历史完整度：资源分支 %d/%d 已返回（失败 %d），%d 查询中，%d 自动深追待排队，%d 达到安全上限；上下文 %d/%d 已返回，%d 查询中，%d 自动深追待排队，%d 达到安全上限%s"),
			Completed, GetDiscoveredResourceHistoryCount(), FailedResourcePixelHistoryKeys.Num(), Pending,
			GetDeferredBackgroundResourceHistoryCount(), GetBudgetDeferredResourceHistoryCount(),
			EventContexts.Num(), DeterministicContextLimit, PendingEventContextIds.Num(), DeferredEventContextIds.Num(),
			BudgetDeferredEventContextDepths.Num(), bWorkerReady ? TEXT("") : TEXT(" · Replay 已释放"));
	}

	void SAnalyzerHome::FinishAutomaticDeepTraceIfIdle()
	{
		if (bDeterministicForegroundCompletionReported || bCaptureLoading || !bWorkerReady
			|| !bSelectionConfirmed || bReplaySynchronizationPending || HasPendingWorkerRequests())
		{
			return;
		}

		int32 ReadySamples = 0;
		int32 FailedSamples = 0;
		for (const FPixelSample& Sample : Samples)
		{
			ReadySamples += Sample.bAnalyzed && !Sample.bPending && !Sample.bFailed ? 1 : 0;
			FailedSamples += !Sample.bPending && Sample.bFailed ? 1 : 0;
		}
		const int32 BoundedResourceBranches = GetDeferredBackgroundResourceHistoryCount()
			+ GetBudgetDeferredResourceHistoryCount();
		const int32 BoundedResourceCandidates = GetDeferredBackgroundResourceHistoryCount()
			+ BudgetDeferredResourcePixelHistoryRequests.Num();
		const int32 BoundedProducerContexts = DeferredEventContextIds.Num()
			+ BudgetDeferredEventContextDepths.Num();
		const TArray<FCausalLaneEvidence> CausalLanes =
			BuildCausalLaneEvidence(EventContexts, EventContextDepths);
		int32 GroupedLaneBranches = 0;
		int32 LaneQueryRecords = 0;
		for (const FCausalLaneEvidence& Lane : CausalLanes)
		{
			GroupedLaneBranches += Lane.Branches.Num();
			LaneQueryRecords += Lane.QueryRecordCount;
		}

		bDeterministicForegroundCompletionReported = true;
		Diagnostics.WriteRecord(TEXT("automatic_deep_trace_complete"), FString::Printf(
			TEXT("readySamples=%d failedSamples=%d requiredResources=%d pendingWorkers=%d causalLanes=%d groupedLaneBranches=%d laneQueryRecords=%d boundedResourceBranches=%d boundedProducerContexts=%d agentRunning=%s"),
			ReadySamples, FailedSamples, GetPendingRequiredResourceHistoryCount(),
			WorkerRequestQueuedSeconds.Num(), CausalLanes.Num(), GroupedLaneBranches, LaneQueryRecords,
			BoundedResourceBranches, BoundedProducerContexts,
			bAgentRunning ? TEXT("true") : TEXT("false")));
		WriteFullTraceSnapshot((BoundedResourceBranches > 0 || BoundedProducerContexts > 0)
			? TEXT("completed-with-safety-boundary") : TEXT("completed"));

		if (ReadySamples > 0)
		{
			const FString BoundarySuffix = (BoundedResourceBranches > 0 || BoundedProducerContexts > 0)
				? FString::Printf(TEXT("；%d 个资源组（%d 个候选坐标）、%d 个 producer 上下文达到自动深追安全上限"),
					BoundedResourceBranches, BoundedResourceCandidates, BoundedProducerContexts)
				: FString();
			SetStatus(FString::Printf(TEXT("选点分析完成 · %d 条并行线索 · %d 个去重分支（%d 条跨资源 Pixel History）· 自动深追已收束"),
				CausalLanes.Num(), GroupedLaneBranches, LaneQueryRecords) + BoundarySuffix);
			if (!bAgentRunning && !bAgentResultDisplayed)
			{
				SetAgentStatus((BoundedResourceBranches > 0 || BoundedProducerContexts > 0)
					? TEXT("自动深追已收束，但达到安全上限；Agent 会明确保留该边界。")
					: TEXT("自动深追已完整收束 · 可以向 Agent 发送问题。"));
			}
		}
		else
		{
			SetStatus(FString::Printf(TEXT("选点分析结束，但没有成功的 Pixel History（失败 %d）；请查看技术证据。"),
				FailedSamples));
		}
	}

	bool SAnalyzerHome::IsCriticalAgentEvent(uint32 EventId) const
	{
		return FocusedTraceEventIds.Contains(EventId)
			|| (LastCandidate.IsSet() && LastCandidate->Event.EventId == EventId)
			|| (LastSignificantCandidate.IsSet() && LastSignificantCandidate->Event.EventId == EventId);
	}

	int32 SAnalyzerHome::ComputeResourceTracePriority(const FResourcePixelHistoryRequest& Request)
	{
		int32 Priority = Request.bExecutedShaderAccess ? 700 : 300;
		Priority += Request.bRequiredForAgent ? 100 : 0;
		if (Request.TracePurpose == TEXT("color"))
		{
			Priority += 80;
		}
		else if (Request.TracePurpose == TEXT("geometry"))
		{
			Priority += 70;
		}
		else if (Request.TracePurpose == TEXT("overlay"))
		{
			Priority += 60;
		}
		const FString Searchable = Request.ResourceName + TEXT(" ") + Request.ShaderBinding;
		if (Searchable.Contains(TEXT("SceneColor"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("ColorTexture"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("Tonemap"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("TSR.Output"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("SelectionOutline"), ESearchCase::IgnoreCase))
		{
			Priority += 80;
		}
		if (Searchable.Contains(TEXT("SceneDepth"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase))
		{
			Priority += 70;
		}
		if (Searchable.Contains(TEXT("Dummy"), ESearchCase::IgnoreCase)
			|| Searchable.Contains(TEXT("LUT"), ESearchCase::IgnoreCase))
		{
			Priority -= 120;
		}
		return Priority - Request.ReverseDepth * 5;
	}

	bool SAnalyzerHome::IsShaderDebugPending(uint32 EventId) const
	{
		for (const TPair<FString, uint32>& Pair : PendingShaderDebugByRequest)
		{
			if (Pair.Value == EventId)
			{
				return true;
			}
		}
		return false;
	}

	bool SAnalyzerHome::QueueFocusedShaderDebug(uint32 EventId)
	{
		if (!bShaderDebuggingAvailable || !bWorkerReady || FailedShaderDebugIds.Contains(EventId))
		{
			return false;
		}
		FEventContextEvidence* Context = EventContexts.Find(EventId);
		if (!Context || Context->ActionKind != TEXT("draw") || !Context->bShaderDebuggable
			|| !EventTracePixels.Contains(EventId))
		{
			return false;
		}
		if (Context->ShaderDebugTrace.IsValid() || IsShaderDebugPending(EventId))
		{
			return true;
		}

		const FIntPoint Pixel = EventTracePixels.FindChecked(EventId);
		const uint32 PrimitiveId = EventTracePrimitiveIds.FindRef(EventId);
		const bool bHasPrimitive = EventTracePrimitiveEvidenceIds.Contains(EventId);
		const FString RequestId = FString::Printf(TEXT("shader-debug-%u-query-%llu"), EventId, ++RequestSerial);
		PendingShaderDebugByRequest.Add(RequestId, EventId);
		Diagnostics.WriteRecord(TEXT("shader_debug_queued"), FString::Printf(
			TEXT("request=%s event=%u pixel=(%d,%d) primitive=%u hasPrimitive=%s focused=true"),
			*RequestId, EventId, Pixel.X, Pixel.Y, PrimitiveId, bHasPrimitive ? TEXT("true") : TEXT("false")));
		if (!SendWorkerRequest(TEXT("shader_debug"), RequestId,
			[EventId, Pixel, PrimitiveId, bHasPrimitive](const TSharedRef<FJsonObject>& Request)
			{
				Request->SetNumberField(TEXT("eventId"), EventId);
				Request->SetNumberField(TEXT("x"), Pixel.X);
				Request->SetNumberField(TEXT("y"), Pixel.Y);
				Request->SetNumberField(TEXT("sample"), 0);
				Request->SetNumberField(TEXT("primitiveId"), PrimitiveId);
				Request->SetBoolField(TEXT("hasPrimitive"), bHasPrimitive);
			}, 900 - EventContextDepths.FindRef(EventId) * 5))
		{
			PendingShaderDebugByRequest.Remove(RequestId);
			FailedShaderDebugIds.Add(EventId);
			return false;
		}
		return true;
	}

	FAgentContextCoverageSelection SAnalyzerHome::BuildAgentContextCoverageSelection() const
	{
		TSet<uint32> CriticalEventIds;
		if (LastCandidate.IsSet())
		{
			CriticalEventIds.Add(LastCandidate->Event.EventId);
		}
		if (LastSignificantCandidate.IsSet())
		{
			CriticalEventIds.Add(LastSignificantCandidate->Event.EventId);
		}
		for (const uint32 EventId : FocusedTraceEventIds)
		{
			CriticalEventIds.Add(EventId);
		}
		return SelectAgentContextsForCausalCoverage(
			EventContexts, EventContextDepths, CriticalEventIds, MaxAgentDeterministicContexts);
	}

	TArray<FString> SAnalyzerHome::GetAgentContextCoverageRoles(const FAgentContextCoverageEvidence& Coverage)
	{
		TArray<FString> Roles;
		if (Coverage.bCritical) Roles.Add(TEXT("critical"));
		if (Coverage.bReferencedPixelWriter) Roles.Add(TEXT("confirmed-pixel-writer"));
		if (Coverage.bAssetMarker) Roles.Add(TEXT("asset-marker"));
		if (Coverage.bSceneRaster) Roles.Add(TEXT("basepass-prepass-gbuffer"));
		if (Coverage.bNanite) Roles.Add(TEXT("nanite"));
		if (Coverage.bDepthStage) Roles.Add(TEXT("depth"));
		if (Coverage.bBranchBoundary || Coverage.bUnresolvedProducer) Roles.Add(TEXT("chain-boundary"));
		if (Roles.IsEmpty()) Roles.Add(TEXT("other"));
		return Roles;
	}

	int32 SAnalyzerHome::GetPendingCriticalContextCount() const
	{
		int32 Count = 0;
		for (const uint32 EventId : PendingEventContextIds)
		{
			Count += IsCriticalAgentEvent(EventId) ? 1 : 0;
		}
		return Count;
	}

	bool SAnalyzerHome::HasPendingCriticalDeterministicQueries() const
	{
		if (!PendingShaderDebugByRequest.IsEmpty() || GetPendingRequiredResourceHistoryCount() > 0)
		{
			return true;
		}
		return GetPendingCriticalContextCount() > 0;
	}

	bool SAnalyzerHome::HasPendingBackgroundDeterministicQueries() const
	{
		if (GetPendingBackgroundResourceHistoryCount() > 0 || !DeferredEventContextIds.IsEmpty()
			|| !BudgetDeferredResourcePixelHistoryRequests.IsEmpty()
			|| !BudgetDeferredEventContextDepths.IsEmpty())
		{
			return true;
		}
		for (const uint32 EventId : PendingEventContextIds)
		{
			if (!IsCriticalAgentEvent(EventId))
			{
				return true;
			}
		}
		return false;
	}

	void SAnalyzerHome::QueryPixel(int32 X, int32 Y)
	{
		if (!bPreviewReadyForSelection)
		{
			SetStatus(TEXT("当前图像不是可查询的 RenderDoc 最终 RT；请等待 Worker 就绪后再选点。"));
			return;
		}
		CancelAgentRun();
		bSelectionConfirmed = false;
		SetAgentOutputText(TEXT("选点已改变；点击“分析当前像素”后更新证据。"));
		SetAgentStatus(TEXT("等待确认选点；尚未启动 Pixel History。"));

		const FIntPoint Pixel(X, Y);
		if (!Samples.IsEmpty() && Samples[0].Pixel == Pixel)
		{
			ResetSamples();
			RenderCausalReport();
			SetStatus(FString::Printf(TEXT("已清除关注像素 (%d, %d)。"), X, Y));
			return;
		}

		const bool bReplacingSelection = Samples.Num() >= MaxPixelSamples;
		if (bReplacingSelection)
		{
			// A new pixel owns a completely separate evidence session. Clearing the old
			// request maps also makes late Worker responses unclaimable by the new point.
			ResetSamples();
		}

		FPixelSample Sample;
		Sample.Id = ++SampleSerial;
		Sample.Pixel = Pixel;
		Samples.Add(MoveTemp(Sample));
		SetAgentOutputText(TEXT("当前像素已改变；点击“分析”后更新证据。"));
		SetAgentStatus(TEXT("等待确认当前像素；尚未启动 Pixel History。"));
		UpdateMarkers();
		UpdateSelectionText();
		RenderCausalReport();
		if (bReplacingSelection)
		{
			SetStatus(FString::Printf(TEXT("已将关注像素替换为 P1 (%d, %d)，点击“分析”后读取该点证据。"), X, Y));
		}
		else
		{
			SetStatus(FString::Printf(TEXT("已选择关注像素 P1 (%d, %d)，点击“分析”后读取该点证据。"), X, Y));
		}
	}

	void SAnalyzerHome::StoreEventContext(const TSharedRef<FJsonObject>& Message)
	{
		FString RequestId;
		Message->TryGetStringField(TEXT("requestId"), RequestId);
		const uint32 EventId = static_cast<uint32>(Message->GetNumberField(TEXT("eventId")));
		const uint32* RequestedEventId = PendingEventContextByRequest.Find(RequestId);
		if (!RequestedEventId || *RequestedEventId != EventId)
		{
			// The selected pixel may have been replaced while this request was running.
			// Never attach an old point's context to the new single-pixel session.
			return;
		}
		AppendFullTraceRecord(TEXT("worker-response"), Message);
		CompleteWorkerRequest(RequestId, TEXT("event_context"), FString::Printf(TEXT("event=%u"), EventId));
		PendingEventContextByRequest.Remove(RequestId);
		PendingEventContextIds.Remove(EventId);

		FEventContextEvidence Context;
		Context.EventId = EventId;
		Message->TryGetStringField(TEXT("action"), Context.Action);
		Message->TryGetStringField(TEXT("actionKind"), Context.ActionKind);
		Message->TryGetStringField(TEXT("markerPath"), Context.MarkerPath);
		Message->TryGetStringField(TEXT("shaderStage"), Context.ShaderStage);
		Message->TryGetStringField(TEXT("shaderEntry"), Context.ShaderEntry);
		Message->TryGetStringField(TEXT("shaderDebugStatus"), Context.ShaderDebugStatus);
		Message->TryGetStringField(TEXT("shaderEncoding"), Context.ShaderEncoding);
		Message->TryGetBoolField(TEXT("shaderDebuggable"), Context.bShaderDebuggable);
		Message->TryGetBoolField(TEXT("sourceDebugInfo"), Context.bSourceDebugInfo);
		double Number = 0.0;
		if (Message->TryGetNumberField(TEXT("shaderInputSignatureCount"), Number))
			Context.ShaderInputSignatureCount = static_cast<int32>(Number);
		if (Message->TryGetNumberField(TEXT("shaderOutputSignatureCount"), Number))
			Context.ShaderOutputSignatureCount = static_cast<int32>(Number);
		if (Message->TryGetNumberField(TEXT("shaderConstantBlockCount"), Number))
			Context.ShaderConstantBlockCount = static_cast<int32>(Number);
		if (Message->TryGetNumberField(TEXT("shaderSamplerCount"), Number))
			Context.ShaderSamplerCount = static_cast<int32>(Number);
		if (Message->TryGetNumberField(TEXT("shaderReadOnlyResourceCount"), Number))
			Context.ShaderReadOnlyResourceCount = static_cast<int32>(Number);
		if (Message->TryGetNumberField(TEXT("shaderReadWriteResourceCount"), Number))
			Context.ShaderReadWriteResourceCount = static_cast<int32>(Number);
		const TSharedPtr<FJsonObject>* PipelineState = nullptr;
		if (Message->TryGetObjectField(TEXT("pipelineState"), PipelineState) && PipelineState)
		{
			Context.PipelineState = *PipelineState;
		}
		for (const TSharedPtr<FJsonValue>& Input : Message->GetArrayField(TEXT("inputs")))
		{
			Context.Inputs.Add(ParseBoundResource(Input->AsObject()));
		}
		for (const TSharedPtr<FJsonValue>& Output : Message->GetArrayField(TEXT("outputs")))
		{
			Context.Outputs.Add(ParseBoundResource(Output->AsObject()));
		}
		const TArray<TSharedPtr<FJsonValue>>* Provenance = nullptr;
		if (Message->TryGetArrayField(TEXT("resourceProvenance"), Provenance) && Provenance)
		{
			Context.ResourceProvenance = *Provenance;
		}
		EventContexts.Add(EventId, MoveTemp(Context));
		const FEventContextEvidence& StoredContext = EventContexts.FindChecked(EventId);
		Diagnostics.WriteRecord(TEXT("event_context_completed"), FString::Printf(
			TEXT("request=%s event=%u depth=%d critical=%s inputs=%d outputs=%d provenance=%d pipeline=%s"),
			*RequestId, EventId, EventContextDepths.FindRef(EventId),
			IsCriticalAgentEvent(EventId) ? TEXT("true") : TEXT("false"),
			StoredContext.Inputs.Num(), StoredContext.Outputs.Num(), StoredContext.ResourceProvenance.Num(),
			StoredContext.PipelineState.IsValid() ? TEXT("available") : TEXT("unavailable")));
		FEventContextEvidence& TraceContext = EventContexts.FindChecked(EventId);
		const bool bSceneSource = IsSceneSourceEvent(TraceContext.ActionKind, TraceContext.MarkerPath);
		if (bSceneSource)
		{
			TraceContext.TraceStopReason = TEXT("scene-source-reached");
			Diagnostics.WriteRecord(TEXT("focused_trace_stop"), FString::Printf(
				TEXT("event=%u depth=%d reason=scene-source-reached marker=%s"), EventId,
				EventContextDepths.FindRef(EventId), *CompactMarkerPath(TraceContext.MarkerPath)));
		}
		if (!QueueFocusedShaderDebug(EventId) && !bSceneSource)
		{
			// Shader debugging is unavailable for copies, dispatches, or unsupported shaders.
			// Use a tightly bounded fallback instead of scanning every descriptor binding.
			ScheduleResourcePixelHistories(TraceContext);
		}
		RenderCausalReport();
		ResumeAgentAfterEventContext(EventId);
		SetStatus(FString::Printf(TEXT("Event %u 的 Pipeline、资源绑定和 Shader 反射已加载；详细内容可展开查看。"), EventId));
		TryResumeAgentAfterDeterministicContexts();
	}

	void SAnalyzerHome::AddResourceTraceBoundary(FEventContextEvidence& Context, const FResourcePixelHistoryRequest& TraceRequest,
		const FString& Status, const FString& Detail)
	{
		TSharedRef<FJsonObject> Boundary = MakeShared<FJsonObject>();
		Boundary->SetNumberField(TEXT("consumerEventId"), TraceRequest.ConsumerEventId);
		Boundary->SetNumberField(TEXT("resourceIndex"), TraceRequest.ResourceIndex);
		Boundary->SetStringField(TEXT("resourceName"), TraceRequest.ResourceName);
		Boundary->SetStringField(TEXT("shaderBinding"), TraceRequest.ShaderBinding);
		Boundary->SetNumberField(TEXT("x"), TraceRequest.Pixel.X);
		Boundary->SetNumberField(TEXT("y"), TraceRequest.Pixel.Y);
		Boundary->SetNumberField(TEXT("mip"), TraceRequest.Mip);
		Boundary->SetNumberField(TEXT("slice"), TraceRequest.Slice);
		Boundary->SetNumberField(TEXT("sample"), TraceRequest.Sample);
		Boundary->SetStringField(TEXT("coordinateMapping"), TraceRequest.Mapping);
		Boundary->SetStringField(TEXT("mappingConfidence"), TraceRequest.MappingConfidence);
		if (!TraceRequest.CoordinateEvidence.IsEmpty())
		{
			Boundary->SetStringField(TEXT("coordinateEvidence"), TraceRequest.CoordinateEvidence);
		}
		Boundary->SetStringField(TEXT("traceKey"), TraceRequest.TraceKey);
		Boundary->SetStringField(TEXT("replayKey"), TraceRequest.ReplayKey);
		Boundary->SetStringField(TEXT("tracePurpose"), TraceRequest.TracePurpose);
		Boundary->SetBoolField(TEXT("executedShaderAccess"), TraceRequest.bExecutedShaderAccess);
		Boundary->SetNumberField(TEXT("collapsedShaderAccessCount"), TraceRequest.CollapsedShaderAccessCount);
		Boundary->SetNumberField(TEXT("adaptiveAttempt"), TraceRequest.AdaptiveAttempt);
		Boundary->SetNumberField(TEXT("adaptiveCandidateCount"), TraceRequest.TotalAdaptiveCandidates);
		Boundary->SetNumberField(TEXT("adaptiveCandidatesRemaining"), TraceRequest.AlternatePixels.Num());
		Boundary->SetStringField(TEXT("branchStatus"), Status);
		Boundary->SetStringField(TEXT("detail"), Detail);
		Context.ResourcePixelHistories.Add(MakeShared<FJsonValueObject>(Boundary));
	}

	void SAnalyzerHome::RemoveResourceTraceBoundary(FEventContextEvidence& Context, const FString& TraceKey,
		const FString& Status)
	{
		Context.ResourcePixelHistories.RemoveAll([&TraceKey, &Status](const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Boundary = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Boundary.IsValid())
			{
				return false;
			}
			FString BoundaryTraceKey;
			FString BoundaryStatus;
			Boundary->TryGetStringField(TEXT("traceKey"), BoundaryTraceKey);
			Boundary->TryGetStringField(TEXT("branchStatus"), BoundaryStatus);
			return BoundaryTraceKey == TraceKey && BoundaryStatus == Status;
		});
	}

	void SAnalyzerHome::ScheduleResourcePixelHistory(FEventContextEvidence& Context,
		const FResourcePixelHistoryRequest& TraceRequest)
	{
		if (ScheduledResourcePixelHistoryKeys.Contains(TraceRequest.TraceKey))
		{
			ResourcePixelHistoryBindingAliases.FindOrAdd(TraceRequest.TraceKey).AddUnique(TraceRequest.ShaderBinding);
			if (TraceRequest.bExecutedShaderAccess)
			{
				for (TPair<FString, FResourcePixelHistoryRequest>& Pair : PendingResourcePixelHistoryByRequest)
				{
					if (Pair.Value.TraceKey != TraceRequest.TraceKey)
					{
						continue;
					}
					Pair.Value.Mapping = TraceRequest.Mapping;
					Pair.Value.MappingConfidence = TraceRequest.MappingConfidence;
					Pair.Value.CoordinateEvidence = TraceRequest.CoordinateEvidence;
					Pair.Value.TracePurpose = TraceRequest.TracePurpose;
					Pair.Value.AlternatePixels = TraceRequest.AlternatePixels;
					Pair.Value.CollapsedShaderAccessCount = TraceRequest.CollapsedShaderAccessCount;
					Pair.Value.TotalAdaptiveCandidates = TraceRequest.TotalAdaptiveCandidates;
					Pair.Value.bExecutedShaderAccess = true;
					Pair.Value.bRequiredForAgent = true;
					Pair.Value.Priority = FMath::Max(Pair.Value.Priority, ComputeResourceTracePriority(TraceRequest));
					for (FQueuedWorkerRequest& Queued : QueuedWorkerRequests)
					{
						if (Queued.RequestId == Pair.Key)
						{
							Queued.Priority = FMath::Max(Queued.Priority, Pair.Value.Priority);
							break;
						}
					}
					break;
				}
				for (const TSharedPtr<FJsonValue>& HistoryValue : Context.ResourcePixelHistories)
				{
					const TSharedPtr<FJsonObject> History = HistoryValue.IsValid() ? HistoryValue->AsObject() : nullptr;
					FString ExistingTraceKey;
					if (History.IsValid() && History->TryGetStringField(TEXT("traceKey"), ExistingTraceKey)
						&& ExistingTraceKey == TraceRequest.TraceKey)
					{
						History->SetStringField(TEXT("coordinateMapping"), TraceRequest.Mapping);
						History->SetStringField(TEXT("mappingConfidence"), TraceRequest.MappingConfidence);
						History->SetStringField(TEXT("coordinateEvidence"), TraceRequest.CoordinateEvidence);
						History->SetStringField(TEXT("tracePurpose"), TraceRequest.TracePurpose);
						History->SetBoolField(TEXT("executedShaderAccess"), true);
						break;
					}
				}
			}
			Diagnostics.WriteRecord(TEXT("trace_branch_deduplicated"), FString::Printf(
				TEXT("key=%s replayKey=%s consumer=%u resource=%d binding=%s sample=%d evidenceUpgrade=%s"),
				*TraceRequest.TraceKey, *TraceRequest.ReplayKey, TraceRequest.ConsumerEventId,
				TraceRequest.ResourceIndex, *TraceRequest.ShaderBinding, TraceRequest.Sample,
				TraceRequest.bExecutedShaderAccess ? TEXT("executed-shader") : TEXT("none")));
			return;
		}
		ResourcePixelHistoryBindingAliases.FindOrAdd(TraceRequest.TraceKey).AddUnique(TraceRequest.ShaderBinding);
		const bool bExecutedShaderCoordinate = TraceRequest.Mapping.StartsWith(TEXT("executed-shader-"));
		if (bExecutedShaderCoordinate && ResourcePixelHistoryQueriesSubmitted >= ResourcePixelHistoryQueryLimit
			&& ResourcePixelHistoryQueryLimit < MaxAutomaticResourcePixelHistoryQueries)
		{
			// An executed texture access is stronger evidence than the earlier proportional
			// coordinate candidate. Preserve it even when the automatic safety budget is
			// reached by allowing this single stronger correction through.
			++ResourcePixelHistoryQueryLimit;
		}
		if (ResourcePixelHistoryQueriesSubmitted >= ResourcePixelHistoryQueryLimit)
		{
			if (!BudgetDeferredResourcePixelHistoryRequests.Contains(TraceRequest.TraceKey))
			{
				BudgetDeferredResourcePixelHistoryRequests.Add(TraceRequest.TraceKey, TraceRequest);
				AddResourceTraceBoundary(Context, TraceRequest, TEXT("query-budget-exhausted"),
					TEXT("The branch is retained as an explicit boundary because the automatic focused replay budget was reached."));
			}
			Diagnostics.WriteRecord(TEXT("trace_branch_budget"), FString::Printf(
				TEXT("key=%s submitted=%d limit=%d budgetDeferred=%d"), *TraceRequest.TraceKey,
				ResourcePixelHistoryQueriesSubmitted, ResourcePixelHistoryQueryLimit,
				BudgetDeferredResourcePixelHistoryRequests.Num()));
			return;
		}
		BudgetDeferredResourcePixelHistoryRequests.Remove(TraceRequest.TraceKey);
		RemoveResourceTraceBoundary(Context, TraceRequest.TraceKey, TEXT("query-budget-exhausted"));
		ScheduledResourcePixelHistoryKeys.Add(TraceRequest.TraceKey);
		++ResourcePixelHistoryQueriesSubmitted;
		const FString RequestId = FString::Printf(TEXT("resource-pixel-%u-%d-s%d-query-%llu"),
			TraceRequest.ConsumerEventId, TraceRequest.ResourceIndex, TraceRequest.Sample, ++RequestSerial);
		PendingResourcePixelHistoryByRequest.Add(RequestId, TraceRequest);
		Diagnostics.WriteRecord(TEXT("trace_branch_queued"), FString::Printf(
			TEXT("request=%s required=%s depth=%d key=%s resource=%s binding=%s pixel=(%d,%d) sample=%d mapping=%s pending=%d"),
			*RequestId, TraceRequest.bRequiredForAgent ? TEXT("true") : TEXT("false"), TraceRequest.ReverseDepth,
			*TraceRequest.TraceKey, *TraceRequest.ResourceName, *TraceRequest.ShaderBinding,
			TraceRequest.Pixel.X, TraceRequest.Pixel.Y, TraceRequest.Sample, *TraceRequest.Mapping,
			PendingResourcePixelHistoryByRequest.Num()));
		if (!SendWorkerRequest(TEXT("pixel_history"), RequestId,
			[TraceRequest](const TSharedRef<FJsonObject>& Request)
			{
				Request->SetNumberField(TEXT("x"), TraceRequest.Pixel.X);
				Request->SetNumberField(TEXT("y"), TraceRequest.Pixel.Y);
				Request->SetNumberField(TEXT("resourceIndex"), TraceRequest.ResourceIndex);
				Request->SetNumberField(TEXT("mip"), TraceRequest.Mip);
				Request->SetNumberField(TEXT("slice"), TraceRequest.Slice);
				Request->SetNumberField(TEXT("sample"), TraceRequest.Sample);
				Request->SetNumberField(TEXT("beforeEventId"), TraceRequest.ConsumerEventId);
				if (TraceRequest.TypeCast != INDEX_NONE)
				{
					Request->SetNumberField(TEXT("typeCast"), TraceRequest.TypeCast);
				}
			}, ComputeResourceTracePriority(TraceRequest)))
		{
			PendingResourcePixelHistoryByRequest.Remove(RequestId);
			FailedResourcePixelHistoryKeys.Add(TraceRequest.TraceKey);
			AddResourceTraceBoundary(Context, TraceRequest, TEXT("queue-failed"),
				TEXT("Replay Worker request could not be queued."));
		}
	}

	void SAnalyzerHome::ScheduleResourcePixelHistories(const FEventContextEvidence& ReadOnlyContext)
	{
		if (!bWorkerReady || ReadOnlyContext.Inputs.IsEmpty())
		{
			return;
		}
		FEventContextEvidence* MutableContext = EventContexts.Find(ReadOnlyContext.EventId);
		if (!MutableContext)
		{
			return;
		}

		FIntPoint ConsumerPixel = !Samples.IsEmpty() ? Samples[0].Pixel : FIntPoint::ZeroValue;
		if (const FIntPoint* TracedPixel = EventTracePixels.Find(ReadOnlyContext.EventId))
		{
			ConsumerPixel = *TracedPixel;
		}
		FIntPoint OutputExtent = CurrentPreviewSize;
		for (const FBoundResourceEvidence& Output : ReadOnlyContext.Outputs)
		{
			if (Output.bTexture && Output.Width > 0 && Output.Height > 0)
			{
				OutputExtent = FIntPoint(Output.Width, Output.Height);
				break;
			}
		}

		const int32 ReverseDepth = EventContextDepths.FindRef(ReadOnlyContext.EventId);
		TArray<FResourcePixelHistoryRequest> FastFrontier;
		TArray<FResourcePixelHistoryRequest> BackgroundFrontier;
		Diagnostics.WriteRecord(TEXT("trace_schedule_begin"), FString::Printf(
			TEXT("event=%u depth=%d inputs=%d outputExtent=%dx%d consumerPixel=(%d,%d) submitted=%d/%d"),
			ReadOnlyContext.EventId, ReverseDepth, ReadOnlyContext.Inputs.Num(), OutputExtent.X, OutputExtent.Y,
			ConsumerPixel.X, ConsumerPixel.Y, ResourcePixelHistoryQueriesSubmitted, ResourcePixelHistoryQueryLimit));

		for (const FBoundResourceEvidence& Input : ReadOnlyContext.Inputs)
		{
			if (!Input.bTexture || Input.ResourceIndex == INDEX_NONE || Input.Width <= 0 || Input.Height <= 0)
			{
				continue;
			}

			FIntPoint InputPixel = ConsumerPixel;
			FString Mapping = TEXT("same-coordinate");
			FString MappingConfidence = TEXT("same-extent");
			if (OutputExtent.X != Input.Width || OutputExtent.Y != Input.Height)
			{
				if (FMath::Abs(OutputExtent.X - Input.Width) <= 1 && FMath::Abs(OutputExtent.Y - Input.Height) <= 1)
				{
					Mapping = TEXT("same-coordinate-near-extent-candidate");
					MappingConfidence = TEXT("candidate");
				}
				else if (OutputExtent.X > 0 && OutputExtent.Y > 0)
				{
					InputPixel.X = FMath::FloorToInt((static_cast<double>(ConsumerPixel.X) + 0.5)
						* static_cast<double>(Input.Width) / static_cast<double>(OutputExtent.X));
					InputPixel.Y = FMath::FloorToInt((static_cast<double>(ConsumerPixel.Y) + 0.5)
						* static_cast<double>(Input.Height) / static_cast<double>(OutputExtent.Y));
					Mapping = TEXT("normalized-pixel-center-candidate");
					MappingConfidence = TEXT("candidate");
				}
			}
			InputPixel.X = FMath::Clamp(InputPixel.X, 0, Input.Width - 1);
			InputPixel.Y = FMath::Clamp(InputPixel.Y, 0, Input.Height - 1);

			const int32 AvailableSamples = FMath::Max(1, Input.Samples);
			const int32 SamplesToTrace = 1;
			for (int32 SampleIndex = 0; SampleIndex < SamplesToTrace; ++SampleIndex)
			{
				FResourcePixelHistoryRequest TraceRequest;
				TraceRequest.ConsumerEventId = ReadOnlyContext.EventId;
				TraceRequest.ResourceIndex = Input.ResourceIndex;
				TraceRequest.ResourceName = Input.Name;
				TraceRequest.ShaderBinding = Input.ShaderBinding;
				TraceRequest.Mapping = Mapping;
				TraceRequest.MappingConfidence = MappingConfidence;
				TraceRequest.Pixel = InputPixel;
				TraceRequest.Mip = Input.FirstMip;
				TraceRequest.Slice = Input.FirstSlice;
				TraceRequest.Sample = SampleIndex;
				TraceRequest.TypeCast = Input.TypeCast;
				TraceRequest.ReverseDepth = ReverseDepth;
				TraceRequest.TracePurpose = ClassifyResourceTracePurpose(Input.Name, Input.ShaderBinding);
				TraceRequest.bRequiredForAgent = IsCriticalAgentEvent(ReadOnlyContext.EventId);
				TraceRequest.bExecutedShaderAccess = false;
				TraceRequest.TraceKey = FString::Printf(TEXT("%u:%d:%d:%d:%d:%d:%d:%d"),
					TraceRequest.ConsumerEventId, TraceRequest.ResourceIndex, TraceRequest.Pixel.X, TraceRequest.Pixel.Y,
					TraceRequest.Mip, TraceRequest.Slice, TraceRequest.Sample, TraceRequest.TypeCast);
				TraceRequest.ReplayKey = BuildReplayPixelHistoryKey(TraceRequest.ResourceIndex, TraceRequest.Pixel,
					TraceRequest.Mip, TraceRequest.Slice, TraceRequest.Sample, TraceRequest.TypeCast);
				TraceRequest.Priority = ComputeResourceTracePriority(TraceRequest);
				(SampleIndex == 0 ? FastFrontier : BackgroundFrontier).Add(MoveTemp(TraceRequest));
			}

			if (AvailableSamples > SamplesToTrace)
			{
				FResourcePixelHistoryRequest Boundary;
				Boundary.ConsumerEventId = ReadOnlyContext.EventId;
				Boundary.ResourceIndex = Input.ResourceIndex;
				Boundary.ResourceName = Input.Name;
				Boundary.ShaderBinding = Input.ShaderBinding;
				Boundary.Pixel = InputPixel;
				Boundary.Mip = Input.FirstMip;
				Boundary.Slice = Input.FirstSlice;
				Boundary.Sample = SamplesToTrace;
				Boundary.Mapping = Mapping;
				Boundary.MappingConfidence = MappingConfidence;
				AddResourceTraceBoundary(*MutableContext, Boundary, TEXT("sample-limit"),
					FString::Printf(TEXT("%d samples exist; only the first %d were queried."), AvailableSamples, SamplesToTrace));
			}
		}

		FastFrontier.Append(BackgroundFrontier);
		FastFrontier.Sort([](const FResourcePixelHistoryRequest& A, const FResourcePixelHistoryRequest& B)
		{
			return A.Priority == B.Priority ? A.TraceKey < B.TraceKey : A.Priority > B.Priority;
		});
		TArray<int32> SelectedFallbackIndices;
		if (!FastFrontier.IsEmpty())
		{
			SelectedFallbackIndices.Add(0);
		}
		for (int32 Index = 0; Index < FastFrontier.Num()
			&& SelectedFallbackIndices.Num() < MaxFallbackTextureBranchesPerEvent; ++Index)
		{
			if (FastFrontier[Index].TracePurpose == TEXT("geometry"))
			{
				SelectedFallbackIndices.AddUnique(Index);
				break;
			}
		}
		for (int32 Index = 0; Index < FastFrontier.Num()
			&& SelectedFallbackIndices.Num() < MaxFallbackTextureBranchesPerEvent; ++Index)
		{
			SelectedFallbackIndices.AddUnique(Index);
		}
		for (int32 Index = 0; Index < FastFrontier.Num(); ++Index)
		{
			if (SelectedFallbackIndices.Contains(Index))
			{
				ScheduleResourcePixelHistory(*MutableContext, FastFrontier[Index]);
			}
			else
			{
				AddResourceTraceBoundary(*MutableContext, FastFrontier[Index], TEXT("focused-fallback-pruned"),
					TEXT("The bound texture was retained as visibility evidence but not replayed because the focused Shader Debug fallback limit was reached."));
			}
		}
		const int32 ScheduledCount = SelectedFallbackIndices.Num();
		Diagnostics.WriteRecord(TEXT("trace_schedule_end"), FString::Printf(
			TEXT("event=%u depth=%d fallbackCandidates=%d scheduled=%d pendingRequired=%d pendingBackground=%d submitted=%d/%d"),
			ReadOnlyContext.EventId, ReverseDepth, FastFrontier.Num(), ScheduledCount,
			GetPendingRequiredResourceHistoryCount(), GetPendingBackgroundResourceHistoryCount(),
			ResourcePixelHistoryQueriesSubmitted, ResourcePixelHistoryQueryLimit));
	}

	int32 SAnalyzerHome::ScheduleExecutedTextureAccessHistories(FEventContextEvidence& Context)
	{
		if (!bWorkerReady || !Context.ShaderDebugTrace.IsValid())
		{
			return 0;
		}
		const TArray<TSharedPtr<FJsonValue>>* TextureAccesses = nullptr;
		if (!Context.ShaderDebugTrace->TryGetArrayField(TEXT("textureAccesses"), TextureAccesses) || !TextureAccesses)
		{
			return 0;
		}

		const int32 ReverseDepth = EventContextDepths.FindRef(Context.EventId);
		FIntPoint ConsumerPixel = !Samples.IsEmpty() ? Samples[0].Pixel : FIntPoint::ZeroValue;
		if (const FIntPoint* TracedPixel = EventTracePixels.Find(Context.EventId))
		{
			ConsumerPixel = *TracedPixel;
		}
		FIntPoint OutputExtent = CurrentPreviewSize;
		for (const FBoundResourceEvidence& Output : Context.Outputs)
		{
			if (Output.bTexture && Output.Width > 0 && Output.Height > 0)
			{
				OutputExtent = FIntPoint(Output.Width, Output.Height);
				break;
			}
		}

		TArray<FResourcePixelHistoryRequest> ResourceRequests;
		int32 ParsedAccessCount = 0;
		for (const TSharedPtr<FJsonValue>& AccessValue : *TextureAccesses)
		{
			const TSharedPtr<FJsonObject> Access = AccessValue.IsValid() ? AccessValue->AsObject() : nullptr;
			if (!Access.IsValid())
			{
				continue;
			}
			FString Disassembly;
			Access->TryGetStringField(TEXT("disassembly"), Disassembly);
			const bool bLoad = Disassembly.Contains(TEXT(".Load("), ESearchCase::IgnoreCase);
			const bool bSample = Disassembly.Contains(TEXT(".Sample"), ESearchCase::IgnoreCase);
			if (!bLoad && !bSample)
			{
				continue;
			}

			const FString PatternText = bLoad
				? TEXT("\\.Load\\(\\s*([_A-Za-z][_A-Za-z0-9\\.]*)\\s*,\\s*([_A-Za-z][_A-Za-z0-9\\.]*)")
				: TEXT("\\.Sample[A-Za-z]*\\(\\s*[_A-Za-z][_A-Za-z0-9\\.]*\\s*,\\s*([_A-Za-z][_A-Za-z0-9\\.]*)\\s*,\\s*([_A-Za-z][_A-Za-z0-9\\.]*)");
			FRegexMatcher CoordinateMatcher(FRegexPattern(PatternText), Disassembly);
			if (!CoordinateMatcher.FindNext())
			{
				continue;
			}
			const FString XVariable = CoordinateMatcher.GetCaptureGroup(1);
			const FString YVariable = CoordinateMatcher.GetCaptureGroup(2);

			const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
			if (!Access->TryGetArrayField(TEXT("variables"), Variables) || !Variables)
			{
				continue;
			}
			auto TryGetScalar = [Variables](const FString& VariableName, double& OutValue)
			{
				for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
				{
					const TSharedPtr<FJsonObject> Variable = VariableValue.IsValid() ? VariableValue->AsObject() : nullptr;
					FString Name;
					const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
					if (!Variable.IsValid() || !Variable->TryGetStringField(TEXT("name"), Name)
						|| Name != VariableName || !Variable->TryGetArrayField(TEXT("values"), Values)
						|| !Values || Values->IsEmpty() || !(*Values)[0].IsValid()
						|| (*Values)[0]->Type != EJson::Number)
					{
						continue;
					}
					OutValue = (*Values)[0]->AsNumber();
					return FMath::IsFinite(OutValue);
				}
				return false;
			};

			double ExecutedX = 0.0;
			double ExecutedY = 0.0;
			if (!TryGetScalar(XVariable, ExecutedX) || !TryGetScalar(YVariable, ExecutedY))
			{
				continue;
			}
			++ParsedAccessCount;
			double InstructionValue = 0.0;
			Access->TryGetNumberField(TEXT("instruction"), InstructionValue);

			for (const FBoundResourceEvidence& Input : Context.Inputs)
			{
				if (!Input.bTexture || Input.ResourceIndex == INDEX_NONE || Input.Width <= 0 || Input.Height <= 0)
				{
					continue;
				}
				const bool bBindingMatched = (!Input.ShaderBinding.IsEmpty()
					&& Disassembly.Contains(Input.ShaderBinding, ESearchCase::CaseSensitive))
					|| (!Input.Name.IsEmpty() && Disassembly.Contains(Input.Name, ESearchCase::CaseSensitive));
				if (!bBindingMatched)
				{
					continue;
				}

				int32 SampleIndex = 0;
				if (Input.Samples > 1)
				{
					FRegexMatcher SampleMatcher(FRegexPattern(TEXT("SampleIndex\\s*=\\s*([0-9]+)")), Disassembly);
					if (SampleMatcher.FindNext())
					{
						SampleIndex = FCString::Atoi(*SampleMatcher.GetCaptureGroup(1));
					}
					if (SampleIndex < 0 || SampleIndex >= Input.Samples)
					{
						continue;
					}
				}

				TArray<FIntPoint> Pixels;
				FString Mapping;
				FString MappingConfidence;
				FString CoordinateEvidence;
				const int32 MipWidth = FMath::Max(1, Input.Width >> Input.FirstMip);
				const int32 MipHeight = FMath::Max(1, Input.Height >> Input.FirstMip);
				FIntPoint ExpectedInputPixel = ConsumerPixel;
				if (OutputExtent.X > 0 && OutputExtent.Y > 0)
				{
					ExpectedInputPixel.X = FMath::Clamp(FMath::FloorToInt(
						(static_cast<double>(ConsumerPixel.X) + 0.5) * MipWidth / OutputExtent.X), 0, MipWidth - 1);
					ExpectedInputPixel.Y = FMath::Clamp(FMath::FloorToInt(
						(static_cast<double>(ConsumerPixel.Y) + 0.5) * MipHeight / OutputExtent.Y), 0, MipHeight - 1);
				}
				if (bLoad)
				{
					Pixels.Add(FIntPoint(
						FMath::Clamp(FMath::RoundToInt(ExecutedX), 0, MipWidth - 1),
						FMath::Clamp(FMath::RoundToInt(ExecutedY), 0, MipHeight - 1)));
					Mapping = TEXT("executed-shader-load-coordinate");
					MappingConfidence = TEXT("confirmed-executed-values");
					CoordinateEvidence = FString::Printf(TEXT("instruction=%d Load(%s=%g,%s=%g)"),
						static_cast<int32>(InstructionValue), *XVariable, ExecutedX, *YVariable, ExecutedY);
				}
				else
				{
					// Preserve the complete executed UV in Shader Debug, but query the footprint
					// adaptively. A structural producer edge does not require replaying every blur tap.
					if (ExecutedX < 0.0 || ExecutedX > 1.0 || ExecutedY < 0.0 || ExecutedY > 1.0)
					{
						continue;
					}
					const double TexelX = ExecutedX * MipWidth - 0.5;
					const double TexelY = ExecutedY * MipHeight - 0.5;
					const int32 X0 = FMath::Clamp(FMath::FloorToInt(TexelX), 0, MipWidth - 1);
					const int32 X1 = FMath::Clamp(FMath::CeilToInt(TexelX), 0, MipWidth - 1);
					const int32 Y0 = FMath::Clamp(FMath::FloorToInt(TexelY), 0, MipHeight - 1);
					const int32 Y1 = FMath::Clamp(FMath::CeilToInt(TexelY), 0, MipHeight - 1);
					Pixels.AddUnique(FIntPoint(X0, Y0));
					Pixels.AddUnique(FIntPoint(X1, Y0));
					Pixels.AddUnique(FIntPoint(X0, Y1));
					Pixels.AddUnique(FIntPoint(X1, Y1));
					Mapping = TEXT("executed-shader-sample-footprint-candidate");
					MappingConfidence = TEXT("confirmed-executed-uv-bounded-filter-footprint");
					CoordinateEvidence = FString::Printf(
						TEXT("instruction=%d Sample(%s=%0.9g,%s=%0.9g) texelCenter=(%0.3f,%0.3f) boundedFootprint=[(%d,%d)-(%d,%d)]"),
						static_cast<int32>(InstructionValue), *XVariable, ExecutedX, *YVariable, ExecutedY,
						TexelX, TexelY, X0, Y0, X1, Y1);
				}
				Pixels.Sort([ExpectedInputPixel](const FIntPoint& A, const FIntPoint& B)
				{
					const int64 ADistance = FMath::Square(static_cast<int64>(A.X - ExpectedInputPixel.X))
						+ FMath::Square(static_cast<int64>(A.Y - ExpectedInputPixel.Y));
					const int64 BDistance = FMath::Square(static_cast<int64>(B.X - ExpectedInputPixel.X))
						+ FMath::Square(static_cast<int64>(B.Y - ExpectedInputPixel.Y));
					return ADistance == BDistance ? (A.X == B.X ? A.Y < B.Y : A.X < B.X) : ADistance < BDistance;
				});
				if (Pixels.IsEmpty())
				{
					continue;
				}

				FResourcePixelHistoryRequest TraceRequest;
				TraceRequest.ConsumerEventId = Context.EventId;
				TraceRequest.ResourceIndex = Input.ResourceIndex;
				TraceRequest.ResourceName = Input.Name;
				TraceRequest.ShaderBinding = Input.ShaderBinding;
				TraceRequest.Mapping = bSample ? TEXT("executed-shader-sample-adaptive-footprint") : Mapping;
				TraceRequest.MappingConfidence = MappingConfidence;
				TraceRequest.CoordinateEvidence = CoordinateEvidence;
				TraceRequest.Pixel = Pixels[0];
				for (int32 PixelIndex = 1; PixelIndex < Pixels.Num(); ++PixelIndex)
				{
					TraceRequest.AlternatePixels.AddUnique(Pixels[PixelIndex]);
				}
				TraceRequest.Mip = Input.FirstMip;
				TraceRequest.Slice = Input.FirstSlice;
				TraceRequest.Sample = SampleIndex;
				TraceRequest.TypeCast = Input.TypeCast;
				TraceRequest.ReverseDepth = ReverseDepth;
				TraceRequest.TracePurpose = ClassifyResourceTracePurpose(Input.Name, Input.ShaderBinding);
				TraceRequest.bRequiredForAgent = true;
				TraceRequest.bExecutedShaderAccess = true;
				TraceRequest.TotalAdaptiveCandidates = Pixels.Num();
				TraceRequest.RepresentativeDistanceSquared =
					FMath::Square(static_cast<double>(TraceRequest.Pixel.X - ExpectedInputPixel.X))
					+ FMath::Square(static_cast<double>(TraceRequest.Pixel.Y - ExpectedInputPixel.Y));
				TraceRequest.TraceKey = FString::Printf(TEXT("%u:%d:%d:%d:%d:%d:%d:%d"),
					TraceRequest.ConsumerEventId, TraceRequest.ResourceIndex, TraceRequest.Pixel.X, TraceRequest.Pixel.Y,
					TraceRequest.Mip, TraceRequest.Slice, TraceRequest.Sample, TraceRequest.TypeCast);
				TraceRequest.ReplayKey = BuildReplayPixelHistoryKey(TraceRequest.ResourceIndex, TraceRequest.Pixel,
					TraceRequest.Mip, TraceRequest.Slice, TraceRequest.Sample, TraceRequest.TypeCast);
				TraceRequest.Priority = ComputeResourceTracePriority(TraceRequest);

				const int32 ExistingIndex = ResourceRequests.IndexOfByPredicate([&TraceRequest](
					const FResourcePixelHistoryRequest& Existing)
				{
					return Existing.ResourceIndex == TraceRequest.ResourceIndex
						&& Existing.Mip == TraceRequest.Mip && Existing.Slice == TraceRequest.Slice
						&& Existing.Sample == TraceRequest.Sample && Existing.TypeCast == TraceRequest.TypeCast;
				});
				if (ExistingIndex == INDEX_NONE)
				{
					ResourceRequests.Add(MoveTemp(TraceRequest));
					continue;
				}

				FResourcePixelHistoryRequest& Existing = ResourceRequests[ExistingIndex];
				const int32 CollapsedCount = Existing.CollapsedShaderAccessCount + 1;
				TArray<FIntPoint> MergedCandidates;
				MergedCandidates.Add(Existing.Pixel);
				MergedCandidates.Append(Existing.AlternatePixels);
				MergedCandidates.AddUnique(TraceRequest.Pixel);
				for (const FIntPoint Alternate : TraceRequest.AlternatePixels)
				{
					MergedCandidates.AddUnique(Alternate);
				}
				MergedCandidates.Sort([ExpectedInputPixel](const FIntPoint& A, const FIntPoint& B)
				{
					const int64 ADistance = FMath::Square(static_cast<int64>(A.X - ExpectedInputPixel.X))
						+ FMath::Square(static_cast<int64>(A.Y - ExpectedInputPixel.Y));
					const int64 BDistance = FMath::Square(static_cast<int64>(B.X - ExpectedInputPixel.X))
						+ FMath::Square(static_cast<int64>(B.Y - ExpectedInputPixel.Y));
					return ADistance == BDistance ? (A.X == B.X ? A.Y < B.Y : A.X < B.X) : ADistance < BDistance;
				});
				const bool bNewExactLoad = TraceRequest.Mapping == TEXT("executed-shader-load-coordinate")
					&& Existing.Mapping != TEXT("executed-shader-load-coordinate");
				if (bNewExactLoad || (TraceRequest.Mapping == Existing.Mapping
					&& TraceRequest.RepresentativeDistanceSquared < Existing.RepresentativeDistanceSquared))
				{
					Existing = TraceRequest;
				}
				Existing.CollapsedShaderAccessCount = CollapsedCount;
				Existing.AlternatePixels.Empty();
				for (const FIntPoint CandidatePixel : MergedCandidates)
				{
					if (CandidatePixel != Existing.Pixel
						&& Existing.AlternatePixels.Num() < MaxAdaptivePixelsPerResourceTrace - 1)
					{
						Existing.AlternatePixels.AddUnique(CandidatePixel);
					}
				}
				Existing.TotalAdaptiveCandidates = 1 + Existing.AlternatePixels.Num();
			}
		}
		ResourceRequests.Sort([](const FResourcePixelHistoryRequest& A, const FResourcePixelHistoryRequest& B)
		{
			return A.Priority == B.Priority ? A.TraceKey < B.TraceKey : A.Priority > B.Priority;
		});
		TArray<int32> SelectedRequestIndices;
		if (!ResourceRequests.IsEmpty())
		{
			SelectedRequestIndices.Add(0);
		}
		static const TCHAR* CoveragePurposes[] = { TEXT("geometry"), TEXT("overlay"), TEXT("color") };
		for (const TCHAR* Purpose : CoveragePurposes)
		{
			for (int32 Index = 0; Index < ResourceRequests.Num()
				&& SelectedRequestIndices.Num() < MaxExecutedTextureBranchesPerEvent; ++Index)
			{
				if (ResourceRequests[Index].TracePurpose == Purpose)
				{
					SelectedRequestIndices.AddUnique(Index);
					break;
				}
			}
		}
		for (int32 Index = 0; Index < ResourceRequests.Num()
			&& SelectedRequestIndices.Num() < MaxExecutedTextureBranchesPerEvent; ++Index)
		{
			SelectedRequestIndices.AddUnique(Index);
		}
		for (int32 Index = 0; Index < ResourceRequests.Num(); ++Index)
		{
			if (SelectedRequestIndices.Contains(Index))
			{
				ScheduleResourcePixelHistory(Context, ResourceRequests[Index]);
			}
			else
			{
				AddResourceTraceBoundary(Context, ResourceRequests[Index], TEXT("focused-resource-limit"),
					TEXT("Executed accesses were collapsed by resource; this resource group was retained but not replayed because the per-event resource limit was reached."));
			}
		}
		const int32 ScheduledCount = SelectedRequestIndices.Num();
		Diagnostics.WriteRecord(TEXT("executed_texture_access_histories_scheduled"), FString::Printf(
			TEXT("event=%u accesses=%d parsed=%d resourceGroups=%d scheduled=%d collapsedAccesses=%d"), Context.EventId,
			TextureAccesses->Num(), ParsedAccessCount, ResourceRequests.Num(), ScheduledCount,
			FMath::Max(0, ParsedAccessCount - ResourceRequests.Num())));
		return ScheduledCount;
	}

	void SAnalyzerHome::EnrichResourcePixelHistoryFromShaderTrace(const FEventContextEvidence& Context,
		const TSharedRef<FJsonObject>& Evidence) const
	{
		if (!Context.ShaderDebugTrace.IsValid())
		{
			return;
		}
		FString ShaderBinding;
		FString CoordinateMapping;
		double X = 0.0;
		double Y = 0.0;
		double SampleIndex = 0.0;
		Evidence->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
		Evidence->TryGetStringField(TEXT("coordinateMapping"), CoordinateMapping);
		Evidence->TryGetNumberField(TEXT("x"), X);
		Evidence->TryGetNumberField(TEXT("y"), Y);
		Evidence->TryGetNumberField(TEXT("sample"), SampleIndex);
		if (ShaderBinding.IsEmpty())
		{
			return;
		}
		bool bMultisampledBinding = false;
		for (const FBoundResourceEvidence& Input : Context.Inputs)
		{
			if (Input.ShaderBinding == ShaderBinding)
			{
				bMultisampledBinding = Input.Samples > 1;
				break;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* TextureAccesses = nullptr;
		if (!Context.ShaderDebugTrace->TryGetArrayField(TEXT("textureAccesses"), TextureAccesses) || !TextureAccesses)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& AccessValue : *TextureAccesses)
		{
			const TSharedPtr<FJsonObject> Access = AccessValue.IsValid() ? AccessValue->AsObject() : nullptr;
			if (!Access.IsValid())
			{
				continue;
			}
			FString Disassembly;
			Access->TryGetStringField(TEXT("disassembly"), Disassembly);
			if (!Disassembly.Contains(ShaderBinding, ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (bMultisampledBinding && !Disassembly.Contains(
				FString::Printf(TEXT("SampleIndex = %d"), static_cast<int32>(SampleIndex)), ESearchCase::CaseSensitive))
			{
				continue;
			}

			bool bSawX = false;
			bool bSawY = false;
			const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
			if (Access->TryGetArrayField(TEXT("variables"), Variables) && Variables)
			{
				for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
				{
					const TSharedPtr<FJsonObject> Variable = VariableValue.IsValid() ? VariableValue->AsObject() : nullptr;
					const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
					if (!Variable.IsValid() || !Variable->TryGetArrayField(TEXT("values"), Values) || !Values)
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& Value : *Values)
					{
						if (!Value.IsValid() || Value->Type != EJson::Number)
						{
							continue;
						}
						const double Number = Value->AsNumber();
						bSawX |= FMath::IsNearlyEqual(Number, X);
						bSawY |= FMath::IsNearlyEqual(Number, Y);
					}
				}
			}
			Evidence->SetBoolField(TEXT("shaderAccessObserved"), true);
			Evidence->SetStringField(TEXT("shaderAccessDisassembly"), Disassembly);
			if (Variables && !Variables->IsEmpty())
			{
				FString ResultVariableName;
				FRegexMatcher ResultMatcher(FRegexPattern(
					TEXT("\\s([_A-Za-z][_A-Za-z0-9\\.]*)\\s*=\\s*__")), Disassembly);
				if (ResultMatcher.FindNext())
				{
					ResultVariableName = ResultMatcher.GetCaptureGroup(1);
				}
				for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
				{
					const TSharedPtr<FJsonObject> Variable = VariableValue.IsValid() ? VariableValue->AsObject() : nullptr;
					const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
					FString VariableName;
					FString VariableType;
					if (!Variable.IsValid() || !Variable->TryGetStringField(TEXT("name"), VariableName)
						|| (!ResultVariableName.IsEmpty() && VariableName != ResultVariableName)
						|| !Variable->TryGetArrayField(TEXT("values"), Values) || !Values || Values->IsEmpty())
					{
						continue;
					}
					Variable->TryGetStringField(TEXT("type"), VariableType);
					TArray<TSharedPtr<FJsonValue>> Components;
					for (int32 ComponentIndex = 0; ComponentIndex < 4; ++ComponentIndex)
					{
						const double Component = Values->IsValidIndex(ComponentIndex)
							&& (*Values)[ComponentIndex].IsValid()
							&& (*Values)[ComponentIndex]->Type == EJson::Number
							? (*Values)[ComponentIndex]->AsNumber() : 0.0;
						Components.Add(MakeShared<FJsonValueNumber>(Component));
					}
					const TSharedRef<FJsonObject> AccessResult = MakeShared<FJsonObject>();
					AccessResult->SetStringField(TEXT("name"), VariableName);
					AccessResult->SetStringField(TEXT("type"), VariableType);
					AccessResult->SetArrayField(TEXT("float"), MoveTemp(Components));
					Evidence->SetObjectField(TEXT("shaderAccessResult"), AccessResult);
					break;
				}
			}
			double Instruction = 0.0;
			if (Access->TryGetNumberField(TEXT("instruction"), Instruction))
			{
				Evidence->SetNumberField(TEXT("shaderAccessInstruction"), Instruction);
			}
			const bool bExecutedSampleFootprint = CoordinateMapping.StartsWith(TEXT("executed-shader-sample"));
			const bool bCoordinateEvidenceMatched = bExecutedSampleFootprint || (bSawX && bSawY);
			Evidence->SetBoolField(TEXT("shaderCoordinateValuesMatched"), bCoordinateEvidenceMatched);
			Evidence->SetBoolField(TEXT("shaderFootprintDerivedFromExecutedValues"), bExecutedSampleFootprint);
			if (!bExecutedSampleFootprint && bSawX && bSawY)
			{
				Evidence->SetStringField(TEXT("coordinateMapping"), TEXT("executed-shader-load-coordinate"));
				Evidence->SetStringField(TEXT("mappingConfidence"), TEXT("confirmed-executed-values"));
			}
			return;
		}
	}

	void SAnalyzerHome::StoreResourcePixelHistory(const TSharedRef<FJsonObject>& Message)
	{
		FString RequestId;
		Message->TryGetStringField(TEXT("requestId"), RequestId);
		const FResourcePixelHistoryRequest* PendingRequest = PendingResourcePixelHistoryByRequest.Find(RequestId);
		if (!PendingRequest)
		{
			return;
		}
		AppendFullTraceRecord(TEXT("worker-response"), Message);
		const FResourcePixelHistoryRequest TraceRequest = *PendingRequest;
		const double QueueToResponseSeconds = CompleteWorkerRequest(RequestId, TEXT("resource_pixel_history"),
			FString::Printf(TEXT("consumer=%u resource=%d sample=%d required=%s"),
				TraceRequest.ConsumerEventId, TraceRequest.ResourceIndex, TraceRequest.Sample,
				TraceRequest.bRequiredForAgent ? TEXT("true") : TEXT("false")));
		PendingResourcePixelHistoryByRequest.Remove(RequestId);
		FEventContextEvidence* Context = EventContexts.Find(TraceRequest.ConsumerEventId);
		if (!Context)
		{
			TryResumeAgentAfterDeterministicContexts();
			return;
		}

		TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetNumberField(TEXT("consumerEventId"), TraceRequest.ConsumerEventId);
		Evidence->SetNumberField(TEXT("resourceIndex"), TraceRequest.ResourceIndex);
		Evidence->SetStringField(TEXT("resourceName"), TraceRequest.ResourceName);
		Evidence->SetStringField(TEXT("shaderBinding"), TraceRequest.ShaderBinding);
		Evidence->SetNumberField(TEXT("x"), TraceRequest.Pixel.X);
		Evidence->SetNumberField(TEXT("y"), TraceRequest.Pixel.Y);
		Evidence->SetNumberField(TEXT("mip"), TraceRequest.Mip);
		Evidence->SetNumberField(TEXT("slice"), TraceRequest.Slice);
		Evidence->SetNumberField(TEXT("sample"), TraceRequest.Sample);
		Evidence->SetStringField(TEXT("coordinateMapping"), TraceRequest.Mapping);
		Evidence->SetStringField(TEXT("mappingConfidence"), TraceRequest.MappingConfidence);
		Evidence->SetStringField(TEXT("traceKey"), TraceRequest.TraceKey);
		Evidence->SetStringField(TEXT("replayKey"), TraceRequest.ReplayKey);
		Evidence->SetStringField(TEXT("tracePurpose"), TraceRequest.TracePurpose);
		Evidence->SetBoolField(TEXT("executedShaderAccess"), TraceRequest.bExecutedShaderAccess);
		Evidence->SetNumberField(TEXT("collapsedShaderAccessCount"), TraceRequest.CollapsedShaderAccessCount);
		Evidence->SetNumberField(TEXT("adaptiveAttempt"), TraceRequest.AdaptiveAttempt);
		Evidence->SetNumberField(TEXT("adaptiveCandidateCount"), TraceRequest.TotalAdaptiveCandidates);
		Evidence->SetNumberField(TEXT("adaptiveCandidatesRemaining"), TraceRequest.AlternatePixels.Num());
		if (!TraceRequest.CoordinateEvidence.IsEmpty())
		{
			Evidence->SetStringField(TEXT("coordinateEvidence"), TraceRequest.CoordinateEvidence);
		}
		Evidence->SetBoolField(TEXT("requiredForAgent"), TraceRequest.bRequiredForAgent);
		Evidence->SetNumberField(TEXT("queueToResponseSeconds"), QueueToResponseSeconds);
		TArray<TSharedPtr<FJsonValue>> BindingAliases;
		if (const TArray<FString>* Aliases = ResourcePixelHistoryBindingAliases.Find(TraceRequest.TraceKey))
		{
			for (const FString& Alias : *Aliases)
			{
				BindingAliases.Add(MakeShared<FJsonValueString>(Alias));
			}
		}
		Evidence->SetArrayField(TEXT("shaderBindingAliases"), MoveTemp(BindingAliases));
		double TotalModifications = 0.0;
		double TotalEvents = 0.0;
		bool bTruncated = false;
		Message->TryGetNumberField(TEXT("totalModifications"), TotalModifications);
		Message->TryGetNumberField(TEXT("totalEvents"), TotalEvents);
		Message->TryGetBoolField(TEXT("truncated"), bTruncated);
		Evidence->SetNumberField(TEXT("totalModifications"), TotalModifications);
		Evidence->SetNumberField(TEXT("totalEvents"), TotalEvents);
		Evidence->SetBoolField(TEXT("detailTailTruncated"), bTruncated);

		TArray<TSharedPtr<FJsonValue>> ConfirmedWriterIds;
		TArray<FEventSummaryEvidence> ParsedSummaries;
		const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
		int32 ExpandedWriterCount = 0;
		int32 ConfirmedWriterCount = 0;
		if (Message->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
		{
			Evidence->SetArrayField(TEXT("eventSummaries"), *EventSummaries);
			for (const TSharedPtr<FJsonValue>& SummaryValue : *EventSummaries)
			{
				const TSharedPtr<FJsonObject> Summary = SummaryValue.IsValid() ? SummaryValue->AsObject() : nullptr;
				if (!Summary.IsValid())
				{
					continue;
				}
				FEventSummaryEvidence Parsed;
				Parsed.EventId = static_cast<uint32>(Summary->GetNumberField(TEXT("eventId")));
				Summary->TryGetStringField(TEXT("action"), Parsed.Action);
				Summary->TryGetStringField(TEXT("actionKind"), Parsed.ActionKind);
				Summary->TryGetStringField(TEXT("markerPath"), Parsed.MarkerPath);
				Parsed.PassedFragments = static_cast<int32>(Summary->GetNumberField(TEXT("passedFragments")));
				Summary->TryGetBoolField(TEXT("changedTextureValue"), Parsed.bChangedTextureValue);
				Summary->TryGetBoolField(TEXT("hasPrimitiveEvidence"), Parsed.bHasPrimitiveEvidence);
				Parsed.PrimitiveId = static_cast<uint32>(Summary->GetNumberField(TEXT("lastPrimitiveId")));
				ParsedSummaries.Add(Parsed);
				if (Parsed.EventId == 0 || Parsed.EventId == TraceRequest.ConsumerEventId
					|| Parsed.EventId >= TraceRequest.ConsumerEventId
					|| (Parsed.PassedFragments <= 0 && !Parsed.bChangedTextureValue))
				{
					continue;
				}
				++ConfirmedWriterCount;
				ConfirmedWriterIds.Add(MakeShared<FJsonValueNumber>(Parsed.EventId));
			}
		}
		Evidence->SetArrayField(TEXT("confirmedWriterEventIds"), MoveTemp(ConfirmedWriterIds));
		const int32 DominatingWriterIndex = SelectDominatingWriterSummaryIndex(
			ParsedSummaries, TraceRequest.ConsumerEventId, TraceRequest.TracePurpose);
		bool bWriterExpanded = false;
		bool bGeometryResetBoundary = false;
		if (ParsedSummaries.IsValidIndex(DominatingWriterIndex))
		{
			const FEventSummaryEvidence& Writer = ParsedSummaries[DominatingWriterIndex];
			if (TraceRequest.TracePurpose == TEXT("geometry") && Writer.ActionKind == TEXT("clear"))
			{
				bGeometryResetBoundary = true;
				Evidence->SetNumberField(TEXT("resetBoundaryEventId"), Writer.EventId);
				Evidence->SetStringField(TEXT("resetBoundaryReason"), TEXT("depth-or-visibility-clear"));
			}
			else
			{
				Evidence->SetNumberField(TEXT("selectedWriterEventId"), Writer.EventId);
				Evidence->SetStringField(TEXT("selectedWriterReason"),
					TraceRequest.TracePurpose == TEXT("geometry")
						? TEXT("latest-passed-value-changing-geometry-draw")
						: (Writer.bChangedTextureValue ? TEXT("latest-value-changing-writer")
							: TEXT("latest-passed-writer-fallback")));
				Evidence->SetNumberField(TEXT("dominatedWriterCount"), FMath::Max(0, ConfirmedWriterCount - 1));
				FocusedTraceEventIds.Add(Writer.EventId);
				EventTracePixels.Add(Writer.EventId, TraceRequest.Pixel);
				if (Writer.bHasPrimitiveEvidence)
				{
					EventTracePrimitiveIds.Add(Writer.EventId, Writer.PrimitiveId);
					EventTracePrimitiveEvidenceIds.Add(Writer.EventId);
				}
				EnsureEventContext(Writer.EventId, TraceRequest.ReverseDepth + 1);
				ExpandedWriterCount = 1;
				bWriterExpanded = true;
			}
		}

		bool bAdaptiveContinuation = false;
		if (!bWriterExpanded && !bGeometryResetBoundary && !TraceRequest.AlternatePixels.IsEmpty())
		{
			FResourcePixelHistoryRequest NextRequest = TraceRequest;
			NextRequest.Pixel = NextRequest.AlternatePixels[0];
			NextRequest.AlternatePixels.RemoveAt(0);
			++NextRequest.AdaptiveAttempt;
			NextRequest.TraceKey = FString::Printf(TEXT("%u:%d:%d:%d:%d:%d:%d:%d"),
				NextRequest.ConsumerEventId, NextRequest.ResourceIndex, NextRequest.Pixel.X, NextRequest.Pixel.Y,
				NextRequest.Mip, NextRequest.Slice, NextRequest.Sample, NextRequest.TypeCast);
			NextRequest.ReplayKey = BuildReplayPixelHistoryKey(NextRequest.ResourceIndex, NextRequest.Pixel,
				NextRequest.Mip, NextRequest.Slice, NextRequest.Sample, NextRequest.TypeCast);
			NextRequest.Priority = ComputeResourceTracePriority(NextRequest);
			Evidence->SetStringField(TEXT("adaptiveNextTraceKey"), NextRequest.TraceKey);
			Evidence->SetNumberField(TEXT("adaptiveNextX"), NextRequest.Pixel.X);
			Evidence->SetNumberField(TEXT("adaptiveNextY"), NextRequest.Pixel.Y);
			ScheduleResourcePixelHistory(*Context, NextRequest);
			bAdaptiveContinuation = ScheduledResourcePixelHistoryKeys.Contains(NextRequest.TraceKey)
				|| BudgetDeferredResourcePixelHistoryRequests.Contains(NextRequest.TraceKey);
		}

		if (bGeometryResetBoundary)
		{
			Evidence->SetStringField(TEXT("branchStatus"), TEXT("geometry-reset-boundary"));
			Evidence->SetStringField(TEXT("detail"),
				TEXT("A depth/visibility clear resets ownership at this coordinate; it is not promoted to a Mesh writer."));
		}
		else if (bWriterExpanded)
		{
			Evidence->SetStringField(TEXT("branchStatus"), TEXT("continued-to-dominating-writer"));
			Evidence->SetStringField(TEXT("detail"), FString::Printf(
				TEXT("%d visible pixel writers retained; one purpose-specific dominating writer expanded. %d executed accesses were collapsed into this resource query."),
				ConfirmedWriterCount, TraceRequest.CollapsedShaderAccessCount));
		}
		else if (bAdaptiveContinuation)
		{
			Evidence->SetStringField(TEXT("branchStatus"), TEXT("adaptive-footprint-continued"));
			Evidence->SetStringField(TEXT("detail"), FString::Printf(
				TEXT("No writer was found at adaptive candidate %d/%d; the next representative footprint coordinate was queued."),
				TraceRequest.AdaptiveAttempt + 1, TraceRequest.TotalAdaptiveCandidates));
		}
		else
		{
			Evidence->SetStringField(TEXT("branchStatus"), TEXT("no-modification-before-consumer"));
		}
		EnrichResourcePixelHistoryFromShaderTrace(*Context, Evidence);
		Context->ResourcePixelHistories.Add(MakeShared<FJsonValueObject>(Evidence));
		Diagnostics.WriteRecord(TEXT("trace_branch_completed"), FString::Printf(
			TEXT("request=%s key=%s required=%s queueToResponse=%.3fs events=%d writers=%d expanded=%d pendingRequired=%d pendingBackground=%d"),
			*RequestId, *TraceRequest.TraceKey, TraceRequest.bRequiredForAgent ? TEXT("true") : TEXT("false"),
			QueueToResponseSeconds, static_cast<int32>(TotalEvents), ConfirmedWriterCount, ExpandedWriterCount,
			GetPendingRequiredResourceHistoryCount(), GetPendingBackgroundResourceHistoryCount()));
		RenderCausalReport();
		TryResumeAgentAfterDeterministicContexts();
	}

	void SAnalyzerHome::StoreShaderDebug(const TSharedRef<FJsonObject>& Message)
	{
		FString RequestId;
		Message->TryGetStringField(TEXT("requestId"), RequestId);
		const uint32 EventId = static_cast<uint32>(Message->GetNumberField(TEXT("eventId")));
		const uint32* RequestedEventId = PendingShaderDebugByRequest.Find(RequestId);
		if (!RequestedEventId || *RequestedEventId != EventId)
		{
			return;
		}
		AppendFullTraceRecord(TEXT("worker-response"), Message);
		CompleteWorkerRequest(RequestId, TEXT("shader_debug"), FString::Printf(TEXT("event=%u"), EventId));
		PendingShaderDebugByRequest.Remove(RequestId);
		FEventContextEvidence& Context = EventContexts.FindOrAdd(EventId);
		Context.EventId = EventId;
		Context.ShaderDebugTrace = Message;
		const bool bSceneSource = IsSceneSourceEvent(Context.ActionKind, Context.MarkerPath);
		const int32 ScheduledExecutedBranches = bSceneSource ? 0 : ScheduleExecutedTextureAccessHistories(Context);
		if (ScheduledExecutedBranches == 0 && !bSceneSource)
		{
			ScheduleResourcePixelHistories(Context);
		}
		for (const TSharedPtr<FJsonValue>& HistoryValue : Context.ResourcePixelHistories)
		{
			const TSharedPtr<FJsonObject> History = HistoryValue.IsValid() ? HistoryValue->AsObject() : nullptr;
			if (History.IsValid())
			{
				EnrichResourcePixelHistoryFromShaderTrace(Context, History.ToSharedRef());
			}
		}
		FailedShaderDebugIds.Remove(EventId);
		Diagnostics.WriteRecord(TEXT("shader_debug_completed"), FString::Printf(
			TEXT("request=%s event=%u executedBranches=%d histories=%d"),
			*RequestId, EventId, ScheduledExecutedBranches, Context.ResourcePixelHistories.Num()));
		RenderCausalReport();
		SetStatus(FString::Printf(TEXT("EID %u 的 Pixel Shader 指令追踪已加载；详细证据已更新。"), EventId));
		TryResumeAgentAfterDeterministicContexts();
	}

	void SAnalyzerHome::EnsureEventContext(uint32 EventId, int32 ReverseDepth)
	{
		if (!bWorkerReady || FailedEventContextIds.Contains(EventId))
		{
			return;
		}
		if (int32* ExistingDepth = EventContextDepths.Find(EventId))
		{
			*ExistingDepth = FMath::Min(*ExistingDepth, ReverseDepth);
		}
		else
		{
			EventContextDepths.Add(EventId, ReverseDepth);
		}
		if (EventContexts.Contains(EventId) || PendingEventContextIds.Contains(EventId)
			|| DeferredEventContextIds.Contains(EventId))
		{
			BudgetDeferredEventContextDepths.Remove(EventId);
			return;
		}
		if (EventContexts.Num() + PendingEventContextIds.Num() + DeferredEventContextIds.Num()
			>= DeterministicContextLimit)
		{
			const bool bNewBudgetBoundary = !BudgetDeferredEventContextDepths.Contains(EventId);
			int32& DeferredDepth = BudgetDeferredEventContextDepths.FindOrAdd(EventId, ReverseDepth);
			DeferredDepth = FMath::Min(DeferredDepth, ReverseDepth);
			if (bNewBudgetBoundary)
			{
				Diagnostics.WriteRecord(TEXT("event_context_budget_deferred"), FString::Printf(
					TEXT("event=%u depth=%d reason=context-limit limit=%d deferred=%d"),
					EventId, ReverseDepth, DeterministicContextLimit, BudgetDeferredEventContextDepths.Num()));
			}
			return;
		}
		BudgetDeferredEventContextDepths.Remove(EventId);
		const FString RequestId = FString::Printf(TEXT("context-%u-query-%llu"), EventId, ++RequestSerial);
		PendingEventContextByRequest.Add(RequestId, EventId);
		PendingEventContextIds.Add(EventId);
		Diagnostics.WriteRecord(TEXT("event_context_queued"), FString::Printf(
			TEXT("request=%s event=%u depth=%d critical=%s pending=%d"),
			*RequestId, EventId, ReverseDepth, IsCriticalAgentEvent(EventId) ? TEXT("true") : TEXT("false"),
			PendingEventContextIds.Num()));
		const int32 ContextPriority = 800 - FMath::Clamp(ReverseDepth, 0, MaxCausalGraphHops) * 10;
		if (!SendWorkerRequest(TEXT("event_context"), RequestId,
			[EventId](const TSharedRef<FJsonObject>& Request)
			{
				Request->SetNumberField(TEXT("eventId"), EventId);
			}, ContextPriority))
		{
			PendingEventContextByRequest.Remove(RequestId);
			PendingEventContextIds.Remove(EventId);
			FailedEventContextIds.Add(EventId);
		}
	}

	void SAnalyzerHome::ScheduleProducerEventContexts(const FEventContextEvidence& Context)
	{
		const int32 ConsumerDepth = EventContextDepths.FindRef(Context.EventId);
		if (ConsumerDepth >= MaxCausalGraphHops - 1)
		{
			return;
		}

		struct FProducerContextCandidate
		{
			uint32 EventId = 0;
			int32 Score = 0;
			int32 OriginalIndex = INDEX_NONE;
			FString ResourceName;
		};
		TArray<FProducerContextCandidate> Candidates;
		for (int32 ProvenanceIndex = 0; ProvenanceIndex < Context.ResourceProvenance.Num(); ++ProvenanceIndex)
		{
			const TSharedPtr<FJsonValue>& ProvenanceValue = Context.ResourceProvenance[ProvenanceIndex];
			const TSharedPtr<FJsonObject> Provenance = ProvenanceValue.IsValid() ? ProvenanceValue->AsObject() : nullptr;
			if (!Provenance.IsValid())
			{
				continue;
			}
			bool bProducerFound = false;
			FString ProducerStatus;
			FString ShaderBinding;
			FString ResourceName;
			FString ProducerMarker;
			double ResourceIndex = INDEX_NONE;
			double ProducerEventId = 0.0;
			Provenance->TryGetBoolField(TEXT("producerFound"), bProducerFound);
			Provenance->TryGetStringField(TEXT("producerStatus"), ProducerStatus);
			Provenance->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
			Provenance->TryGetStringField(TEXT("resource"), ResourceName);
			Provenance->TryGetStringField(TEXT("producerMarkerPath"), ProducerMarker);
			Provenance->TryGetNumberField(TEXT("resourceIndex"), ResourceIndex);
			Provenance->TryGetNumberField(TEXT("producerEventId"), ProducerEventId);
			if (!bProducerFound || ProducerStatus != TEXT("confirmed-resource-write") || ProducerEventId <= 0.0)
			{
				continue;
			}
			bool bPixelBranchPending = false;
			for (const TPair<FString, FResourcePixelHistoryRequest>& Pair : PendingResourcePixelHistoryByRequest)
			{
				if (Pair.Value.ConsumerEventId == Context.EventId
					&& Pair.Value.ResourceIndex == static_cast<int32>(ResourceIndex)
					&& (ShaderBinding.IsEmpty() || Pair.Value.ShaderBinding == ShaderBinding))
				{
					bPixelBranchPending = true;
					break;
				}
			}
			const bool bPixelBranchRecorded = Context.ResourcePixelHistories.ContainsByPredicate(
				[ResourceIndex, &ShaderBinding](const TSharedPtr<FJsonValue>& Value)
				{
					const TSharedPtr<FJsonObject> History = Value.IsValid() ? Value->AsObject() : nullptr;
					if (!History.IsValid())
					{
						return false;
					}
					double HistoryResourceIndex = INDEX_NONE;
					FString HistoryBinding;
					History->TryGetNumberField(TEXT("resourceIndex"), HistoryResourceIndex);
					History->TryGetStringField(TEXT("shaderBinding"), HistoryBinding);
					return static_cast<int32>(HistoryResourceIndex) == static_cast<int32>(ResourceIndex)
						&& (ShaderBinding.IsEmpty() || HistoryBinding == ShaderBinding);
				});
			if (bPixelBranchPending || bPixelBranchRecorded)
			{
				continue;
			}
			const uint32 EventId = static_cast<uint32>(ProducerEventId);
			if (EventId == Context.EventId)
			{
				continue;
			}
			FProducerContextCandidate Candidate;
			Candidate.EventId = EventId;
			Candidate.OriginalIndex = ProvenanceIndex;
			Candidate.ResourceName = ResourceName;
			const FString Searchable = ResourceName + TEXT(" ") + ShaderBinding + TEXT(" ") + ProducerMarker;
			Candidate.Score += Searchable.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT(" SM_"), ESearchCase::IgnoreCase) ? 700 : 0;
			Candidate.Score += Searchable.Contains(TEXT("BasePass"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("PrePass"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase) ? 650 : 0;
			Candidate.Score += Searchable.Contains(TEXT("GPUScene.Primitive"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("GPUScene.Instance"), ESearchCase::IgnoreCase) ? 600 : 0;
			Candidate.Score += Searchable.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase) ? 580 : 0;
			Candidate.Score += Searchable.Contains(TEXT("Nanite"), ESearchCase::IgnoreCase) ? 520 : 0;
			Candidate.Score += Searchable.Contains(TEXT("SceneDepth"), ESearchCase::IgnoreCase)
				|| Searchable.Contains(TEXT("SceneColor"), ESearchCase::IgnoreCase) ? 480 : 0;
			Candidate.Score += Searchable.Contains(TEXT("Material"), ESearchCase::IgnoreCase) ? 420 : 0;
			Candidate.Score -= ProvenanceIndex;
			const int32 ExistingIndex = Candidates.IndexOfByPredicate([EventId](const FProducerContextCandidate& Existing)
			{
				return Existing.EventId == EventId;
			});
			if (ExistingIndex == INDEX_NONE)
			{
				Candidates.Add(MoveTemp(Candidate));
			}
			else if (Candidate.Score > Candidates[ExistingIndex].Score)
			{
				Candidates[ExistingIndex] = MoveTemp(Candidate);
			}
		}

		Candidates.Sort([](const FProducerContextCandidate& A, const FProducerContextCandidate& B)
		{
			return A.Score == B.Score ? A.OriginalIndex < B.OriginalIndex : A.Score > B.Score;
		});
		const int32 ScheduledCount = FMath::Min(Candidates.Num(), MaxRecursiveProducerContextsPerEvent);
		for (int32 CandidateIndex = 0; CandidateIndex < ScheduledCount; ++CandidateIndex)
		{
			const FProducerContextCandidate& Candidate = Candidates[CandidateIndex];
			EnsureEventContext(Candidate.EventId, ConsumerDepth + 1);
			Diagnostics.WriteRecord(TEXT("producer_context_coverage_selected"), FString::Printf(
				TEXT("consumer=%u producer=%u depth=%d score=%d resource=%s rank=%d/%d candidates=%d"),
				Context.EventId, Candidate.EventId, ConsumerDepth + 1, Candidate.Score,
				*Candidate.ResourceName, CandidateIndex + 1, ScheduledCount, Candidates.Num()));
		}
		for (int32 CandidateIndex = ScheduledCount; CandidateIndex < Candidates.Num(); ++CandidateIndex)
		{
			const FProducerContextCandidate& Candidate = Candidates[CandidateIndex];
			if (EventContexts.Contains(Candidate.EventId) || PendingEventContextIds.Contains(Candidate.EventId)
				|| FailedEventContextIds.Contains(Candidate.EventId))
			{
				BudgetDeferredEventContextDepths.Remove(Candidate.EventId);
				continue;
			}
			int32& KnownDepth = EventContextDepths.FindOrAdd(Candidate.EventId, ConsumerDepth + 1);
			KnownDepth = FMath::Min(KnownDepth, ConsumerDepth + 1);
			int32& DeferredDepth = BudgetDeferredEventContextDepths.FindOrAdd(
				Candidate.EventId, ConsumerDepth + 1);
			DeferredDepth = FMath::Min(DeferredDepth, ConsumerDepth + 1);
			Diagnostics.WriteRecord(TEXT("producer_context_coverage_deferred"), FString::Printf(
				TEXT("consumer=%u producer=%u depth=%d score=%d resource=%s reason=per-consumer-limit limit=%d candidates=%d"),
				Context.EventId, Candidate.EventId, ConsumerDepth + 1, Candidate.Score,
				*Candidate.ResourceName, MaxRecursiveProducerContextsPerEvent, Candidates.Num()));
		}
	}

	void SAnalyzerHome::EnsureRelevantEventContexts()
	{
		if (!bWorkerReady)
		{
			return;
		}

		TArray<uint32> RelevantEventIds;
		for (const FPixelSample& Sample : Samples)
		{
			const int32 DominatingWriterIndex = SelectDominatingWriterSummaryIndex(
				Sample.EventSummaries, 0, TEXT("color"));
			if (Sample.EventSummaries.IsValidIndex(DominatingWriterIndex))
			{
				const FEventSummaryEvidence& Event = Sample.EventSummaries[DominatingWriterIndex];
				if (Event.ActionKind == TEXT("present"))
				{
					continue;
				}
				RelevantEventIds.AddUnique(Event.EventId);
				FocusedTraceEventIds.Add(Event.EventId);
				EventTracePixels.Add(Event.EventId, Sample.Pixel);
				if (Event.bHasPrimitiveEvidence)
				{
					EventTracePrimitiveIds.Add(Event.EventId, Event.PrimitiveId);
					EventTracePrimitiveEvidenceIds.Add(Event.EventId);
				}
			}
		}

		for (const uint32 EventId : RelevantEventIds)
		{
			EnsureEventContext(EventId);
		}
	}

	void SAnalyzerHome::EnsureCandidateShaderDebug()
	{
		if (!bShaderDebuggingAvailable || !bWorkerReady)
		{
			return;
		}
		for (const uint32 EventId : FocusedTraceEventIds)
		{
			QueueFocusedShaderDebug(EventId);
		}
	}

	void SAnalyzerHome::TryResumeAgentAfterDeterministicContexts()
	{
		if (bAgentWaitingForDeterministicContexts && !HasPendingCriticalDeterministicQueries())
		{
			bAgentWaitingForDeterministicContexts = false;
			StartAgentAnalysis();
		}
	}

	void SAnalyzerHome::StorePixelHistory(const TSharedRef<FJsonObject>& Message)
	{
		FString RequestId;
		Message->TryGetStringField(TEXT("requestId"), RequestId);
		const uint64* SampleId = PendingSampleByRequest.Find(RequestId);
		if (!SampleId)
		{
			return;
		}
		FPixelSample* Sample = FindSample(*SampleId);
		if (Sample)
		{
			FullTraceTargetPixelHistory = Message;
			AppendFullTraceRecord(TEXT("worker-response"), Message);
		}
		CompleteWorkerRequest(RequestId, TEXT("target_pixel_history"));
		PendingSampleByRequest.Remove(RequestId);
		if (!Sample)
		{
			return;
		}

		Sample->bPending = false;
		Sample->bFailed = false;
		Sample->bAnalyzed = true;
		Sample->TotalModifications = static_cast<int32>(Message->GetNumberField(TEXT("totalModifications")));
		Sample->bTruncated = Message->GetBoolField(TEXT("truncated"));
		Sample->bEventSummaryComplete = false;
		Sample->EventSummaries.Empty();
		Sample->Modifications.Empty();
		const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
		double TotalEvents = 0.0;
		if (Message->TryGetArrayField(TEXT("eventSummaries"), EventSummaries)
			&& EventSummaries && Message->TryGetNumberField(TEXT("totalEvents"), TotalEvents))
		{
			Sample->bEventSummaryComplete = static_cast<int32>(TotalEvents) == EventSummaries->Num();
			for (const TSharedPtr<FJsonValue>& JsonValue : *EventSummaries)
			{
				const TSharedPtr<FJsonObject> Summary = JsonValue.IsValid() ? JsonValue->AsObject() : nullptr;
				if (!Summary.IsValid())
				{
					Sample->bEventSummaryComplete = false;
					continue;
				}
				FEventSummaryEvidence Evidence;
				Evidence.EventId = static_cast<uint32>(Summary->GetNumberField(TEXT("eventId")));
				Summary->TryGetStringField(TEXT("action"), Evidence.Action);
				Summary->TryGetStringField(TEXT("actionKind"), Evidence.ActionKind);
				Summary->TryGetStringField(TEXT("markerPath"), Evidence.MarkerPath);
				if (Evidence.Action.IsEmpty())
				{
					Evidence.Action = TEXT("Unnamed action");
				}
				if (Evidence.ActionKind.IsEmpty())
				{
					Evidence.ActionKind = TEXT("other");
				}
				Evidence.ActionFlags = static_cast<uint32>(Summary->GetNumberField(TEXT("actionFlags")));
				Evidence.PassedFragments = static_cast<int32>(Summary->GetNumberField(TEXT("passedFragments")));
				Evidence.RejectedFragments = static_cast<int32>(Summary->GetNumberField(TEXT("rejectedFragments")));
				Summary->TryGetBoolField(TEXT("directShaderWrite"), Evidence.bDirectShaderWrite);
				Summary->TryGetBoolField(TEXT("unboundPixelShader"), Evidence.bUnboundPixelShader);
				Summary->TryGetBoolField(TEXT("changedTextureValue"), Evidence.bChangedTextureValue);
				Summary->TryGetBoolField(TEXT("hasPrimitiveEvidence"), Evidence.bHasPrimitiveEvidence);
				Evidence.PrimitiveId = static_cast<uint32>(Summary->GetNumberField(TEXT("lastPrimitiveId")));
				for (const TSharedPtr<FJsonValue>& Failure : Summary->GetArrayField(TEXT("failureReasons")))
				{
					Evidence.FailureReasons.AddUnique(Failure->AsString());
				}
				Evidence.BeforeValue = ParsePixelValue(Summary->GetObjectField(TEXT("firstBefore")));
				Evidence.ShaderOutputValue = ParsePixelValue(Summary->GetObjectField(TEXT("lastShaderOutput")));
				Evidence.AfterValue = ParsePixelValue(Summary->GetObjectField(TEXT("lastAfter")));
				Evidence.Before = Evidence.BeforeValue.Text;
				Evidence.ShaderOutput = Evidence.ShaderOutputValue.Text;
				Evidence.After = Evidence.AfterValue.Text;
				Sample->EventSummaries.Add(MoveTemp(Evidence));
			}
		}
		for (const TSharedPtr<FJsonValue>& JsonValue : Message->GetArrayField(TEXT("modifications")))
		{
			const TSharedPtr<FJsonObject> Modification = JsonValue->AsObject();
			if (!Modification.IsValid())
			{
				continue;
			}
			FPixelModificationEvidence Evidence;
			Evidence.EventId = static_cast<uint32>(Modification->GetNumberField(TEXT("eventId")));
			Modification->TryGetStringField(TEXT("action"), Evidence.Action);
			Modification->TryGetStringField(TEXT("actionKind"), Evidence.ActionKind);
			Modification->TryGetStringField(TEXT("markerPath"), Evidence.MarkerPath);
			Evidence.ActionFlags = static_cast<uint32>(Modification->GetNumberField(TEXT("actionFlags")));
			Modification->TryGetBoolField(TEXT("passed"), Evidence.bPassed);
			Modification->TryGetBoolField(TEXT("directShaderWrite"), Evidence.bDirectShaderWrite);
			Modification->TryGetBoolField(TEXT("unboundPixelShader"), Evidence.bUnboundPixelShader);
			Modification->TryGetBoolField(TEXT("changedTextureValue"), Evidence.bChangedTextureValue);
			Evidence.PrimitiveId = static_cast<uint32>(Modification->GetNumberField(TEXT("primitiveId")));
			Evidence.FragmentIndex = static_cast<uint32>(Modification->GetNumberField(TEXT("fragmentIndex")));
			for (const TSharedPtr<FJsonValue>& Failure : Modification->GetArrayField(TEXT("failureReasons")))
			{
				Evidence.FailureReasons.Add(Failure->AsString());
			}
			Evidence.BeforeValue = ParsePixelValue(Modification->GetObjectField(TEXT("before")));
			Evidence.ShaderOutputValue = ParsePixelValue(Modification->GetObjectField(TEXT("shaderOutput")));
			Evidence.AfterValue = ParsePixelValue(Modification->GetObjectField(TEXT("after")));
			Evidence.Before = Evidence.BeforeValue.Text;
			Evidence.ShaderOutput = Evidence.ShaderOutputValue.Text;
			Evidence.After = Evidence.AfterValue.Text;
			if (Evidence.Action.IsEmpty())
			{
				Evidence.Action = TEXT("Unnamed action");
			}
			if (Evidence.ActionKind.IsEmpty())
			{
				Evidence.ActionKind = TEXT("other");
			}
			Sample->Modifications.Add(MoveTemp(Evidence));
		}
		UpdateSelectionText();
		RenderCausalReport();
		if (!bAgentRunning)
		{
			SetAgentStatus(HasPendingWorkerRequests()
				? TEXT("目标 Pixel History 已返回 · 正在自动深追资源/sample 与 producer 上下文")
				: TEXT("自动深追已收束 · 可以围绕当前 P1 向 Agent 提问"));
		}
		const uint64 CompletedSampleId = Sample->Id;
		const int32 SampleIndex = Samples.IndexOfByPredicate([CompletedSampleId](const FPixelSample& Item) { return Item.Id == CompletedSampleId; });
		SetStatus(FString::Printf(TEXT("关注点 P%d (%d, %d)：%d 个 RenderDoc modification；正在自动深追完整资源链。"),
			SampleIndex + 1, Sample->Pixel.X, Sample->Pixel.Y, Sample->TotalModifications));
	}
}
