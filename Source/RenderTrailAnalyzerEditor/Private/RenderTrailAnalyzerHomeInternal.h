#pragma once

#include "RenderTrailAnalyzerHome.h"

#include "RenderTrailProtocol.h"
#include "RenderTrailAgentClient.h"
#include "RenderTrailAgentProtocol.h"
#include "RenderTrailAnalyzerDiagnostics.h"
#include "RenderTrailAnalyzerEvidence.h"
#include "RenderTrailAnalyzerImageView.h"
#include "RenderTrailAnalyzerPrompt.h"
#include "RenderTrailAnalyzerResultView.h"
#include "RenderTrailEvidenceFormatting.h"
#include "RenderTrailModelBrokerSettings.h"
#include "RenderTrailReplayWorkerClient.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "DesktopPlatformModule.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IDesktopPlatform.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Internationalization/Regex.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Rendering/DrawElements.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"


DECLARE_LOG_CATEGORY_EXTERN(LogRenderTrailAnalyzer, Log, All);

namespace UE::RenderTrail::Private
{
	struct FResourcePixelHistoryRequest
	{
		uint32 ConsumerEventId = 0;
		int32 ResourceIndex = INDEX_NONE;
		FString ResourceName;
		FString ShaderBinding;
		FString Mapping;
		FString MappingConfidence;
		FString CoordinateEvidence;
		FString TraceKey;
		FString ReplayKey;
		FString TracePurpose = TEXT("color");
		FIntPoint Pixel = FIntPoint::ZeroValue;
		TArray<FIntPoint> AlternatePixels;
		int32 Mip = 0;
		int32 Slice = 0;
		int32 Sample = 0;
		int32 TypeCast = INDEX_NONE;
		int32 ReverseDepth = 0;
		int32 Priority = 0;
		int32 CollapsedShaderAccessCount = 1;
		int32 AdaptiveAttempt = 0;
		int32 TotalAdaptiveCandidates = 1;
		double RepresentativeDistanceSquared = MAX_dbl;
		bool bRequiredForAgent = false;
		bool bExecutedShaderAccess = false;
	};

	struct FQueuedWorkerRequest
	{
		FString Command;
		FString RequestId;
		FString Payload;
		uint64 AnalysisGeneration = 0;
		uint64 QueueOrdinal = 0;
		double EnqueuedAtSeconds = 0.0;
		int32 Priority = 0;
	};

