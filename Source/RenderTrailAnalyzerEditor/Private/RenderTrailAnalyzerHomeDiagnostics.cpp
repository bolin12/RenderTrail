#include "RenderTrailAnalyzerHomeInternal.h"

namespace UE::RenderTrail::Private
{
	FString SAnalyzerHome::GetCapturePath() const
	{
		return CapturePathBox.IsValid() ? CapturePathBox->GetText().ToString() : FString();
	}

	void SAnalyzerHome::SetStatus(const FString& Value)
	{
		CurrentStatus = Value;
		RefreshLiveReplayLogView();
	}

	FString SAnalyzerHome::BuildLiveReplayLogDisplay() const
	{
		FString Display = LiveReplayLog;
		if (!Display.IsEmpty() && !Display.EndsWith(TEXT("\n")))
		{
			Display += TEXT("\n");
		}
		Display += FString::Printf(TEXT("[当前状态] %s"), CurrentStatus.IsEmpty() ? TEXT("就绪。") : *CurrentStatus);
		return Display;
	}

	void SAnalyzerHome::RefreshLiveReplayLogView()
	{
		if (!StatusText.IsValid() || bLiveReplayLogPaused)
		{
			return;
		}
		StatusText->SetText(FText::FromString(BuildLiveReplayLogDisplay()));
		StatusText->ScrollTo(ETextLocation::EndOfDocument);
	}

