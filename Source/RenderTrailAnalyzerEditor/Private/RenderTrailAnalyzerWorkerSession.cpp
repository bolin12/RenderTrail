#include "RenderTrailAnalyzerHomeInternal.h"

namespace UE::RenderTrail::Private
{
	FReply SAnalyzerHome::ClearSamples()
	{
		ResetSamples();
		const bool bPreviewReady = bWorkerReady || bPreviewReadyForSelection || bReplayStartDeferred;
		SetEvidence(bPreviewReady
			? TEXT("关注像素已清空。直接在画面选择一个需要解释的像素。")
			: TEXT("先载入 .rdc 截帧，再选择需要解释的像素。"));
		SetStatus(bPreviewReady ? TEXT("关注像素已清空，可以重新选择。") : TEXT("关注像素已清空。"));
		return FReply::Handled();
	}

	FReply SAnalyzerHome::ClearCurrentInfo()
	{
		CancelAgentRun();
		LastAgentQuestion.Empty();
		LastAgentAnswer.Empty();
		bAgentResultDisplayed = false;
		LastCandidate.Reset();
		LastSignificantCandidate.Reset();
		bLastCandidateHasDivergence = false;
		const bool bHasSelection = !Samples.IsEmpty();
		SetAgentOutputText(bHasSelection
			? TEXT("当前报告已清空。已选像素及其分析状态仍保留，未变化的点不会重复读取。")
			: TEXT("当前没有可显示的分析信息。"));
		SetAgentStatus(bHasSelection
			? TEXT("当前信息已清空 · 选点与已分析状态保留")
			: TEXT("当前信息已清空 · 等待选择像素"));
		SetReportCards(
			bHasSelection ? TEXT("当前信息已清空；选点状态仍保留。") : TEXT("当前没有分析信息。"),
			bHasSelection ? TEXT("已选 P 点\n↓\n等待重新运行") : TEXT("选择关注像素后生成。"),
			TEXT("已清空当前显示信息；已分析点不会因重复点击而再次查询。"),
			TEXT("报告显示已清空；Pixel History 缓存仍保留。"));
		UpdateSelectionText();
		return FReply::Handled();
	}

	FReply SAnalyzerHome::ConfirmPixelSelection()
	{
		if (Samples.IsEmpty())
		{
			SetStatus(TEXT("请先在预览中选择至少一个像素。"));
			return FReply::Handled();
		}
		if (!bWorkerReady)
		{
			if (bReplayStartDeferred && !Samples.IsEmpty())
			{
				bQueuePixelHistoryAfterWorkerReady = true;
				SetStatus(TEXT("正在按需打开 Replay Worker；打开后自动读取选中像素的 Pixel History…"));
				MarkReplaySynchronizationPending();
				StartWorker(true);
				return FReply::Handled();
			}
			if (bPreviewReadyForSelection && bCaptureLoading)
			{
				bQueuePixelHistoryAfterWorkerReady = true;
				SetStatus(TEXT("预览已就绪，Replay Worker 仍在后台加载；Pixel History 会在就绪后自动开始。"));
				MarkReplaySynchronizationPending();
				return FReply::Handled();
			}
			SetStatus(TEXT("Replay Worker is not ready."));
			return FReply::Handled();
		}
		bReplaySynchronizationPending = false;
		const bool bSelectionChanged = !bSelectionConfirmed;
		CancelAgentRun();
		bSelectionConfirmed = true;
		if (bSelectionChanged)
		{
			bDeterministicForegroundCompletionReported = false;
			EventContexts.Empty();
			EventContextDepths.Empty();
			EventTracePixels.Empty();
			EventTracePrimitiveIds.Empty();
			EventTracePrimitiveEvidenceIds.Empty();
			FocusedTraceEventIds.Empty();
			PendingEventContextByRequest.Empty();
			PendingResourcePixelHistoryByRequest.Empty();
			ScheduledResourcePixelHistoryKeys.Empty();
			ResourcePixelHistoryBindingAliases.Empty();
			DeferredResourceHistoryContextIds.Empty();
			DeferredResourceHistoryBranchCounts.Empty();
			DeferredEventContextIds.Empty();
			BudgetDeferredResourcePixelHistoryRequests.Empty();
			BudgetDeferredEventContextDepths.Empty();
			FailedResourcePixelHistoryKeys.Empty();
			ResourcePixelHistoryQueriesSubmitted = 0;
			ResourcePixelHistoryQueryLimit = MaxAutomaticResourcePixelHistoryQueries;
			DeterministicContextLimit = MaxAutomaticDeterministicContextEvents;
			PendingEventContextIds.Empty();
			FailedEventContextIds.Empty();
			LastCandidate.Reset();
			LastSignificantCandidate.Reset();
			bLastCandidateHasDivergence = false;
			InitializeFullTraceArtifact();
		}

		int32 QueuedCount = 0;
		for (FPixelSample& Sample : Samples)
		{
			if (Sample.bPending)
			{
				++QueuedCount;
				continue;
			}
			if (Sample.bAnalyzed && !Sample.bFailed)
			{
				continue;
			}
			Sample.bPending = true;
			Sample.bFailed = false;
			Sample.bAnalyzed = false;
			Sample.bTruncated = false;
			Sample.TotalModifications = 0;
			Sample.Error.Empty();
			Sample.Modifications.Empty();

			const FString RequestId = FString::Printf(TEXT("sample-%llu-query-%llu"), Sample.Id, ++RequestSerial);
			PendingSampleByRequest.Add(RequestId, Sample.Id);
			SendWorkerRequest(TEXT("pixel_history"), RequestId,
				[&Sample](const TSharedRef<FJsonObject>& Request)
				{
					Request->SetNumberField(TEXT("x"), Sample.Pixel.X);
					Request->SetNumberField(TEXT("y"), Sample.Pixel.Y);
					Request->SetNumberField(TEXT("sample"), 0);
				}, 1000);
			++QueuedCount;
		}

		UpdateSelectionText();
		RenderCausalReport();
		if (QueuedCount == 0)
		{
			SetAgentStatus(HasPendingWorkerRequests()
				? TEXT("自动深追仍在进行；完成后可以围绕当前 P1 向 Agent 提问。")
				: TEXT("像素与自动深追证据已就绪，可以围绕当前 P1 向 Agent 提问。"));
			SetStatus(HasPendingWorkerRequests()
				? TEXT("选点未变化，正在继续自动深追，不重复已完成查询。")
				: TEXT("选点未变化，已复用完整的自动深追结果。"));
		}
		else
		{
			SetAgentStatus(TEXT("规则分析与自动深追进行中；全部分支收束后可以向 Agent 提问。"));
			SetStatus(TEXT("已确认当前像素，正在读取 Pixel History 并自动深追资源/sample 与 producer 上下文…"));
		}
		return FReply::Handled();
	}