	class SAnalyzerHome final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SAnalyzerHome) {}
			SLATE_ARGUMENT(FString, InitialCapture)
		SLATE_END_ARGS()

		~SAnalyzerHome() override;

		void Construct(const FArguments& Args);

		TSharedRef<SWidget> BuildRootLayout(const FString& InitialCapture);

		TSharedRef<SWidget> BuildCaptureToolbar(const FString& InitialCapture);

		TSharedRef<SWidget> BuildImagePanel();

		TSharedRef<SWidget> BuildInspectorPanel();

		TSharedRef<SWidget> BuildAgentComposer();

		FReply ShowOverviewPage();

		FReply ShowEvidencePage();

		void OpenCapture(const FString& CapturePath);

		virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	private:
		FString GetCapturePath() const;

		void SetStatus(const FString& Value);

		FString BuildLiveReplayLogDisplay() const;

		void RefreshLiveReplayLogView();

		void AppendLiveReplayLog(const FString& Text);

		void ResetLiveReplayLog(const FString& Capture, int64 CaptureSize);

		void TailLiveReplayLogFile(const FString& Path, int32& InOutCharsRead);

		void PollLiveReplayLogs(double Now);

		FReply CopyStatusToClipboard();

		FReply ToggleLiveReplayLog();

		FReply OpenDiagnosticsDirectory();

		FReply OpenFullTraceDirectory();

		FReply ReleaseReplayResources();

		void SetCaptureLoadPhase(const FString& Phase);

		void FinishCaptureLoad(const FString& Result);

		bool HasPendingWorkerRequests() const;

		int32 GetPendingRequiredResourceHistoryCount() const;

		int32 GetPendingBackgroundResourceHistoryCount() const;

		int32 GetDeferredBackgroundResourceHistoryCount() const;

		int32 GetBudgetDeferredResourceHistoryCount() const;

		int32 GetDiscoveredResourceHistoryCount() const;

		FString BuildHistoryCoverageText() const;

		void FinishAutomaticDeepTraceIfIdle();

		bool IsCriticalAgentEvent(uint32 EventId) const;

		static int32 ComputeResourceTracePriority(const FResourcePixelHistoryRequest& Request);

		bool IsShaderDebugPending(uint32 EventId) const;

		bool QueueFocusedShaderDebug(uint32 EventId);

		FAgentContextCoverageSelection BuildAgentContextCoverageSelection() const;

		static TArray<FString> GetAgentContextCoverageRoles(const FAgentContextCoverageEvidence& Coverage);

		int32 GetPendingCriticalContextCount() const;

		bool HasPendingCriticalDeterministicQueries() const;

		bool HasPendingBackgroundDeterministicQueries() const;

		void WriteWorkerHeartbeat(double Now, const TCHAR* Activity);

		void SetEvidence(const FString& Value);

		void SetReportCards(const FString& Summary, const FString& CausalPath, const FString& Suspects,
			const FString& TechnicalEvidence);

		void SetAgentOutputText(const FString& Value);

		void SetAgentStatus(const FString& Status);

		static FString SerializeJson(const TSharedRef<FJsonObject>& Object);

		static FString SerializeJsonPretty(const TSharedRef<FJsonObject>& Object);

		void InitializeFullTraceArtifact();

		void AppendFullTraceRecord(const FString& Direction, const TSharedRef<FJsonObject>& Payload);

		static TSharedRef<FJsonObject> BuildFullBoundResourceJson(const FBoundResourceEvidence& Resource);

		TSharedRef<FJsonObject> BuildFullEventContextJson(const FEventContextEvidence& Context) const;

		void WriteFullTraceSnapshot(const FString& CompletionState);

		void AddAgentMessage(const FString& Role, const FString& Content);

		void AddAgentMessage(const FString& Role, const FString& Content, const FString& ReasoningContent);

		TArray<TSharedPtr<FJsonObject>> SelectAgentResourceHistoriesForCoverage(
			const FEventContextEvidence& Context) const;

		static TArray<int32> SelectAgentBoundResourcesForCoverage(
			const TArray<FBoundResourceEvidence>& Resources, int32 MaxResources);

		TArray<TSharedPtr<FJsonObject>> SelectAgentResourceProvenanceForCoverage(
			const FEventContextEvidence& Context) const;

		TSharedRef<FJsonObject> BuildCompactResourceHistoryForAgent(const TSharedPtr<FJsonObject>& History) const;

		TSharedRef<FJsonObject> BuildCompactShaderDebugForAgent(const TSharedPtr<FJsonObject>& Trace) const;

		TSharedRef<FJsonObject> BuildCompactResourceProvenanceForAgent(
			const TSharedPtr<FJsonObject>& Provenance) const;

		FString BuildAgentPrefilterEvidence() const;

		bool AgentEvidenceContainsEvent(uint32 EventId) const;

		FString BuildAgentEventObservation(uint32 EventId) const;

		const FPixelSample* FindAgentSample(const FString& Label) const;

		FString BuildAgentSampleObservation(const FString& Label) const;

		FReply StartAgentAnalysis();

		FReply RunPrimaryAnalysis();

		void SendAgentTurn();


		void SendAgentBrokerCompletion();



		void HandleAgentBrokerResponse(FRenderTrailAgentResponse&& Response);



		bool NormalizeAnswerOnlyAgentObject(const TSharedPtr<FJsonObject>& Action);

		void HandleAgentAction(const FString& Content, const FString& ReasoningContent = FString());

		void ResumeAgentAfterEventContext(uint32 EventId);

		TArray<FRenderTrailResultLane> BuildDeterministicResultLanes() const;

		void DisplayAgentFinal(const TSharedRef<FJsonObject>& Final);

		void FinishAgentWithError(const FString& Error);

		void CancelAgentRun();

		FReply ClearSamples();

		FReply ClearCurrentInfo();

		FReply ConfirmPixelSelection();

		void MarkReplaySynchronizationPending();

		void CancelQueuedWorkerRequestsForNewAnalysisGeneration(const TCHAR* Reason);

		void ResetSamples();

		void UpdateSelectionText();

		void UpdateMarkers();

		FPixelSample* FindSample(uint64 SampleId);

		FReply BrowseCapture();

		FReply LoadCapture();


		bool LaunchWorkerProcess(const FString& Worker, const FString& Capture, const FString& InPreviewPath);

		void StartWorker(bool bPreserveSelection = false);

		void StopWorker();

		void PollWorkerPipes();

		bool TryDispatchNextWorkerRequest();

		bool SendWorkerRequest(const FString& Command, const FString& RequestId,
			TFunctionRef<void(const TSharedRef<FJsonObject>&)> Populate, int32 Priority = 0);

		double CompleteWorkerRequest(const FString& RequestId, const FString& ResultType, const FString& Detail = FString());

		void ReleasePreview();

		static bool IsPreviewCacheValid(const FString& CapturePath, const FString& InPreviewPath);

		static bool TryGetPixelExactPreviewSize(
			const FString& CapturePath, const FString& InPreviewPath, FIntPoint& OutSize);

		bool LoadPreview(const FString& Path, FIntPoint ExpectedSize);

		void HandleWorkerMessage(const FString& Line);

		void QueryPixel(int32 X, int32 Y);

		void StoreEventContext(const TSharedRef<FJsonObject>& Message);

		void AddResourceTraceBoundary(FEventContextEvidence& Context, const FResourcePixelHistoryRequest& TraceRequest,
			const FString& Status, const FString& Detail);

		void RemoveResourceTraceBoundary(FEventContextEvidence& Context, const FString& TraceKey,
			const FString& Status);

		void ScheduleResourcePixelHistory(FEventContextEvidence& Context,
			const FResourcePixelHistoryRequest& TraceRequest);

		void ScheduleResourcePixelHistories(const FEventContextEvidence& ReadOnlyContext);

		int32 ScheduleExecutedTextureAccessHistories(FEventContextEvidence& Context);

		void EnrichResourcePixelHistoryFromShaderTrace(const FEventContextEvidence& Context,
			const TSharedRef<FJsonObject>& Evidence) const;

		void StoreResourcePixelHistory(const TSharedRef<FJsonObject>& Message);

		void StoreShaderDebug(const TSharedRef<FJsonObject>& Message);

		void EnsureEventContext(uint32 EventId, int32 ReverseDepth = 0);

		void ScheduleProducerEventContexts(const FEventContextEvidence& Context);

		void EnsureRelevantEventContexts();

		void EnsureCandidateShaderDebug();

		void TryResumeAgentAfterDeterministicContexts();

		void StorePixelHistory(const TSharedRef<FJsonObject>& Message);

		static void AddHypothesis(TArray<FString>& Hypotheses, const FString& Hypothesis);

		static void AddFailureHypothesis(const FString& Failure, TArray<FString>& Hypotheses);

		void RenderCausalReport();
		TSharedPtr<SEditableTextBox> CapturePathBox;
		TSharedPtr<SMultiLineEditableTextBox> StatusText;
		TSharedPtr<STextBlock> SelectionText;
		TSharedPtr<SMultiLineEditableText> EvidenceText;
		TSharedPtr<SMultiLineEditableTextBox> AgentIntentTextBox;
		TSharedPtr<SMultiLineEditableText> AgentOutputText;
		TSharedPtr<SMultiLineEditableText> AgentStatusText;
		TSharedPtr<STextBlock> AgentRunButtonText;
		TSharedPtr<SWidgetSwitcher> InspectorSwitcher;
		TSharedPtr<STextBlock> OverviewTabText;
		TSharedPtr<STextBlock> EvidenceTabText;
		TSharedPtr<SRenderTrailAnalyzerResultView> AgentResultView;
		TSharedPtr<SRenderTrailImageView> ImageView;
		TSharedPtr<FSlateDynamicImageBrush> PreviewBrush;
		TArray<FPixelSample> Samples;
		TMap<FString, uint64> PendingSampleByRequest;
		TMap<uint32, FEventContextEvidence> EventContexts;
		TMap<uint32, int32> EventContextDepths;
		TMap<FString, uint32> PendingEventContextByRequest;
		TMap<FString, uint32> PendingShaderDebugByRequest;
		TMap<FString, FResourcePixelHistoryRequest> PendingResourcePixelHistoryByRequest;
		TMap<FString, FResourcePixelHistoryRequest> BudgetDeferredResourcePixelHistoryRequests;
		TMap<FString, TArray<FString>> ResourcePixelHistoryBindingAliases;
		TSet<uint32> DeferredResourceHistoryContextIds;
		TMap<uint32, int32> DeferredResourceHistoryBranchCounts;
		TSet<uint32> DeferredEventContextIds;
		TMap<uint32, int32> BudgetDeferredEventContextDepths;
		TMap<FString, double> WorkerRequestQueuedSeconds;
		TMap<FString, FString> WorkerRequestCommands;
		TArray<FQueuedWorkerRequest> QueuedWorkerRequests;
		TMap<uint32, FIntPoint> EventTracePixels;
		TMap<uint32, uint32> EventTracePrimitiveIds;
		TSet<uint32> EventTracePrimitiveEvidenceIds;
		TSet<uint32> FocusedTraceEventIds;
		TSet<FString> ScheduledResourcePixelHistoryKeys;
		TSet<FString> FailedResourcePixelHistoryKeys;
		TSet<uint32> PendingEventContextIds;
		TSet<uint32> FailedEventContextIds;
		TSet<uint32> FailedShaderDebugIds;
		TOptional<FCausalCandidate> LastCandidate;
		TOptional<FCausalCandidate> LastSignificantCandidate;
		FString LastReportSummary;
		FString LastReportCausalPath;
		FString LastAgentQuestion;
		FString LastAgentAnswer;
		TArray<TSharedPtr<FJsonValue>> AgentMessages;
		TArray<TSharedPtr<FJsonValue>> FullTraceRecords;
		TSharedPtr<FJsonObject> FullTraceTargetPixelHistory;
		TSharedPtr<FRenderTrailAgentClient> AgentClient;
		TOptional<uint32> AgentPendingEventId;
		FString LastWorkerError;
		FRenderTrailAnalyzerDiagnostics Diagnostics;
		FRenderTrailReplayWorkerClient ReplayWorker;
		FString NativePreviewPath;
		FString PreviewPath;
		FString CurrentStatus = TEXT("就绪。");
		FString LiveReplayLog;
		FString WorkerDiagnosticsTailPath;
		FString RenderDocDiagnosticsTailPath;
		FString FullTraceDirectory;
		FString FullTraceJsonlPath;
		FString FullTraceSnapshotPath;
		FString FullTraceCreatedAt;
		FString ReplayTargetFormat;
		FString ActiveWorkerRequestId;
		FString ActiveWorkerStage;
		FString DispatchedWorkerRequestId;
		FIntPoint CurrentPreviewSize = FIntPoint::ZeroValue;
		int32 ReplayTargetResourceIndex = INDEX_NONE;
		int32 ReplayTargetSamples = 1;
		int32 ResourcePixelHistoryQueriesSubmitted = 0;
		int32 ResourcePixelHistoryQueryLimit = MaxAutomaticResourcePixelHistoryQueries;
		int32 DeterministicContextLimit = MaxAutomaticDeterministicContextEvents;
		int32 WorkerDiagnosticsCharsRead = 0;
		int32 RenderDocDiagnosticsCharsRead = 0;
		uint64 PreviewSerial = 0;
		uint64 RequestSerial = 0;
		uint64 SampleSerial = 0;
		int32 AgentStep = 0;
		int32 FullTraceRecordCount = 0;
		int32 FullTraceRequestCount = 0;
		int32 FullTraceResponseCount = 0;
		bool bWorkerReady = false;
		bool bAgentRunning = false;
		bool bAgentWaitingForDeterministicContexts = false;
		bool bAgentResultDisplayed = false;
		bool bDeterministicForegroundCompletionReported = false;
		bool bShaderDebuggingAvailable = false;
		bool bSelectionConfirmed = false;
		bool bLastCandidateHasDivergence = false;
		bool bCaptureLoading = false;
		bool bPreviewReadyForSelection = false;
		bool bReplaySynchronizationPending = false;
		bool bReplayStartDeferred = false;
		bool bQueuePixelHistoryAfterWorkerReady = false;
		bool bLiveReplayLogPaused = false;
		bool bPollingWorkerPipes = false;
		bool bFullTraceWriteFailed = false;
		double CaptureLoadStartSeconds = 0.0;
		double LastCaptureLoadStatusSeconds = 0.0;
		double LastWorkerHeartbeatSeconds = 0.0;
		double LastReplayQueryStatusSeconds = 0.0;
		double ActiveWorkerRequestStartSeconds = 0.0;
		double LastLiveReplayLogPollSeconds = 0.0;
		uint64 WorkerQueueOrdinal = 0;
		uint64 AnalysisGeneration = 1;
		uint64 DispatchedWorkerRequestGeneration = 0;
		FString CaptureLoadPhase;
		FString LastWorkerDiagnosticPhase;
		static constexpr int32 MaxLiveReplayLogChars = 128 * 1024;
		static constexpr int32 MaxPixelSamples = 1;
		static constexpr int32 MaxDisplayedTraceHops = 24;
		static constexpr int32 MaxDisplayedFrontierResources = 6;
		static constexpr int32 MaxAgentPrefilterEventsPerSample = 6;
		static constexpr int32 MaxAgentEventChainPerSample = 48;
		static constexpr int32 MaxAgentEventsPerResourceHistory = 4;
		static constexpr int32 MaxAgentTextureAccesses = 24;
		static constexpr int32 MaxAgentDeterministicContexts = 12;
		static constexpr int32 MaxAgentCausalLaneBranches = 12;
		static constexpr int32 MaxDisplayedResultLaneBranches = 8;
		static constexpr int32 MaxAgentBoundResourcesPerContext = 8;
		static constexpr int32 MaxAgentResourceHistoriesPerContext = 8;
		static constexpr int32 MaxAgentContextIndexLinks = 8;
		static constexpr int32 MaxAgentContextIndexMarkerChars = 180;
		static constexpr int32 MaxAutomaticDeterministicContextEvents = 24;
		static constexpr int32 MaxAutomaticResourcePixelHistoryQueries = 64;
		static constexpr int32 MaxSamplesPerResourceTrace = 4;
		static constexpr int32 MaxWriterContextsPerResourceTrace = 1;
		static constexpr int32 MaxExecutedTextureBranchesPerEvent = 12;
		static constexpr int32 MaxAdaptivePixelsPerResourceTrace = 4;
		static constexpr int32 MaxFallbackTextureBranchesPerEvent = 2;
		static constexpr int32 MaxAgentSteps = 1;
	};
}