	void SAnalyzerHome::AppendLiveReplayLog(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return;
		}
		if (!LiveReplayLog.IsEmpty() && !LiveReplayLog.EndsWith(TEXT("\n")) && !Text.StartsWith(TEXT("\n")))
		{
			LiveReplayLog += TEXT("\n");
		}
		LiveReplayLog += Text;
		if (LiveReplayLog.Len() > MaxLiveReplayLogChars)
		{
			const int32 MinimumCut = LiveReplayLog.Len() - MaxLiveReplayLogChars;
			int32 Cut = LiveReplayLog.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, MinimumCut);
			Cut = Cut == INDEX_NONE ? MinimumCut : Cut + 1;
			LiveReplayLog.RightChopInline(Cut, EAllowShrinking::No);
			LiveReplayLog = TEXT("[UI] 更早的实时日志已从界面截断；完整内容仍保存在日志文件中。\n") + LiveReplayLog;
		}
		RefreshLiveReplayLogView();
	}

	void SAnalyzerHome::ResetLiveReplayLog(const FString& Capture, int64 CaptureSize)
	{
		LiveReplayLog.Empty();
		WorkerDiagnosticsCharsRead = 0;
		RenderDocDiagnosticsCharsRead = 0;
		LastLiveReplayLogPollSeconds = 0.0;
		WorkerDiagnosticsTailPath = Diagnostics.GetWorkerDiagnosticsFilePath();
		RenderDocDiagnosticsTailPath = WorkerDiagnosticsTailPath.IsEmpty()
			? FString() : FPaths::ChangeExtension(WorkerDiagnosticsTailPath, TEXT("renderdoc.live.log"));
		AppendLiveReplayLog(FString::Printf(
			TEXT("[%s] [UI] Replay 会话开始\ncapture=%s\nbytes=%lld\nworkerLog=%s\nrenderDocLiveLog=%s\n\n"),
			*FDateTime::Now().ToIso8601(), *Capture, CaptureSize,
			WorkerDiagnosticsTailPath.IsEmpty() ? TEXT("disabled") : *WorkerDiagnosticsTailPath,
			RenderDocDiagnosticsTailPath.IsEmpty() ? TEXT("disabled") : *RenderDocDiagnosticsTailPath));
		if (CaptureSize >= 1024LL * 1024LL * 1024LL)
		{
			AppendLiveReplayLog(TEXT("[UI 警告] RDC 超过 1 GiB。完整 OpenCapture 会在 Worker 中重新创建整帧资源；若 Unreal Editor 仍占用同一 GPU，可能发生显存超额、shared-memory 分页和极慢的 GPU fence 等待。\n\n"));
		}
	}

	void SAnalyzerHome::TailLiveReplayLogFile(const FString& Path, int32& InOutCharsRead)
	{
		if (Path.IsEmpty())
		{
			return;
		}
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *Path))
		{
			return;
		}
		if (Contents.Len() < InOutCharsRead)
		{
			InOutCharsRead = 0;
			AppendLiveReplayLog(FString::Printf(TEXT("[%s] [UI] 日志文件已重置：%s\n"),
				*FDateTime::Now().ToIso8601(), *Path));
		}
		if (Contents.Len() > InOutCharsRead)
		{
			const FString Delta = Contents.Mid(InOutCharsRead);
			InOutCharsRead = Contents.Len();
			AppendLiveReplayLog(Delta);
		}
	}

	void SAnalyzerHome::PollLiveReplayLogs(double Now)
	{
		if (Now - LastLiveReplayLogPollSeconds < 0.75)
		{
			return;
		}
		TailLiveReplayLogFile(WorkerDiagnosticsTailPath, WorkerDiagnosticsCharsRead);
		TailLiveReplayLogFile(RenderDocDiagnosticsTailPath, RenderDocDiagnosticsCharsRead);
		LastLiveReplayLogPollSeconds = Now;
	}

	FReply SAnalyzerHome::CopyStatusToClipboard()
	{
		const FString Value = BuildLiveReplayLogDisplay();
		if (!Value.IsEmpty())
		{
			FPlatformApplicationMisc::ClipboardCopy(*Value);
		}
		return FReply::Handled();
	}

	FReply SAnalyzerHome::ToggleLiveReplayLog()
	{
		bLiveReplayLogPaused = !bLiveReplayLogPaused;
		if (!bLiveReplayLogPaused)
		{
			RefreshLiveReplayLogView();
		}
		return FReply::Handled();
	}

	FReply SAnalyzerHome::OpenDiagnosticsDirectory()
	{
		const FString Directory = WorkerDiagnosticsTailPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectLogDir(), TEXT("RenderTrailDiagnostics"))
			: FPaths::GetPath(WorkerDiagnosticsTailPath);
		IFileManager::Get().MakeDirectory(*Directory, true);
		FPlatformProcess::ExploreFolder(*Directory);
		return FReply::Handled();
	}

	FReply SAnalyzerHome::OpenFullTraceDirectory()
	{
		if (FullTraceDirectory.IsEmpty())
		{
			SetStatus(TEXT("请先分析一个像素，以生成全量追踪文件。"));
			return FReply::Handled();
		}
		IFileManager::Get().MakeDirectory(*FullTraceDirectory, true);
		FPlatformProcess::ExploreFolder(*FullTraceDirectory);
		return FReply::Handled();
	}

	FReply SAnalyzerHome::ReleaseReplayResources()
	{
		if (!bWorkerReady)
		{
			SetStatus(TEXT("Replay 当前未驻留，无需释放。"));
			return FReply::Handled();
		}
		if (HasPendingWorkerRequests() || bAgentRunning)
		{
			SetStatus(TEXT("仍有 Replay 查询或 Agent 请求进行中；完成后再释放，避免留下半条证据链。"));
			return FReply::Handled();
		}

		const FString ReleaseDetail = FString::Printf(
			TEXT("submittedResources=%d discoveredResources=%d budgetDeferredResources=%d contexts=%d budgetDeferredContexts=%d reportChars=%d"),
			ResourcePixelHistoryQueriesSubmitted, GetDiscoveredResourceHistoryCount(),
			BudgetDeferredResourcePixelHistoryRequests.Num(), EventContexts.Num(),
			BudgetDeferredEventContextDepths.Num(), LastReportSummary.Len() + LastReportCausalPath.Len());
		Diagnostics.WriteRecord(TEXT("manual_replay_release_begin"), ReleaseDetail);
		StopWorker();
		bReplaySynchronizationPending = false;
		bQueuePixelHistoryAfterWorkerReady = false;
		bReplayStartDeferred = PreviewBrush.IsValid();
		bPreviewReadyForSelection = PreviewBrush.IsValid();
		Diagnostics.WriteRecord(TEXT("manual_replay_release_end"), FString::Printf(
			TEXT("previewRetained=%s reportRetained=true"), PreviewBrush.IsValid() ? TEXT("true") : TEXT("false")));
		AppendLiveReplayLog(FString::Printf(
			TEXT("[%s] [UI] Replay Worker 已释放；预览、像素证据与报告仍保留。\n"),
			*FDateTime::Now().ToIso8601()));
		SetStatus(TEXT("Replay GPU/内存资源已释放；当前报告仍可阅读和询问。新查询会按需重新打开 Replay。"));
		SetAgentStatus(TEXT("Replay 已释放 · 当前已收集证据仍可用于 Agent；重新分析像素时会自动完成深追。"));
		return FReply::Handled();
	}

	void SAnalyzerHome::SetCaptureLoadPhase(const FString& Phase)
	{
		CaptureLoadPhase = Phase;
		if (bCaptureLoading)
		{
			const double Elapsed = FPlatformTime::Seconds() - CaptureLoadStartSeconds;
			SetStatus(FString::Printf(TEXT("正在载入截帧… %.1fs · %s"), Elapsed, *CaptureLoadPhase));
			UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Capture load phase: %s (elapsed=%.3fs)"), *Phase, Elapsed);
		}
	}

	void SAnalyzerHome::FinishCaptureLoad(const FString& Result)
	{
		if (!bCaptureLoading)
		{
			return;
		}
		const double Elapsed = FPlatformTime::Seconds() - CaptureLoadStartSeconds;
		bCaptureLoading = false;
		UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Capture load finished: elapsed=%.3fs result=%s capture='%s'"),
			Elapsed, *Result, *GetCapturePath());
	}

	void SAnalyzerHome::WriteWorkerHeartbeat(double Now, const TCHAR* Activity)
	{
		const double Elapsed = bCaptureLoading ? Now - CaptureLoadStartSeconds : 0.0;
		const FString Phase = LastWorkerDiagnosticPhase.IsEmpty() ? CaptureLoadPhase : LastWorkerDiagnosticPhase;
		const FString Detail = FString::Printf(
			TEXT("activity=%s elapsed=%.3fs running=%s phase='%s' request='%s' requestElapsed=%.3fs analysisGeneration=%llu requestGeneration=%llu locallyQueued=%d pendingPixel=%d pendingContext=%d pendingShader=%d pendingResourceRequired=%d pendingResourceBackground=%d budgetDeferredResources=%d budgetDeferredContexts=%d"),
			Activity, Elapsed, ReplayWorker.IsRunning() ? TEXT("true") : TEXT("false"), *Phase,
			*ActiveWorkerRequestId, ActiveWorkerRequestStartSeconds > 0.0 ? Now - ActiveWorkerRequestStartSeconds : 0.0,
			AnalysisGeneration, DispatchedWorkerRequestGeneration, QueuedWorkerRequests.Num(),
			PendingSampleByRequest.Num(), PendingEventContextByRequest.Num(), PendingShaderDebugByRequest.Num(),
			GetPendingRequiredResourceHistoryCount(), GetPendingBackgroundResourceHistoryCount(),
			BudgetDeferredResourcePixelHistoryRequests.Num(), BudgetDeferredEventContextDepths.Num());
		UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Replay Worker heartbeat: %s"), *Detail);
		if (Diagnostics.HasSession() && Diagnostics.GetOptions().bWorkerProtocol)
		{
			Diagnostics.WriteRecord(TEXT("worker_wait"), Detail);
		}
		LastWorkerHeartbeatSeconds = Now;
	}

	void SAnalyzerHome::SetEvidence(const FString& Value)
	{
		SetReportCards(Value, TEXT("选择关注像素后生成。"), TEXT("尚无候选原因。"), Value);
	}

	void SAnalyzerHome::SetReportCards(const FString& Summary, const FString& CausalPath, const FString& Suspects,
		const FString& TechnicalEvidence)
	{
		LastReportSummary = Summary;
		LastReportCausalPath = CausalPath;
		if (AgentResultView.IsValid() && !bAgentResultDisplayed)
		{
			AgentResultView->SetDeterministicReport(Summary, CausalPath, Suspects);
		}
		if (EvidenceText.IsValid())
			EvidenceText->SetText(FText::FromString(TechnicalEvidence));
	}

	void SAnalyzerHome::SetAgentOutputText(const FString& Value)
	{
		if (AgentOutputText.IsValid())
		{
			AgentOutputText->SetText(FText::FromString(Value));
		}
	}

	void SAnalyzerHome::SetAgentStatus(const FString& Status)
	{
		if (AgentStatusText.IsValid())
		{
			AgentStatusText->SetText(FText::FromString(Status));
		}
	}

	FString SAnalyzerHome::SerializeJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
		FJsonSerializer::Serialize(Object, Writer);
		return Result;
	}

	FString SAnalyzerHome::SerializeJsonPretty(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Result);
		FJsonSerializer::Serialize(Object, Writer);
		return Result;
	}

	void SAnalyzerHome::InitializeFullTraceArtifact()
	{
		FullTraceRecords.Empty();
		FullTraceTargetPixelHistory.Reset();
		FullTraceRecordCount = 0;
		FullTraceRequestCount = 0;
		FullTraceResponseCount = 0;
		bFullTraceWriteFailed = false;
		FullTraceCreatedAt = FDateTime::Now().ToIso8601();
		FullTraceDirectory.Empty();
		FullTraceJsonlPath.Empty();
		FullTraceSnapshotPath.Empty();
		if (Samples.IsEmpty())
		{
			return;
		}

		FString CaptureName = FPaths::GetBaseFilename(GetCapturePath());
		if (CaptureName.IsEmpty())
		{
			CaptureName = TEXT("capture");
		}
		CaptureName.ReplaceInline(TEXT(" "), TEXT("_"));
		const FPixelSample& Sample = Samples[0];
		const FString SessionName = FString::Printf(TEXT("%s_P1_%d_%d_G%llu_%s"),
			*CaptureName, Sample.Pixel.X, Sample.Pixel.Y, AnalysisGeneration,
			*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		FullTraceDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("RenderTrailTraces"), SessionName));
		FullTraceJsonlPath = FPaths::Combine(FullTraceDirectory, TEXT("trace-records.jsonl"));
		FullTraceSnapshotPath = FPaths::Combine(FullTraceDirectory, TEXT("full-trace.json"));
		IFileManager::Get().MakeDirectory(*FullTraceDirectory, true);
		if (!FFileHelper::SaveStringToFile(TEXT(""), *FullTraceJsonlPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			bFullTraceWriteFailed = true;
		}
		WriteFullTraceSnapshot(TEXT("collecting"));
		Diagnostics.WriteRecord(TEXT("full_trace_artifact_started"), FString::Printf(
			TEXT("pixel=(%d,%d) generation=%llu jsonl=%s snapshot=%s"),
			Sample.Pixel.X, Sample.Pixel.Y, AnalysisGeneration,
			*FullTraceJsonlPath, *FullTraceSnapshotPath));
	}

	void SAnalyzerHome::AppendFullTraceRecord(const FString& Direction, const TSharedRef<FJsonObject>& Payload)
	{
		if (FullTraceJsonlPath.IsEmpty())
		{
			return;
		}
		TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetNumberField(TEXT("recordIndex"), ++FullTraceRecordCount);
		Record->SetStringField(TEXT("timestamp"), FDateTime::Now().ToIso8601());
		Record->SetStringField(TEXT("direction"), Direction);
		Record->SetNumberField(TEXT("analysisGeneration"), static_cast<double>(AnalysisGeneration));
		Record->SetObjectField(TEXT("payload"), Payload);
		FullTraceRecords.Add(MakeShared<FJsonValueObject>(Record));
		if (Direction == TEXT("analyzer-request"))
		{
			++FullTraceRequestCount;
		}
		else if (Direction == TEXT("worker-response"))
		{
			++FullTraceResponseCount;
		}

		const FString JsonLine = SerializeJson(Record) + LINE_TERMINATOR;
		if (!FFileHelper::SaveStringToFile(JsonLine, *FullTraceJsonlPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append)
			&& !bFullTraceWriteFailed)
		{
			bFullTraceWriteFailed = true;
			Diagnostics.WriteRecord(TEXT("full_trace_append_failed"), FullTraceJsonlPath);
		}
	}

	TSharedRef<FJsonObject> SAnalyzerHome::BuildFullBoundResourceJson(const FBoundResourceEvidence& Resource)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("resourceIndex"), Resource.ResourceIndex);
		Json->SetStringField(TEXT("name"), Resource.Name);
		Json->SetStringField(TEXT("format"), Resource.Format);
		Json->SetStringField(TEXT("stage"), Resource.Stage);
		Json->SetStringField(TEXT("access"), Resource.Access);
		Json->SetStringField(TEXT("shaderBinding"), Resource.ShaderBinding);
		Json->SetBoolField(TEXT("texture"), Resource.bTexture);
		Json->SetNumberField(TEXT("width"), Resource.Width);
		Json->SetNumberField(TEXT("height"), Resource.Height);
		Json->SetNumberField(TEXT("samples"), Resource.Samples);
		Json->SetNumberField(TEXT("bindingIndex"), Resource.BindingIndex);
		Json->SetNumberField(TEXT("arrayElement"), Resource.ArrayElement);
		Json->SetNumberField(TEXT("firstMip"), Resource.FirstMip);
		Json->SetNumberField(TEXT("firstSlice"), Resource.FirstSlice);
		Json->SetNumberField(TEXT("typeCast"), Resource.TypeCast);
		return Json;
	}

	TSharedRef<FJsonObject> SAnalyzerHome::BuildFullEventContextJson(const FEventContextEvidence& Context) const
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("eventId"), Context.EventId);
		Json->SetNumberField(TEXT("reverseDepth"), EventContextDepths.FindRef(Context.EventId));
		Json->SetStringField(TEXT("action"), Context.Action);
		Json->SetStringField(TEXT("actionKind"), Context.ActionKind);
		Json->SetStringField(TEXT("markerPath"), Context.MarkerPath);
		Json->SetStringField(TEXT("shaderStage"), Context.ShaderStage);
		Json->SetStringField(TEXT("shaderEntry"), Context.ShaderEntry);
		Json->SetStringField(TEXT("shaderDebugStatus"), Context.ShaderDebugStatus);
		Json->SetStringField(TEXT("shaderEncoding"), Context.ShaderEncoding);
		Json->SetBoolField(TEXT("shaderDebuggable"), Context.bShaderDebuggable);
		Json->SetBoolField(TEXT("sourceDebugInfo"), Context.bSourceDebugInfo);
		Json->SetStringField(TEXT("traceStopReason"), Context.TraceStopReason);
		Json->SetBoolField(TEXT("focusedTraceEvent"), FocusedTraceEventIds.Contains(Context.EventId));
		Json->SetNumberField(TEXT("shaderInputSignatureCount"), Context.ShaderInputSignatureCount);
		Json->SetNumberField(TEXT("shaderOutputSignatureCount"), Context.ShaderOutputSignatureCount);
		Json->SetNumberField(TEXT("shaderConstantBlockCount"), Context.ShaderConstantBlockCount);
		Json->SetNumberField(TEXT("shaderSamplerCount"), Context.ShaderSamplerCount);
		Json->SetNumberField(TEXT("shaderReadOnlyResourceCount"), Context.ShaderReadOnlyResourceCount);
		Json->SetNumberField(TEXT("shaderReadWriteResourceCount"), Context.ShaderReadWriteResourceCount);

		TArray<TSharedPtr<FJsonValue>> Inputs;
		Inputs.Reserve(Context.Inputs.Num());
		for (const FBoundResourceEvidence& Resource : Context.Inputs)
		{
			Inputs.Add(MakeShared<FJsonValueObject>(BuildFullBoundResourceJson(Resource)));
		}
		Json->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
		TArray<TSharedPtr<FJsonValue>> Outputs;
		Outputs.Reserve(Context.Outputs.Num());
		for (const FBoundResourceEvidence& Resource : Context.Outputs)
		{
			Outputs.Add(MakeShared<FJsonValueObject>(BuildFullBoundResourceJson(Resource)));
		}
		Json->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
		Json->SetArrayField(TEXT("resourceProvenance"), Context.ResourceProvenance);
		Json->SetArrayField(TEXT("resourcePixelHistories"), Context.ResourcePixelHistories);
		if (Context.PipelineState.IsValid())
		{
			Json->SetObjectField(TEXT("pipelineState"), Context.PipelineState);
		}
		if (Context.ShaderDebugTrace.IsValid())
		{
			Json->SetObjectField(TEXT("shaderDebugTrace"), Context.ShaderDebugTrace);
		}
		return Json;
	}

	void SAnalyzerHome::WriteFullTraceSnapshot(const FString& CompletionState)
	{
		if (FullTraceSnapshotPath.IsEmpty())
		{
			return;
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schemaVersion"), 2);
		Root->SetStringField(TEXT("completionState"), CompletionState);
		Root->SetStringField(TEXT("createdAt"), FullTraceCreatedAt);
		Root->SetStringField(TEXT("updatedAt"), FDateTime::Now().ToIso8601());
		Root->SetStringField(TEXT("capturePath"), GetCapturePath());
		Root->SetStringField(TEXT("recordsJsonlPath"), FullTraceJsonlPath);
		Root->SetStringField(TEXT("snapshotPath"), FullTraceSnapshotPath);
		Root->SetNumberField(TEXT("analysisGeneration"), static_cast<double>(AnalysisGeneration));
		if (!Samples.IsEmpty())
		{
			Root->SetNumberField(TEXT("pixelX"), Samples[0].Pixel.X);
			Root->SetNumberField(TEXT("pixelY"), Samples[0].Pixel.Y);
		}
		Root->SetNumberField(TEXT("recordCount"), FullTraceRecordCount);
		Root->SetNumberField(TEXT("requestCount"), FullTraceRequestCount);
		Root->SetNumberField(TEXT("responseCount"), FullTraceResponseCount);
		Root->SetNumberField(TEXT("eventContextCount"), EventContexts.Num());
		Root->SetNumberField(TEXT("eventContextSafetyLimit"), DeterministicContextLimit);
		Root->SetNumberField(TEXT("resourcePixelHistoryQueryCount"), ResourcePixelHistoryQueriesSubmitted);
		Root->SetNumberField(TEXT("resourcePixelHistorySafetyLimit"), ResourcePixelHistoryQueryLimit);
		Root->SetNumberField(TEXT("budgetDeferredResourceGroupCount"), GetBudgetDeferredResourceHistoryCount());
		Root->SetNumberField(TEXT("budgetDeferredResourceCandidateCount"),
			BudgetDeferredResourcePixelHistoryRequests.Num());
		const FAgentContextCoverageSelection AgentContextSelection = BuildAgentContextCoverageSelection();
		Root->SetNumberField(TEXT("agentEventContextsIncluded"),
			AgentContextSelection.DetailedEventIds.Num());
		Root->SetNumberField(TEXT("agentEventContextProjectionLimit"), MaxAgentDeterministicContexts);
		Root->SetStringField(TEXT("agentEventContextSelectionPolicy"), TEXT("causal-coverage"));
		TArray<TSharedPtr<FJsonValue>> AgentSelectedEventIds;
		for (const uint32 EventId : AgentContextSelection.DetailedEventIds)
		{
			AgentSelectedEventIds.Add(MakeShared<FJsonValueNumber>(EventId));
		}
		Root->SetArrayField(TEXT("agentSelectedEventContextIds"), MoveTemp(AgentSelectedEventIds));
		Root->SetBoolField(TEXT("agentProjectionIsFullTrace"), false);
		Root->SetBoolField(TEXT("rawRecordWriteFailed"), bFullTraceWriteFailed);
		if (FullTraceTargetPixelHistory.IsValid())
		{
			Root->SetObjectField(TEXT("targetPixelHistory"), FullTraceTargetPixelHistory);
		}

		TArray<uint32> ContextIds;
		EventContexts.GenerateKeyArray(ContextIds);
		ContextIds.Sort([this](uint32 A, uint32 B)
		{
			const int32 DepthA = EventContextDepths.FindRef(A);
			const int32 DepthB = EventContextDepths.FindRef(B);
			return DepthA == DepthB ? A < B : DepthA < DepthB;
		});
		TArray<TSharedPtr<FJsonValue>> FullEventContexts;
		FullEventContexts.Reserve(ContextIds.Num());
		for (const uint32 EventId : ContextIds)
		{
			FullEventContexts.Add(MakeShared<FJsonValueObject>(
				BuildFullEventContextJson(EventContexts.FindChecked(EventId))));
		}
		Root->SetArrayField(TEXT("eventContexts"), MoveTemp(FullEventContexts));

		auto SetSortedEventIds = [&Root](const TCHAR* Field, const TSet<uint32>& EventIds)
		{
			TArray<uint32> SortedIds = EventIds.Array();
			SortedIds.Sort();
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(SortedIds.Num());
			for (const uint32 EventId : SortedIds)
			{
				Values.Add(MakeShared<FJsonValueNumber>(EventId));
			}
			Root->SetArrayField(Field, MoveTemp(Values));
		};
		SetSortedEventIds(TEXT("pendingEventContextIds"), PendingEventContextIds);
		SetSortedEventIds(TEXT("deferredEventContextIds"), DeferredEventContextIds);
		SetSortedEventIds(TEXT("failedEventContextIds"), FailedEventContextIds);
		SetSortedEventIds(TEXT("failedShaderDebugEventIds"), FailedShaderDebugIds);

		TArray<uint32> BudgetDeferredEventIds;
		BudgetDeferredEventContextDepths.GenerateKeyArray(BudgetDeferredEventIds);
		BudgetDeferredEventIds.Sort();
		TArray<TSharedPtr<FJsonValue>> BudgetDeferredContexts;
		for (const uint32 EventId : BudgetDeferredEventIds)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("eventId"), EventId);
			Item->SetNumberField(TEXT("reverseDepth"), BudgetDeferredEventContextDepths.FindRef(EventId));
			BudgetDeferredContexts.Add(MakeShared<FJsonValueObject>(Item));
		}
		Root->SetArrayField(TEXT("budgetDeferredEventContexts"), MoveTemp(BudgetDeferredContexts));

		TArray<FString> DeferredTraceKeys;
		BudgetDeferredResourcePixelHistoryRequests.GenerateKeyArray(DeferredTraceKeys);
		DeferredTraceKeys.Sort();
		TArray<TSharedPtr<FJsonValue>> DeferredResourceBranches;
		for (const FString& TraceKey : DeferredTraceKeys)
		{
			const FResourcePixelHistoryRequest& Request =
				BudgetDeferredResourcePixelHistoryRequests.FindChecked(TraceKey);
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("traceKey"), TraceKey);
			Item->SetNumberField(TEXT("consumerEventId"), Request.ConsumerEventId);
			Item->SetNumberField(TEXT("resourceIndex"), Request.ResourceIndex);
			Item->SetStringField(TEXT("resourceName"), Request.ResourceName);
			Item->SetStringField(TEXT("shaderBinding"), Request.ShaderBinding);
			Item->SetStringField(TEXT("tracePurpose"), Request.TracePurpose);
			Item->SetStringField(TEXT("coordinateMapping"), Request.Mapping);
			Item->SetNumberField(TEXT("x"), Request.Pixel.X);
			Item->SetNumberField(TEXT("y"), Request.Pixel.Y);
			Item->SetNumberField(TEXT("sample"), Request.Sample);
			Item->SetNumberField(TEXT("collapsedShaderAccessCount"), Request.CollapsedShaderAccessCount);
			Item->SetNumberField(TEXT("adaptiveAttempt"), Request.AdaptiveAttempt);
			Item->SetNumberField(TEXT("adaptiveCandidateCount"), Request.TotalAdaptiveCandidates);
			DeferredResourceBranches.Add(MakeShared<FJsonValueObject>(Item));
		}
		Root->SetArrayField(TEXT("budgetDeferredResourcePixelHistories"), MoveTemp(DeferredResourceBranches));
		Root->SetArrayField(TEXT("records"), FullTraceRecords);

		if (!FFileHelper::SaveStringToFile(SerializeJsonPretty(Root), *FullTraceSnapshotPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			bFullTraceWriteFailed = true;
			Diagnostics.WriteRecord(TEXT("full_trace_snapshot_failed"), FullTraceSnapshotPath);
		}
	}
}