	void SAnalyzerHome::MarkReplaySynchronizationPending()
	{
		bReplaySynchronizationPending = true;
		SetStatus(TEXT("已选点；正在等待同步：ReplayController → 目标 RT → Pixel History。"));
		SetAgentStatus(TEXT("分析已排队，等待 Replay 完整同步；同步完成后才会读取事件上下文和 Shader Debug。"));
	}

	void SAnalyzerHome::CancelQueuedWorkerRequestsForNewAnalysisGeneration(const TCHAR* Reason)
	{
		const uint64 PreviousGeneration = AnalysisGeneration;
		++AnalysisGeneration;
		const int32 RemovedQueuedRequests = QueuedWorkerRequests.Num();
		for (const FQueuedWorkerRequest& Request : QueuedWorkerRequests)
		{
			WorkerRequestQueuedSeconds.Remove(Request.RequestId);
			WorkerRequestCommands.Remove(Request.RequestId);
		}
		QueuedWorkerRequests.Empty();
		const bool bFinishingPreviousRequest = !DispatchedWorkerRequestId.IsEmpty()
			&& DispatchedWorkerRequestGeneration != AnalysisGeneration;
		Diagnostics.WriteRecord(TEXT("analysis_generation_changed"), FString::Printf(
			TEXT("reason=%s previousGeneration=%llu currentGeneration=%llu removedQueued=%d activeRequest=%s activeGeneration=%llu finishingPrevious=%s"),
			Reason, PreviousGeneration, AnalysisGeneration, RemovedQueuedRequests,
			DispatchedWorkerRequestId.IsEmpty() ? TEXT("none") : *DispatchedWorkerRequestId,
			DispatchedWorkerRequestGeneration, bFinishingPreviousRequest ? TEXT("true") : TEXT("false")));
		if (bFinishingPreviousRequest)
		{
			ActiveWorkerStage = TEXT("finishing superseded pixel request; result will be discarded");
		}
		else
		{
			ActiveWorkerRequestId.Empty();
			ActiveWorkerStage.Empty();
			ActiveWorkerRequestStartSeconds = 0.0;
		}
	}

	void SAnalyzerHome::ResetSamples()
	{
		WriteFullTraceSnapshot(TEXT("selection-reset"));
		const int32 PreviousSamples = Samples.Num();
		const int32 PreviousEventContexts = EventContexts.Num();
		const int32 PreviousPendingSamples = PendingSampleByRequest.Num();
		const int32 PreviousPendingContexts = PendingEventContextByRequest.Num();
		const int32 PreviousPendingShaders = PendingShaderDebugByRequest.Num();
		const int32 PreviousPendingResourceHistories = PendingResourcePixelHistoryByRequest.Num();
		CancelQueuedWorkerRequestsForNewAnalysisGeneration(TEXT("selection-reset"));
		CancelAgentRun();
		bSelectionConfirmed = false;
		bReplaySynchronizationPending = false;
		bQueuePixelHistoryAfterWorkerReady = false;
		Samples.Empty();
		PendingSampleByRequest.Empty();
		EventContexts.Empty();
		EventContextDepths.Empty();
		PendingEventContextByRequest.Empty();
		PendingShaderDebugByRequest.Empty();
		PendingResourcePixelHistoryByRequest.Empty();
		ScheduledResourcePixelHistoryKeys.Empty();
		ResourcePixelHistoryBindingAliases.Empty();
		DeferredResourceHistoryContextIds.Empty();
		DeferredResourceHistoryBranchCounts.Empty();
		DeferredEventContextIds.Empty();
		BudgetDeferredResourcePixelHistoryRequests.Empty();
		BudgetDeferredEventContextDepths.Empty();
		FailedResourcePixelHistoryKeys.Empty();
		ResourcePixelHistoryQueriesSubmitted = 0;
		ResourcePixelHistoryQueryLimit = MaxAutomaticResourcePixelHistoryQueries;
		DeterministicContextLimit = MaxAutomaticDeterministicContextEvents;
		bDeterministicForegroundCompletionReported = false;
		EventTracePixels.Empty();
		EventTracePrimitiveIds.Empty();
		EventTracePrimitiveEvidenceIds.Empty();
		FocusedTraceEventIds.Empty();
		PendingEventContextIds.Empty();
		FailedEventContextIds.Empty();
		FailedShaderDebugIds.Empty();
		LastCandidate.Reset();
		LastSignificantCandidate.Reset();
		LastAgentQuestion.Empty();
		LastAgentAnswer.Empty();
		bAgentResultDisplayed = false;
		bLastCandidateHasDivergence = false;
		Diagnostics.WriteRecord(TEXT("selection_state_cleared"), FString::Printf(
			TEXT("generation=%llu samples=%d eventContexts=%d pendingSamples=%d pendingContexts=%d pendingShaders=%d pendingResourceHistories=%d activeRequest=%s activeGeneration=%llu"),
			AnalysisGeneration, PreviousSamples, PreviousEventContexts, PreviousPendingSamples,
			PreviousPendingContexts, PreviousPendingShaders, PreviousPendingResourceHistories,
			DispatchedWorkerRequestId.IsEmpty() ? TEXT("none") : *DispatchedWorkerRequestId,
			DispatchedWorkerRequestGeneration));
		SetAgentOutputText(TEXT("选择并分析像素后，可以围绕最终写入、Pass、Pipeline、Shader 或证据断点继续提问。"));
		SetAgentStatus(TEXT("未运行 · 只发送像素摘要；.rdc/图像不上传；Key 不落盘"));
		if (ImageView.IsValid())
		{
			ImageView->SetMarkers({});
		}
		UpdateSelectionText();
	}

	void SAnalyzerHome::UpdateSelectionText()
	{
		if (SelectionText.IsValid())
		{
			if (Samples.IsEmpty())
			{
				SelectionText->SetText(FText::FromString(TEXT("尚未选择关注像素；点击画面选择一个点")));
				return;
			}
			const FPixelSample& Sample = Samples[0];
			const TCHAR* State = Sample.bPending
				? TEXT("查询中")
				: Sample.bFailed
					? TEXT("失败")
					: Sample.bAnalyzed
						? TEXT("已分析")
						: TEXT("待分析");
			SelectionText->SetText(FText::FromString(FString::Printf(
				TEXT("当前像素 P1 (%d,%d) · %s · %s；点击其他位置可直接替换"),
				Sample.Pixel.X, Sample.Pixel.Y, State, bSelectionConfirmed ? TEXT("已确认") : TEXT("待确认"))));
		}
	}

