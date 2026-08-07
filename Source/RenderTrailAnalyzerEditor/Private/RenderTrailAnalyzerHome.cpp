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
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "IDesktopPlatform.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogRenderTrailAnalyzer, Log, All);

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
		FString TraceKey;
		FIntPoint Pixel = FIntPoint::ZeroValue;
		int32 Mip = 0;
		int32 Slice = 0;
		int32 Sample = 0;
		int32 TypeCast = INDEX_NONE;
		int32 ReverseDepth = 0;
		bool bRequiredForAgent = false;
	};

	class SAnalyzerHome final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SAnalyzerHome) {}
			SLATE_ARGUMENT(FString, InitialCapture)
		SLATE_END_ARGS()

		~SAnalyzerHome() override
		{
			CancelAgentRun();
			StopWorker();
			ReleasePreview();
		}

		void Construct(const FArguments& Args)
		{
			Diagnostics.LoadConfiguration();
			AgentClient = MakeShared<FRenderTrailAgentClient>();

			ChildSlot
			[
				BuildRootLayout(Args._InitialCapture)
			];

			if (!Args._InitialCapture.IsEmpty() && FPaths::FileExists(Args._InitialCapture))
			{
				StartWorker();
			}
		}

		TSharedRef<SWidget> BuildRootLayout(const FString& InitialCapture)
		{
			return SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(FLinearColor(0.035f, 0.04f, 0.05f, 1.0f))
				.Padding(8.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 7)
					[
						BuildCaptureToolbar(InitialCapture)
					]
					+ SVerticalBox::Slot().FillHeight(1.0f)
					[
						SNew(SSplitter)
						+ SSplitter::Slot().Value(0.64f)
						[
							BuildImagePanel()
						]
						+ SSplitter::Slot().Value(0.36f)
						[
							BuildInspectorPanel()
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
					[
						SAssignNew(StatusText, STextBlock)
						.Text(FText::FromString(TEXT("就绪。")))
						.AutoWrapText(true)
						.ColorAndOpacity(FLinearColor(0.56f, 0.63f, 0.72f))
					]
				];
		}

		TSharedRef<SWidget> BuildCaptureToolbar(const FString& InitialCapture)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("RenderTrail")))
					.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 16))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SAssignNew(CapturePathBox, SEditableTextBox)
					.Text(FText::FromString(InitialCapture))
					.HintText(FText::FromString(TEXT("选择一个 .rdc 截帧")))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(7, 0, 0, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("更换截帧")))
					.OnClicked(this, &SAnalyzerHome::BrowseCapture)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(7, 0, 0, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("载入 / 重载")))
					.OnClicked(this, &SAnalyzerHome::LoadCapture)
				];
		}

		TSharedRef<SWidget> BuildImagePanel()
		{
			return SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(FLinearColor(0.015f, 0.018f, 0.022f, 1.0f))
				.Padding(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().FillHeight(1.0f)
					[
						SAssignNew(ImageView, SRenderTrailImageView)
						.OnPixelPicked(FOnPixelPicked::CreateSP(this, &SAnalyzerHome::QueryPixel))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8, 6)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("单击选择或替换 P1 · 滚轮缩放 · 中键/右键平移")))
						.ColorAndOpacity(FLinearColor(0.52f, 0.59f, 0.67f))
					]
				];
		}

		TSharedRef<SWidget> BuildInspectorPanel()
		{
			return SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(FLinearColor(0.025f, 0.03f, 0.04f, 1.0f))
				.Padding(10.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SAssignNew(SelectionText, STextBlock)
							.Text(FText::FromString(TEXT("尚未选择像素")))
							.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11))
							.AutoWrapText(true)
							.ColorAndOpacity(FLinearColor(0.80f, 0.86f, 0.93f))
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("清除")))
							.ToolTipText(FText::FromString(TEXT("清除当前像素及其分析结果。")))
							.OnClicked(this, &SAnalyzerHome::ClearSamples)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("分析当前像素")))
							.ToolTipText(FText::FromString(TEXT("确认当前 P1 并读取 Pixel History；不会自动运行 Agent。")))
							.OnClicked(this, &SAnalyzerHome::ConfirmPixelSelection)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 7)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(SButton)
							.OnClicked(this, &SAnalyzerHome::ShowOverviewPage)
							[
								SAssignNew(OverviewTabText, STextBlock)
								.Text(FText::FromString(TEXT("● 结论与链条")))
							]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(5, 0, 0, 0)
						[
							SNew(SButton)
							.OnClicked(this, &SAnalyzerHome::ShowEvidencePage)
							[
								SAssignNew(EvidenceTabText, STextBlock)
								.Text(FText::FromString(TEXT("技术证据")))
							]
						]
					]
					+ SVerticalBox::Slot().FillHeight(1.0f)
					[
						SAssignNew(InspectorSwitcher, SWidgetSwitcher)
						.WidgetIndex(0)
						+ SWidgetSwitcher::Slot()
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot().Padding(0, 0, 2, 0)
							[
								SAssignNew(AgentResultView, SRenderTrailAnalyzerResultView)
							]
						]
						+ SWidgetSwitcher::Slot()
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot().Padding(2)
							[
								SAssignNew(EvidenceText, SMultiLineEditableText)
								.Text(FText::FromString(TEXT("尚无技术证据。")))
								.AutoWrapText(true)
								.IsReadOnly(true)
								.AllowContextMenu(true)
								.ClearTextSelectionOnFocusLoss(false)
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
					[
						BuildAgentComposer()
					]
				];
		}

		TSharedRef<SWidget> BuildAgentComposer()
		{
			return SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(FLinearColor(0.085f, 0.05f, 0.115f, 1.0f))
				.Padding(9.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("询问 Agent · 上下文固定为当前 P1")))
							.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 10))
							.ColorAndOpacity(FLinearColor(0.78f, 0.58f, 1.0f))
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("清空回答")))
							.OnClicked(this, &SAnalyzerHome::ClearCurrentInfo)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 5)
					[
						SAssignNew(AgentOutputText, SMultiLineEditableText)
						.Text(FText::FromString(TEXT("完成规则分析后，可围绕最终写入、Pass、Pipeline、Shader 或链条断点继续提问。")))
						.AutoWrapText(true)
						.IsReadOnly(true)
						.AllowContextMenu(true)
						.ClearTextSelectionOnFocusLoss(false)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox)
						.HeightOverride(58.0f)
						[
							SAssignNew(AgentIntentTextBox, SMultiLineEditableTextBox)
							.HintText(FText::FromString(TEXT("例如：为什么它只是末端微调？上游链在哪一步中断？")))
							.AutoWrapText(true)
							.AllowMultiLine(true)
							.ClearTextSelectionOnFocusLoss(false)
							.AllowContextMenu(true)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SAssignNew(AgentStatusText, SMultiLineEditableText)
							.Text(FText::FromString(TEXT("未运行 · 只发送当前像素摘要；.rdc/图像不上传")))
							.AutoWrapText(true)
							.IsReadOnly(true)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
						[
							SNew(SButton)
							.ToolTipText(FText::FromString(TEXT("发送当前问题与已收集的 P1 有界证据。")))
							.OnClicked(this, &SAnalyzerHome::StartAgentAnalysis)
							[
								SAssignNew(AgentRunButtonText, STextBlock)
								.Text(FText::FromString(TEXT("发送")))
							]
						]
					]
				];
		}

		FReply ShowOverviewPage()
		{
			if (InspectorSwitcher.IsValid())
			{
				InspectorSwitcher->SetActiveWidgetIndex(0);
			}
			if (OverviewTabText.IsValid())
			{
				OverviewTabText->SetText(FText::FromString(TEXT("● 结论与链条")));
			}
			if (EvidenceTabText.IsValid())
			{
				EvidenceTabText->SetText(FText::FromString(TEXT("技术证据")));
			}
			return FReply::Handled();
		}

		FReply ShowEvidencePage()
		{
			if (InspectorSwitcher.IsValid())
			{
				InspectorSwitcher->SetActiveWidgetIndex(1);
			}
			if (OverviewTabText.IsValid())
			{
				OverviewTabText->SetText(FText::FromString(TEXT("结论与链条")));
			}
			if (EvidenceTabText.IsValid())
			{
				EvidenceTabText->SetText(FText::FromString(TEXT("● 技术证据")));
			}
			return FReply::Handled();
		}

		void OpenCapture(const FString& CapturePath)
		{
			if (!CapturePathBox.IsValid())
			{
				return;
			}
			CapturePathBox->SetText(FText::FromString(FPaths::ConvertRelativePathToFull(CapturePath)));
			StartWorker();
		}

		virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
		{
			SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
			PollWorkerPipes();
			if (bCaptureLoading)
			{
				const double Now = FPlatformTime::Seconds();
				if (Now - LastCaptureLoadStatusSeconds >= 0.5)
				{
					SetStatus(FString::Printf(TEXT("正在载入截帧… %.1fs · %s"),
						Now - CaptureLoadStartSeconds, *CaptureLoadPhase));
					LastCaptureLoadStatusSeconds = Now;
				}
				if (Now - LastWorkerHeartbeatSeconds >= 5.0)
				{
					WriteWorkerHeartbeat(Now, TEXT("capture_load"));
				}
			}
			else if (ReplayWorker.IsRunning() && HasPendingWorkerRequests())
			{
				const double Now = FPlatformTime::Seconds();
				if (!ActiveWorkerRequestId.IsEmpty() && Now - LastReplayQueryStatusSeconds >= 0.5)
				{
					const double RequestElapsed = ActiveWorkerRequestStartSeconds > 0.0
						? Now - ActiveWorkerRequestStartSeconds : 0.0;
					const FString QueryStatus = FString::Printf(
						TEXT("Replay 查询中：%s · %s · %.1fs；必需 %d，后台 %d，事件 %d，Shader %d"),
						*ActiveWorkerRequestId, ActiveWorkerStage.IsEmpty() ? TEXT("waiting") : *ActiveWorkerStage,
						RequestElapsed, GetPendingRequiredResourceHistoryCount(), GetPendingBackgroundResourceHistoryCount(),
						PendingEventContextIds.Num(), PendingShaderDebugByRequest.Num());
					SetStatus(QueryStatus);
					if (bAgentWaitingForDeterministicContexts)
					{
						SetAgentStatus(TEXT("快速前沿 · ") + QueryStatus);
					}
					LastReplayQueryStatusSeconds = Now;
				}
				if (Now - LastWorkerHeartbeatSeconds >= 5.0)
				{
					WriteWorkerHeartbeat(Now, TEXT("replay_query"));
				}
			}
		}

	private:
		FString GetCapturePath() const
		{
			return CapturePathBox.IsValid() ? CapturePathBox->GetText().ToString() : FString();
		}

		void SetStatus(const FString& Value)
		{
			if (StatusText.IsValid())
			{
				StatusText->SetText(FText::FromString(Value));
			}
		}

		void SetCaptureLoadPhase(const FString& Phase)
		{
			CaptureLoadPhase = Phase;
			if (bCaptureLoading)
			{
				const double Elapsed = FPlatformTime::Seconds() - CaptureLoadStartSeconds;
				SetStatus(FString::Printf(TEXT("正在载入截帧… %.1fs · %s"), Elapsed, *CaptureLoadPhase));
				UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Capture load phase: %s (elapsed=%.3fs)"), *Phase, Elapsed);
			}
		}

		void FinishCaptureLoad(const FString& Result)
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

		bool HasPendingWorkerRequests() const
		{
			return !PendingSampleByRequest.IsEmpty()
				|| !PendingEventContextByRequest.IsEmpty()
				|| !PendingShaderDebugByRequest.IsEmpty()
				|| !PendingResourcePixelHistoryByRequest.IsEmpty();
		}

		int32 GetPendingRequiredResourceHistoryCount() const
		{
			int32 Count = 0;
			for (const TPair<FString, FResourcePixelHistoryRequest>& Pair : PendingResourcePixelHistoryByRequest)
			{
				Count += Pair.Value.bRequiredForAgent ? 1 : 0;
			}
			return Count;
		}

		int32 GetPendingBackgroundResourceHistoryCount() const
		{
			int32 Count = PendingResourcePixelHistoryByRequest.Num() - GetPendingRequiredResourceHistoryCount();
			for (const TPair<uint32, int32>& Pair : DeferredResourceHistoryBranchCounts)
			{
				Count += Pair.Value;
			}
			return Count;
		}

		bool IsCriticalAgentEvent(uint32 EventId) const
		{
			return (LastCandidate.IsSet() && LastCandidate->Event.EventId == EventId)
				|| (LastSignificantCandidate.IsSet() && LastSignificantCandidate->Event.EventId == EventId);
		}

		int32 GetPendingCriticalContextCount() const
		{
			int32 Count = 0;
			for (const uint32 EventId : PendingEventContextIds)
			{
				Count += IsCriticalAgentEvent(EventId) ? 1 : 0;
			}
			return Count;
		}

		bool HasPendingCriticalDeterministicQueries() const
		{
			if (!PendingShaderDebugByRequest.IsEmpty() || GetPendingRequiredResourceHistoryCount() > 0)
			{
				return true;
			}
			return GetPendingCriticalContextCount() > 0;
		}

		bool HasPendingBackgroundDeterministicQueries() const
		{
			if (GetPendingBackgroundResourceHistoryCount() > 0 || !DeferredEventContextIds.IsEmpty())
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

		void WriteWorkerHeartbeat(double Now, const TCHAR* Activity)
		{
			const double Elapsed = bCaptureLoading ? Now - CaptureLoadStartSeconds : 0.0;
			const FString Phase = LastWorkerDiagnosticPhase.IsEmpty() ? CaptureLoadPhase : LastWorkerDiagnosticPhase;
			const FString Detail = FString::Printf(
				TEXT("activity=%s elapsed=%.3fs running=%s phase='%s' request='%s' requestElapsed=%.3fs pendingPixel=%d pendingContext=%d pendingShader=%d pendingResourceRequired=%d pendingResourceBackground=%d"),
				Activity, Elapsed, ReplayWorker.IsRunning() ? TEXT("true") : TEXT("false"), *Phase,
				*ActiveWorkerRequestId, ActiveWorkerRequestStartSeconds > 0.0 ? Now - ActiveWorkerRequestStartSeconds : 0.0,
				PendingSampleByRequest.Num(), PendingEventContextByRequest.Num(), PendingShaderDebugByRequest.Num(),
				GetPendingRequiredResourceHistoryCount(), GetPendingBackgroundResourceHistoryCount());
			UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Replay Worker heartbeat: %s"), *Detail);
			if (Diagnostics.HasSession() && Diagnostics.GetOptions().bWorkerProtocol)
			{
				Diagnostics.WriteRecord(TEXT("worker_wait"), Detail);
			}
			LastWorkerHeartbeatSeconds = Now;
		}

		void SetEvidence(const FString& Value)
		{
			SetReportCards(Value, TEXT("选择关注像素后生成。"), TEXT("尚无候选原因。"), Value);
		}

		void SetReportCards(const FString& Summary, const FString& CausalPath, const FString& Suspects,
			const FString& TechnicalEvidence)
		{
			LastReportSummary = Summary;
			LastReportCausalPath = CausalPath;
			if (AgentResultView.IsValid())
			{
				AgentResultView->SetDeterministicReport(Summary, CausalPath, Suspects);
			}
			if (EvidenceText.IsValid())
				EvidenceText->SetText(FText::FromString(TechnicalEvidence));
		}

		void SetAgentOutputText(const FString& Value)
		{
			if (AgentOutputText.IsValid())
			{
				AgentOutputText->SetText(FText::FromString(Value));
			}
		}

		void SetAgentStatus(const FString& Status)
		{
			if (AgentStatusText.IsValid())
			{
				AgentStatusText->SetText(FText::FromString(Status));
			}
		}

		static FString SerializeJson(const TSharedRef<FJsonObject>& Object)
		{
			FString Result;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
			FJsonSerializer::Serialize(Object, Writer);
			return Result;
		}

		void AddAgentMessage(const FString& Role, const FString& Content)
		{
			AddAgentMessage(Role, Content, FString());
		}

		void AddAgentMessage(const FString& Role, const FString& Content, const FString& ReasoningContent)
		{
			TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
			Message->SetStringField(TEXT("role"), Role);
			Message->SetStringField(TEXT("content"), Content);
			if (!ReasoningContent.IsEmpty())
			{
				Message->SetStringField(TEXT("reasoning_content"), ReasoningContent);
			}
			AgentMessages.Add(MakeShared<FJsonValueObject>(Message));
		}

		TSharedRef<FJsonObject> BuildCompactResourceHistoryForAgent(const TSharedPtr<FJsonObject>& History) const
		{
			TSharedRef<FJsonObject> Compact = MakeShared<FJsonObject>();
			if (!History.IsValid())
			{
				Compact->SetStringField(TEXT("branchStatus"), TEXT("invalid-history"));
				return Compact;
			}
			static const TCHAR* StringFields[] = {
				TEXT("resourceName"), TEXT("shaderBinding"), TEXT("coordinateMapping"), TEXT("mappingConfidence"),
				TEXT("branchStatus"), TEXT("detail"), TEXT("error"), TEXT("shaderAccessDisassembly") };
			for (const TCHAR* Field : StringFields)
			{
				FString Value;
				if (History->TryGetStringField(Field, Value))
				{
					Compact->SetStringField(Field, Value.Left(800));
				}
			}
			static const TCHAR* NumberFields[] = {
				TEXT("consumerEventId"), TEXT("resourceIndex"), TEXT("x"), TEXT("y"), TEXT("mip"), TEXT("slice"),
				TEXT("sample"), TEXT("totalModifications"), TEXT("totalEvents"), TEXT("shaderAccessInstruction"),
				TEXT("queueToResponseSeconds") };
			for (const TCHAR* Field : NumberFields)
			{
				double Value = 0.0;
				if (History->TryGetNumberField(Field, Value))
				{
					Compact->SetNumberField(Field, Value);
				}
			}
			static const TCHAR* BoolFields[] = {
				TEXT("requiredForAgent"), TEXT("detailTailTruncated"), TEXT("shaderAccessObserved"),
				TEXT("shaderCoordinateValuesMatched") };
			for (const TCHAR* Field : BoolFields)
			{
				bool bValue = false;
				if (History->TryGetBoolField(Field, bValue))
				{
					Compact->SetBoolField(Field, bValue);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Aliases = nullptr;
			if (History->TryGetArrayField(TEXT("shaderBindingAliases"), Aliases) && Aliases)
			{
				Compact->SetArrayField(TEXT("shaderBindingAliases"), *Aliases);
			}
			const TArray<TSharedPtr<FJsonValue>>* WriterIds = nullptr;
			if (History->TryGetArrayField(TEXT("confirmedWriterEventIds"), WriterIds) && WriterIds)
			{
				Compact->SetArrayField(TEXT("confirmedWriterEventIds"), *WriterIds);
			}

			const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
			TArray<TSharedPtr<FJsonValue>> CompactEvents;
			if (History->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
			{
				const int32 First = FMath::Max(0, EventSummaries->Num() - MaxAgentEventsPerResourceHistory);
				for (int32 Index = First; Index < EventSummaries->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Event = (*EventSummaries)[Index].IsValid()
						? (*EventSummaries)[Index]->AsObject() : nullptr;
					if (!Event.IsValid())
					{
						continue;
					}
					TSharedRef<FJsonObject> CompactEvent = MakeShared<FJsonObject>();
					double Number = 0.0;
					FString TextValue;
					bool bValue = false;
					if (Event->TryGetNumberField(TEXT("eventId"), Number)) CompactEvent->SetNumberField(TEXT("eventId"), Number);
					if (Event->TryGetNumberField(TEXT("passedFragments"), Number)) CompactEvent->SetNumberField(TEXT("passedFragments"), Number);
					if (Event->TryGetNumberField(TEXT("rejectedFragments"), Number)) CompactEvent->SetNumberField(TEXT("rejectedFragments"), Number);
					if (Event->TryGetStringField(TEXT("action"), TextValue)) CompactEvent->SetStringField(TEXT("action"), TextValue);
					if (Event->TryGetStringField(TEXT("actionKind"), TextValue)) CompactEvent->SetStringField(TEXT("actionKind"), TextValue);
					if (Event->TryGetStringField(TEXT("markerPath"), TextValue)) CompactEvent->SetStringField(TEXT("marker"), CompactMarkerPath(TextValue));
					if (Event->TryGetBoolField(TEXT("changedTextureValue"), bValue)) CompactEvent->SetBoolField(TEXT("changedTextureValue"), bValue);
					if (Event->TryGetBoolField(TEXT("directShaderWrite"), bValue)) CompactEvent->SetBoolField(TEXT("directShaderWrite"), bValue);
					const TSharedPtr<FJsonObject>* PixelValue = nullptr;
					if (Event->TryGetObjectField(TEXT("firstBefore"), PixelValue) && PixelValue) CompactEvent->SetObjectField(TEXT("firstBefore"), *PixelValue);
					if (Event->TryGetObjectField(TEXT("lastShaderOutput"), PixelValue) && PixelValue) CompactEvent->SetObjectField(TEXT("lastShaderOutput"), *PixelValue);
					if (Event->TryGetObjectField(TEXT("lastAfter"), PixelValue) && PixelValue) CompactEvent->SetObjectField(TEXT("lastAfter"), *PixelValue);
					CompactEvents.Add(MakeShared<FJsonValueObject>(CompactEvent));
				}
			}
			Compact->SetArrayField(TEXT("latestEventSummaries"), MoveTemp(CompactEvents));
			return Compact;
		}

		TSharedRef<FJsonObject> BuildCompactShaderDebugForAgent(const TSharedPtr<FJsonObject>& Trace) const
		{
			TSharedRef<FJsonObject> Compact = MakeShared<FJsonObject>();
			if (!Trace.IsValid())
			{
				return Compact;
			}
			static const TCHAR* StringFields[] = { TEXT("stage"), TEXT("debugStatus") };
			for (const TCHAR* Field : StringFields)
			{
				FString Value;
				if (Trace->TryGetStringField(Field, Value)) Compact->SetStringField(Field, Value);
			}
			static const TCHAR* NumberFields[] = { TEXT("eventId"), TEXT("x"), TEXT("y"), TEXT("sample"),
				TEXT("instructionInfoCount"), TEXT("sourceVariableMappingCount"), TEXT("stepCount"), TEXT("textureAccessCount") };
			for (const TCHAR* Field : NumberFields)
			{
				double Value = 0.0;
				if (Trace->TryGetNumberField(Field, Value)) Compact->SetNumberField(Field, Value);
			}
			bool bCompleted = false;
			if (Trace->TryGetBoolField(TEXT("completed"), bCompleted)) Compact->SetBoolField(TEXT("completed"), bCompleted);
			const TArray<TSharedPtr<FJsonValue>>* Accesses = nullptr;
			TArray<TSharedPtr<FJsonValue>> CompactAccesses;
			if (Trace->TryGetArrayField(TEXT("textureAccesses"), Accesses) && Accesses)
			{
				for (int32 Index = 0; Index < Accesses->Num() && Index < MaxAgentTextureAccesses; ++Index)
				{
					const TSharedPtr<FJsonObject> Access = (*Accesses)[Index].IsValid() ? (*Accesses)[Index]->AsObject() : nullptr;
					if (!Access.IsValid()) continue;
					TSharedRef<FJsonObject> CompactAccess = MakeShared<FJsonObject>();
					double Number = 0.0;
					FString Disassembly;
					if (Access->TryGetNumberField(TEXT("instruction"), Number)) CompactAccess->SetNumberField(TEXT("instruction"), Number);
					if (Access->TryGetStringField(TEXT("disassembly"), Disassembly)) CompactAccess->SetStringField(TEXT("disassembly"), Disassembly.Left(600));
					const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
					TArray<TSharedPtr<FJsonValue>> CompactVariables;
					if (Access->TryGetArrayField(TEXT("variables"), Variables) && Variables)
					{
						for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
						{
							const TSharedPtr<FJsonObject> Variable = VariableValue.IsValid() ? VariableValue->AsObject() : nullptr;
							if (!Variable.IsValid()) continue;
							TSharedRef<FJsonObject> CompactVariable = MakeShared<FJsonObject>();
							FString Name;
							if (Variable->TryGetStringField(TEXT("name"), Name)) CompactVariable->SetStringField(TEXT("name"), Name);
							const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
							if (Variable->TryGetArrayField(TEXT("values"), Values) && Values) CompactVariable->SetArrayField(TEXT("values"), *Values);
							CompactVariables.Add(MakeShared<FJsonValueObject>(CompactVariable));
						}
					}
					CompactAccess->SetArrayField(TEXT("variables"), MoveTemp(CompactVariables));
					CompactAccesses.Add(MakeShared<FJsonValueObject>(CompactAccess));
				}
			}
			Compact->SetArrayField(TEXT("textureAccesses"), MoveTemp(CompactAccesses));
			return Compact;
		}

		TSharedRef<FJsonObject> BuildCompactResourceProvenanceForAgent(
			const TSharedPtr<FJsonObject>& Provenance) const
		{
			TSharedRef<FJsonObject> Compact = MakeShared<FJsonObject>();
			if (!Provenance.IsValid())
			{
				return Compact;
			}
			static const TCHAR* StringFields[] = {
				TEXT("resource"), TEXT("shaderBinding"), TEXT("readEvidence"), TEXT("pixelContribution"),
				TEXT("pixelTraceStatus"), TEXT("dimensionRelation"), TEXT("coordinateMapping"),
				TEXT("producerUsage"), TEXT("producerStatus"), TEXT("relation"), TEXT("producerAction"),
				TEXT("producerKind"), TEXT("chainBreak") };
			for (const TCHAR* Field : StringFields)
			{
				FString Value;
				if (Provenance->TryGetStringField(Field, Value))
				{
					Compact->SetStringField(Field, Value.Left(800));
				}
			}
			static const TCHAR* NumberFields[] = {
				TEXT("resourceIndex"), TEXT("producerEventId"), TEXT("invalidatingEventId") };
			for (const TCHAR* Field : NumberFields)
			{
				double Value = 0.0;
				if (Provenance->TryGetNumberField(Field, Value))
				{
					Compact->SetNumberField(Field, Value);
				}
			}
			bool bProducerFound = false;
			if (Provenance->TryGetBoolField(TEXT("producerFound"), bProducerFound))
			{
				Compact->SetBoolField(TEXT("producerFound"), bProducerFound);
			}
			return Compact;
		}

		FString BuildAgentPrefilterEvidence() const
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("evidenceType"), TEXT("prefiltered_pixel_forensics"));
			Root->SetStringField(TEXT("capture"), FPaths::GetCleanFilename(GetCapturePath()));
			Root->SetStringField(TEXT("ruleSummary"), LastReportSummary);
			Root->SetStringField(TEXT("boundedCausalPath"), LastReportCausalPath);
			TSharedRef<FJsonObject> ReplayTarget = MakeShared<FJsonObject>();
			ReplayTarget->SetNumberField(TEXT("resourceIndex"), ReplayTargetResourceIndex);
			ReplayTarget->SetNumberField(TEXT("width"), CurrentPreviewSize.X);
			ReplayTarget->SetNumberField(TEXT("height"), CurrentPreviewSize.Y);
			ReplayTarget->SetNumberField(TEXT("samples"), ReplayTargetSamples);
			ReplayTarget->SetStringField(TEXT("format"), ReplayTargetFormat);
			Root->SetObjectField(TEXT("replayTarget"), ReplayTarget);

			FString MetadataJson;
			UE::RenderTrail::FCaptureMetadata Metadata;
			FString MetadataError;
			if (FFileHelper::LoadFileToString(MetadataJson, *UE::RenderTrail::GetMetadataPathForCapture(GetCapturePath()))
				&& UE::RenderTrail::FCaptureMetadata::FromJson(MetadataJson, Metadata, MetadataError))
			{
				TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
				Context->SetStringField(TEXT("project"), Metadata.ProjectName);
				Context->SetStringField(TEXT("map"), Metadata.MapName);
				Context->SetStringField(TEXT("engine"), Metadata.EngineVersion);
				Context->SetBoolField(TEXT("pie"), Metadata.bIsPIE);
				Root->SetObjectField(TEXT("ueContext"), Context);
			}

			TArray<TSharedPtr<FJsonValue>> SampleValues;
			for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
			{
				const FPixelSample& Sample = Samples[SampleIndex];
				TSharedRef<FJsonObject> SampleJson = MakeShared<FJsonObject>();
				const FString Label = FString::Printf(TEXT("P%d"), SampleIndex + 1);
				SampleJson->SetStringField(TEXT("label"), Label);
				SampleJson->SetStringField(TEXT("role"), TEXT("point_of_interest"));
				SampleJson->SetNumberField(TEXT("x"), Sample.Pixel.X);
				SampleJson->SetNumberField(TEXT("y"), Sample.Pixel.Y);
				SampleJson->SetNumberField(TEXT("modificationCount"), Sample.TotalModifications);
				const TArray<FEventEvidence> Events = AggregateEvents(Sample);
				const FString FinalObservedValue = !Sample.Modifications.IsEmpty()
					? Sample.Modifications.Last().After
					: (!Events.IsEmpty() ? Events.Last().After : TEXT("unavailable"));
				SampleJson->SetStringField(TEXT("finalObservedValue"), FinalObservedValue);
				SampleJson->SetBoolField(TEXT("detailTailTruncated"), Sample.bTruncated);

				TArray<TSharedPtr<FJsonValue>> EventValues;
				const int32 First = FMath::Max(0, Events.Num() - MaxAgentPrefilterEventsPerSample);
				for (int32 Index = Events.Num() - 1; Index >= First; --Index)
				{
					const FEventEvidence& Event = Events[Index];
					TSharedRef<FJsonObject> EventJson = MakeShared<FJsonObject>();
					EventJson->SetNumberField(TEXT("eventId"), Event.EventId);
					EventJson->SetStringField(TEXT("kind"), Event.ActionKind);
					EventJson->SetStringField(TEXT("action"), Event.Action);
					EventJson->SetStringField(TEXT("marker"), CompactMarkerPath(Event.MarkerPath));
					EventJson->SetNumberField(TEXT("actionFlags"), Event.ActionFlags);
					EventJson->SetStringField(TEXT("result"), DescribeEventResult(Event));
					EventJson->SetNumberField(TEXT("passedFragments"), Event.PassedFragments);
					EventJson->SetNumberField(TEXT("rejectedFragments"), Event.RejectedFragments);
					AddColorDeltaJson(EventJson, Event);
					EventValues.Add(MakeShared<FJsonValueObject>(EventJson));
				}
				SampleJson->SetArrayField(TEXT("latestRelevantEvents"), EventValues);
				TArray<TSharedPtr<FJsonValue>> CompleteEventChain;
				const int32 AgentChainFirst = FMath::Max(0, Events.Num() - MaxAgentEventChainPerSample);
				CompleteEventChain.Reserve(Events.Num() - AgentChainFirst);
				for (int32 EventIndex = AgentChainFirst; EventIndex < Events.Num(); ++EventIndex)
				{
					const FEventEvidence& Event = Events[EventIndex];
					TSharedRef<FJsonObject> EventJson = MakeShared<FJsonObject>();
					EventJson->SetNumberField(TEXT("eventId"), Event.EventId);
					EventJson->SetStringField(TEXT("kind"), Event.ActionKind);
					EventJson->SetStringField(TEXT("action"), Event.Action);
					EventJson->SetStringField(TEXT("marker"), CompactMarkerPath(Event.MarkerPath));
					EventJson->SetNumberField(TEXT("actionFlags"), Event.ActionFlags);
					EventJson->SetStringField(TEXT("semantics"), ClassifySemantics(Event));
					EventJson->SetStringField(TEXT("result"), DescribeEventResult(Event));
					EventJson->SetStringField(TEXT("before"), Event.Before);
					EventJson->SetStringField(TEXT("shaderOutput"), Event.ShaderOutput);
					EventJson->SetStringField(TEXT("after"), Event.After);
					EventJson->SetNumberField(TEXT("passedFragments"), Event.PassedFragments);
					EventJson->SetNumberField(TEXT("rejectedFragments"), Event.RejectedFragments);
					EventJson->SetBoolField(TEXT("directShaderWrite"), Event.bDirectShaderWrite);
					EventJson->SetBoolField(TEXT("changedTextureValue"), Event.bChangedTextureValue);
					EventJson->SetBoolField(TEXT("hasPrimitiveEvidence"), Event.bHasPrimitiveEvidence);
					EventJson->SetNumberField(TEXT("primitiveId"), Event.PrimitiveId);
					AddColorDeltaJson(EventJson, Event);
					TArray<TSharedPtr<FJsonValue>> FailureValues;
					for (const FString& Failure : Event.FailureReasons)
					{
						FailureValues.Add(MakeShared<FJsonValueString>(Failure));
					}
					EventJson->SetArrayField(TEXT("failureReasons"), MoveTemp(FailureValues));
					CompleteEventChain.Add(MakeShared<FJsonValueObject>(EventJson));
				}
				SampleJson->SetArrayField(TEXT("completeEventChain"), MoveTemp(CompleteEventChain));
				SampleJson->SetBoolField(TEXT("eventChainComplete"), Sample.bEventSummaryComplete && AgentChainFirst == 0);
				SampleJson->SetNumberField(TEXT("eventChainEventCount"), Events.Num());
				SampleJson->SetNumberField(TEXT("eventChainStartIndex"), AgentChainFirst);
				TSharedRef<FJsonObject> AgentCausalGraph =
					BuildPixelCausalGraph(Sample, Events, EventContexts, MaxDisplayedFrontierResources);
				const TArray<TSharedPtr<FJsonValue>>* WriterHops = nullptr;
				if (AgentCausalGraph->TryGetArrayField(TEXT("targetWriterHops"), WriterHops) && WriterHops)
				{
					for (const TSharedPtr<FJsonValue>& HopValue : *WriterHops)
					{
						const TSharedPtr<FJsonObject> Hop = HopValue.IsValid() ? HopValue->AsObject() : nullptr;
						if (!Hop.IsValid()) continue;
						const TArray<TSharedPtr<FJsonValue>>* Histories = nullptr;
						if (Hop->TryGetArrayField(TEXT("resourcePixelHistories"), Histories) && Histories)
						{
							Hop->SetNumberField(TEXT("resourcePixelHistoryCount"), Histories->Num());
							Hop->RemoveField(TEXT("resourcePixelHistories"));
						}
					}
				}
				SampleJson->SetObjectField(TEXT("causalGraph"), AgentCausalGraph);
				SampleValues.Add(MakeShared<FJsonValueObject>(SampleJson));
			}
			Root->SetArrayField(TEXT("samples"), SampleValues);

			if (LastCandidate.IsSet())
			{
				const FCausalCandidate& Candidate = LastCandidate.GetValue();
				TSharedRef<FJsonObject> CandidateJson = MakeShared<FJsonObject>();
				CandidateJson->SetNumberField(TEXT("eventId"), Candidate.Event.EventId);
				CandidateJson->SetStringField(TEXT("action"), Candidate.Event.Action);
				CandidateJson->SetStringField(TEXT("kind"), Candidate.Event.ActionKind);
				CandidateJson->SetStringField(TEXT("semantics"), ClassifySemantics(Candidate.Event));
				CandidateJson->SetStringField(TEXT("marker"), CompactMarkerPath(Candidate.Event.MarkerPath));
				CandidateJson->SetStringField(TEXT("result"), DescribeEventResult(Candidate.Event));
				CandidateJson->SetStringField(TEXT("causalRole"), TEXT("final-writer"));
				AddColorDeltaJson(CandidateJson, Candidate.Event);
				CandidateJson->SetBoolField(TEXT("pointDivergence"), bLastCandidateHasDivergence);
				CandidateJson->SetNumberField(TEXT("sampleCoverage"), Candidate.SampleCoverage);
				Root->SetObjectField(TEXT("candidate"), CandidateJson);
				Root->SetObjectField(TEXT("finalWriter"), CandidateJson);
			}
			if (LastSignificantCandidate.IsSet())
			{
				const FCausalCandidate& Candidate = LastSignificantCandidate.GetValue();
				TSharedRef<FJsonObject> CandidateJson = MakeShared<FJsonObject>();
				CandidateJson->SetNumberField(TEXT("eventId"), Candidate.Event.EventId);
				CandidateJson->SetStringField(TEXT("action"), Candidate.Event.Action);
				CandidateJson->SetStringField(TEXT("kind"), Candidate.Event.ActionKind);
				CandidateJson->SetStringField(TEXT("semantics"), ClassifySemantics(Candidate.Event));
				CandidateJson->SetStringField(TEXT("marker"), CompactMarkerPath(Candidate.Event.MarkerPath));
				CandidateJson->SetStringField(TEXT("result"), DescribeEventResult(Candidate.Event));
				CandidateJson->SetStringField(TEXT("causalRole"), TEXT("significant-upstream-writer-candidate"));
				AddColorDeltaJson(CandidateJson, Candidate.Event);
				Root->SetObjectField(TEXT("significantWriterCandidate"), CandidateJson);
			}

			TArray<uint32> DeterministicContextIds;
			EventContexts.GenerateKeyArray(DeterministicContextIds);
			DeterministicContextIds.Sort([this](uint32 A, uint32 B)
			{
				const bool bCriticalA = IsCriticalAgentEvent(A);
				const bool bCriticalB = IsCriticalAgentEvent(B);
				if (bCriticalA != bCriticalB)
				{
					return bCriticalA;
				}
				const int32 DepthA = EventContextDepths.FindRef(A);
				const int32 DepthB = EventContextDepths.FindRef(B);
				return DepthA == DepthB ? A > B : DepthA < DepthB;
			});

			TArray<TSharedPtr<FJsonValue>> DeterministicContexts;
			const int32 ContextCount = FMath::Min(DeterministicContextIds.Num(), MaxAgentDeterministicContexts);
			for (int32 ContextIndex = 0; ContextIndex < ContextCount; ++ContextIndex)
			{
				const uint32 ContextEventId = DeterministicContextIds[ContextIndex];
				const FEventContextEvidence& Context = EventContexts.FindChecked(ContextEventId);
				const int32 ReverseDepth = EventContextDepths.FindRef(Context.EventId);
				TSharedRef<FJsonObject> ContextJson = MakeShared<FJsonObject>();
				ContextJson->SetNumberField(TEXT("eventId"), Context.EventId);
				ContextJson->SetStringField(TEXT("action"), Context.Action);
				ContextJson->SetStringField(TEXT("actionKind"), Context.ActionKind);
				ContextJson->SetStringField(TEXT("marker"), CompactMarkerPath(Context.MarkerPath));
				ContextJson->SetNumberField(TEXT("reverseDepth"), ReverseDepth);
				ContextJson->SetStringField(TEXT("shaderStage"), Context.ShaderStage);
				ContextJson->SetStringField(TEXT("shaderEntry"), Context.ShaderEntry);
				ContextJson->SetStringField(TEXT("shaderDebugStatus"), Context.ShaderDebugStatus);
				ContextJson->SetBoolField(TEXT("shaderDebuggable"), Context.bShaderDebuggable);
				ContextJson->SetBoolField(TEXT("sourceSymbols"), Context.bSourceDebugInfo);
				ContextJson->SetStringField(TEXT("shaderEncoding"), Context.ShaderEncoding);
				ContextJson->SetNumberField(TEXT("inputSignatureCount"), Context.ShaderInputSignatureCount);
				ContextJson->SetNumberField(TEXT("outputSignatureCount"), Context.ShaderOutputSignatureCount);
				ContextJson->SetNumberField(TEXT("constantBlockCount"), Context.ShaderConstantBlockCount);
				ContextJson->SetNumberField(TEXT("samplerCount"), Context.ShaderSamplerCount);
				ContextJson->SetNumberField(TEXT("readOnlyResourceCount"), Context.ShaderReadOnlyResourceCount);
				ContextJson->SetNumberField(TEXT("readWriteResourceCount"), Context.ShaderReadWriteResourceCount);
				ContextJson->SetBoolField(TEXT("fixedFunctionStateAvailable"), Context.PipelineState.IsValid());
				if (IsCriticalAgentEvent(Context.EventId) && Context.PipelineState.IsValid())
				{
					ContextJson->SetObjectField(TEXT("fixedFunctionState"), Context.PipelineState);
				}
				if (Context.ShaderDebugTrace.IsValid())
				{
					ContextJson->SetObjectField(TEXT("shaderDebugTrace"),
						BuildCompactShaderDebugForAgent(Context.ShaderDebugTrace));
				}
				TArray<TSharedPtr<FJsonValue>> Inputs;
				for (int32 InputIndex = 0;
					InputIndex < Context.Inputs.Num() && InputIndex < MaxAgentBoundResourcesPerContext;
					++InputIndex)
				{
					const FBoundResourceEvidence& Input = Context.Inputs[InputIndex];
					TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
					Resource->SetNumberField(TEXT("resourceIndex"), Input.ResourceIndex);
					Resource->SetStringField(TEXT("name"), Input.Name);
					Resource->SetStringField(TEXT("format"), Input.Format);
					Resource->SetStringField(TEXT("stage"), Input.Stage);
					Resource->SetStringField(TEXT("access"), Input.Access);
					Resource->SetStringField(TEXT("shaderBinding"), Input.ShaderBinding);
					Resource->SetNumberField(TEXT("width"), Input.Width);
					Resource->SetNumberField(TEXT("height"), Input.Height);
					Resource->SetNumberField(TEXT("samples"), Input.Samples);
					Inputs.Add(MakeShared<FJsonValueObject>(Resource));
				}
				ContextJson->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
				ContextJson->SetNumberField(TEXT("inputCount"), Context.Inputs.Num());
				TArray<TSharedPtr<FJsonValue>> Outputs;
				for (int32 OutputIndex = 0;
					OutputIndex < Context.Outputs.Num() && OutputIndex < MaxAgentBoundResourcesPerContext;
					++OutputIndex)
				{
					const FBoundResourceEvidence& Output = Context.Outputs[OutputIndex];
					TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
					Resource->SetNumberField(TEXT("resourceIndex"), Output.ResourceIndex);
					Resource->SetStringField(TEXT("name"), Output.Name);
					Resource->SetStringField(TEXT("format"), Output.Format);
					Resource->SetStringField(TEXT("stage"), Output.Stage);
					Resource->SetStringField(TEXT("access"), Output.Access);
					Resource->SetNumberField(TEXT("width"), Output.Width);
					Resource->SetNumberField(TEXT("height"), Output.Height);
					Outputs.Add(MakeShared<FJsonValueObject>(Resource));
				}
				ContextJson->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
				ContextJson->SetNumberField(TEXT("outputCount"), Context.Outputs.Num());

				TArray<TSharedPtr<FJsonValue>> CompactProvenance;
				for (int32 ProvenanceIndex = 0;
					ProvenanceIndex < Context.ResourceProvenance.Num()
						&& ProvenanceIndex < MaxAgentBoundResourcesPerContext;
					++ProvenanceIndex)
				{
					const TSharedPtr<FJsonObject> Provenance = Context.ResourceProvenance[ProvenanceIndex].IsValid()
						? Context.ResourceProvenance[ProvenanceIndex]->AsObject() : nullptr;
					CompactProvenance.Add(MakeShared<FJsonValueObject>(
						BuildCompactResourceProvenanceForAgent(Provenance)));
				}
				ContextJson->SetArrayField(TEXT("resourceProvenance"), MoveTemp(CompactProvenance));
				ContextJson->SetNumberField(TEXT("resourceProvenanceCount"), Context.ResourceProvenance.Num());

				TArray<TSharedPtr<FJsonValue>> CompactHistories;
				for (int32 RequiredPass = 1;
					RequiredPass >= 0 && CompactHistories.Num() < MaxAgentResourceHistoriesPerContext;
					--RequiredPass)
				{
					for (const TSharedPtr<FJsonValue>& HistoryValue : Context.ResourcePixelHistories)
					{
						const TSharedPtr<FJsonObject> History = HistoryValue.IsValid()
							? HistoryValue->AsObject() : nullptr;
						if (!History.IsValid())
						{
							continue;
						}
						bool bRequired = false;
						History->TryGetBoolField(TEXT("requiredForAgent"), bRequired);
						if (bRequired != (RequiredPass == 1))
						{
							continue;
						}
						CompactHistories.Add(MakeShared<FJsonValueObject>(
							BuildCompactResourceHistoryForAgent(History)));
						if (CompactHistories.Num() >= MaxAgentResourceHistoriesPerContext)
						{
							break;
						}
					}
				}
				ContextJson->SetArrayField(TEXT("resourcePixelHistories"), MoveTemp(CompactHistories));
				ContextJson->SetNumberField(TEXT("resourcePixelHistoryCount"), Context.ResourcePixelHistories.Num());
				DeterministicContexts.Add(MakeShared<FJsonValueObject>(ContextJson));
			}
			Root->SetArrayField(TEXT("deterministicEventContexts"), MoveTemp(DeterministicContexts));
			Root->SetNumberField(TEXT("deterministicEventContextCount"), EventContexts.Num());
			Root->SetNumberField(TEXT("deterministicEventContextsIncluded"), ContextCount);
			Root->SetBoolField(TEXT("criticalDeterministicCollectionComplete"),
				!HasPendingCriticalDeterministicQueries());
			Root->SetBoolField(TEXT("backgroundDeterministicCollectionPending"),
				HasPendingBackgroundDeterministicQueries());
			Root->SetNumberField(TEXT("pendingCriticalEventContexts"), GetPendingCriticalContextCount());
			Root->SetNumberField(TEXT("pendingRequiredResourcePixelHistories"),
				GetPendingRequiredResourceHistoryCount());
			Root->SetNumberField(TEXT("pendingBackgroundResourcePixelHistories"),
				GetPendingBackgroundResourceHistoryCount());
			Root->SetBoolField(TEXT("deterministicContextCollectionComplete"),
				PendingEventContextIds.IsEmpty() && PendingResourcePixelHistoryByRequest.IsEmpty()
					&& PendingShaderDebugByRequest.IsEmpty());
			Root->SetNumberField(TEXT("deterministicContextFailureCount"), FailedEventContextIds.Num());
			Root->SetNumberField(TEXT("shaderDebugFailureCount"), FailedShaderDebugIds.Num());
			Root->SetNumberField(TEXT("resourcePixelHistoryFailureCount"), FailedResourcePixelHistoryKeys.Num());
			Root->SetNumberField(TEXT("resourcePixelHistoryQueryCount"), ResourcePixelHistoryQueriesSubmitted);
			Root->SetNumberField(TEXT("resourcePixelHistoryBranchCount"), ScheduledResourcePixelHistoryKeys.Num());
			Root->SetNumberField(TEXT("resourcePixelHistoryQueryLimit"), MaxResourcePixelHistoryQueries);
			Root->SetNumberField(TEXT("deterministicContextLimit"), MaxDeterministicContextEvents);
			Root->SetNumberField(TEXT("agentEventChainLimitPerSample"), MaxAgentEventChainPerSample);
			return SerializeJson(Root);
		}

		bool AgentEvidenceContainsEvent(uint32 EventId) const
		{
			for (const FPixelSample& Sample : Samples)
			{
				if (Sample.Modifications.ContainsByPredicate([EventId](const FPixelModificationEvidence& Item) { return Item.EventId == EventId; }))
				{
					return true;
				}
			}
			return false;
		}

		FString BuildAgentEventObservation(uint32 EventId) const
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("tool"), TEXT("inspect_event"));
			Root->SetNumberField(TEXT("eventId"), EventId);
			TArray<TSharedPtr<FJsonValue>> Outcomes;
			for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
			{
				const FPixelSample& Sample = Samples[SampleIndex];
				const FString Label = FString::Printf(TEXT("P%d"), SampleIndex + 1);
				const TArray<FEventEvidence> Events = AggregateEvents(Sample);
				if (const FEventEvidence* Event = FindEvent(Events, EventId))
				{
					TSharedRef<FJsonObject> Outcome = MakeShared<FJsonObject>();
					Outcome->SetStringField(TEXT("sample"), Label);
					Outcome->SetStringField(TEXT("action"), Event->Action);
					Outcome->SetStringField(TEXT("kind"), Event->ActionKind);
					Outcome->SetStringField(TEXT("marker"), CompactMarkerPath(Event->MarkerPath));
					Outcome->SetStringField(TEXT("result"), DescribeEventResult(*Event));
					Outcome->SetStringField(TEXT("before"), Event->Before);
					Outcome->SetStringField(TEXT("shaderOutput"), Event->ShaderOutput);
					Outcome->SetStringField(TEXT("after"), Event->After);
					Outcomes.Add(MakeShared<FJsonValueObject>(Outcome));
				}
			}
			Root->SetArrayField(TEXT("sampleOutcomes"), Outcomes);

			if (const FEventContextEvidence* Context = EventContexts.Find(EventId))
			{
				TSharedRef<FJsonObject> Pipeline = MakeShared<FJsonObject>();
				Pipeline->SetStringField(TEXT("shaderStage"), Context->ShaderStage);
				Pipeline->SetStringField(TEXT("shaderEntry"), Context->ShaderEntry);
				Pipeline->SetBoolField(TEXT("shaderDebuggable"), Context->bShaderDebuggable);
				Pipeline->SetBoolField(TEXT("sourceSymbols"), Context->bSourceDebugInfo);
				TSharedRef<FJsonObject> Reflection = MakeShared<FJsonObject>();
				Reflection->SetStringField(TEXT("encoding"), Context->ShaderEncoding);
				Reflection->SetNumberField(TEXT("inputSignatureCount"), Context->ShaderInputSignatureCount);
				Reflection->SetNumberField(TEXT("outputSignatureCount"), Context->ShaderOutputSignatureCount);
				Reflection->SetNumberField(TEXT("constantBlockCount"), Context->ShaderConstantBlockCount);
				Reflection->SetNumberField(TEXT("samplerCount"), Context->ShaderSamplerCount);
				Reflection->SetNumberField(TEXT("readOnlyResourceCount"), Context->ShaderReadOnlyResourceCount);
				Reflection->SetNumberField(TEXT("readWriteResourceCount"), Context->ShaderReadWriteResourceCount);
				Pipeline->SetObjectField(TEXT("shaderReflection"), Reflection);
				Pipeline->SetStringField(TEXT("algorithmEvidence"),
					(Context->bShaderDebuggable && Context->bSourceDebugInfo)
						? TEXT("source/debug information exists; instruction trace not executed")
						: TEXT("not proven from reflection and bindings"));
				TArray<TSharedPtr<FJsonValue>> Inputs;
				for (int32 Index = 0; Index < Context->Inputs.Num() && Index < MaxDisplayedFrontierResources; ++Index)
				{
					const FBoundResourceEvidence& Input = Context->Inputs[Index];
					TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
					Resource->SetStringField(TEXT("name"), Input.Name);
					Resource->SetStringField(TEXT("shaderBinding"), Input.ShaderBinding);
					Resource->SetStringField(TEXT("stage"), Input.Stage);
					Resource->SetStringField(TEXT("access"), Input.Access);
					Resource->SetNumberField(TEXT("width"), Input.Width);
					Resource->SetNumberField(TEXT("height"), Input.Height);
					Resource->SetNumberField(TEXT("samples"), Input.Samples);
					Inputs.Add(MakeShared<FJsonValueObject>(Resource));
				}
				Pipeline->SetArrayField(TEXT("usedInputs"), Inputs);
				TArray<TSharedPtr<FJsonValue>> Outputs;
				for (int32 Index = 0; Index < Context->Outputs.Num() && Index < MaxDisplayedFrontierResources; ++Index)
				{
					const FBoundResourceEvidence& Output = Context->Outputs[Index];
					TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
					Resource->SetStringField(TEXT("name"), Output.Name);
					Resource->SetStringField(TEXT("stage"), Output.Stage);
					Resource->SetStringField(TEXT("access"), Output.Access);
					Resource->SetNumberField(TEXT("width"), Output.Width);
					Resource->SetNumberField(TEXT("height"), Output.Height);
					Outputs.Add(MakeShared<FJsonValueObject>(Resource));
				}
				Pipeline->SetArrayField(TEXT("usedOutputs"), Outputs);
				Pipeline->SetArrayField(TEXT("resourceProvenance"), Context->ResourceProvenance);
				Pipeline->SetArrayField(TEXT("resourcePixelHistories"), Context->ResourcePixelHistories);
				if (Context->PipelineState.IsValid())
				{
					Pipeline->SetObjectField(TEXT("fixedFunctionState"), Context->PipelineState);
				}
				if (Context->ShaderDebugTrace.IsValid())
				{
					Pipeline->SetObjectField(TEXT("shaderDebugTrace"), Context->ShaderDebugTrace);
				}
				Root->SetObjectField(TEXT("pipeline"), Pipeline);
			}
			else
			{
				Root->SetStringField(TEXT("pipeline"), FailedEventContextIds.Contains(EventId) ? TEXT("query_failed") : TEXT("not_loaded"));
			}
			return SerializeJson(Root);
		}

		const FPixelSample* FindAgentSample(const FString& Label) const
		{
			if (!Label.StartsWith(TEXT("P"), ESearchCase::IgnoreCase))
			{
				return nullptr;
			}
			int32 OneBasedIndex = 0;
			if (!LexTryParseString(OneBasedIndex, *Label.Mid(1)) || !Samples.IsValidIndex(OneBasedIndex - 1))
			{
				return nullptr;
			}
			return &Samples[OneBasedIndex - 1];
		}

		FString BuildAgentSampleObservation(const FString& Label) const
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("tool"), TEXT("inspect_sample"));
			Root->SetStringField(TEXT("sample"), Label);
			const FPixelSample* Sample = FindAgentSample(Label);
			if (!Sample)
			{
				Root->SetStringField(TEXT("error"), TEXT("unknown sample label"));
				return SerializeJson(Root);
			}

			Root->SetNumberField(TEXT("x"), Sample->Pixel.X);
			Root->SetNumberField(TEXT("y"), Sample->Pixel.Y);
			Root->SetStringField(TEXT("finalObservedValue"), Sample->Modifications.IsEmpty() ? TEXT("unavailable") : Sample->Modifications.Last().After);
			const TArray<FEventEvidence> Events = AggregateEvents(*Sample);
			TArray<TSharedPtr<FJsonValue>> History;
			const int32 First = FMath::Max(0, Events.Num() - MaxDisplayedTraceHops);
			for (int32 Index = Events.Num() - 1; Index >= First; --Index)
			{
				const FEventEvidence& Event = Events[Index];
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("eventId"), Event.EventId);
				Item->SetStringField(TEXT("kind"), Event.ActionKind);
				Item->SetStringField(TEXT("action"), Event.Action);
				Item->SetStringField(TEXT("marker"), CompactMarkerPath(Event.MarkerPath));
				Item->SetStringField(TEXT("result"), DescribeEventResult(Event));
				History.Add(MakeShared<FJsonValueObject>(Item));
			}
			Root->SetArrayField(TEXT("latestFirstHistory"), History);
			return SerializeJson(Root);
		}

		FReply StartAgentAnalysis()
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
			if (HasPendingCriticalDeterministicQueries())
			{
				bAgentWaitingForDeterministicContexts = true;
				SetAgentStatus(FString::Printf(TEXT("正在收集快速确定性前沿（根事件=%d、Shader=%d、必需资源/sample=%d；后台深追=%d）；快速前沿完成后立即进行语义提炼。"),
					GetPendingCriticalContextCount(),
					PendingShaderDebugByRequest.Num(), GetPendingRequiredResourceHistoryCount(),
					GetPendingBackgroundResourceHistoryCount()));
				return FReply::Handled();
			}

			AgentMessages.Empty();
			AgentStep = 0;
			AgentPendingEventId.Reset();
			bAgentRunning = true;
			if (AgentRunButtonText.IsValid())
				AgentRunButtonText->SetText(FText::FromString(TEXT("处理中…")));
			SetAgentOutputText(HasPendingBackgroundDeterministicQueries()
				? TEXT("快速确定性前沿已完成；Agent 正在整理当前证据，后台深层溯源继续更新技术报告。")
				: TEXT("确定性溯源已完成，Agent 只负责整理已收集的证据。"));
			const FString PrefilterEvidence = BuildAgentPrefilterEvidence();
			Diagnostics.WriteRecord(TEXT("agent_evidence_compaction"), FString::Printf(
				TEXT("chars=%d contextsIncluded=%d contextsTotal=%d pendingCriticalContexts=%d pendingRequiredResources=%d pendingBackgroundResources=%d pendingShader=%d"),
				PrefilterEvidence.Len(), FMath::Min(EventContexts.Num(), MaxAgentDeterministicContexts), EventContexts.Num(),
				GetPendingCriticalContextCount(), GetPendingRequiredResourceHistoryCount(),
				GetPendingBackgroundResourceHistoryCount(), PendingShaderDebugByRequest.Num()));
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
			ReleaseBackgroundDeterministicQueries();
			return FReply::Handled();
		}

		FReply RunPrimaryAnalysis()
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

		void SendAgentTurn()
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


		void SendAgentBrokerCompletion()
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
			const TWeakPtr<SAnalyzerHome> WeakThis = SharedThis(this);
			AgentClient->Submit(Request, [WeakThis](FRenderTrailAgentResponse&& Response)
			{
				if (const TSharedPtr<SAnalyzerHome> Pinned = WeakThis.Pin())
				{
					Pinned->HandleAgentBrokerResponse(MoveTemp(Response));
				}
			});
			SetAgentStatus(FString::Printf(TEXT("Agent turn %d/%d - RenderTrail Model Broker completing..."), AgentStep, MaxAgentSteps));
			return;

		}



		void HandleAgentBrokerResponse(FRenderTrailAgentResponse&& Response)
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



		bool NormalizeAnswerOnlyAgentObject(const TSharedPtr<FJsonObject>& Action)
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

		void HandleAgentAction(const FString& Content, const FString& ReasoningContent = FString())
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
						Unknowns.Add(MakeShared<FJsonValueString>(TEXT("后台深层资源溯源仍在继续；可在完成后再次运行语义整理。")));
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

		void ResumeAgentAfterEventContext(uint32 EventId)
		{
			if (!bAgentRunning || !AgentPendingEventId.IsSet() || AgentPendingEventId.GetValue() != EventId)
				return;
			AgentPendingEventId.Reset();
			AddAgentMessage(TEXT("user"), TEXT("TOOL_RESULT\n") + BuildAgentEventObservation(EventId));
			SendAgentTurn();
		}

		void DisplayAgentFinal(const TSharedRef<FJsonObject>& Final)
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
			FString UnknownText;
			const TArray<TSharedPtr<FJsonValue>>* Unknowns = nullptr;
			if (Final->TryGetArrayField(TEXT("unknowns"), Unknowns) && Unknowns)
			{
				for (const TSharedPtr<FJsonValue>& Unknown : *Unknowns)
					UnknownText += FString::Printf(TEXT("• %s\n"), *Unknown->AsString());
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
			Output += FString::Printf(TEXT("\n类型：%s\n作用：%s\n依据：%s\n归属：%s\n"),
				*InfluenceType, *InfluenceEffect, *InfluenceEvidence, *MeshName);
			if (!MeshEvidence.IsEmpty() && MeshName.Equals(TEXT("unknown"), ESearchCase::IgnoreCase))
			{
				Output += FString::Printf(TEXT("归属依据：%s\n"), *MeshEvidence);
			}
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
			if (!ProcessText.IsEmpty())
			{
				Output += TEXT("\n像素链\n");
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
			ViewModel.ProcessText = ProcessText;
			ViewModel.PipelineText = PipelineText;
			ViewModel.ShaderText = ShaderEvidenceText;
			ViewModel.UnknownText = UnknownText;
			ViewModel.RawReport = Output;

			FRenderTrailResultFact PassFact;
			PassFact.Label = TEXT("关键 Pass / Event");
			PassFact.Value = InfluenceHeading;
			ViewModel.Facts.Add(MoveTemp(PassFact));
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
			LastAgentQuestion = RequestedQuestion;
			LastAgentAnswer = Answer;
			Diagnostics.WriteAgentLog(TEXT("RunComplete"), Output);
			if (AgentOutputText.IsValid())
			{
				AgentOutputText->SetText(FText::FromString(TEXT("回答已完成，并按“结论 → 颜色变化 → 证据链 → 缺口”分层显示。可修改问题后再次发送。")));
			}
			bAgentRunning = false;
			bAgentWaitingForDeterministicContexts = false;
			AgentPendingEventId.Reset();
			if (AgentRunButtonText.IsValid())
				AgentRunButtonText->SetText(FText::FromString(TEXT("重新发送")));
			SetAgentStatus(FString::Printf(TEXT("完成 · %d 个模型轮次 · 只使用所选像素的有界证据"), AgentStep));
		}

		void FinishAgentWithError(const FString& Error)
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

		void CancelAgentRun()
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

		FReply ClearSamples()
		{
			ResetSamples();
			const bool bPreviewReady = bWorkerReady || bPreviewReadyForSelection || bReplayStartDeferred;
			SetEvidence(bPreviewReady
				? TEXT("关注像素已清空。直接在画面选择一个需要解释的像素。")
				: TEXT("先载入 .rdc 截帧，再选择需要解释的像素。"));
			SetStatus(bPreviewReady ? TEXT("关注像素已清空，可以重新选择。") : TEXT("关注像素已清空。"));
			return FReply::Handled();
		}

		FReply ClearCurrentInfo()
		{
			CancelAgentRun();
			LastAgentQuestion.Empty();
			LastAgentAnswer.Empty();
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

		FReply ConfirmPixelSelection()
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
				EventContexts.Empty();
				EventContextDepths.Empty();
				EventTracePixels.Empty();
				PendingEventContextByRequest.Empty();
				PendingResourcePixelHistoryByRequest.Empty();
				ScheduledResourcePixelHistoryKeys.Empty();
				ResourcePixelHistoryBindingAliases.Empty();
				DeferredResourceHistoryContextIds.Empty();
				DeferredResourceHistoryBranchCounts.Empty();
				DeferredEventContextIds.Empty();
				FailedResourcePixelHistoryKeys.Empty();
				ResourcePixelHistoryQueriesSubmitted = 0;
				bBackgroundDeterministicQueriesReleased = false;
				PendingEventContextIds.Empty();
				FailedEventContextIds.Empty();
				LastCandidate.Reset();
				LastSignificantCandidate.Reset();
				bLastCandidateHasDivergence = false;
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
					});
				++QueuedCount;
			}

			UpdateSelectionText();
			RenderCausalReport();
			if (QueuedCount == 0)
			{
				SetAgentStatus(TEXT("Pixel History 已就绪，可以围绕当前 P1 向 Agent 提问。"));
				SetStatus(TEXT("选点未变化，已复用当前像素的 Pixel History，不重复查询。"));
			}
			else
			{
				SetAgentStatus(TEXT("规则分析进行中；Pixel History 返回后可以向 Agent 提问。"));
				SetStatus(TEXT("已确认当前像素，正在读取 Pixel History…"));
			}
			return FReply::Handled();
		}

		void MarkReplaySynchronizationPending()
		{
			bReplaySynchronizationPending = true;
			SetStatus(TEXT("已选点；正在等待同步：ReplayController → 目标 RT → Pixel History。"));
			SetAgentStatus(TEXT("分析已排队，等待 Replay 完整同步；同步完成后才会读取事件上下文和 Shader Debug。"));
		}

		void ResetSamples()
		{
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
			FailedResourcePixelHistoryKeys.Empty();
			WorkerRequestQueuedSeconds.Empty();
			WorkerRequestCommands.Empty();
			ActiveWorkerRequestId.Empty();
			ActiveWorkerStage.Empty();
			ActiveWorkerRequestStartSeconds = 0.0;
			ResourcePixelHistoryQueriesSubmitted = 0;
			bBackgroundDeterministicQueriesReleased = false;
			EventTracePixels.Empty();
			PendingEventContextIds.Empty();
			FailedEventContextIds.Empty();
			FailedShaderDebugIds.Empty();
			LastCandidate.Reset();
			LastSignificantCandidate.Reset();
			LastAgentQuestion.Empty();
			LastAgentAnswer.Empty();
			bLastCandidateHasDivergence = false;
			SetAgentOutputText(TEXT("选择并分析像素后，可以围绕最终写入、Pass、Pipeline、Shader 或证据断点继续提问。"));
			SetAgentStatus(TEXT("未运行 · 只发送像素摘要；.rdc/图像不上传；Key 不落盘"));
			if (ImageView.IsValid())
			{
				ImageView->SetMarkers({});
			}
			UpdateSelectionText();
		}

		void UpdateSelectionText()
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

		void UpdateMarkers()
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

		FPixelSample* FindSample(uint64 SampleId)
		{
			return Samples.FindByPredicate([SampleId](const FPixelSample& Sample) { return Sample.Id == SampleId; });
		}

		FReply BrowseCapture()
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

		FReply LoadCapture()
		{
			StartWorker();
			return FReply::Handled();
		}


		bool LaunchWorkerProcess(const FString& Worker, const FString& Capture, const FString& InPreviewPath)
		{
			FString Args;
			FString Error;
			const bool bLaunched = ReplayWorker.Launch(Worker, Capture, InPreviewPath,
				Diagnostics.GetOptions().bEnabled && Diagnostics.GetOptions().bFullEvidencePayload, Args, Error);
			if (Diagnostics.GetOptions().bWorkerProtocol)
			{
				Diagnostics.WriteRecord(TEXT("worker_launch"), Args);
			}
			SetCaptureLoadPhase(TEXT("Starting isolated Replay Worker and opening full Replay"));
			SetEvidence(TEXT("原生预览仍可选点；隔离 Replay Worker 正在按需建立完整 RenderDoc Replay。"));
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Starting isolated Replay Worker. Worker='%s' Capture='%s' Preview='%s'"),
				*Worker, *Capture, *InPreviewPath);
			if (!bLaunched)
			{
				SetStatus(Error);
				return false;
			}
			SetStatus(TEXT("正在按需载入完整 Replay；当前预览和选点保持可用。"));
			return true;
		}

		void StartWorker(bool bPreserveSelection = false)
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
			PreviewPath = UE::RenderTrail::GetPreviewPathForCapture(Capture);
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
			if (!bPreserveSelection && TryGetPixelExactPreviewSize(Capture, PreviewPath, NativePreviewSize)
				&& LoadPreview(PreviewPath, NativePreviewSize))
			{
				bPreviewReadyForSelection = true;
				bReplayStartDeferred = true;
				UE_LOG(LogRenderTrailAnalyzer, Display,
					TEXT("Native capture preview loaded; deferring Replay Worker until pixel analysis. capture='%s' preview='%s'"),
					*Capture, *PreviewPath);
				FinishCaptureLoad(TEXT("native preview ready; replay deferred"));
				SetStatus(TEXT("原生最终画面已就绪；现在可以选点，完整 Replay 将在点击“分析当前像素”后载入。"));
				SetReportCards(
					TEXT("原生最终画面已载入，完整 Replay 尚未启动。"),
					TEXT("选择 P1 后点击“分析当前像素”，再按需建立 Replay 和 Pixel History。"),
					TEXT("尚无因果证据。"),
					TEXT("预览与捕获 Viewport 同尺寸；当前未占用额外 Replay GPU/内存资源。"));
				return;
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

		void StopWorker()
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
			bWorkerReady = false;
			bPreviewReadyForSelection = false;
		}

		void PollWorkerPipes()
		{
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

			if (Result.bExited)
			{
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
		}

		bool SendWorkerRequest(const FString& Command, const FString& RequestId,
			TFunctionRef<void(const TSharedRef<FJsonObject>&)> Populate)
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
			if (Diagnostics.GetOptions().bWorkerProtocol)
			{
				Diagnostics.WriteRecord(TEXT("analyzer_to_worker"), Payload);
			}
			const double QueuedAt = FPlatformTime::Seconds();
			WorkerRequestQueuedSeconds.Add(RequestId, QueuedAt);
			WorkerRequestCommands.Add(RequestId, Command);
			Diagnostics.WriteRecord(TEXT("worker_request_queued"), FString::Printf(
				TEXT("request=%s command=%s queueOrdinal=%llu pendingTotal=%d critical=%s"),
				*RequestId, *Command, ++WorkerQueueOrdinal, WorkerRequestQueuedSeconds.Num(),
				HasPendingCriticalDeterministicQueries() ? TEXT("true") : TEXT("false")));
			const bool bWritten = ReplayWorker.Write(Payload);
			if (!bWritten)
			{
				WorkerRequestQueuedSeconds.Remove(RequestId);
				WorkerRequestCommands.Remove(RequestId);
				Diagnostics.WriteRecord(TEXT("worker_request_queue_failed"), FString::Printf(
					TEXT("request=%s command=%s"), *RequestId, *Command));
			}
			return bWritten;
		}

		double CompleteWorkerRequest(const FString& RequestId, const FString& ResultType, const FString& Detail = FString())
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
			if (ActiveWorkerRequestId == RequestId)
			{
				ActiveWorkerRequestId.Empty();
				ActiveWorkerStage.Empty();
				ActiveWorkerRequestStartSeconds = 0.0;
			}
			return QueueToResponseSeconds;
		}

		void ReleasePreview()
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

		static bool IsPreviewCacheValid(const FString& CapturePath, const FString& InPreviewPath)
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

		static bool TryGetPixelExactPreviewSize(
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

		bool LoadPreview(const FString& Path, FIntPoint ExpectedSize)
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

		void HandleWorkerMessage(const FString& Line)
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
			if (Type == TEXT("progress"))
			{
				FString Phase;
				double WorkerElapsed = 0.0;
				Message->TryGetStringField(TEXT("phase"), Phase);
				Message->TryGetNumberField(TEXT("elapsedSeconds"), WorkerElapsed);
				SetCaptureLoadPhase(FString::Printf(TEXT("Isolated Replay Worker: %s (%.1fs)"), *Phase, WorkerElapsed));
				LastWorkerDiagnosticPhase = Phase;
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
				LastWorkerDiagnosticPhase = FString::Printf(TEXT("%s %s: %s"), *Stage, *State, *Detail);
				if (!RequestId.IsEmpty())
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
				if (!Path.IsEmpty() && LoadPreview(Path, FIntPoint::ZeroValue))
				{
					bPreviewReadyForSelection = true;
					SetCaptureLoadPhase(FString::Printf(TEXT("Fast preview ready (%.0fx%.0f); full Replay Worker continues in background"),
						PreviewWidth, PreviewHeight));
					SetStatus(TEXT("快速预览已就绪；完整 Replay Worker 正在后台加载，现在可以选择像素。"));
					SetReportCards(
						TEXT("最终画面预览已就绪；完整 Replay 数据仍在加载。"),
						TEXT("现在可以选择像素；Pixel History 请求会等待隔离 Replay Worker 就绪。"),
						TEXT("尚无因果证据。"),
						FString::Printf(TEXT("预览来源：%s。Replay 就绪后会用精确最终 RT 替换当前预览。"),
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
				bPreviewReadyForSelection = true;
				const int32 Width = static_cast<int32>(Message->GetNumberField(TEXT("width")));
				const int32 Height = static_cast<int32>(Message->GetNumberField(TEXT("height")));
				const FString Path = Message->GetStringField(TEXT("previewPath"));
				const FString Target = Message->GetStringField(TEXT("targetName"));
				const FString Version = Message->GetStringField(TEXT("renderDocVersion"));
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
					TEXT("Isolated Replay Worker ready: elapsed=%.3fs RenderDoc=%s Size=%dx%d Target='%s' Resource=%d Samples=%d Format='%s' PixelHistory=%s ShaderDebug=%s Preview='%s'"),
					bCaptureLoading ? FPlatformTime::Seconds() - CaptureLoadStartSeconds : 0.0,
					*Version, Width, Height, *Target, ReplayTargetResourceIndex, ReplayTargetSamples, *ReplayTargetFormat,
					bPixelHistory ? TEXT("yes") : TEXT("no"),
					bShaderDebug ? TEXT("yes") : TEXT("no"), *Path);
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
				SetStatus(FString::Printf(TEXT("RenderDoc %s | %dx%d | %s | Pixel History: %s | Shader Debug: %s | 载入耗时 %.1fs"),
					*Version, Width, Height, *Target, bPixelHistory ? TEXT("yes") : TEXT("no"), bShaderDebug ? TEXT("yes") : TEXT("no"), TotalLoadElapsed));
				SetReportCards(
					TEXT("截帧已载入。直接选择一个需要解释的位置，不必浏览整棵事件树。"),
					TEXT("最终画面\n↓\n等待选择 P1"),
					TEXT("尚无候选原因。"),
					TEXT("截帧已由隔离 Replay Worker 完整载入；等待确认选点，尚未执行 Pixel History 查询。"));
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
				LastWorkerError = FString::Printf(TEXT("%s: %s"), *Stage, *Error);
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
					FailedShaderDebugIds.Add(*EventId);
					PendingShaderDebugByRequest.Remove(RequestId);
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

		void QueryPixel(int32 X, int32 Y)
		{
			if (!bWorkerReady && !bPreviewReadyForSelection && !bReplayStartDeferred)
			{
				SetStatus(TEXT("Replay Worker is not ready."));
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

		void StoreEventContext(const TSharedRef<FJsonObject>& Message)
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
			ScheduleResourcePixelHistories(EventContexts.FindChecked(EventId));
			ScheduleProducerEventContexts(EventContexts.FindChecked(EventId));
			RenderCausalReport();
			ResumeAgentAfterEventContext(EventId);
			SetStatus(FString::Printf(TEXT("Event %u 的 Pipeline、资源绑定和 Shader 反射已加载；详细内容可展开查看。"), EventId));
			TryResumeAgentAfterDeterministicContexts();
		}

		void AddResourceTraceBoundary(FEventContextEvidence& Context, const FResourcePixelHistoryRequest& TraceRequest,
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
			Boundary->SetStringField(TEXT("branchStatus"), Status);
			Boundary->SetStringField(TEXT("detail"), Detail);
			Context.ResourcePixelHistories.Add(MakeShared<FJsonValueObject>(Boundary));
		}

		void ScheduleResourcePixelHistories(const FEventContextEvidence& ReadOnlyContext)
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

			FIntPoint ConsumerPixel = EventTracePixels.FindRef(ReadOnlyContext.EventId);
			if (ConsumerPixel == FIntPoint::ZeroValue && !Samples.IsEmpty())
			{
				ConsumerPixel = Samples[0].Pixel;
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
				ConsumerPixel.X, ConsumerPixel.Y, ResourcePixelHistoryQueriesSubmitted, MaxResourcePixelHistoryQueries));

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
				const int32 SamplesToTrace = FMath::Min(AvailableSamples, MaxSamplesPerResourceTrace);
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
					TraceRequest.bRequiredForAgent = IsCriticalAgentEvent(ReadOnlyContext.EventId)
						&& SampleIndex == 0;
					TraceRequest.TraceKey = FString::Printf(TEXT("%u:%d:%d:%d:%d:%d:%d:%d"),
						TraceRequest.ConsumerEventId, TraceRequest.ResourceIndex, TraceRequest.Pixel.X, TraceRequest.Pixel.Y,
						TraceRequest.Mip, TraceRequest.Slice, TraceRequest.Sample, TraceRequest.TypeCast);
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

			auto ScheduleTrace = [this, MutableContext](const FResourcePixelHistoryRequest& TraceRequest)
			{
				if (ScheduledResourcePixelHistoryKeys.Contains(TraceRequest.TraceKey))
				{
					ResourcePixelHistoryBindingAliases.FindOrAdd(TraceRequest.TraceKey).AddUnique(TraceRequest.ShaderBinding);
					Diagnostics.WriteRecord(TEXT("trace_branch_deduplicated"), FString::Printf(
						TEXT("key=%s consumer=%u resource=%d binding=%s sample=%d"), *TraceRequest.TraceKey,
						TraceRequest.ConsumerEventId, TraceRequest.ResourceIndex, *TraceRequest.ShaderBinding, TraceRequest.Sample));
					return;
				}
				ScheduledResourcePixelHistoryKeys.Add(TraceRequest.TraceKey);
				ResourcePixelHistoryBindingAliases.FindOrAdd(TraceRequest.TraceKey).AddUnique(TraceRequest.ShaderBinding);
				if (ResourcePixelHistoryQueriesSubmitted >= MaxResourcePixelHistoryQueries)
				{
					AddResourceTraceBoundary(*MutableContext, TraceRequest, TEXT("query-budget-exhausted"),
						TEXT("The sample branch is recorded but was not queried because the bounded replay budget was reached."));
					Diagnostics.WriteRecord(TEXT("trace_branch_budget"), FString::Printf(
						TEXT("key=%s submitted=%d limit=%d"), *TraceRequest.TraceKey,
						ResourcePixelHistoryQueriesSubmitted, MaxResourcePixelHistoryQueries));
					return;
				}
				++ResourcePixelHistoryQueriesSubmitted;
				const FString RequestId = FString::Printf(TEXT("resource-pixel-%u-%d-s%d-query-%llu"),
					TraceRequest.ConsumerEventId, TraceRequest.ResourceIndex, TraceRequest.Sample, ++RequestSerial);
				PendingResourcePixelHistoryByRequest.Add(RequestId, TraceRequest);
				Diagnostics.WriteRecord(TEXT("trace_branch_queued"), FString::Printf(
					TEXT("request=%s required=%s depth=%d key=%s resource=%s binding=%s pixel=(%d,%d) sample=%d pending=%d"),
					*RequestId, TraceRequest.bRequiredForAgent ? TEXT("true") : TEXT("false"), TraceRequest.ReverseDepth,
					*TraceRequest.TraceKey, *TraceRequest.ResourceName, *TraceRequest.ShaderBinding,
					TraceRequest.Pixel.X, TraceRequest.Pixel.Y, TraceRequest.Sample,
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
					}))
				{
					PendingResourcePixelHistoryByRequest.Remove(RequestId);
					FailedResourcePixelHistoryKeys.Add(TraceRequest.TraceKey);
					AddResourceTraceBoundary(*MutableContext, TraceRequest, TEXT("queue-failed"),
						TEXT("Replay Worker request could not be queued."));
				}
			};

			for (const FResourcePixelHistoryRequest& TraceRequest : FastFrontier)
			{
				ScheduleTrace(TraceRequest);
			}
			if (bBackgroundDeterministicQueriesReleased)
			{
				for (const FResourcePixelHistoryRequest& TraceRequest : BackgroundFrontier)
				{
					ScheduleTrace(TraceRequest);
				}
			}
			else if (!BackgroundFrontier.IsEmpty())
			{
				DeferredResourceHistoryContextIds.Add(ReadOnlyContext.EventId);
				DeferredResourceHistoryBranchCounts.Add(ReadOnlyContext.EventId, BackgroundFrontier.Num());
				Diagnostics.WriteRecord(TEXT("trace_background_deferred"), FString::Printf(
					TEXT("event=%u depth=%d branches=%d pendingCriticalContexts=%d pendingRequired=%d"),
					ReadOnlyContext.EventId, ReverseDepth, BackgroundFrontier.Num(),
					GetPendingCriticalContextCount(), GetPendingRequiredResourceHistoryCount()));
			}
			Diagnostics.WriteRecord(TEXT("trace_schedule_end"), FString::Printf(
				TEXT("event=%u depth=%d fast=%d background=%d backgroundDeferred=%s pendingRequired=%d pendingBackground=%d submitted=%d/%d"),
				ReadOnlyContext.EventId, ReverseDepth, FastFrontier.Num(), BackgroundFrontier.Num(),
				(!bBackgroundDeterministicQueriesReleased && !BackgroundFrontier.IsEmpty()) ? TEXT("true") : TEXT("false"),
				GetPendingRequiredResourceHistoryCount(), GetPendingBackgroundResourceHistoryCount(),
				ResourcePixelHistoryQueriesSubmitted, MaxResourcePixelHistoryQueries));
		}

		void EnrichResourcePixelHistoryFromShaderTrace(const FEventContextEvidence& Context,
			const TSharedRef<FJsonObject>& Evidence) const
		{
			if (!Context.ShaderDebugTrace.IsValid())
			{
				return;
			}
			FString ShaderBinding;
			double X = 0.0;
			double Y = 0.0;
			double SampleIndex = 0.0;
			Evidence->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
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
				double Instruction = 0.0;
				if (Access->TryGetNumberField(TEXT("instruction"), Instruction))
				{
					Evidence->SetNumberField(TEXT("shaderAccessInstruction"), Instruction);
				}
				Evidence->SetBoolField(TEXT("shaderCoordinateValuesMatched"), bSawX && bSawY);
				if (bSawX && bSawY)
				{
					Evidence->SetStringField(TEXT("coordinateMapping"), TEXT("executed-shader-load-coordinate"));
					Evidence->SetStringField(TEXT("mappingConfidence"), TEXT("confirmed-executed-values"));
				}
				return;
			}
		}

		void StoreResourcePixelHistory(const TSharedRef<FJsonObject>& Message)
		{
			FString RequestId;
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			const FResourcePixelHistoryRequest* PendingRequest = PendingResourcePixelHistoryByRequest.Find(RequestId);
			if (!PendingRequest)
			{
				return;
			}
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
			const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
			int32 ExpandedWriterCount = 0;
			int32 ConfirmedWriterCount = 0;
			if (Message->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
			{
				Evidence->SetArrayField(TEXT("eventSummaries"), *EventSummaries);
				for (int32 Index = EventSummaries->Num() - 1; Index >= 0; --Index)
				{
					const TSharedPtr<FJsonObject> Summary = (*EventSummaries)[Index].IsValid()
						? (*EventSummaries)[Index]->AsObject() : nullptr;
					if (!Summary.IsValid())
					{
						continue;
					}
					double EventIdValue = 0.0;
					double PassedFragments = 0.0;
					bool bChangedTextureValue = false;
					Summary->TryGetNumberField(TEXT("eventId"), EventIdValue);
					Summary->TryGetNumberField(TEXT("passedFragments"), PassedFragments);
					Summary->TryGetBoolField(TEXT("changedTextureValue"), bChangedTextureValue);
					const uint32 WriterEventId = static_cast<uint32>(EventIdValue);
					if (WriterEventId == 0 || WriterEventId == TraceRequest.ConsumerEventId
						|| (PassedFragments <= 0.0 && !bChangedTextureValue))
					{
						continue;
					}
					++ConfirmedWriterCount;
					ConfirmedWriterIds.Add(MakeShared<FJsonValueNumber>(WriterEventId));
					EventTracePixels.FindOrAdd(WriterEventId, TraceRequest.Pixel);
					if (ExpandedWriterCount < MaxWriterContextsPerResourceTrace)
					{
						EnsureEventContext(WriterEventId, TraceRequest.ReverseDepth + 1);
						++ExpandedWriterCount;
					}
				}
			}
			Evidence->SetArrayField(TEXT("confirmedWriterEventIds"), MoveTemp(ConfirmedWriterIds));
			if (ConfirmedWriterCount == 0)
			{
				Evidence->SetStringField(TEXT("branchStatus"), TEXT("no-modification-before-consumer"));
			}
			else if (ConfirmedWriterCount > ExpandedWriterCount)
			{
				Evidence->SetStringField(TEXT("branchStatus"), TEXT("writer-context-limit"));
				Evidence->SetStringField(TEXT("detail"), FString::Printf(TEXT("%d pixel writers found; %d latest writer contexts expanded."),
					ConfirmedWriterCount, ExpandedWriterCount));
			}
			else
			{
				Evidence->SetStringField(TEXT("branchStatus"), TEXT("continued-to-pixel-writer"));
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

		void StoreShaderDebug(const TSharedRef<FJsonObject>& Message)
		{
			FString RequestId;
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			const uint32 EventId = static_cast<uint32>(Message->GetNumberField(TEXT("eventId")));
			const uint32* RequestedEventId = PendingShaderDebugByRequest.Find(RequestId);
			if (!RequestedEventId || *RequestedEventId != EventId)
			{
				return;
			}
			CompleteWorkerRequest(RequestId, TEXT("shader_debug"), FString::Printf(TEXT("event=%u"), EventId));
			PendingShaderDebugByRequest.Remove(RequestId);
			FEventContextEvidence& Context = EventContexts.FindOrAdd(EventId);
			Context.EventId = EventId;
			Context.ShaderDebugTrace = Message;
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
				TEXT("request=%s event=%u histories=%d"),
				*RequestId, EventId, Context.ResourcePixelHistories.Num()));
			RenderCausalReport();
			SetStatus(FString::Printf(TEXT("EID %u 的 Pixel Shader 指令追踪已加载；详细证据已更新。"), EventId));
			TryResumeAgentAfterDeterministicContexts();
		}

		void EnsureEventContext(uint32 EventId, int32 ReverseDepth = 0)
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
				return;
			}
			if (EventContexts.Num() + PendingEventContextIds.Num() + DeferredEventContextIds.Num()
				>= MaxDeterministicContextEvents)
			{
				Diagnostics.WriteRecord(TEXT("event_context_skipped"), FString::Printf(
					TEXT("event=%u depth=%d reason=context-limit limit=%d"),
					EventId, ReverseDepth, MaxDeterministicContextEvents));
				return;
			}
			if (!IsCriticalAgentEvent(EventId) && !bBackgroundDeterministicQueriesReleased)
			{
				DeferredEventContextIds.Add(EventId);
				Diagnostics.WriteRecord(TEXT("event_context_deferred"), FString::Printf(
					TEXT("event=%u depth=%d pendingCritical=%d"),
					EventId, ReverseDepth, GetPendingCriticalContextCount()));
				return;
			}
			const FString RequestId = FString::Printf(TEXT("context-%u-query-%llu"), EventId, ++RequestSerial);
			PendingEventContextByRequest.Add(RequestId, EventId);
			PendingEventContextIds.Add(EventId);
			Diagnostics.WriteRecord(TEXT("event_context_queued"), FString::Printf(
				TEXT("request=%s event=%u depth=%d critical=%s pending=%d"),
				*RequestId, EventId, ReverseDepth, IsCriticalAgentEvent(EventId) ? TEXT("true") : TEXT("false"),
				PendingEventContextIds.Num()));
			if (!SendWorkerRequest(TEXT("event_context"), RequestId,
				[EventId](const TSharedRef<FJsonObject>& Request)
				{
					Request->SetNumberField(TEXT("eventId"), EventId);
				}))
			{
				PendingEventContextByRequest.Remove(RequestId);
				PendingEventContextIds.Remove(EventId);
				FailedEventContextIds.Add(EventId);
			}
		}

		void ReleaseBackgroundDeterministicQueries()
		{
			if (bBackgroundDeterministicQueriesReleased || HasPendingCriticalDeterministicQueries())
			{
				return;
			}
			bBackgroundDeterministicQueriesReleased = true;
			Diagnostics.WriteRecord(TEXT("background_trace_release_begin"), FString::Printf(
				TEXT("resourceContexts=%d eventContexts=%d"),
				DeferredResourceHistoryContextIds.Num(), DeferredEventContextIds.Num()));

			TArray<uint32> ResourceContextIds = DeferredResourceHistoryContextIds.Array();
			DeferredResourceHistoryContextIds.Empty();
			DeferredResourceHistoryBranchCounts.Empty();
			for (const uint32 EventId : ResourceContextIds)
			{
				if (const FEventContextEvidence* Context = EventContexts.Find(EventId))
				{
					ScheduleResourcePixelHistories(*Context);
				}
			}

			TArray<uint32> EventIds = DeferredEventContextIds.Array();
			DeferredEventContextIds.Empty();
			EventIds.Sort([this](uint32 A, uint32 B)
			{
				const int32 DepthA = EventContextDepths.FindRef(A);
				const int32 DepthB = EventContextDepths.FindRef(B);
				return DepthA == DepthB ? A > B : DepthA < DepthB;
			});
			for (const uint32 EventId : EventIds)
			{
				EnsureEventContext(EventId, EventContextDepths.FindRef(EventId));
			}
			Diagnostics.WriteRecord(TEXT("background_trace_release_end"), FString::Printf(
				TEXT("pendingBackgroundResources=%d pendingEventContexts=%d"),
				GetPendingBackgroundResourceHistoryCount(), PendingEventContextIds.Num()));
		}

		void ScheduleProducerEventContexts(const FEventContextEvidence& Context)
		{
			const int32 ConsumerDepth = EventContextDepths.FindRef(Context.EventId);
			if (ConsumerDepth >= MaxCausalGraphHops - 1)
			{
				return;
			}

			int32 ScheduledForConsumer = 0;
			for (const TSharedPtr<FJsonValue>& ProvenanceValue : Context.ResourceProvenance)
			{
				if (ScheduledForConsumer >= MaxRecursiveProducerContextsPerEvent
					|| EventContexts.Num() + PendingEventContextIds.Num() >= MaxDeterministicContextEvents)
				{
					break;
				}
				const TSharedPtr<FJsonObject> Provenance = ProvenanceValue.IsValid() ? ProvenanceValue->AsObject() : nullptr;
				if (!Provenance.IsValid())
				{
					continue;
				}
				bool bProducerFound = false;
				FString ProducerStatus;
				FString ShaderBinding;
				double ResourceIndex = INDEX_NONE;
				double ProducerEventId = 0.0;
				Provenance->TryGetBoolField(TEXT("producerFound"), bProducerFound);
				Provenance->TryGetStringField(TEXT("producerStatus"), ProducerStatus);
				Provenance->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
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
				EnsureEventContext(EventId, ConsumerDepth + 1);
				++ScheduledForConsumer;
			}
		}

		void EnsureRelevantEventContexts()
		{
			if (!bWorkerReady)
			{
				return;
			}

			TArray<uint32> RelevantEventIds;
			int32 WriterContextCount = 0;
			for (const FPixelSample& Sample : Samples)
			{
				const TArray<FEventEvidence> Events = AggregateEvents(Sample);
				for (int32 Index = Events.Num() - 1; Index >= 0; --Index)
				{
					const FEventEvidence& Event = Events[Index];
					if (!IsConfirmedPixelWriter(Event) || Event.ActionKind == TEXT("present"))
					{
						continue;
					}
					RelevantEventIds.AddUnique(Event.EventId);
					++WriterContextCount;
					if (WriterContextCount >= MaxCausalGraphHops)
					{
						break;
					}
				}
				if (WriterContextCount >= MaxCausalGraphHops)
				{
					break;
				}
			}
			for (const FPixelSample& Sample : Samples)
			{
				const TArray<FEventEvidence> Events = AggregateEvents(Sample);
				for (int32 Index = Events.Num() - 1; Index >= 0 && RelevantEventIds.Num() < MaxInitialEventContexts; --Index)
				{
					const FEventEvidence& Event = Events[Index];
					if (Event.ActionKind != TEXT("present") && !IsConfirmedPixelWriter(Event))
					{
						RelevantEventIds.AddUnique(Event.EventId);
					}
				}
			}

			for (const uint32 EventId : RelevantEventIds)
			{
				EnsureEventContext(EventId);
			}
		}

		void EnsureCandidateShaderDebug()
		{
			if (!bShaderDebuggingAvailable || !LastCandidate.IsSet() || !bWorkerReady)
			{
				return;
			}

			TArray<FEventEvidence> DebugCandidates;
			DebugCandidates.Add(LastCandidate->Event);
			if (LastSignificantCandidate.IsSet()
				&& LastSignificantCandidate->Event.EventId != LastCandidate->Event.EventId)
			{
				DebugCandidates.Add(LastSignificantCandidate->Event);
			}

			for (const FEventEvidence& Candidate : DebugCandidates)
			{
				if (Candidate.ActionKind != TEXT("draw") || FailedShaderDebugIds.Contains(Candidate.EventId))
				{
					continue;
				}
				if (const FEventContextEvidence* Context = EventContexts.Find(Candidate.EventId))
				{
					if (Context->ShaderDebugTrace.IsValid())
					{
						continue;
					}
				}
				bool bShaderDebugPending = false;
				for (const TPair<FString, uint32>& Pair : PendingShaderDebugByRequest)
				{
					if (Pair.Value == Candidate.EventId)
					{
						bShaderDebugPending = true;
						break;
					}
				}
				if (bShaderDebugPending)
				{
					continue;
				}

				const FPixelSample* SourceSample = nullptr;
				for (const FPixelSample& Sample : Samples)
				{
					const TArray<FEventEvidence> Events = AggregateEvents(Sample);
					if (FindEvent(Events, Candidate.EventId))
					{
						SourceSample = &Sample;
						break;
					}
				}
				if (!SourceSample)
				{
					continue;
				}

				const FString RequestId = FString::Printf(TEXT("shader-debug-%u-query-%llu"), Candidate.EventId, ++RequestSerial);
				PendingShaderDebugByRequest.Add(RequestId, Candidate.EventId);
				Diagnostics.WriteRecord(TEXT("shader_debug_queued"), FString::Printf(
					TEXT("request=%s event=%u pixel=(%d,%d) primitive=%u hasPrimitive=%s"),
					*RequestId, Candidate.EventId, SourceSample->Pixel.X, SourceSample->Pixel.Y,
					Candidate.PrimitiveId, Candidate.bHasPrimitiveEvidence ? TEXT("true") : TEXT("false")));
				if (!SendWorkerRequest(TEXT("shader_debug"), RequestId,
					[Candidate, SourceSample](const TSharedRef<FJsonObject>& Request)
					{
						Request->SetNumberField(TEXT("eventId"), Candidate.EventId);
						Request->SetNumberField(TEXT("x"), SourceSample->Pixel.X);
						Request->SetNumberField(TEXT("y"), SourceSample->Pixel.Y);
						Request->SetNumberField(TEXT("sample"), 0);
						Request->SetNumberField(TEXT("primitiveId"), Candidate.PrimitiveId);
						Request->SetBoolField(TEXT("hasPrimitive"), Candidate.bHasPrimitiveEvidence);
					}))
				{
					PendingShaderDebugByRequest.Remove(RequestId);
					FailedShaderDebugIds.Add(Candidate.EventId);
				}
			}
		}

		void TryResumeAgentAfterDeterministicContexts()
		{
			if (bAgentWaitingForDeterministicContexts && !HasPendingCriticalDeterministicQueries())
			{
				bAgentWaitingForDeterministicContexts = false;
				StartAgentAnalysis();
			}
			ReleaseBackgroundDeterministicQueries();
		}

		void StorePixelHistory(const TSharedRef<FJsonObject>& Message)
		{
			FString RequestId;
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			const uint64* SampleId = PendingSampleByRequest.Find(RequestId);
			if (!SampleId)
			{
				return;
			}
			FPixelSample* Sample = FindSample(*SampleId);
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
			for (const FEventSummaryEvidence& Event : Sample->EventSummaries)
			{
				if (Event.EventId != 0 && Event.ActionKind != TEXT("present")
					&& (Event.PassedFragments > 0 || Event.bChangedTextureValue))
				{
					EventTracePixels.FindOrAdd(Event.EventId, Sample->Pixel);
				}
			}

			UpdateSelectionText();
			RenderCausalReport();
			if (!bAgentRunning)
			{
				const bool bAnyPending = Samples.ContainsByPredicate([](const FPixelSample& Item)
				{
					return Item.bPending;
				});
				SetAgentStatus(bAnyPending
					? TEXT("部分 Pixel History 已返回 · 仍有查询进行中")
					: TEXT("确定性溯源完成 · 可以围绕当前 P1 向 Agent 提问"));
			}
			const uint64 CompletedSampleId = Sample->Id;
			const int32 SampleIndex = Samples.IndexOfByPredicate([CompletedSampleId](const FPixelSample& Item) { return Item.Id == CompletedSampleId; });
			SetStatus(FString::Printf(TEXT("关注点 P%d (%d, %d)：%d 个 RenderDoc modification，分析已刷新。"),
				SampleIndex + 1, Sample->Pixel.X, Sample->Pixel.Y, Sample->TotalModifications));
		}

		static void AddHypothesis(TArray<FString>& Hypotheses, const FString& Hypothesis)
		{
			if (!Hypothesis.IsEmpty() && Hypotheses.Num() < 3)
			{
				Hypotheses.AddUnique(Hypothesis);
			}
		}

		static void AddFailureHypothesis(const FString& Failure, TArray<FString>& Hypotheses)
		{
			if (Failure == TEXT("depth-test"))
				AddHypothesis(Hypotheses, TEXT("深度测试拒绝：检查遮挡顺序、深度输出和当前深度比较方式。"));
			else if (Failure == TEXT("stencil-test"))
				AddHypothesis(Hypotheses, TEXT("模板测试拦截：检查模板参考值、掩码，以及更早写入模板的事件。"));
			else if (Failure == TEXT("shader-discard"))
				AddHypothesis(Hypotheses, TEXT("Shader 主动丢弃：检查透明度遮罩、clip 逻辑及其输入值。"));
			else if (Failure == TEXT("backface-cull"))
				AddHypothesis(Hypotheses, TEXT("背面剔除：检查顶点绕序、镜像变换和材质双面设置。"));
			else if (Failure == TEXT("scissor-clip") || Failure == TEXT("viewport-clip") || Failure == TEXT("depth-clip") || Failure == TEXT("depth-bounds"))
				AddHypothesis(Hypotheses, TEXT("光栅范围裁剪：检查 Viewport、Scissor、投影矩阵和深度范围。"));
			else if (Failure == TEXT("sample-mask"))
				AddHypothesis(Hypotheses, TEXT("采样覆盖不足：检查 MSAA Sample Mask 和 Alpha-to-Coverage。"));
			else if (Failure == TEXT("predication-skipped"))
				AddHypothesis(Hypotheses, TEXT("条件渲染跳过：检查 GPU Predicate 及生成该条件的上游事件。"));
		}

		void RenderCausalReport()
		{
			LastCandidate.Reset();
			LastSignificantCandidate.Reset();
			bLastCandidateHasDivergence = false;
			if (Samples.IsEmpty())
			{
				SetReportCards(
					TEXT("等待选择关注像素。这里不要求你提供一个“正确”的对照点。"),
					TEXT("最终画面\n↓\n等待选择 P1"),
					TEXT("尚无候选原因。"),
					TEXT("尚未执行 Pixel History 查询。"));
				return;
			}

			if (!bSelectionConfirmed)
			{
				SetReportCards(
					TEXT("选点已就绪，等待确认后开始分析。"),
					TEXT("最终画面\n↓\n已选 P 点\n↓\n等待确认"),
					TEXT("当前不会读取 Pixel History，也不会进行因果判断。"),
					TEXT("选点阶段不会触发后台查询；点击“分析”后才开始读取证据。"));
				return;
			}

			FString Report = FString::Printf(
				TEXT("BOUNDED SINGLE-PIXEL CAUSAL REPORT\n\nReplay target: resource=%d format=%s extent=%dx%d samples=%d\n\nPixel\n"),
				ReplayTargetResourceIndex, ReplayTargetFormat.IsEmpty() ? TEXT("unknown") : *ReplayTargetFormat,
				CurrentPreviewSize.X, CurrentPreviewSize.Y, ReplayTargetSamples);
			bool bAnyPending = false;
			bool bAnyIncompleteEventSummary = false;
			TArray<const FPixelSample*> ReadySamples;
			for (int32 Index = 0; Index < Samples.Num(); ++Index)
			{
				const FPixelSample& Sample = Samples[Index];
				bAnyPending |= Sample.bPending;
				FString State;
				if (Sample.bPending)
				{
					State = TEXT("querying");
				}
				else if (Sample.bFailed)
				{
					State = FString::Printf(TEXT("failed - %s"), *Sample.Error);
				}
				else
				{
					ReadySamples.Add(&Sample);
					bAnyIncompleteEventSummary |= !Sample.bEventSummaryComplete;
					State = FString::Printf(TEXT("%d modifications%s"), Sample.TotalModifications,
						Sample.bTruncated
							? (Sample.bEventSummaryComplete ? TEXT(", detail tail truncated; event summary complete") : TEXT(", event summary incomplete"))
							: TEXT(""));
				}
				Report += FString::Printf(TEXT("- P%d (%d, %d): %s\n"),
					Index + 1, Sample.Pixel.X, Sample.Pixel.Y, *State);
			}

			if (bAnyPending)
			{
				Report += TEXT("\nWaiting for bounded RenderDoc queries. No causal claim is made yet.");
				SetReportCards(
					TEXT("正在查询所选像素的 Pixel History；结果返回前不做因果判断。"),
					TEXT("最终画面\n↓\n正在收集 P 点的事件证据…"),
					TEXT("等待查询完成。"),
					Report);
				return;
			}

			if (ReadySamples.IsEmpty())
			{
				Report += TEXT("\nThe selected pixel query failed. The chain is broken before pixel evidence.");
				SetReportCards(
					TEXT("当前像素查询失败，目前没有 Pixel History 可以支持结论。"),
					TEXT("最终画面\n↓\nPixel History 不可用\n↓\n■ 因果链中断"),
					TEXT("可能是目标纹理、API 或当前截帧不支持 Pixel History；尚未证明任何渲染原因。"),
					Report);
				return;
			}

			if (bAnyIncompleteEventSummary)
			{
				Report += TEXT("\nEvent summary is incomplete for at least one point; the deterministic chain is stopped instead of guessing from a truncated tail.\n");
				SetReportCards(
					TEXT("Pixel History 返回了不完整的事件摘要，当前不会继续生成溯源结论。"),
					TEXT("最终画面\n↓\n事件摘要不完整\n↓\n■ 溯源暂停"),
					TEXT("需要重新编译并使用支持完整 eventSummaries 的 Replay Worker；这不是 Agent 可以补齐的缺口。"),
					Report);
				return;
			}

			TMap<uint64, TArray<FEventEvidence>> AggregatedBySample;
			for (const FPixelSample* Sample : ReadySamples)
			{
				AggregatedBySample.Add(Sample->Id, AggregateEvents(*Sample));
			}

			const FPixelSample* BaseSample = ReadySamples[0];
			const TArray<FEventEvidence>* BaseEvents = AggregatedBySample.Find(BaseSample->Id);

			FCausalCandidate Candidate;
			bool bHasCandidate = false;
			int32 FinalWriterIndex = INDEX_NONE;
			if (BaseEvents)
			{
				for (int32 EventIndex = BaseEvents->Num() - 1; EventIndex >= 0; --EventIndex)
				{
					const FEventEvidence& Event = (*BaseEvents)[EventIndex];
					if (Event.ActionKind == TEXT("present")
						|| (Event.PassedFragments <= 0 && !(Event.bDirectShaderWrite && Event.bChangedTextureValue)))
					{
						continue;
					}
					Candidate.Event = Event;
					Candidate.SampleCoverage = 1;
					bHasCandidate = true;
					FinalWriterIndex = EventIndex;
					break;
				}
			}

			if (bHasCandidate)
			{
				LastCandidate = Candidate;
				bLastCandidateHasDivergence = false;
				if (Candidate.Event.ColorDeltaMax >= 0.0
					&& Candidate.Event.ColorDeltaMax < SignificantColorDeltaThreshold && BaseEvents)
				{
					for (int32 EventIndex = FinalWriterIndex - 1; EventIndex >= 0; --EventIndex)
					{
						const FEventEvidence& Event = (*BaseEvents)[EventIndex];
						if (!IsConfirmedPixelWriter(Event) || Event.ActionKind == TEXT("present")
							|| Event.ColorDeltaMax < SignificantColorDeltaThreshold)
						{
							continue;
						}
						FCausalCandidate SignificantCandidate;
						SignificantCandidate.Event = Event;
						SignificantCandidate.SampleCoverage = 1;
						LastSignificantCandidate = MoveTemp(SignificantCandidate);
						break;
					}
				}
			}
			EnsureRelevantEventContexts();
			EnsureCandidateShaderDebug();

			FString CausalPath = TEXT("最终画面");
			TArray<FString> Hypotheses;
			for (const FPixelSample* Sample : ReadySamples)
			{
				const int32 DisplayIndex = Samples.IndexOfByPredicate([Sample](const FPixelSample& Item)
				{
					return Item.Id == Sample->Id;
				});
				const TArray<FEventEvidence>* Events = AggregatedBySample.Find(Sample->Id);
				const FEventEvidence* LatestWriter = nullptr;
				if (Events)
				{
					for (int32 EventIndex = Events->Num() - 1; EventIndex >= 0; --EventIndex)
					{
						const FEventEvidence& Event = (*Events)[EventIndex];
						if (Event.PassedFragments > 0 || (Event.bDirectShaderWrite && Event.bChangedTextureValue))
						{
							LatestWriter = &Event;
							break;
						}
					}
				}
				if (LatestWriter)
				{
					Report += FString::Printf(TEXT("- P%d latest observed writer: EID %u [%s] %s.\n"),
						DisplayIndex + 1, LatestWriter->EventId, *LatestWriter->ActionKind, *LatestWriter->Action);
					CausalPath += FString::Printf(TEXT("\n↓\nP%d (%d,%d) ← EID %u · %s"),
						DisplayIndex + 1, Sample->Pixel.X, Sample->Pixel.Y, LatestWriter->EventId, *LatestWriter->Action);
				}
				else
				{
					Report += FString::Printf(TEXT("- P%d has no confirmed write in the selected target history.\n"), DisplayIndex + 1);
					CausalPath += FString::Printf(TEXT("\n↓\nP%d (%d,%d) ← 未确认写入"), DisplayIndex + 1, Sample->Pixel.X, Sample->Pixel.Y);
					AddHypothesis(Hypotheses, FString::Printf(
						TEXT("P%d 没有确认写入：可能只保留了 Clear/背景、没有光栅覆盖，或目标不支持完整 Pixel History；这不等于已经证明对象被剔除。"),
						DisplayIndex + 1));
				}
			}

			FString Summary;
			if (bHasCandidate)
			{
				const FString Semantics = ClassifySemantics(Candidate.Event);
				const bool bDeltaKnown = Candidate.Event.ColorDeltaMax >= 0.0;
				const bool bMinorFinalAdjustment = bDeltaKnown
					&& Candidate.Event.ColorDeltaMax < SignificantColorDeltaThreshold;
				if (!bDeltaKnown)
				{
					Summary = FString::Printf(TEXT("EID %u 是 P1 的最后物理写入者，但 Before/After 数值不完整，当前不能判断它是末端微调还是主要颜色形成事件。"),
						Candidate.Event.EventId);
					CausalPath += FString::Printf(TEXT("\n↓\n最后写入 EID %u · %s（颜色差值不可用）"),
						Candidate.Event.EventId, *Candidate.Event.Action);
				}
				else if (bMinorFinalAdjustment)
				{
					Summary = LastSignificantCandidate.IsSet()
						? FString::Printf(TEXT("EID %u 是 P1 的最后物理写入者，但最大颜色变化仅 %.6g，属于末端微调；更早的 EID %u 是当前最近的显著写入候选，颜色形成原因仍需沿资源依赖继续证明。"),
							Candidate.Event.EventId, Candidate.Event.ColorDeltaMax, LastSignificantCandidate->Event.EventId)
						: FString::Printf(TEXT("EID %u 是 P1 的最后物理写入者，但最大颜色变化仅 %.6g，属于末端微调；当前链中尚未找到可确认的显著上游写入。"),
							Candidate.Event.EventId, Candidate.Event.ColorDeltaMax);
					CausalPath += FString::Printf(TEXT("\n↓\n最后写入 EID %u · %s（末端微调，Δmax=%.6g）"),
						Candidate.Event.EventId, *Candidate.Event.Action, Candidate.Event.ColorDeltaMax);
					if (LastSignificantCandidate.IsSet())
					{
						CausalPath += FString::Printf(TEXT("\n↓ 继续反向追踪\n显著写入候选 EID %u · %s（Δmax=%.6g）"),
							LastSignificantCandidate->Event.EventId, *LastSignificantCandidate->Event.Action,
							LastSignificantCandidate->Event.ColorDeltaMax);
					}
				}
				else
				{
					Summary = FString::Printf(TEXT("EID %u 是 P1 的最后物理写入者，并造成显著颜色变化（Δmax=%.6g）；它是当前主要写入候选，但跨 RT、材质和物体归属仍只接受显式证据。"),
						Candidate.Event.EventId, Candidate.Event.ColorDeltaMax);
					CausalPath += FString::Printf(TEXT("\n↓\n最后且显著写入 EID %u · %s（Δmax=%.6g）"),
						Candidate.Event.EventId, *Candidate.Event.Action, Candidate.Event.ColorDeltaMax);
				}
				CausalPath += FString::Printf(TEXT("\n↓\n■ 当前反向追踪边界：%s"),
					Semantics == TEXT("resample/nonlinear")
						? TEXT("发生缩放/重采样，不能假定同坐标继续上溯")
						: TEXT("输入资源仅按 producer 关系有界展开；像素贡献与坐标映射必须另行证明"));
				Report += FString::Printf(
					TEXT("\nFinal writer\n- EID %u [%s / %s] %s\n- result: %s\n- color change: %s；deltaMax=%.9g；deltaL1=%.9g\n"),
					Candidate.Event.EventId, *Candidate.Event.ActionKind, *Semantics, *Candidate.Event.Action,
					*DescribeEventResult(Candidate.Event), *ClassifyColorDelta(Candidate.Event),
					Candidate.Event.ColorDeltaMax, Candidate.Event.ColorDeltaL1);
				if (LastSignificantCandidate.IsSet())
				{
					const FEventEvidence& Significant = LastSignificantCandidate->Event;
					Report += FString::Printf(TEXT("\nSignificant upstream writer candidate\n- EID %u [%s / %s] %s\n- color change: %s；deltaMax=%.9g；deltaL1=%.9g\n"),
						Significant.EventId, *Significant.ActionKind, *ClassifySemantics(Significant), *Significant.Action,
						*ClassifyColorDelta(Significant), Significant.ColorDeltaMax, Significant.ColorDeltaL1);
				}

				for (const FString& Failure : Candidate.Event.FailureReasons)
				{
					AddFailureHypothesis(Failure, Hypotheses);
				}
				if (Candidate.Event.bUnboundPixelShader)
				{
					AddHypothesis(Hypotheses, TEXT("该事件没有可确认的像素着色器输出；检查 PS/RT 绑定，但不要据此推断 Mesh 身份。"));
				}
				if (Candidate.Event.bDirectShaderWrite)
				{
					AddHypothesis(Hypotheses, Candidate.Event.bChangedTextureValue
						? TEXT("UAV/Storage 写入改变了像素；当前证据指向 Dispatch/直接写入路径。")
						: TEXT("UAV/Storage 事件没有改变数值，可能只是命中历史中的干扰项。"));
				}
				else if (Candidate.Event.PassedFragments > 0 && Candidate.Event.ActionKind == TEXT("draw"))
				{
					AddHypothesis(Hypotheses, Semantics == TEXT("scene-write")
						? TEXT("场景 Draw 确认写入：可检查已证明的 Shader 输出、资源绑定以及可用的 UE Marker/Mesh 映射。")
						: TEXT("后处理 Draw 写入最终目标：优先看绑定输入，不能仅凭全屏 Draw 名称判断根因。"));
				}

				Report += TEXT("\nEvent Context（Shader、资源与 Pipeline）\n");
				if (const FEventContextEvidence* Context = EventContexts.Find(Candidate.Event.EventId))
				{
					Report += FString::Printf(TEXT("- Shader %s%s | debuggable: %s | source symbols: %s\n"),
						*Context->ShaderStage,
						Context->ShaderEntry.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" entry %s"), *Context->ShaderEntry),
						Context->bShaderDebuggable ? TEXT("yes") : TEXT("no"),
						Context->bSourceDebugInfo ? TEXT("yes") : TEXT("no"));
					Report += FString::Printf(TEXT("- Shader reflection：encoding=%s；inputSig=%d；outputSig=%d；constantBlocks=%d；samplers=%d；RO=%d；RW=%d\n"),
						Context->ShaderEncoding.IsEmpty() ? TEXT("unknown") : *Context->ShaderEncoding,
						Context->ShaderInputSignatureCount, Context->ShaderOutputSignatureCount,
						Context->ShaderConstantBlockCount, Context->ShaderSamplerCount,
						Context->ShaderReadOnlyResourceCount, Context->ShaderReadWriteResourceCount);
					int32 ShownInputs = 0;
					for (const FBoundResourceEvidence& Input : Context->Inputs)
					{
						if (ShownInputs++ >= MaxDisplayedFrontierResources)
						{
							break;
						}
						Report += Input.bTexture
							? FString::Printf(TEXT("- Bound input candidate: %s binding=%s [%s, %s] %s %dx%d samples=%d；像素贡献尚未由绑定本身证明\n"),
								*Input.Name, Input.ShaderBinding.IsEmpty() ? TEXT("unknown") : *Input.ShaderBinding,
								*Input.Stage, *Input.Access, *Input.Format, Input.Width, Input.Height, Input.Samples)
							: FString::Printf(TEXT("- Bound input candidate: %s [%s, %s] buffer/resource；像素贡献尚未证明\n"),
								*Input.Name, *Input.Stage, *Input.Access);
					}
					int32 ShownOutputs = 0;
					for (const FBoundResourceEvidence& Output : Context->Outputs)
					{
						if (ShownOutputs++ >= MaxDisplayedFrontierResources)
						{
							break;
						}
						Report += Output.bTexture
							? FString::Printf(TEXT("- Output: %s [%s, %s] %dx%d\n"),
								*Output.Name, *Output.Stage, *Output.Access, Output.Width, Output.Height)
							: FString::Printf(TEXT("- Output: %s [%s, %s] buffer/resource\n"),
								*Output.Name, *Output.Stage, *Output.Access);
					}
					for (const TSharedPtr<FJsonValue>& ProvenanceValue : Context->ResourceProvenance)
					{
						const TSharedPtr<FJsonObject> Provenance = ProvenanceValue.IsValid() ? ProvenanceValue->AsObject() : nullptr;
						if (!Provenance.IsValid())
						{
							continue;
						}
						FString ResourceName;
						FString ProducerAction;
						FString ProducerKind;
						FString ProducerStatus;
						FString ProducerUsage;
						FString CoordinateMapping;
						FString PixelTraceStatus;
						FString ChainBreak;
						bool bProducerFound = false;
						double ProducerEventId = 0.0;
						double InvalidatingEventId = 0.0;
						Provenance->TryGetStringField(TEXT("resource"), ResourceName);
						Provenance->TryGetStringField(TEXT("producerAction"), ProducerAction);
						Provenance->TryGetStringField(TEXT("producerKind"), ProducerKind);
						Provenance->TryGetStringField(TEXT("producerStatus"), ProducerStatus);
						Provenance->TryGetStringField(TEXT("producerUsage"), ProducerUsage);
						Provenance->TryGetStringField(TEXT("coordinateMapping"), CoordinateMapping);
						Provenance->TryGetStringField(TEXT("pixelTraceStatus"), PixelTraceStatus);
						Provenance->TryGetStringField(TEXT("chainBreak"), ChainBreak);
						Provenance->TryGetBoolField(TEXT("producerFound"), bProducerFound);
						Provenance->TryGetNumberField(TEXT("producerEventId"), ProducerEventId);
						Provenance->TryGetNumberField(TEXT("invalidatingEventId"), InvalidatingEventId);
						if (ProducerStatus == TEXT("invalidated"))
						{
							Report += FString::Printf(TEXT("- Resource chain break: %s 在 EID %u 被 Discard；Discard 不是 producer。%s\n"),
								*ResourceName, static_cast<uint32>(InvalidatingEventId), ChainBreak.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" %s"), *ChainBreak));
						}
						else if (bProducerFound)
						{
							Report += FString::Printf(TEXT("- Resource producer edge: %s ← EID %u [%s/%s] %s；%s；pixel trace=%s；该资源关系不自动证明像素贡献\n"), *ResourceName,
								static_cast<uint32>(ProducerEventId), *ProducerKind, *ProducerUsage, *ProducerAction, *CoordinateMapping,
								PixelTraceStatus.IsEmpty() ? TEXT("unknown") : *PixelTraceStatus);
						}
						else
						{
							Report += FString::Printf(TEXT("- Resource chain break: %s ← 未找到此前有效写入；%s；%s\n"),
								*ResourceName, *CoordinateMapping, *ChainBreak);
						}
					}

					if (!Context->ResourcePixelHistories.IsEmpty())
					{
						Report += TEXT("\nResource/sample Pixel History branches\n");
						for (const TSharedPtr<FJsonValue>& HistoryValue : Context->ResourcePixelHistories)
						{
							const TSharedPtr<FJsonObject> History = HistoryValue.IsValid() ? HistoryValue->AsObject() : nullptr;
							if (!History.IsValid())
							{
								continue;
							}
							FString ResourceName;
							FString ShaderBinding;
							FString BranchStatus;
							FString Mapping;
							double X = 0.0;
							double Y = 0.0;
							double SampleIndex = 0.0;
							double TotalEvents = 0.0;
							History->TryGetStringField(TEXT("resourceName"), ResourceName);
							History->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
							History->TryGetStringField(TEXT("branchStatus"), BranchStatus);
							History->TryGetStringField(TEXT("coordinateMapping"), Mapping);
							History->TryGetNumberField(TEXT("x"), X);
							History->TryGetNumberField(TEXT("y"), Y);
							History->TryGetNumberField(TEXT("sample"), SampleIndex);
							History->TryGetNumberField(TEXT("totalEvents"), TotalEvents);
							Report += FString::Printf(TEXT("- %s/%s pixel=(%d,%d) sample=%d mapping=%s events=%d status=%s\n"),
								*ResourceName, ShaderBinding.IsEmpty() ? TEXT("unknown-binding") : *ShaderBinding,
								static_cast<int32>(X), static_cast<int32>(Y), static_cast<int32>(SampleIndex),
								Mapping.IsEmpty() ? TEXT("unknown") : *Mapping, static_cast<int32>(TotalEvents),
								BranchStatus.IsEmpty() ? TEXT("unknown") : *BranchStatus);
						}
					}

					Report += TEXT("\nPipeline 固定功能状态\n");
					Report += EvidenceFormatting::FormatPipelineState(Context->PipelineState);
					Report += TEXT("\nShader 算法证据\n");
					if (Context->ShaderDebugTrace.IsValid())
					{
						Report += EvidenceFormatting::FormatShaderDebugTrace(Context->ShaderDebugTrace);
					}
					else if (Context->bShaderDebuggable && Context->bSourceDebugInfo)
					{
						Report += TEXT("- Shader has debug/source information; this report currently has reflection and bindings only, not an instruction-level trace.\n");
					}
					else
					{
						Report += TEXT("- Algorithm is not proven: entry name and bound input do not identify the math performed by Main. Shader source/debug trace is unavailable for this event.\n");
					}
					if (!Context->ShaderDebugStatus.IsEmpty())
					{
						Report += FString::Printf(TEXT("- Shader debug status: %s\n"), *Context->ShaderDebugStatus);
					}
				}
				else if (FailedEventContextIds.Contains(Candidate.Event.EventId))
				{
					Report += TEXT("- Event-context query failed; the chain remains broken here.\n");
				}
				else
				{
					Report += TEXT("- Querying this event's used inputs and shader-debug capability.\n");
				}
			}
			else
			{
				Summary = TEXT("Pixel History 没有提供可用的非 Present 事件；当前只能确认最终目标处的因果链中断。");
				CausalPath += TEXT("\n↓\n未找到可追踪 GPU 写入\n↓\n■ 因果链中断");
				AddHypothesis(Hypotheses,
					TEXT("当前目标可能只有背景/Clear、没有可见 Fragment，或该 API/资源的 Pixel History 不完整；不能据此断言具体剔除原因。"));
			}

			if (Hypotheses.IsEmpty())
			{
				AddHypothesis(Hypotheses,
					TEXT("当前证据只说明像素实际经历的 GPU 过程；没有设计期参考值时，不判断颜色“应该”是什么。"));
			}
			FString Suspects;
				Report += TEXT("\n证据支持的可能性\n");
			for (int32 Index = 0; Index < Hypotheses.Num(); ++Index)
			{
				Report += FString::Printf(TEXT("%d. %s\n"), Index + 1, *Hypotheses[Index]);
				Suspects += FString::Printf(TEXT("%d  %s%s"), Index + 1, *Hypotheses[Index],
					Index + 1 < Hypotheses.Num() ? TEXT("\n\n") : TEXT(""));
			}

			for (const FPixelSample* Sample : ReadySamples)
			{
				const int32 DisplayIndex = Samples.IndexOfByPredicate([Sample](const FPixelSample& Item)
				{
					return Item.Id == Sample->Id;
				});
				if (const TArray<FEventEvidence>* Events = AggregatedBySample.Find(Sample->Id))
				{
					Report += FString::Printf(TEXT("\nPass / event chain P%d (marker-derived; showing latest %d/%d events)\n"),
						DisplayIndex + 1, FMath::Min(MaxDisplayedTraceHops, Events->Num()), Events->Num());
					Report += TEXT("Pass boundary is inferred from RenderDoc action/marker data; a generic Slate ElementBatch marker is not a guaranteed RenderGraph pass name.\n");
					const int32 First = FMath::Max(0, Events->Num() - MaxDisplayedTraceHops);
					int32 Hop = 0;
					for (int32 EventIndex = Events->Num() - 1; EventIndex >= First; --EventIndex)
					{
						const FEventEvidence& Event = (*Events)[EventIndex];
						Report += FString::Printf(TEXT("%d. EID %u [%s] %s\n   marker/pass: %s\n   semantic: %s | flags=%u | result: %s\n   pixel: before=%s | shaderOutput=%s | after=%s\n   change: %s | deltaMax=%.9g | deltaL1=%.9g\n"),
							++Hop, Event.EventId, *Event.ActionKind, *Event.Action,
							*CompactMarkerPath(Event.MarkerPath), *ClassifySemantics(Event), Event.ActionFlags, *DescribeEventResult(Event),
							*Event.Before, *Event.ShaderOutput, *Event.After,
							*ClassifyColorDelta(Event), Event.ColorDeltaMax, Event.ColorDeltaL1);
						if (const FEventContextEvidence* Context = EventContexts.Find(Event.EventId))
						{
							Report += FString::Printf(TEXT("   context: shader=%s%s; inputs=%d; outputs=%d; fixedFunction=%s; debugTrace=%s\n"),
								*Context->ShaderStage,
								Context->ShaderEntry.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("/%s"), *Context->ShaderEntry),
								Context->Inputs.Num(), Context->Outputs.Num(),
								Context->PipelineState.IsValid() ? TEXT("available") : TEXT("unavailable"),
								Context->ShaderDebugTrace.IsValid() ? TEXT("available") : TEXT("not-run"));
						}
					}
				}
			}

			TArray<uint32> RecursiveContextIds;
			for (const TPair<uint32, FEventContextEvidence>& Pair : EventContexts)
			{
				if (EventContextDepths.FindRef(Pair.Key) > 0)
				{
					RecursiveContextIds.Add(Pair.Key);
				}
			}
			RecursiveContextIds.Sort([this](uint32 A, uint32 B)
			{
				const int32 DepthA = EventContextDepths.FindRef(A);
				const int32 DepthB = EventContextDepths.FindRef(B);
				return DepthA == DepthB ? A > B : DepthA < DepthB;
			});
			if (!RecursiveContextIds.IsEmpty())
			{
				Report += TEXT("\nRecursive resource-producer frontier (resource relation confirmed; P1 contribution/coordinate may remain unproven)\n");
				for (int32 Index = 0; Index < RecursiveContextIds.Num() && Index < MaxInitialEventContexts; ++Index)
				{
					const uint32 EventId = RecursiveContextIds[Index];
					const FEventContextEvidence& Context = EventContexts.FindChecked(EventId);
					Report += FString::Printf(TEXT("- depth=%d EID %u [%s] %s\n  marker/pass: %s\n  shader: %s%s；inputs=%d；outputs=%d；resource/sample branches=%d；pipeline=%s\n"),
						EventContextDepths.FindRef(EventId), EventId, *Context.ActionKind, *Context.Action,
						*CompactMarkerPath(Context.MarkerPath), *Context.ShaderStage,
						Context.ShaderEntry.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("/%s"), *Context.ShaderEntry),
						Context.Inputs.Num(), Context.Outputs.Num(), Context.ResourcePixelHistories.Num(),
						Context.PipelineState.IsValid() ? TEXT("available") : TEXT("unavailable"));
				}
			}

			Report += FString::Printf(TEXT("\nConfidence: %s. Applies only to the observed GPU chain, not design intent or engine-side cause.\n"),
				ReadySamples.Num() >= 2 ? TEXT("medium") : TEXT("low"));
			SetReportCards(Summary, CausalPath, Suspects, Report);
		}
		TSharedPtr<SEditableTextBox> CapturePathBox;
		TSharedPtr<STextBlock> StatusText;
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
		TMap<FString, TArray<FString>> ResourcePixelHistoryBindingAliases;
		TSet<uint32> DeferredResourceHistoryContextIds;
		TMap<uint32, int32> DeferredResourceHistoryBranchCounts;
		TSet<uint32> DeferredEventContextIds;
		TMap<FString, double> WorkerRequestQueuedSeconds;
		TMap<FString, FString> WorkerRequestCommands;
		TMap<uint32, FIntPoint> EventTracePixels;
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
		TSharedPtr<FRenderTrailAgentClient> AgentClient;
		TOptional<uint32> AgentPendingEventId;
		FString LastWorkerError;
		FRenderTrailAnalyzerDiagnostics Diagnostics;
		FRenderTrailReplayWorkerClient ReplayWorker;
		FString PreviewPath;
		FString ReplayTargetFormat;
		FString ActiveWorkerRequestId;
		FString ActiveWorkerStage;
		FIntPoint CurrentPreviewSize = FIntPoint::ZeroValue;
		int32 ReplayTargetResourceIndex = INDEX_NONE;
		int32 ReplayTargetSamples = 1;
		int32 ResourcePixelHistoryQueriesSubmitted = 0;
		uint64 PreviewSerial = 0;
		uint64 RequestSerial = 0;
		uint64 SampleSerial = 0;
		int32 AgentStep = 0;
		bool bWorkerReady = false;
		bool bAgentRunning = false;
		bool bAgentWaitingForDeterministicContexts = false;
		bool bShaderDebuggingAvailable = false;
		bool bSelectionConfirmed = false;
		bool bLastCandidateHasDivergence = false;
		bool bCaptureLoading = false;
		bool bPreviewReadyForSelection = false;
		bool bReplaySynchronizationPending = false;
		bool bReplayStartDeferred = false;
		bool bQueuePixelHistoryAfterWorkerReady = false;
		bool bBackgroundDeterministicQueriesReleased = false;
		double CaptureLoadStartSeconds = 0.0;
		double LastCaptureLoadStatusSeconds = 0.0;
		double LastWorkerHeartbeatSeconds = 0.0;
		double LastReplayQueryStatusSeconds = 0.0;
		double ActiveWorkerRequestStartSeconds = 0.0;
		uint64 WorkerQueueOrdinal = 0;
		FString CaptureLoadPhase;
		FString LastWorkerDiagnosticPhase;
		static constexpr int32 MaxPixelSamples = 1;
		static constexpr int32 MaxDisplayedTraceHops = 24;
		static constexpr int32 MaxDisplayedFrontierResources = 6;
		static constexpr int32 MaxAgentPrefilterEventsPerSample = 6;
		static constexpr int32 MaxAgentEventChainPerSample = 48;
		static constexpr int32 MaxAgentEventsPerResourceHistory = 4;
		static constexpr int32 MaxAgentTextureAccesses = 24;
		static constexpr int32 MaxAgentDeterministicContexts = 12;
		static constexpr int32 MaxAgentBoundResourcesPerContext = 8;
		static constexpr int32 MaxAgentResourceHistoriesPerContext = 8;
		static constexpr int32 MaxDeterministicContextEvents = 24;
		static constexpr int32 MaxResourcePixelHistoryQueries = 48;
		static constexpr int32 MaxSamplesPerResourceTrace = 8;
		static constexpr int32 MaxWriterContextsPerResourceTrace = 3;
		static constexpr int32 MaxAgentSteps = 1;
	};
}

namespace UE::RenderTrail::Private
{
	SRenderTrailAnalyzerHome::~SRenderTrailAnalyzerHome() = default;

	void SRenderTrailAnalyzerHome::Construct(const FArguments& Args)
	{
		SAssignNew(Implementation, SAnalyzerHome)
			.InitialCapture(Args._InitialCapture);
		ChildSlot
		[
			Implementation.ToSharedRef()
		];
	}

	void SRenderTrailAnalyzerHome::OpenCapture(const FString& CapturePath)
	{
		if (Implementation.IsValid())
		{
			Implementation->OpenCapture(CapturePath);
		}
	}
}