	void SAnalyzerHome::UpdateMarkers()
	{
		if (!ImageView.IsValid())
		{
			return;
		}
		TArray<FPixelMarker> Markers;
		Markers.Reserve(Samples.Num());
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			Markers.Add({Samples[Index].Pixel});
		}
		ImageView->SetMarkers(Markers);
	}

	FPixelSample* SAnalyzerHome::FindSample(uint64 SampleId)
	{
		return Samples.FindByPredicate([SampleId](const FPixelSample& Sample) { return Sample.Id == SampleId; });
	}

	FReply SAnalyzerHome::BrowseCapture()
	{
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (!DesktopPlatform)
		{
			SetStatus(TEXT("DesktopPlatform is unavailable."));
			return FReply::Handled();
		}
		TArray<FString> Files;
		if (DesktopPlatform->OpenFileDialog(
			FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
			TEXT("Open RenderDoc capture"), FPaths::ProjectSavedDir(), TEXT(""), TEXT("RenderDoc capture (*.rdc)|*.rdc"),
			EFileDialogFlags::None, Files) && !Files.IsEmpty())
		{
			CapturePathBox->SetText(FText::FromString(Files[0]));
			StartWorker();
		}
		return FReply::Handled();
	}

	FReply SAnalyzerHome::LoadCapture()
	{
		StartWorker();
		return FReply::Handled();
	}

	bool SAnalyzerHome::LaunchWorkerProcess(const FString& Worker, const FString& Capture, const FString& InPreviewPath)
	{
		FString Args;
		FString Error;
		const FRenderTrailDiagnosticsOptions& DiagnosticOptions = Diagnostics.GetOptions();
		const bool bLaunched = ReplayWorker.Launch(Worker, Capture, InPreviewPath,
			DiagnosticOptions.bEnabled && DiagnosticOptions.bFullEvidencePayload,
			DiagnosticOptions.bEnabled && DiagnosticOptions.bGpuCrashDiagnostics,
			DiagnosticOptions.bFastReplay,
			DiagnosticOptions.bEnabled && DiagnosticOptions.bGpuCrashDiagnostics && DiagnosticOptions.bRenderDocDRED,
			Diagnostics.GetWorkerDiagnosticsFilePath(), Args, Error);
		if (Diagnostics.GetOptions().bWorkerProtocol)
		{
			Diagnostics.WriteRecord(TEXT("worker_launch"), FString::Printf(
				TEXT("pid=%u\nworker=%s\nargs=%s\nworkerDiagnostics=%s"),
				ReplayWorker.GetProcessId(), *Worker, *Args,
				Diagnostics.GetWorkerDiagnosticsFilePath().IsEmpty()
					? TEXT("disabled") : *Diagnostics.GetWorkerDiagnosticsFilePath()));
		}
		SetCaptureLoadPhase(TEXT("Starting isolated Replay Worker and opening full Replay"));
		SetEvidence(TEXT("当前仅显示临时预览；隔离 Replay Worker 正在导出可用于选点的权威最终 RT。"));
		UE_LOG(LogRenderTrailAnalyzer, Display,
			TEXT("Starting isolated Replay Worker. Worker='%s' Capture='%s' Preview='%s'"),
			*Worker, *Capture, *InPreviewPath);
		if (!bLaunched)
		{
			SetStatus(Error);
			return false;
		}
		SetStatus(TEXT("正在载入完整 Replay；最终 RT 导出完成前暂不允许选点。"));
		return true;
	}

	void SAnalyzerHome::StartWorker(bool bPreserveSelection)
	{
		if (bCaptureLoading)
		{
			FinishCaptureLoad(TEXT("restarted"));
		}
		const FString Capture = FPaths::ConvertRelativePathToFull(GetCapturePath());
		if (!FPaths::FileExists(Capture))
		{
			SetStatus(TEXT("Capture file does not exist."));
			return;
		}
		NativePreviewPath = UE::RenderTrail::GetPreviewPathForCapture(Capture);
		PreviewPath = UE::RenderTrail::GetReplayPreviewPathForCapture(Capture);
		CaptureLoadStartSeconds = FPlatformTime::Seconds();
		LastCaptureLoadStatusSeconds = CaptureLoadStartSeconds;
		LastWorkerHeartbeatSeconds = CaptureLoadStartSeconds;
		LastWorkerDiagnosticPhase.Empty();
		bCaptureLoading = true;
		CaptureLoadPhase = TEXT("Preparing isolated Replay Worker");
		const int64 CaptureSize = IFileManager::Get().FileSize(*Capture);
		UE_LOG(LogRenderTrailAnalyzer, Display,
			TEXT("Capture load started: capture='%s' bytes=%lld"),
			*Capture, CaptureSize);
		SetCaptureLoadPhase(TEXT("Closing previous replay session"));
		StopWorker();
		Diagnostics.BeginSession(Capture, CaptureSize);
		ResetLiveReplayLog(Capture, CaptureSize);
		if (!bPreserveSelection)
		{
			ReleasePreview();
			ResetSamples();
			bQueuePixelHistoryAfterWorkerReady = false;
		}
		bReplayStartDeferred = false;
		bWorkerReady = false;
		ReplayTargetResourceIndex = INDEX_NONE;
		ReplayTargetSamples = 1;
		ReplayTargetFormat.Empty();
		bPreviewReadyForSelection = bPreserveSelection && PreviewBrush.IsValid();
		LastWorkerError.Empty();
		FIntPoint NativePreviewSize = FIntPoint::ZeroValue;
		if (!bPreserveSelection && TryGetPixelExactPreviewSize(Capture, NativePreviewPath, NativePreviewSize)
			&& LoadPreview(NativePreviewPath, NativePreviewSize))
		{
			bPreviewReadyForSelection = false;
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Native capture preview loaded as display-only provisional image. capture='%s' nativePreview='%s' replayPreview='%s'"),
				*Capture, *NativePreviewPath, *PreviewPath);
			SetStatus(TEXT("UE 原生预览已显示，但它不是 RenderDoc 最终 RT；正在后台导出权威最终 RT，暂不允许选点。"));
			SetReportCards(
				TEXT("UE 原生预览仅用于等待期间查看，不能作为 Pixel History 坐标基准。"),
				TEXT("正在导出 RenderDoc 最终 RT\n↓\n就绪后再选择 P1"),
				TEXT("尚无因果证据。"),
				FString::Printf(TEXT("原生预览：%s\n权威最终 RT：%s\n两个文件独立保存，不再相互覆盖。"),
					*NativePreviewPath, *PreviewPath));
		}

		const FString Worker = FRenderTrailReplayWorkerClient::GetDefaultExecutablePath();
		if (!FPaths::FileExists(Worker))
		{
			FinishCaptureLoad(TEXT("worker missing"));
			SetStatus(FString::Printf(TEXT("Replay Worker is missing: %s. Build the standard RenderTrailReplayWorker Program target first."), *Worker));
			UE_LOG(LogRenderTrailAnalyzer, Error, TEXT("Replay Worker is missing: %s"), *Worker);
			return;
		}

		if (!LaunchWorkerProcess(Worker, Capture, PreviewPath))
		{
			bPreviewReadyForSelection = false;
			FinishCaptureLoad(TEXT("worker launch failed"));
		}
		else if (bPreviewReadyForSelection)
		{
			SetStatus(TEXT("原生预览保持可用；完整 Replay Worker 正在按需加载。"));
			SetReportCards(
				TEXT("原生预览保持可用，完整 Replay 数据正在加载。"),
				TEXT("已确认的 P1 会在 Replay Worker 就绪后自动执行 Pixel History。"),
				TEXT("尚无因果证据。"),
				TEXT("Replay 仅因本次像素分析按需启动。"));
		}
	}

	void SAnalyzerHome::StopWorker()
	{
		if (Diagnostics.HasSession())
		{
			Diagnostics.WriteRecord(TEXT("worker_stop"), TEXT("stop requested"));
		}
		if (ReplayWorker.IsRunning() && Diagnostics.GetOptions().bWorkerProtocol)
		{
			Diagnostics.WriteRecord(TEXT("analyzer_to_worker"), TEXT("{\"command\":\"shutdown\"}"));
		}
		const FRenderTrailReplayWorkerStopResult StopResult = ReplayWorker.Stop();
		const FString StopDetail = FString::Printf(
			TEXT("hadProcess=%s wasRunning=%s shutdownWritten=%s graceful=%s forced=%s elapsed=%.3fs"),
			StopResult.bHadProcess ? TEXT("true") : TEXT("false"),
			StopResult.bWasRunning ? TEXT("true") : TEXT("false"),
			StopResult.bShutdownWritten ? TEXT("true") : TEXT("false"),
			StopResult.bExitedGracefully ? TEXT("true") : TEXT("false"),
			StopResult.bForcedTermination ? TEXT("true") : TEXT("false"), StopResult.ElapsedSeconds);
		if (Diagnostics.HasSession())
		{
			Diagnostics.WriteRecord(TEXT("worker_stop_complete"), StopDetail);
		}
		if (StopResult.bForcedTermination)
		{
			UE_LOG(LogRenderTrailAnalyzer, Warning, TEXT("Replay Worker stop complete: %s"), *StopDetail);
		}
		else
		{
			UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Replay Worker stop complete: %s"), *StopDetail);
		}
		QueuedWorkerRequests.Empty();
		WorkerRequestQueuedSeconds.Empty();
		WorkerRequestCommands.Empty();
		DispatchedWorkerRequestId.Empty();
		DispatchedWorkerRequestGeneration = 0;
		ActiveWorkerRequestId.Empty();
		ActiveWorkerStage.Empty();
		ActiveWorkerRequestStartSeconds = 0.0;
		bWorkerReady = false;
		bPreviewReadyForSelection = false;
	}

	void SAnalyzerHome::PollWorkerPipes()
	{
		bPollingWorkerPipes = true;
		FRenderTrailReplayWorkerPollResult Result = ReplayWorker.Poll();
		for (const FString& Line : Result.OutputLines)
		{
			if (Diagnostics.GetOptions().bWorkerProtocol)
			{
				Diagnostics.WriteRecord(TEXT("worker_stdout"), Line);
			}
			HandleWorkerMessage(Line);
		}
		if (!Result.ErrorChunk.IsEmpty() && Diagnostics.GetOptions().bWorkerProtocol)
		{
			Diagnostics.WriteRecord(TEXT("worker_stderr"), Result.ErrorChunk);
		}
		if (!Result.ErrorChunk.IsEmpty())
		{
			AppendLiveReplayLog(FString::Printf(TEXT("[%s] [worker.stderr]\n%s\n"),
				*FDateTime::Now().ToIso8601(), *Result.ErrorChunk));
		}

		if (Result.bExited)
		{
			AppendLiveReplayLog(FString::Printf(
				TEXT("[%s] [worker.exit] returnCodeAvailable=%s returnCode=%d detail=%s\n"),
				*FDateTime::Now().ToIso8601(),
				Result.bReturnCodeAvailable ? TEXT("true") : TEXT("false"), Result.ReturnCode, *Result.ExitDetail));
			if (Diagnostics.HasSession())
			{
				Diagnostics.WriteRecord(TEXT("worker_exit"), FString::Printf(
					TEXT("returnCodeAvailable=%s\nreturnCode=%d\ndetail=%s\nlastPhase=%s\nworkerDiagnostics=%s"),
					Result.bReturnCodeAvailable ? TEXT("true") : TEXT("false"), Result.ReturnCode,
					*Result.ExitDetail, *LastWorkerDiagnosticPhase,
					Diagnostics.GetWorkerDiagnosticsFilePath().IsEmpty()
						? TEXT("disabled") : *Diagnostics.GetWorkerDiagnosticsFilePath()));
			}
			if (!Result.PartialOutput.IsEmpty() && Diagnostics.GetOptions().bWorkerProtocol)
			{
				Diagnostics.WriteRecord(TEXT("worker_stdout_partial"), Result.PartialOutput);
			}
			if (!bWorkerReady && bCaptureLoading)
			{
				LastWorkerError = Result.ExitDetail;
				FinishCaptureLoad(TEXT("worker exited"));
				SetStatus(FString::Printf(TEXT("Replay Worker exited: %s"), *Result.ExitDetail));
				UE_LOG(LogRenderTrailAnalyzer, Error, TEXT("Replay Worker exited before ready. Detail='%s'"), *Result.ExitDetail);
			}
		}
		bPollingWorkerPipes = false;
		TryDispatchNextWorkerRequest();
	}

	bool SAnalyzerHome::TryDispatchNextWorkerRequest()
	{
		if (bPollingWorkerPipes || !DispatchedWorkerRequestId.IsEmpty()
			|| QueuedWorkerRequests.IsEmpty())
		{
			return true;
		}
		if (!ReplayWorker.IsRunning())
		{
			return false;
		}

		while (!QueuedWorkerRequests.IsEmpty())
		{
			int32 SelectedIndex = 0;
			for (int32 Index = 1; Index < QueuedWorkerRequests.Num(); ++Index)
			{
				const FQueuedWorkerRequest& Candidate = QueuedWorkerRequests[Index];
				const FQueuedWorkerRequest& Selected = QueuedWorkerRequests[SelectedIndex];
				if (Candidate.Priority > Selected.Priority
					|| (Candidate.Priority == Selected.Priority && Candidate.QueueOrdinal < Selected.QueueOrdinal))
				{
					SelectedIndex = Index;
				}
			}
			FQueuedWorkerRequest Queued = MoveTemp(QueuedWorkerRequests[SelectedIndex]);
			QueuedWorkerRequests.RemoveAt(SelectedIndex, 1, EAllowShrinking::No);
			if (Queued.AnalysisGeneration != AnalysisGeneration)
			{
				WorkerRequestQueuedSeconds.Remove(Queued.RequestId);
				WorkerRequestCommands.Remove(Queued.RequestId);
				Diagnostics.WriteRecord(TEXT("worker_request_generation_dropped"), FString::Printf(
					TEXT("request=%s command=%s requestGeneration=%llu currentGeneration=%llu locallyQueued=%d"),
					*Queued.RequestId, *Queued.Command, Queued.AnalysisGeneration, AnalysisGeneration,
					QueuedWorkerRequests.Num()));
				continue;
			}

			DispatchedWorkerRequestId = Queued.RequestId;
			DispatchedWorkerRequestGeneration = Queued.AnalysisGeneration;
			ActiveWorkerRequestId = Queued.RequestId;
			ActiveWorkerStage = TEXT("dispatched; waiting for Worker");
			ActiveWorkerRequestStartSeconds = FPlatformTime::Seconds();
			if (Diagnostics.GetOptions().bWorkerProtocol)
			{
				Diagnostics.WriteRecord(TEXT("analyzer_to_worker"), Queued.Payload);
			}
			Diagnostics.WriteRecord(TEXT("worker_request_dispatched"), FString::Printf(
				TEXT("request=%s command=%s generation=%llu priority=%d locallyQueued=%d trackedTotal=%d waitBeforeDispatch=%.3fs"),
				*Queued.RequestId, *Queued.Command, Queued.AnalysisGeneration, Queued.Priority, QueuedWorkerRequests.Num(),
				WorkerRequestQueuedSeconds.Num(),
				FMath::Max(0.0, FPlatformTime::Seconds() - Queued.EnqueuedAtSeconds)));
			if (ReplayWorker.Write(Queued.Payload))
			{
				return true;
			}

			Diagnostics.WriteRecord(TEXT("worker_request_dispatch_failed"), FString::Printf(
				TEXT("request=%s command=%s generation=%llu"),
				*Queued.RequestId, *Queued.Command, Queued.AnalysisGeneration));
			WorkerRequestQueuedSeconds.Remove(Queued.RequestId);
			WorkerRequestCommands.Remove(Queued.RequestId);
			DispatchedWorkerRequestId.Empty();
			DispatchedWorkerRequestGeneration = 0;
			ActiveWorkerRequestId.Empty();
			ActiveWorkerStage.Empty();
			ActiveWorkerRequestStartSeconds = 0.0;
			SetStatus(TEXT("Replay Worker request could not be dispatched."));
			return false;
		}
		return true;
	}

	bool SAnalyzerHome::SendWorkerRequest(const FString& Command, const FString& RequestId,
		TFunctionRef<void(const TSharedRef<FJsonObject>&)> Populate, int32 Priority)
	{
		if (!ReplayWorker.IsRunning())
		{
			SetStatus(TEXT("Replay Worker is not running."));
			return false;
		}
		const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("command"), Command);
		Request->SetStringField(TEXT("requestId"), RequestId);
		Populate(Request);
		const FString Payload = SerializeJson(Request);
		AppendFullTraceRecord(TEXT("analyzer-request"), Request);
		const double QueuedAt = FPlatformTime::Seconds();
		WorkerRequestQueuedSeconds.Add(RequestId, QueuedAt);
		WorkerRequestCommands.Add(RequestId, Command);
		FQueuedWorkerRequest& Queued = QueuedWorkerRequests.Emplace_GetRef();
		Queued.Command = Command;
		Queued.RequestId = RequestId;
		Queued.Payload = Payload;
		Queued.AnalysisGeneration = AnalysisGeneration;
		Queued.QueueOrdinal = ++WorkerQueueOrdinal;
		Queued.EnqueuedAtSeconds = QueuedAt;
		Queued.Priority = Priority;
		Diagnostics.WriteRecord(TEXT("worker_request_queued"), FString::Printf(
			TEXT("request=%s command=%s queueOrdinal=%llu generation=%llu priority=%d pendingTotal=%d locallyQueued=%d active=%s critical=%s"),
			*RequestId, *Command, Queued.QueueOrdinal, AnalysisGeneration, Priority,
			WorkerRequestQueuedSeconds.Num(), QueuedWorkerRequests.Num(),
			DispatchedWorkerRequestId.IsEmpty() ? TEXT("none") : *DispatchedWorkerRequestId,
			HasPendingCriticalDeterministicQueries() ? TEXT("true") : TEXT("false")));
		return TryDispatchNextWorkerRequest();
	}

	double SAnalyzerHome::CompleteWorkerRequest(const FString& RequestId, const FString& ResultType, const FString& Detail)
	{
		const double Now = FPlatformTime::Seconds();
		const double* QueuedAt = WorkerRequestQueuedSeconds.Find(RequestId);
		const double QueueToResponseSeconds = QueuedAt ? Now - *QueuedAt : -1.0;
		const FString Command = WorkerRequestCommands.FindRef(RequestId);
		Diagnostics.WriteRecord(TEXT("worker_request_completed"), FString::Printf(
			TEXT("request=%s command=%s result=%s queueToResponse=%.3fs remaining=%d detail=%s"),
			*RequestId, *Command, *ResultType, QueueToResponseSeconds,
			FMath::Max(0, WorkerRequestQueuedSeconds.Num() - (QueuedAt ? 1 : 0)), *Detail));
		WorkerRequestQueuedSeconds.Remove(RequestId);
		WorkerRequestCommands.Remove(RequestId);
		if (DispatchedWorkerRequestId == RequestId)
		{
			DispatchedWorkerRequestId.Empty();
			DispatchedWorkerRequestGeneration = 0;
		}
		if (ActiveWorkerRequestId == RequestId)
		{
			ActiveWorkerRequestId.Empty();
			ActiveWorkerStage.Empty();
			ActiveWorkerRequestStartSeconds = 0.0;
		}
		return QueueToResponseSeconds;
	}

	void SAnalyzerHome::ReleasePreview()
	{
		if (ImageView.IsValid())
		{
			ImageView->SetImage(nullptr, FIntPoint::ZeroValue);
		}
		if (PreviewBrush.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().GetRenderer()->ReleaseDynamicResource(*PreviewBrush);
		}
		PreviewBrush.Reset();
		CurrentPreviewSize = FIntPoint::ZeroValue;
	}

	bool SAnalyzerHome::IsPreviewCacheValid(const FString& CapturePath, const FString& InPreviewPath)
	{
		if (!IFileManager::Get().FileExists(*InPreviewPath))
		{
			return false;
		}
		const FDateTime CaptureTimestamp = IFileManager::Get().GetTimeStamp(*CapturePath);
		const FDateTime PreviewTimestamp = IFileManager::Get().GetTimeStamp(*InPreviewPath);
		return CaptureTimestamp != FDateTime::MinValue()
			&& PreviewTimestamp != FDateTime::MinValue()
			&& PreviewTimestamp >= CaptureTimestamp;
	}

	bool SAnalyzerHome::TryGetPixelExactPreviewSize(
		const FString& CapturePath, const FString& InPreviewPath, FIntPoint& OutSize)
	{
		OutSize = FIntPoint::ZeroValue;
		if (!IsPreviewCacheValid(CapturePath, InPreviewPath))
		{
			return false;
		}
		FString MetadataJson;
		UE::RenderTrail::FCaptureMetadata Metadata;
		FString MetadataError;
		if (!FFileHelper::LoadFileToString(
			MetadataJson, *UE::RenderTrail::GetMetadataPathForCapture(CapturePath))
			|| !UE::RenderTrail::FCaptureMetadata::FromJson(MetadataJson, Metadata, MetadataError)
			|| !Metadata.bPreviewPixelExact
			|| Metadata.PreviewWidth <= 0
			|| Metadata.PreviewHeight <= 0
			|| Metadata.PreviewPath.IsEmpty()
			|| !FPaths::IsSamePath(
				FPaths::ConvertRelativePathToFull(Metadata.PreviewPath),
				FPaths::ConvertRelativePathToFull(InPreviewPath)))
		{
			return false;
		}
		OutSize = FIntPoint(Metadata.PreviewWidth, Metadata.PreviewHeight);
		return true;
	}

	bool SAnalyzerHome::LoadPreview(const FString& Path, FIntPoint ExpectedSize)
	{
		TArray64<uint8> Compressed;
		if (!FFileHelper::LoadFileToArray(Compressed, *Path))
		{
			return false;
		}
		IImageWrapperModule& ImageWrapper = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		FImage Image;
		if (!ImageWrapper.DecompressImage(Compressed.GetData(), Compressed.Num(), Image))
		{
			return false;
		}
		Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const FIntPoint ActualSize(Image.SizeX, Image.SizeY);
		if (ExpectedSize != FIntPoint::ZeroValue && ActualSize != ExpectedSize)
		{
			return false;
		}
		TArray<uint8> Pixels(MoveTemp(Image.RawData));
		// Final UE render targets often carry alpha=0 even though their RGB is the intended
		// viewport image. Standalone Slate composites that alpha, so preview as opaque RGB.
		for (int64 Alpha = 3; Alpha < Pixels.Num(); Alpha += 4)
		{
			Pixels[Alpha] = 255;
		}
		const FString ResourceString = FString::Printf(TEXT("RenderTrailPreview_%s_%llu"), *FPaths::GetBaseFilename(Path), ++PreviewSerial);
		const FName ResourceName(*ResourceString);
		if (!FSlateApplication::Get().GetRenderer()->GenerateDynamicImageResource(ResourceName, Image.SizeX, Image.SizeY, Pixels))
		{
			return false;
		}
		PreviewBrush = MakeShared<FSlateDynamicImageBrush>(ResourceName, FVector2D(Image.SizeX, Image.SizeY));
		ImageView->SetImage(PreviewBrush, ActualSize, MoveTemp(Pixels));
		CurrentPreviewSize = ActualSize;
		return true;
	}

	void SAnalyzerHome::HandleWorkerMessage(const FString& Line)
	{
		TSharedPtr<FJsonObject> Message;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, Message) || !Message.IsValid())
		{
			// The worker's stdout may also contain Unreal startup diagnostics. Only
			// JSON lines are part of the RenderTrail protocol.
			return;
		}
		FString Type;
		Message->TryGetStringField(TEXT("type"), Type);
		if (Type == TEXT("pixel_history") || Type == TEXT("event_context")
			|| Type == TEXT("shader_debug") || Type == TEXT("error"))
		{
			FString TerminalRequestId;
			Message->TryGetStringField(TEXT("requestId"), TerminalRequestId);
			if (!TerminalRequestId.IsEmpty() && TerminalRequestId == DispatchedWorkerRequestId
				&& DispatchedWorkerRequestGeneration != AnalysisGeneration)
			{
				const uint64 SupersededGeneration = DispatchedWorkerRequestGeneration;
				CompleteWorkerRequest(TerminalRequestId, TEXT("superseded_result"), Type);
				Diagnostics.WriteRecord(TEXT("worker_superseded_result_discarded"), FString::Printf(
					TEXT("request=%s type=%s requestGeneration=%llu currentGeneration=%llu locallyQueued=%d"),
					*TerminalRequestId, *Type, SupersededGeneration, AnalysisGeneration,
					QueuedWorkerRequests.Num()));
				return;
			}
		}
		if (Type == TEXT("progress"))
		{
			FString Phase;
			double WorkerElapsed = 0.0;
			Message->TryGetStringField(TEXT("phase"), Phase);
			Message->TryGetNumberField(TEXT("elapsedSeconds"), WorkerElapsed);
			SetCaptureLoadPhase(FString::Printf(TEXT("Isolated Replay Worker: %s (%.1fs)"), *Phase, WorkerElapsed));
			LastWorkerDiagnosticPhase = Phase;
			if (WorkerDiagnosticsTailPath.IsEmpty())
			{
				AppendLiveReplayLog(FString::Printf(TEXT("[%s] [worker.progress] elapsed=%.1fs %s\n"),
					*FDateTime::Now().ToIso8601(), WorkerElapsed, *Phase));
			}
			return;
		}
		if (Type == TEXT("diagnostic"))
		{
			FString Stage;
			FString State;
			FString Detail;
			FString RequestId;
			double StageElapsed = 0.0;
			Message->TryGetStringField(TEXT("stage"), Stage);
			Message->TryGetStringField(TEXT("state"), State);
			Message->TryGetStringField(TEXT("detail"), Detail);
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			Message->TryGetNumberField(TEXT("stageElapsedSeconds"), StageElapsed);
			if (!RequestId.IsEmpty() && RequestId == DispatchedWorkerRequestId
				&& DispatchedWorkerRequestGeneration == AnalysisGeneration)
			{
				AppendFullTraceRecord(TEXT("worker-diagnostic"), Message.ToSharedRef());
			}
			LastWorkerDiagnosticPhase = FString::Printf(TEXT("%s %s: %s"), *Stage, *State, *Detail);
			if (!RequestId.IsEmpty() && RequestId == DispatchedWorkerRequestId)
			{
				ActiveWorkerRequestId = RequestId;
				ActiveWorkerStage = FString::Printf(TEXT("%s %s"), *Stage, *State);
				if (ActiveWorkerRequestStartSeconds <= 0.0 || State == TEXT("begin"))
				{
					ActiveWorkerRequestStartSeconds = WorkerRequestQueuedSeconds.FindRef(RequestId);
					if (ActiveWorkerRequestStartSeconds <= 0.0)
					{
						ActiveWorkerRequestStartSeconds = FPlatformTime::Seconds();
					}
				}
			}
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Replay Worker diagnostic: stage='%s' state='%s' stageElapsed=%.3fs detail='%s'"),
				*Stage, *State, StageElapsed, *Detail);
			if (WorkerDiagnosticsTailPath.IsEmpty())
			{
				AppendLiveReplayLog(FString::Printf(
					TEXT("[%s] [worker.diagnostic] stage=%s state=%s elapsed=%.3fs request=%s\n%s\n"),
					*FDateTime::Now().ToIso8601(), *Stage, *State, StageElapsed,
					RequestId.IsEmpty() ? TEXT("none") : *RequestId, *Detail));
			}
			return;
		}
		if (Type == TEXT("preview"))
		{
			FString Path;
			FString Source;
			double PreviewWidth = 0.0;
			double PreviewHeight = 0.0;
			Message->TryGetStringField(TEXT("previewPath"), Path);
			Message->TryGetStringField(TEXT("source"), Source);
			Message->TryGetNumberField(TEXT("width"), PreviewWidth);
			Message->TryGetNumberField(TEXT("height"), PreviewHeight);
			// Embedded capture thumbnails are useful only when no image is available yet.
			// They are never authoritative pixel coordinates, and must not replace a retained
			// RenderDoc final-RT image while Replay is being reopened.
			if (PreviewBrush.IsValid())
			{
				SetCaptureLoadPhase(TEXT("Provisional image retained; full Replay Worker continues in background"));
				return;
			}
			if (!Path.IsEmpty() && LoadPreview(Path, FIntPoint::ZeroValue))
			{
				bPreviewReadyForSelection = false;
				SetCaptureLoadPhase(FString::Printf(TEXT("Fast preview ready (%.0fx%.0f); full Replay Worker continues in background"),
					PreviewWidth, PreviewHeight));
				SetStatus(TEXT("截帧内嵌缩略图已显示；完整 Replay Worker 正在后台导出最终 RT，暂不允许选点。"));
				SetReportCards(
					TEXT("当前是截帧内嵌缩略图，不是可查询的最终 RT。"),
					TEXT("正在导出 RenderDoc 最终 RT\n↓\n就绪后再选择 P1"),
					TEXT("尚无因果证据。"),
					FString::Printf(TEXT("临时预览来源：%s。它不会被用于 Pixel History 坐标。"),
						Source.IsEmpty() ? TEXT("截帧内嵌缩略图") : *Source));
			}
			else
			{
				UE_LOG(LogRenderTrailAnalyzer, Warning, TEXT("Fast capture preview could not be loaded: path='%s'"), *Path);
			}
			return;
		}
		if (Type == TEXT("ready"))
		{
			bWorkerReady = true;
			bPreviewReadyForSelection = false;
			const int32 Width = static_cast<int32>(Message->GetNumberField(TEXT("width")));
			const int32 Height = static_cast<int32>(Message->GetNumberField(TEXT("height")));
			const FString Path = Message->GetStringField(TEXT("previewPath"));
			const FString Target = Message->GetStringField(TEXT("targetName"));
			const FString Version = Message->GetStringField(TEXT("renderDocVersion"));
			FString PreviewSource = TEXT("unknown");
			FString TargetSelection = TEXT("unknown");
			Message->TryGetStringField(TEXT("previewSource"), PreviewSource);
			Message->TryGetStringField(TEXT("targetSelection"), TargetSelection);
			FString ReplayOptimisation = TEXT("unknown");
			Message->TryGetStringField(TEXT("replayOptimisation"), ReplayOptimisation);
			const bool bPixelHistory = Message->GetBoolField(TEXT("pixelHistorySupported"));
			const bool bShaderDebug = Message->GetBoolField(TEXT("shaderDebuggingSupported"));
			double TargetResourceIndexValue = INDEX_NONE;
			double TargetSamplesValue = 1.0;
			Message->TryGetNumberField(TEXT("targetResourceIndex"), TargetResourceIndexValue);
			Message->TryGetNumberField(TEXT("targetSamples"), TargetSamplesValue);
			Message->TryGetStringField(TEXT("targetFormat"), ReplayTargetFormat);
			ReplayTargetResourceIndex = static_cast<int32>(TargetResourceIndexValue);
			ReplayTargetSamples = FMath::Max(1, static_cast<int32>(TargetSamplesValue));
			const FIntPoint ReplayTargetSize(Width, Height);
			const bool bSelectionCoordinatesChanged = !Samples.IsEmpty()
				&& CurrentPreviewSize != FIntPoint::ZeroValue
				&& CurrentPreviewSize != ReplayTargetSize;
			bShaderDebuggingAvailable = bShaderDebug;
			bool bPreviewCached = false;
			Message->TryGetBoolField(TEXT("previewCached"), bPreviewCached);
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Isolated Replay Worker ready: elapsed=%.3fs RenderDoc=%s ReplayOptimisation=%s Size=%dx%d Target='%s' Resource=%d Samples=%d Format='%s' PixelHistory=%s ShaderDebug=%s Preview='%s'"),
				bCaptureLoading ? FPlatformTime::Seconds() - CaptureLoadStartSeconds : 0.0,
				*Version, *ReplayOptimisation, Width, Height, *Target, ReplayTargetResourceIndex, ReplayTargetSamples, *ReplayTargetFormat,
				bPixelHistory ? TEXT("yes") : TEXT("no"),
				bShaderDebug ? TEXT("yes") : TEXT("no"), *Path);
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Authoritative preview source='%s' targetSelection='%s'"), *PreviewSource, *TargetSelection);
			SetCaptureLoadPhase(bPreviewCached
				? TEXT("Isolated Replay Worker ready; reusing cached preview")
				: TEXT("Isolated Replay Worker ready; decoding exported preview"));
			const double PreviewStartSeconds = FPlatformTime::Seconds();
			if (!LoadPreview(Path, FIntPoint(Width, Height)))
			{
				UE_LOG(LogRenderTrailAnalyzer, Error, TEXT("Capture preview load failed: elapsed=%.3fs path='%s'"),
					FPlatformTime::Seconds() - PreviewStartSeconds, *Path);
				FinishCaptureLoad(TEXT("preview load failed"));
				SetStatus(FString::Printf(TEXT("Replay opened, but preview could not be loaded: %s"), *Path));
				return;
			}
			bPreviewReadyForSelection = true;
			if (bSelectionCoordinatesChanged)
			{
				bQueuePixelHistoryAfterWorkerReady = false;
				ResetSamples();
				UpdateSelectionText();
			}
			UpdateMarkers();
			const double PreviewElapsed = FPlatformTime::Seconds() - PreviewStartSeconds;
			UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Capture preview loaded: elapsed=%.3fs path='%s'"),
				PreviewElapsed, *Path);
			const double TotalLoadElapsed = bCaptureLoading ? FPlatformTime::Seconds() - CaptureLoadStartSeconds : 0.0;
			FinishCaptureLoad(TEXT("ready"));
			if (bSelectionCoordinatesChanged)
			{
				SetStatus(FString::Printf(TEXT("最终 RT 尺寸为 %dx%d，与选点预览不同；为避免分析错误像素，已清除 P1，请在精确最终 RT 上重新选择。"),
					Width, Height));
				SetReportCards(
					TEXT("完整 Replay 已就绪，但原生 Viewport 预览与最终 RT 尺寸不同。"),
					TEXT("坐标映射未被证明\n↓\n请在精确最终 RT 上重新选择 P1"),
					TEXT("旧 P1 已清除，未执行 Pixel History。"),
					TEXT("RenderTrail 不会按比例猜测像素祖先坐标。"));
				return;
			}
			SetStatus(FString::Printf(TEXT("RenderDoc 最终 RT 已就绪 | %s | Replay %s | %dx%d | %s | Pixel History: %s | Shader Debug: %s | 载入耗时 %.1fs"),
				*Version, *ReplayOptimisation, Width, Height, *Target,
				bPixelHistory ? TEXT("yes") : TEXT("no"), bShaderDebug ? TEXT("yes") : TEXT("no"), TotalLoadElapsed));
			SetReportCards(
				TEXT("RenderDoc 导出的权威最终 RT 已载入；现在的显示图就是选点与 Pixel History 共用的坐标域。"),
				TEXT("RenderDoc 最终 RT\n↓\n等待选择 P1"),
				TEXT("尚无候选原因。"),
				FString::Printf(TEXT("source=%s\ntargetSelection=%s\npreview=%s"),
					*PreviewSource, *TargetSelection, *Path));
			if (bQueuePixelHistoryAfterWorkerReady)
			{
				bQueuePixelHistoryAfterWorkerReady = false;
				ConfirmPixelSelection();
			}
			return;
		}
		if (Type == TEXT("pixel_history"))
		{
			FString RequestId;
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			if (PendingResourcePixelHistoryByRequest.Contains(RequestId))
			{
				StoreResourcePixelHistory(Message.ToSharedRef());
			}
			else
			{
				StorePixelHistory(Message.ToSharedRef());
			}
			return;
		}
		if (Type == TEXT("event_context"))
		{
			StoreEventContext(Message.ToSharedRef());
			return;
		}
		if (Type == TEXT("shader_debug"))
		{
			StoreShaderDebug(Message.ToSharedRef());
			return;
		}
		if (Type == TEXT("error"))
		{
			FString Stage;
			FString Error;
			FString RequestId;
			Message->TryGetStringField(TEXT("stage"), Stage);
			Message->TryGetStringField(TEXT("message"), Error);
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			const bool bKnownRequest = RequestId.IsEmpty()
				|| PendingSampleByRequest.Contains(RequestId)
				|| PendingEventContextByRequest.Contains(RequestId)
				|| PendingShaderDebugByRequest.Contains(RequestId)
				|| PendingResourcePixelHistoryByRequest.Contains(RequestId);
			if (!bKnownRequest)
			{
				UE_LOG(LogRenderTrailAnalyzer, Verbose,
					TEXT("Ignoring stale Replay Worker error after pixel replacement. Stage='%s' Request='%s' Message='%s'"),
					*Stage, *RequestId, *Error.Left(2000));
				return;
			}
			AppendFullTraceRecord(TEXT("worker-response"), Message.ToSharedRef());
			LastWorkerError = FString::Printf(TEXT("%s: %s"), *Stage, *Error);
			AppendLiveReplayLog(FString::Printf(TEXT("[%s] [worker.error] stage=%s request=%s\n%s\n"),
				*FDateTime::Now().ToIso8601(), *Stage, RequestId.IsEmpty() ? TEXT("none") : *RequestId, *Error));
			if (!RequestId.IsEmpty())
			{
				CompleteWorkerRequest(RequestId, TEXT("error"), Stage);
			}
			if (bCaptureLoading)
			{
				FinishCaptureLoad(FString::Printf(TEXT("worker error: %s"), *Stage));
				bReplaySynchronizationPending = false;
				bQueuePixelHistoryAfterWorkerReady = false;
			}
			UE_LOG(LogRenderTrailAnalyzer, Error, TEXT("Isolated Replay Worker error. Stage='%s' Request='%s' Message='%s'"),
				*Stage, *RequestId, *Error.Left(2000));
			if (const uint64* SampleId = PendingSampleByRequest.Find(RequestId))
			{
				if (FPixelSample* Sample = FindSample(*SampleId))
				{
					Sample->bPending = false;
					Sample->bFailed = true;
					Sample->bAnalyzed = false;
					Sample->Error = Error;
				}
				PendingSampleByRequest.Remove(RequestId);
				UpdateSelectionText();
				RenderCausalReport();
			}
			if (const uint32* EventId = PendingEventContextByRequest.Find(RequestId))
			{
				const uint32 FailedEventId = *EventId;
				FailedEventContextIds.Add(FailedEventId);
				PendingEventContextIds.Remove(FailedEventId);
				PendingEventContextByRequest.Remove(RequestId);
				RenderCausalReport();
				ResumeAgentAfterEventContext(FailedEventId);
				TryResumeAgentAfterDeterministicContexts();
			}
			if (const uint32* EventId = PendingShaderDebugByRequest.Find(RequestId))
			{
				const uint32 FailedEventId = *EventId;
				FailedShaderDebugIds.Add(FailedEventId);
				PendingShaderDebugByRequest.Remove(RequestId);
				if (FEventContextEvidence* Context = EventContexts.Find(FailedEventId))
				{
					if (!IsSceneSourceEvent(Context->ActionKind, Context->MarkerPath))
					{
						ScheduleResourcePixelHistories(*Context);
					}
				}
				RenderCausalReport();
				TryResumeAgentAfterDeterministicContexts();
			}
			if (const FResourcePixelHistoryRequest* TraceRequest = PendingResourcePixelHistoryByRequest.Find(RequestId))
			{
				const FResourcePixelHistoryRequest FailedRequest = *TraceRequest;
				PendingResourcePixelHistoryByRequest.Remove(RequestId);
				FailedResourcePixelHistoryKeys.Add(FailedRequest.TraceKey);
				if (FEventContextEvidence* Context = EventContexts.Find(FailedRequest.ConsumerEventId))
				{
					TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
					Failure->SetNumberField(TEXT("consumerEventId"), FailedRequest.ConsumerEventId);
					Failure->SetNumberField(TEXT("resourceIndex"), FailedRequest.ResourceIndex);
					Failure->SetStringField(TEXT("resourceName"), FailedRequest.ResourceName);
					Failure->SetStringField(TEXT("shaderBinding"), FailedRequest.ShaderBinding);
					Failure->SetNumberField(TEXT("x"), FailedRequest.Pixel.X);
					Failure->SetNumberField(TEXT("y"), FailedRequest.Pixel.Y);
					Failure->SetNumberField(TEXT("sample"), FailedRequest.Sample);
					Failure->SetStringField(TEXT("branchStatus"), TEXT("query-failed"));
					Failure->SetStringField(TEXT("error"), Error);
					Context->ResourcePixelHistories.Add(MakeShared<FJsonValueObject>(Failure));
				}
				RenderCausalReport();
				TryResumeAgentAfterDeterministicContexts();
			}
			SetStatus(FString::Printf(TEXT("%s: %s"), *Stage, *Error));
		}
	}
}
