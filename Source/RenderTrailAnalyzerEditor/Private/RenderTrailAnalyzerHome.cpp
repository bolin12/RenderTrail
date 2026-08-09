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
						SNew(SExpandableArea)
						.InitiallyCollapsed(false)
						.AllowAnimatedTransition(false)
						.BorderBackgroundColor(FLinearColor(0.085f, 0.095f, 0.115f, 1.0f))
						.BodyBorderBackgroundColor(FLinearColor(0.025f, 0.03f, 0.04f, 1.0f))
						.HeaderContent()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("实时 Replay 日志 · 自动读取 Worker 与 RenderDoc 日志尾部")))
								.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 10))
								.ColorAndOpacity(FLinearColor(0.72f, 0.80f, 0.90f))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(7, 0, 0, 0)
							[
								SNew(SButton)
								.Text_Lambda([this]()
								{
									return FText::FromString(bLiveReplayLogPaused ? TEXT("继续刷新") : TEXT("暂停刷新"));
								})
								.ToolTipText(FText::FromString(TEXT("暂停只影响界面更新；后台日志仍会继续收集。")))
								.OnClicked(this, &SAnalyzerHome::ToggleLiveReplayLog)
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(7, 0, 0, 0)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("释放 Replay")))
								.ToolTipText(FText::FromString(TEXT("保留预览、像素证据与报告，只关闭隔离 Worker 以回收 Replay 的 GPU/系统内存。需要新查询时可重新点击“分析当前像素”。")))
								.IsEnabled_Lambda([this]()
								{
									return bWorkerReady && !HasPendingWorkerRequests() && !bAgentRunning;
								})
								.OnClicked(this, &SAnalyzerHome::ReleaseReplayResources)
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(7, 0, 0, 0)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("打开日志目录")))
								.ToolTipText(FText::FromString(TEXT("打开包含 Analyzer、Worker 与 RenderDoc 完整日志的目录。")))
								.OnClicked(this, &SAnalyzerHome::OpenDiagnosticsDirectory)
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(7, 0, 0, 0)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("打开全量追踪")))
								.ToolTipText(FText::FromString(TEXT("打开当前像素的所有原始 Worker 请求、响应与完整追踪快照。")))
								.IsEnabled_Lambda([this]() { return !FullTraceDirectory.IsEmpty(); })
								.OnClicked(this, &SAnalyzerHome::OpenFullTraceDirectory)
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(7, 0, 0, 0)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("复制日志")))
								.ToolTipText(FText::FromString(TEXT("复制当前滚动日志和最新状态。")))
								.OnClicked(this, &SAnalyzerHome::CopyStatusToClipboard)
							]
						]
						.BodyContent()
						[
							SNew(SBox)
							.HeightOverride(190.0f)
							[
								SAssignNew(StatusText, SMultiLineEditableTextBox)
								.Text(FText::FromString(TEXT("[当前状态] 就绪。")))
								.AutoWrapText(false)
								.AllowMultiLine(true)
								.IsReadOnly(true)
								.AllowContextMenu(true)
								.ClearTextSelectionOnFocusLoss(false)
								.ToolTipText(FText::FromString(TEXT("持续显示 Worker 阶段、心跳、显存/内存和 RenderDoc 原生日志；可选择并按 Ctrl+C。")))
							]
						]
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
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[
								SAssignNew(SelectionText, STextBlock)
								.Text(FText::FromString(TEXT("尚未选择像素")))
								.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11))
								.AutoWrapText(true)
								.ColorAndOpacity(FLinearColor(0.80f, 0.86f, 0.93f))
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
							[
								SNew(STextBlock)
								.Text_Lambda([this]() { return FText::FromString(BuildHistoryCoverageText()); })
								.AutoWrapText(true)
								.ColorAndOpacity(FLinearColor(0.54f, 0.65f, 0.75f))
							]
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
							.ToolTipText(FText::FromString(TEXT("确认当前 P1，自动完成 Pixel History、Shader Debug、资源/sample 与 producer 上下文深追；不会自动运行 Agent。")))
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
			FinishAutomaticDeepTraceIfIdle();
			PollLiveReplayLogs(FPlatformTime::Seconds());
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
						SetAgentStatus(TEXT("自动深追 · ") + QueryStatus);
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
			CurrentStatus = Value;
			RefreshLiveReplayLogView();
		}

		FString BuildLiveReplayLogDisplay() const
		{
			FString Display = LiveReplayLog;
			if (!Display.IsEmpty() && !Display.EndsWith(TEXT("\n")))
			{
				Display += TEXT("\n");
			}
			Display += FString::Printf(TEXT("[当前状态] %s"), CurrentStatus.IsEmpty() ? TEXT("就绪。") : *CurrentStatus);
			return Display;
		}

		void RefreshLiveReplayLogView()
		{
			if (!StatusText.IsValid() || bLiveReplayLogPaused)
			{
				return;
			}
			StatusText->SetText(FText::FromString(BuildLiveReplayLogDisplay()));
			StatusText->ScrollTo(ETextLocation::EndOfDocument);
		}

		void AppendLiveReplayLog(const FString& Text)
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

		void ResetLiveReplayLog(const FString& Capture, int64 CaptureSize)
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

		void TailLiveReplayLogFile(const FString& Path, int32& InOutCharsRead)
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

		void PollLiveReplayLogs(double Now)
		{
			if (Now - LastLiveReplayLogPollSeconds < 0.75)
			{
				return;
			}
			TailLiveReplayLogFile(WorkerDiagnosticsTailPath, WorkerDiagnosticsCharsRead);
			TailLiveReplayLogFile(RenderDocDiagnosticsTailPath, RenderDocDiagnosticsCharsRead);
			LastLiveReplayLogPollSeconds = Now;
		}

		FReply CopyStatusToClipboard()
		{
			const FString Value = BuildLiveReplayLogDisplay();
			if (!Value.IsEmpty())
			{
				FPlatformApplicationMisc::ClipboardCopy(*Value);
			}
			return FReply::Handled();
		}

		FReply ToggleLiveReplayLog()
		{
			bLiveReplayLogPaused = !bLiveReplayLogPaused;
			if (!bLiveReplayLogPaused)
			{
				RefreshLiveReplayLogView();
			}
			return FReply::Handled();
		}

		FReply OpenDiagnosticsDirectory()
		{
			const FString Directory = WorkerDiagnosticsTailPath.IsEmpty()
				? FPaths::Combine(FPaths::ProjectLogDir(), TEXT("RenderTrailDiagnostics"))
				: FPaths::GetPath(WorkerDiagnosticsTailPath);
			IFileManager::Get().MakeDirectory(*Directory, true);
			FPlatformProcess::ExploreFolder(*Directory);
			return FReply::Handled();
		}

		FReply OpenFullTraceDirectory()
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

		FReply ReleaseReplayResources()
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
				|| !PendingResourcePixelHistoryByRequest.IsEmpty()
				|| !DispatchedWorkerRequestId.IsEmpty()
				|| !QueuedWorkerRequests.IsEmpty();
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

		int32 GetDeferredBackgroundResourceHistoryCount() const
		{
			int32 Count = 0;
			for (const TPair<uint32, int32>& Pair : DeferredResourceHistoryBranchCounts)
			{
				Count += Pair.Value;
			}
			return Count;
		}

		int32 GetBudgetDeferredResourceHistoryCount() const
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

		int32 GetDiscoveredResourceHistoryCount() const
		{
			return ScheduledResourcePixelHistoryKeys.Num() + GetBudgetDeferredResourceHistoryCount()
				+ GetDeferredBackgroundResourceHistoryCount();
		}

		FString BuildHistoryCoverageText() const
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

		void FinishAutomaticDeepTraceIfIdle()
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

		bool IsCriticalAgentEvent(uint32 EventId) const
		{
			return FocusedTraceEventIds.Contains(EventId)
				|| (LastCandidate.IsSet() && LastCandidate->Event.EventId == EventId)
				|| (LastSignificantCandidate.IsSet() && LastSignificantCandidate->Event.EventId == EventId);
		}

		static int32 ComputeResourceTracePriority(const FResourcePixelHistoryRequest& Request)
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

		bool IsShaderDebugPending(uint32 EventId) const
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

		bool QueueFocusedShaderDebug(uint32 EventId)
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

		FAgentContextCoverageSelection BuildAgentContextCoverageSelection() const
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

		static TArray<FString> GetAgentContextCoverageRoles(const FAgentContextCoverageEvidence& Coverage)
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

		void WriteWorkerHeartbeat(double Now, const TCHAR* Activity)
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

		void SetEvidence(const FString& Value)
		{
			SetReportCards(Value, TEXT("选择关注像素后生成。"), TEXT("尚无候选原因。"), Value);
		}

		void SetReportCards(const FString& Summary, const FString& CausalPath, const FString& Suspects,
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

		static FString SerializeJsonPretty(const TSharedRef<FJsonObject>& Object)
		{
			FString Result;
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Result);
			FJsonSerializer::Serialize(Object, Writer);
			return Result;
		}

		void InitializeFullTraceArtifact()
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

		void AppendFullTraceRecord(const FString& Direction, const TSharedRef<FJsonObject>& Payload)
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

		static TSharedRef<FJsonObject> BuildFullBoundResourceJson(const FBoundResourceEvidence& Resource)
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

		TSharedRef<FJsonObject> BuildFullEventContextJson(const FEventContextEvidence& Context) const
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

		void WriteFullTraceSnapshot(const FString& CompletionState)
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

		TArray<TSharedPtr<FJsonObject>> SelectAgentResourceHistoriesForCoverage(
			const FEventContextEvidence& Context) const
		{
			struct FHistoryCandidate
			{
				TSharedPtr<FJsonObject> History;
				int32 OriginalIndex = INDEX_NONE;
				int64 Score = 0;
				bool bRequired = false;
				bool bConfirmedWriter = false;
				bool bAssetOrSceneEvent = false;
				bool bNanite = false;
				bool bDepth = false;
				bool bSceneColorOrGBuffer = false;
				bool bBoundary = false;
				bool bConfirmedMapping = false;
			};

			TArray<FHistoryCandidate> Candidates;
			for (int32 HistoryIndex = 0; HistoryIndex < Context.ResourcePixelHistories.Num(); ++HistoryIndex)
			{
				const TSharedPtr<FJsonObject> History = Context.ResourcePixelHistories[HistoryIndex].IsValid()
					? Context.ResourcePixelHistories[HistoryIndex]->AsObject() : nullptr;
				if (!History.IsValid())
				{
					continue;
				}
				FHistoryCandidate Candidate;
				Candidate.History = History;
				Candidate.OriginalIndex = HistoryIndex;
				FString ResourceName;
				FString ShaderBinding;
				FString BranchStatus;
				FString MappingConfidence;
				History->TryGetStringField(TEXT("resourceName"), ResourceName);
				History->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
				History->TryGetStringField(TEXT("branchStatus"), BranchStatus);
				History->TryGetStringField(TEXT("mappingConfidence"), MappingConfidence);
				History->TryGetBoolField(TEXT("requiredForAgent"), Candidate.bRequired);
				const FString Searchable = ResourceName + TEXT(" ") + ShaderBinding;
				Candidate.bNanite = Searchable.Contains(TEXT("Nanite"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase);
				Candidate.bDepth = Searchable.Contains(TEXT("Depth"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("HZB"), ESearchCase::IgnoreCase);
				Candidate.bSceneColorOrGBuffer = Searchable.Contains(TEXT("SceneColor"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase);
				Candidate.bBoundary = !BranchStatus.IsEmpty()
					&& BranchStatus != TEXT("continued-to-dominating-writer")
					&& BranchStatus != TEXT("continued-to-pixel-writer")
					&& BranchStatus != TEXT("adaptive-footprint-continued")
					&& BranchStatus != TEXT("no-modification-before-consumer");
				Candidate.bConfirmedMapping = MappingConfidence == TEXT("confirmed-executed-values");

				double SelectedWriterEventId = 0.0;
				Candidate.bConfirmedWriter = History->TryGetNumberField(
					TEXT("selectedWriterEventId"), SelectedWriterEventId) && SelectedWriterEventId > 0.0;
				const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
				if (History->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
				{
					for (const TSharedPtr<FJsonValue>& EventValue : *EventSummaries)
					{
						const TSharedPtr<FJsonObject> Event = EventValue.IsValid() ? EventValue->AsObject() : nullptr;
						if (!Event.IsValid())
						{
							continue;
						}
						FString Marker;
						Event->TryGetStringField(TEXT("markerPath"), Marker);
						Candidate.bAssetOrSceneEvent |= Marker.Contains(TEXT("BasePass"), ESearchCase::IgnoreCase)
							|| Marker.Contains(TEXT("PrePass"), ESearchCase::IgnoreCase)
							|| Marker.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase)
							|| Marker.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase)
							|| Marker.Contains(TEXT("/Engine/"), ESearchCase::IgnoreCase)
							|| Marker.Contains(TEXT(" SM_"), ESearchCase::IgnoreCase);
					}
				}

				Candidate.Score += Candidate.bRequired ? 150000 : 0;
				Candidate.Score += Candidate.bConfirmedWriter ? 130000 : 0;
				Candidate.Score += Candidate.bAssetOrSceneEvent ? 110000 : 0;
				Candidate.Score += Candidate.bConfirmedMapping ? 100000 : 0;
				Candidate.Score += Candidate.bNanite ? 90000 : 0;
				Candidate.Score += Candidate.bDepth ? 80000 : 0;
				Candidate.Score += Candidate.bSceneColorOrGBuffer ? 70000 : 0;
				Candidate.Score += Candidate.bBoundary ? 60000 : 0;
				Candidate.Score -= HistoryIndex;
				Candidates.Add(MoveTemp(Candidate));
			}

			Candidates.Sort([](const FHistoryCandidate& A, const FHistoryCandidate& B)
			{
				return A.Score == B.Score ? A.OriginalIndex < B.OriginalIndex : A.Score > B.Score;
			});
			TArray<TSharedPtr<FJsonObject>> Selected;
			auto AddBest = [&Candidates, &Selected](TFunctionRef<bool(const FHistoryCandidate&)> Predicate)
			{
				for (const FHistoryCandidate& Candidate : Candidates)
				{
					if (Predicate(Candidate) && !Selected.Contains(Candidate.History))
					{
						Selected.Add(Candidate.History);
						return;
					}
				}
			};
			AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bRequired; });
			AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bConfirmedWriter; });
			AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bAssetOrSceneEvent; });
			AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bNanite; });
			AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bDepth; });
			AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bSceneColorOrGBuffer; });
			AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bBoundary; });
			AddBest([](const FHistoryCandidate& Candidate) { return Candidate.bConfirmedMapping; });
			for (const FHistoryCandidate& Candidate : Candidates)
			{
				if (Selected.Num() >= MaxAgentResourceHistoriesPerContext)
				{
					break;
				}
				Selected.AddUnique(Candidate.History);
			}
			if (Selected.Num() > MaxAgentResourceHistoriesPerContext)
			{
				Selected.SetNum(MaxAgentResourceHistoriesPerContext, EAllowShrinking::No);
			}
			return Selected;
		}

		static TArray<int32> SelectAgentBoundResourcesForCoverage(
			const TArray<FBoundResourceEvidence>& Resources, int32 MaxResources)
		{
			struct FResourceCandidate
			{
				int32 Index = INDEX_NONE;
				int64 Score = 0;
				bool bSceneColor = false;
				bool bDepth = false;
				bool bGBuffer = false;
				bool bNanite = false;
				bool bGpuSceneOrMaterial = false;
			};
			TArray<FResourceCandidate> Candidates;
			for (int32 Index = 0; Index < Resources.Num(); ++Index)
			{
				const FBoundResourceEvidence& Resource = Resources[Index];
				const FString Searchable = Resource.Name + TEXT(" ") + Resource.ShaderBinding;
				FResourceCandidate Candidate;
				Candidate.Index = Index;
				Candidate.bSceneColor = Searchable.Contains(TEXT("SceneColor"), ESearchCase::IgnoreCase);
				Candidate.bDepth = Searchable.Contains(TEXT("Depth"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("HZB"), ESearchCase::IgnoreCase);
				Candidate.bGBuffer = Searchable.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase);
				Candidate.bNanite = Searchable.Contains(TEXT("Nanite"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase);
				Candidate.bGpuSceneOrMaterial = Searchable.Contains(TEXT("GPUScene"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("PrimitiveData"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("Material"), ESearchCase::IgnoreCase);
				Candidate.Score += Candidate.bSceneColor ? 100000 : 0;
				Candidate.Score += Candidate.bDepth ? 95000 : 0;
				Candidate.Score += Candidate.bGBuffer ? 90000 : 0;
				Candidate.Score += Candidate.bNanite ? 85000 : 0;
				Candidate.Score += Candidate.bGpuSceneOrMaterial ? 80000 : 0;
				Candidate.Score += Resource.bTexture ? 10000 : 0;
				Candidate.Score -= Index;
				Candidates.Add(Candidate);
			}
			Candidates.Sort([](const FResourceCandidate& A, const FResourceCandidate& B)
			{
				return A.Score == B.Score ? A.Index < B.Index : A.Score > B.Score;
			});
			TArray<int32> Selected;
			auto AddBest = [&Candidates, &Selected](TFunctionRef<bool(const FResourceCandidate&)> Predicate)
			{
				for (const FResourceCandidate& Candidate : Candidates)
				{
					if (Predicate(Candidate) && !Selected.Contains(Candidate.Index))
					{
						Selected.Add(Candidate.Index);
						return;
					}
				}
			};
			AddBest([](const FResourceCandidate& Candidate) { return Candidate.bSceneColor; });
			AddBest([](const FResourceCandidate& Candidate) { return Candidate.bDepth; });
			AddBest([](const FResourceCandidate& Candidate) { return Candidate.bGBuffer; });
			AddBest([](const FResourceCandidate& Candidate) { return Candidate.bNanite; });
			AddBest([](const FResourceCandidate& Candidate) { return Candidate.bGpuSceneOrMaterial; });
			for (const FResourceCandidate& Candidate : Candidates)
			{
				if (Selected.Num() >= MaxResources)
				{
					break;
				}
				Selected.AddUnique(Candidate.Index);
			}
			if (Selected.Num() > MaxResources)
			{
				Selected.SetNum(MaxResources, EAllowShrinking::No);
			}
			return Selected;
		}

		TArray<TSharedPtr<FJsonObject>> SelectAgentResourceProvenanceForCoverage(
			const FEventContextEvidence& Context) const
		{
			struct FProvenanceCandidate
			{
				TSharedPtr<FJsonObject> Provenance;
				int32 OriginalIndex = INDEX_NONE;
				int64 Score = 0;
				bool bAsset = false;
				bool bVisBuffer = false;
				bool bGpuScene = false;
				bool bNanite = false;
				bool bSceneStage = false;
				bool bConfirmedProducer = false;
				bool bChainBreak = false;
				bool bConfirmedMapping = false;
			};
			TArray<FProvenanceCandidate> Candidates;
			for (int32 Index = 0; Index < Context.ResourceProvenance.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Provenance = Context.ResourceProvenance[Index].IsValid()
					? Context.ResourceProvenance[Index]->AsObject() : nullptr;
				if (!Provenance.IsValid())
				{
					continue;
				}
				FString ResourceName;
				FString ShaderBinding;
				FString ProducerMarker;
				FString ProducerStatus;
				FString ChainBreak;
				FString MappingConfidence;
				bool bProducerFound = false;
				Provenance->TryGetStringField(TEXT("resource"), ResourceName);
				Provenance->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
				Provenance->TryGetStringField(TEXT("producerMarkerPath"), ProducerMarker);
				Provenance->TryGetStringField(TEXT("producerStatus"), ProducerStatus);
				Provenance->TryGetStringField(TEXT("chainBreak"), ChainBreak);
				Provenance->TryGetStringField(TEXT("mappingConfidence"), MappingConfidence);
				Provenance->TryGetBoolField(TEXT("producerFound"), bProducerFound);
				const FString Searchable = ResourceName + TEXT(" ") + ShaderBinding + TEXT(" ") + ProducerMarker;
				FProvenanceCandidate Candidate;
				Candidate.Provenance = Provenance;
				Candidate.OriginalIndex = Index;
				Candidate.bAsset = Searchable.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT(" SM_"), ESearchCase::IgnoreCase);
				Candidate.bVisBuffer = Searchable.Contains(TEXT("VisBuffer"), ESearchCase::IgnoreCase);
				Candidate.bGpuScene = Searchable.Contains(TEXT("GPUScene"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("PrimitiveData"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("InstanceSceneData"), ESearchCase::IgnoreCase);
				Candidate.bNanite = Searchable.Contains(TEXT("Nanite"), ESearchCase::IgnoreCase);
				Candidate.bSceneStage = Searchable.Contains(TEXT("BasePass"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("PrePass"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("GBuffer"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("SceneDepth"), ESearchCase::IgnoreCase)
					|| Searchable.Contains(TEXT("SceneColor"), ESearchCase::IgnoreCase);
				Candidate.bConfirmedProducer = bProducerFound && ProducerStatus == TEXT("confirmed-resource-write");
				Candidate.bChainBreak = !ChainBreak.IsEmpty() || (!ProducerStatus.IsEmpty()
					&& ProducerStatus != TEXT("confirmed-resource-write"));
				Candidate.bConfirmedMapping = MappingConfidence == TEXT("confirmed-executed-values");
				Candidate.Score += Candidate.bAsset ? 120000 : 0;
				Candidate.Score += Candidate.bVisBuffer ? 115000 : 0;
				Candidate.Score += Candidate.bGpuScene ? 110000 : 0;
				Candidate.Score += Candidate.bNanite ? 100000 : 0;
				Candidate.Score += Candidate.bSceneStage ? 95000 : 0;
				Candidate.Score += Candidate.bConfirmedMapping ? 90000 : 0;
				Candidate.Score += Candidate.bConfirmedProducer ? 80000 : 0;
				Candidate.Score += Candidate.bChainBreak ? 50000 : 0;
				Candidate.Score -= Index;
				Candidates.Add(MoveTemp(Candidate));
			}
			Candidates.Sort([](const FProvenanceCandidate& A, const FProvenanceCandidate& B)
			{
				return A.Score == B.Score ? A.OriginalIndex < B.OriginalIndex : A.Score > B.Score;
			});
			TArray<TSharedPtr<FJsonObject>> Selected;
			auto AddBest = [&Candidates, &Selected](TFunctionRef<bool(const FProvenanceCandidate&)> Predicate)
			{
				for (const FProvenanceCandidate& Candidate : Candidates)
				{
					if (Predicate(Candidate) && !Selected.Contains(Candidate.Provenance))
					{
						Selected.Add(Candidate.Provenance);
						return;
					}
				}
			};
			AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bAsset; });
			AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bVisBuffer; });
			AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bGpuScene; });
			AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bNanite; });
			AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bSceneStage; });
			AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bConfirmedMapping; });
			AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bConfirmedProducer; });
			AddBest([](const FProvenanceCandidate& Candidate) { return Candidate.bChainBreak; });
			for (const FProvenanceCandidate& Candidate : Candidates)
			{
				if (Selected.Num() >= MaxAgentBoundResourcesPerContext)
				{
					break;
				}
				Selected.AddUnique(Candidate.Provenance);
			}
			if (Selected.Num() > MaxAgentBoundResourcesPerContext)
			{
				Selected.SetNum(MaxAgentBoundResourcesPerContext, EAllowShrinking::No);
			}
			return Selected;
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
				TEXT("coordinateEvidence"), TEXT("branchStatus"), TEXT("detail"), TEXT("error"), TEXT("shaderAccessDisassembly"),
				TEXT("tracePurpose"), TEXT("selectedWriterReason"), TEXT("resetBoundaryReason"),
				TEXT("adaptiveNextTraceKey") };
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
				TEXT("queueToResponseSeconds"), TEXT("selectedWriterEventId"), TEXT("dominatedWriterCount"),
				TEXT("resetBoundaryEventId"), TEXT("collapsedShaderAccessCount"), TEXT("adaptiveAttempt"),
				TEXT("adaptiveCandidateCount"), TEXT("adaptiveCandidatesRemaining"), TEXT("adaptiveNextX"), TEXT("adaptiveNextY") };
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
				TEXT("shaderCoordinateValuesMatched"), TEXT("shaderFootprintDerivedFromExecutedValues"),
				TEXT("executedShaderAccess") };
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
			const TSharedPtr<FJsonObject>* ShaderAccessResult = nullptr;
			if (History->TryGetObjectField(TEXT("shaderAccessResult"), ShaderAccessResult) && ShaderAccessResult)
			{
				Compact->SetObjectField(TEXT("shaderAccessResult"), *ShaderAccessResult);
			}

			const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
			TArray<TSharedPtr<FJsonValue>> CompactEvents;
			if (History->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
			{
				TArray<int32> SelectedIndices;
				if (EventSummaries->Num() <= MaxAgentEventsPerResourceHistory)
				{
					for (int32 Index = 0; Index < EventSummaries->Num(); ++Index)
					{
						SelectedIndices.Add(Index);
					}
				}
				else
				{
					struct FScoredEventIndex
					{
						int32 Index = INDEX_NONE;
						int32 Score = 0;
					};
					TArray<FScoredEventIndex> ScoredIndices;
					for (int32 Index = 0; Index < EventSummaries->Num(); ++Index)
					{
						const TSharedPtr<FJsonObject> Event = (*EventSummaries)[Index].IsValid()
							? (*EventSummaries)[Index]->AsObject() : nullptr;
						if (!Event.IsValid())
						{
							continue;
						}
						FString Marker;
						FString ActionKind;
						bool bChanged = false;
						double Passed = 0.0;
						Event->TryGetStringField(TEXT("markerPath"), Marker);
						Event->TryGetStringField(TEXT("actionKind"), ActionKind);
						Event->TryGetBoolField(TEXT("changedTextureValue"), bChanged);
						Event->TryGetNumberField(TEXT("passedFragments"), Passed);
						int32 Score = Index;
						Score += bChanged ? 80 : 0;
						Score += Passed > 0.0 ? 40 : 0;
						Score += (ActionKind == TEXT("draw") || ActionKind == TEXT("dispatch")) ? 30 : 0;
						Score += (Marker.Contains(TEXT("BasePass")) || Marker.Contains(TEXT("PrePass"))
							|| Marker.Contains(TEXT("DepthPass")) || Marker.Contains(TEXT("GBuffer"))) ? 220 : 0;
						Score += Marker.Contains(TEXT("Nanite")) ? 140 : 0;
						Score += (Marker.Contains(TEXT("SM_")) || Marker.Contains(TEXT("/Game/"))
							|| Marker.Contains(TEXT("/Engine/"))) ? 100 : 0;
						Score -= ActionKind == TEXT("clear") ? 160 : 0;
						FScoredEventIndex Scored;
						Scored.Index = Index;
						Scored.Score = Score;
						ScoredIndices.Add(MoveTemp(Scored));
					}
					ScoredIndices.Sort([](const FScoredEventIndex& A, const FScoredEventIndex& B)
					{
						return A.Score == B.Score ? A.Index > B.Index : A.Score > B.Score;
					});
					double SelectedWriterEventId = 0.0;
					History->TryGetNumberField(TEXT("selectedWriterEventId"), SelectedWriterEventId);
					for (int32 Index = 0; Index < EventSummaries->Num(); ++Index)
					{
						const TSharedPtr<FJsonObject> Event = (*EventSummaries)[Index].IsValid()
							? (*EventSummaries)[Index]->AsObject() : nullptr;
						double EventId = 0.0;
						if (Event.IsValid() && Event->TryGetNumberField(TEXT("eventId"), EventId)
							&& static_cast<uint32>(EventId) == static_cast<uint32>(SelectedWriterEventId))
						{
							SelectedIndices.AddUnique(Index);
							break;
						}
					}
					SelectedIndices.AddUnique(EventSummaries->Num() - 1);
					for (const FScoredEventIndex& Scored : ScoredIndices)
					{
						SelectedIndices.AddUnique(Scored.Index);
						if (SelectedIndices.Num() >= MaxAgentEventsPerResourceHistory)
						{
							break;
						}
					}
					SelectedIndices.Sort();
				}
				for (const int32 Index : SelectedIndices)
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
					if (Event->TryGetBoolField(TEXT("hasPrimitiveEvidence"), bValue)) CompactEvent->SetBoolField(TEXT("hasPrimitiveEvidence"), bValue);
					if (Event->TryGetNumberField(TEXT("lastPrimitiveId"), Number)) CompactEvent->SetNumberField(TEXT("lastPrimitiveId"), Number);
					const TArray<TSharedPtr<FJsonValue>>* FailureReasons = nullptr;
					if (Event->TryGetArrayField(TEXT("failureReasons"), FailureReasons) && FailureReasons)
					{
						CompactEvent->SetArrayField(TEXT("failureReasons"), *FailureReasons);
					}
					const TSharedPtr<FJsonObject>* PixelValue = nullptr;
					if (Event->TryGetObjectField(TEXT("firstBefore"), PixelValue) && PixelValue) CompactEvent->SetObjectField(TEXT("firstBefore"), *PixelValue);
					if (Event->TryGetObjectField(TEXT("lastShaderOutput"), PixelValue) && PixelValue) CompactEvent->SetObjectField(TEXT("lastShaderOutput"), *PixelValue);
					if (Event->TryGetObjectField(TEXT("lastAfter"), PixelValue) && PixelValue) CompactEvent->SetObjectField(TEXT("lastAfter"), *PixelValue);
					CompactEvents.Add(MakeShared<FJsonValueObject>(CompactEvent));
				}
			}
			Compact->SetStringField(TEXT("eventSummarySelection"),
				TEXT("causal-priority-selection; full ordered history remains in technical evidence"));
			Compact->SetArrayField(TEXT("selectedEventSummaries"), MoveTemp(CompactEvents));
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

			const TArray<FCausalLaneEvidence> CausalLanes =
				BuildCausalLaneEvidence(EventContexts, EventContextDepths);
			TArray<TSharedPtr<FJsonValue>> CausalLaneValues;
			for (const FCausalLaneEvidence& Lane : CausalLanes)
			{
				const TSharedRef<FJsonObject> LaneJson = MakeShared<FJsonObject>();
				LaneJson->SetStringField(TEXT("kind"), Lane.TracePurpose);
				LaneJson->SetStringField(TEXT("meaning"),
					Lane.TracePurpose == TEXT("geometry")
						? TEXT("geometry/visibility ownership; not an RGB chronology")
						: (Lane.TracePurpose == TEXT("overlay")
							? TEXT("editor overlay/composite resources; separate from scene color and geometry")
							: TEXT("color-resource producer DAG; structural edges do not alone prove final pixel contribution")));
				LaneJson->SetNumberField(TEXT("groupedBranchCount"), Lane.Branches.Num());
				LaneJson->SetNumberField(TEXT("evidenceRecordCount"), Lane.EvidenceRecordCount);
				LaneJson->SetNumberField(TEXT("queryRecordCount"), Lane.QueryRecordCount);
				LaneJson->SetNumberField(TEXT("confirmedProducerCount"), Lane.ConfirmedProducerCount);
				LaneJson->SetNumberField(TEXT("resetBoundaryCount"), Lane.ResetBoundaryCount);
				LaneJson->SetNumberField(TEXT("unresolvedBoundaryCount"), Lane.UnresolvedBoundaryCount);
				LaneJson->SetStringField(TEXT("selectionPolicy"),
					TEXT("all collected contexts; duplicate MSAA samples and adaptive coordinates grouped by consumer/resource/producer"));

				TArray<TSharedPtr<FJsonValue>> BranchValues;
				const int32 IncludedBranches = FMath::Min(Lane.Branches.Num(), MaxAgentCausalLaneBranches);
				for (int32 BranchIndex = 0; BranchIndex < IncludedBranches; ++BranchIndex)
				{
					const FCausalLaneBranchEvidence& Branch = Lane.Branches[BranchIndex];
					const TSharedRef<FJsonObject> BranchJson = MakeShared<FJsonObject>();
					BranchJson->SetNumberField(TEXT("consumerEventId"), Branch.ConsumerEventId);
					BranchJson->SetNumberField(TEXT("producerEventId"), Branch.ProducerEventId);
					BranchJson->SetNumberField(TEXT("resetBoundaryEventId"), Branch.ResetBoundaryEventId);
					BranchJson->SetNumberField(TEXT("resourceIndex"), Branch.ResourceIndex);
					BranchJson->SetNumberField(TEXT("reverseDepth"), Branch.ReverseDepth);
					BranchJson->SetStringField(TEXT("resource"), Branch.ResourceName);
					BranchJson->SetStringField(TEXT("shaderBinding"), Branch.ShaderBinding);
					BranchJson->SetStringField(TEXT("resourceAccess"), Branch.ResourceAccess);
					BranchJson->SetStringField(TEXT("branchStatus"), Branch.BranchStatus);
					BranchJson->SetStringField(TEXT("mappingConfidence"), Branch.MappingConfidence);
					BranchJson->SetStringField(TEXT("edgeRole"), Branch.EdgeRole);
					BranchJson->SetStringField(TEXT("edgeConfidence"), Branch.EdgeConfidence);
					BranchJson->SetStringField(TEXT("executedSampleValue"), Branch.ExecutedSampleValue);
					BranchJson->SetStringField(TEXT("producerBeforeValue"), Branch.ProducerBeforeValue);
					BranchJson->SetStringField(TEXT("producerShaderOutputValue"), Branch.ProducerShaderOutputValue);
					BranchJson->SetStringField(TEXT("producerWrittenValue"), Branch.ProducerWrittenValue);
					BranchJson->SetStringField(TEXT("producerWrittenValueSource"), TEXT("Pixel History lastAfter/postMod"));
					BranchJson->SetStringField(TEXT("producerActionKind"), Branch.ProducerActionKind);
					BranchJson->SetBoolField(TEXT("executedShaderAccess"), Branch.bExecutedShaderAccess);
					BranchJson->SetBoolField(TEXT("producerChangedValue"), Branch.bProducerChangedValue);
					BranchJson->SetBoolField(TEXT("producerValueMatchesExecutedSample"), Branch.bProducerValueMatchesExecutedSample);
					BranchJson->SetNumberField(TEXT("evidenceRecordCount"), Branch.EvidenceRecordCount);
					BranchJson->SetNumberField(TEXT("queryRecordCount"), Branch.QueryRecordCount);
					BranchJson->SetNumberField(TEXT("collapsedShaderAccessCount"), Branch.CollapsedShaderAccessCount);
					TArray<TSharedPtr<FJsonValue>> SampleIndices;
					for (const int32 Sample : Branch.Samples)
					{
						SampleIndices.Add(MakeShared<FJsonValueNumber>(Sample));
					}
					BranchJson->SetArrayField(TEXT("samples"), MoveTemp(SampleIndices));
					if (const FEventContextEvidence* Consumer = EventContexts.Find(Branch.ConsumerEventId))
					{
						BranchJson->SetStringField(TEXT("consumerAction"), Consumer->Action);
						BranchJson->SetStringField(TEXT("consumerMarker"), CompactMarkerPath(Consumer->MarkerPath));
					}
					if (const FEventContextEvidence* Producer = EventContexts.Find(Branch.ProducerEventId))
					{
						BranchJson->SetStringField(TEXT("producerAction"), Producer->Action);
						BranchJson->SetStringField(TEXT("producerMarker"), CompactMarkerPath(Producer->MarkerPath));
						BranchJson->SetStringField(TEXT("producerShaderEntry"), Producer->ShaderEntry);
					}
					BranchValues.Add(MakeShared<FJsonValueObject>(BranchJson));
				}
				LaneJson->SetNumberField(TEXT("includedBranchCount"), IncludedBranches);
				LaneJson->SetArrayField(TEXT("branches"), MoveTemp(BranchValues));
				CausalLaneValues.Add(MakeShared<FJsonValueObject>(LaneJson));
			}
			Root->SetNumberField(TEXT("causalLaneCount"), CausalLanes.Num());
			Root->SetArrayField(TEXT("causalLanes"), MoveTemp(CausalLaneValues));
			Root->SetStringField(TEXT("causalLanePolicy"),
				TEXT("color, geometry, and overlay are parallel evidence lanes; never concatenate them into one chronological process"));
			const uint32 PrimaryRootEventId = LastCandidate.IsSet() ? LastCandidate->Event.EventId : 0;
			const FPrimaryCausalPathEvidence PrimaryPath = BuildPrimaryColorPathEvidence(
				CausalLanes, EventContexts, PrimaryRootEventId);
			const TSharedRef<FJsonObject> PrimaryPathJson = MakeShared<FJsonObject>();
			PrimaryPathJson->SetNumberField(TEXT("rootEventId"), PrimaryPath.RootEventId);
			PrimaryPathJson->SetStringField(TEXT("stopReason"), PrimaryPath.StopReason);
			PrimaryPathJson->SetBoolField(TEXT("reachedConfirmedSceneSource"), PrimaryPath.bReachedConfirmedSceneSource);
			PrimaryPathJson->SetBoolField(TEXT("reachedExplicitBoundary"), PrimaryPath.bReachedExplicitBoundary);
			TArray<TSharedPtr<FJsonValue>> PrimaryPathEdges;
			for (const FCausalLaneBranchEvidence& Branch : PrimaryPath.Branches)
			{
				const TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
				Edge->SetNumberField(TEXT("consumerEventId"), Branch.ConsumerEventId);
				Edge->SetNumberField(TEXT("producerEventId"), Branch.ProducerEventId);
				Edge->SetNumberField(TEXT("resourceIndex"), Branch.ResourceIndex);
				Edge->SetStringField(TEXT("resource"), Branch.ResourceName);
				Edge->SetStringField(TEXT("lane"), Branch.TracePurpose);
				Edge->SetStringField(TEXT("edgeRole"), Branch.EdgeRole);
				Edge->SetStringField(TEXT("edgeConfidence"), Branch.EdgeConfidence);
				Edge->SetStringField(TEXT("executedSampleValue"), Branch.ExecutedSampleValue);
				Edge->SetStringField(TEXT("producerWrittenValue"), Branch.ProducerWrittenValue);
				Edge->SetStringField(TEXT("branchStatus"), Branch.BranchStatus);
				PrimaryPathEdges.Add(MakeShared<FJsonValueObject>(Edge));
			}
			PrimaryPathJson->SetArrayField(TEXT("edges"), MoveTemp(PrimaryPathEdges));
			Root->SetObjectField(TEXT("primaryColorPath"), PrimaryPathJson);

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

			const FAgentContextCoverageSelection ContextSelection = BuildAgentContextCoverageSelection();
			TArray<TSharedPtr<FJsonValue>> DeterministicContexts;
			const int32 ContextCount = ContextSelection.DetailedEventIds.Num();
			for (int32 ContextIndex = 0; ContextIndex < ContextCount; ++ContextIndex)
			{
				const uint32 ContextEventId = ContextSelection.DetailedEventIds[ContextIndex];
				const FEventContextEvidence& Context = EventContexts.FindChecked(ContextEventId);
				const int32 ReverseDepth = EventContextDepths.FindRef(Context.EventId);
				const FAgentContextCoverageEvidence& Coverage =
					ContextSelection.CoverageByEventId.FindChecked(ContextEventId);
				TSharedRef<FJsonObject> ContextJson = MakeShared<FJsonObject>();
				ContextJson->SetNumberField(TEXT("eventId"), Context.EventId);
				ContextJson->SetStringField(TEXT("action"), Context.Action);
				ContextJson->SetStringField(TEXT("actionKind"), Context.ActionKind);
				ContextJson->SetStringField(TEXT("marker"), CompactMarkerPath(Context.MarkerPath));
				ContextJson->SetNumberField(TEXT("reverseDepth"), ReverseDepth);
				if (Coverage.CausalDistance != MAX_int32)
				{
					ContextJson->SetNumberField(TEXT("causalDistanceFromCritical"), Coverage.CausalDistance);
				}
				ContextJson->SetNumberField(TEXT("coveragePriorityScore"),
					static_cast<double>(Coverage.PriorityScore));
				ContextJson->SetNumberField(TEXT("observedPassedFragments"), Coverage.PassedFragments);
				ContextJson->SetNumberField(TEXT("observedRejectedFragments"), Coverage.RejectedFragments);
				ContextJson->SetBoolField(TEXT("observedTextureValueChange"), Coverage.bChangedTextureValue);
				TArray<TSharedPtr<FJsonValue>> CoverageRoles;
				for (const FString& Role : GetAgentContextCoverageRoles(Coverage))
				{
					CoverageRoles.Add(MakeShared<FJsonValueString>(Role));
				}
				ContextJson->SetArrayField(TEXT("coverageRoles"), MoveTemp(CoverageRoles));
				TArray<TSharedPtr<FJsonValue>> SelectionReasons;
				if (const TArray<FString>* Reasons = ContextSelection.SelectionReasons.Find(ContextEventId))
				{
					for (const FString& Reason : *Reasons)
					{
						SelectionReasons.Add(MakeShared<FJsonValueString>(Reason));
					}
				}
				ContextJson->SetArrayField(TEXT("detailSelectionReasons"), MoveTemp(SelectionReasons));
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
				for (const int32 InputIndex : SelectAgentBoundResourcesForCoverage(
					Context.Inputs, MaxAgentBoundResourcesPerContext))
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
				ContextJson->SetStringField(TEXT("boundResourceSelectionPolicy"), TEXT("semantic-coverage"));
				TArray<TSharedPtr<FJsonValue>> Outputs;
				for (const int32 OutputIndex : SelectAgentBoundResourcesForCoverage(
					Context.Outputs, MaxAgentBoundResourcesPerContext))
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
				for (const TSharedPtr<FJsonObject>& Provenance :
					SelectAgentResourceProvenanceForCoverage(Context))
				{
					CompactProvenance.Add(MakeShared<FJsonValueObject>(
						BuildCompactResourceProvenanceForAgent(Provenance)));
				}
				ContextJson->SetArrayField(TEXT("resourceProvenance"), MoveTemp(CompactProvenance));
				ContextJson->SetNumberField(TEXT("resourceProvenanceCount"), Context.ResourceProvenance.Num());
				ContextJson->SetStringField(TEXT("resourceProvenanceSelectionPolicy"), TEXT("semantic-coverage"));

				TArray<TSharedPtr<FJsonValue>> CompactHistories;
				for (const TSharedPtr<FJsonObject>& History : SelectAgentResourceHistoriesForCoverage(Context))
				{
					CompactHistories.Add(MakeShared<FJsonValueObject>(
						BuildCompactResourceHistoryForAgent(History)));
				}
				ContextJson->SetArrayField(TEXT("resourcePixelHistories"), MoveTemp(CompactHistories));
				ContextJson->SetNumberField(TEXT("resourcePixelHistoryCount"), Context.ResourcePixelHistories.Num());
				ContextJson->SetStringField(TEXT("resourcePixelHistorySelectionPolicy"), TEXT("causal-coverage"));
				DeterministicContexts.Add(MakeShared<FJsonValueObject>(ContextJson));
			}
			Root->SetArrayField(TEXT("deterministicEventContexts"), MoveTemp(DeterministicContexts));

			TArray<uint32> ContextIndexIds;
			ContextSelection.CoverageByEventId.GenerateKeyArray(ContextIndexIds);
			ContextIndexIds.Sort([&ContextSelection](uint32 A, uint32 B)
			{
				const FAgentContextCoverageEvidence& CoverageA =
					ContextSelection.CoverageByEventId.FindChecked(A);
				const FAgentContextCoverageEvidence& CoverageB =
					ContextSelection.CoverageByEventId.FindChecked(B);
				const bool bReachableA = CoverageA.CausalDistance != MAX_int32;
				const bool bReachableB = CoverageB.CausalDistance != MAX_int32;
				if (bReachableA != bReachableB)
				{
					return bReachableA;
				}
				if (bReachableA && CoverageA.CausalDistance != CoverageB.CausalDistance)
				{
					return CoverageA.CausalDistance < CoverageB.CausalDistance;
				}
				return CoverageA.ReverseDepth == CoverageB.ReverseDepth
					? A > B : CoverageA.ReverseDepth < CoverageB.ReverseDepth;
			});

			TSharedRef<FJsonObject> ContextIndexSummary = MakeShared<FJsonObject>();
			int32 PixelWriterContextCount = 0;
			int32 AssetMarkerContextCount = 0;
			int32 SceneRasterContextCount = 0;
			int32 NaniteContextCount = 0;
			int32 DepthContextCount = 0;
			int32 BoundaryContextCount = 0;
			TArray<TSharedPtr<FJsonValue>> ContextIndex;
			ContextIndex.Reserve(ContextIndexIds.Num());
			for (const uint32 ContextEventId : ContextIndexIds)
			{
				const FEventContextEvidence& Context = EventContexts.FindChecked(ContextEventId);
				const FAgentContextCoverageEvidence& Coverage =
					ContextSelection.CoverageByEventId.FindChecked(ContextEventId);
				PixelWriterContextCount += Coverage.bReferencedPixelWriter ? 1 : 0;
				AssetMarkerContextCount += Coverage.bAssetMarker ? 1 : 0;
				SceneRasterContextCount += Coverage.bSceneRaster ? 1 : 0;
				NaniteContextCount += Coverage.bNanite ? 1 : 0;
				DepthContextCount += Coverage.bDepthStage ? 1 : 0;
				BoundaryContextCount += Coverage.bBranchBoundary || Coverage.bUnresolvedProducer ? 1 : 0;

				TSharedRef<FJsonObject> IndexEntry = MakeShared<FJsonObject>();
				IndexEntry->SetNumberField(TEXT("eventId"), ContextEventId);
				IndexEntry->SetNumberField(TEXT("reverseDepth"), Coverage.ReverseDepth);
				if (Coverage.CausalDistance != MAX_int32)
				{
					IndexEntry->SetNumberField(TEXT("causalDistanceFromCritical"), Coverage.CausalDistance);
				}
				IndexEntry->SetBoolField(TEXT("selectedForDetail"),
					ContextSelection.DetailedEventIds.Contains(ContextEventId));
				IndexEntry->SetBoolField(TEXT("referencedAsPixelWriter"), Coverage.bReferencedPixelWriter);
				IndexEntry->SetNumberField(TEXT("passedFragments"), Coverage.PassedFragments);
				IndexEntry->SetNumberField(TEXT("rejectedFragments"), Coverage.RejectedFragments);
				IndexEntry->SetBoolField(TEXT("changedTextureValue"), Coverage.bChangedTextureValue);
				IndexEntry->SetStringField(TEXT("shaderEntry"), Context.ShaderEntry);
				TArray<FString> Roles = GetAgentContextCoverageRoles(Coverage);
				TArray<TSharedPtr<FJsonValue>> RoleValues;
				for (const FString& Role : Roles)
				{
					RoleValues.Add(MakeShared<FJsonValueString>(Role));
				}
				IndexEntry->SetArrayField(TEXT("coverageRoles"), MoveTemp(RoleValues));
				const bool bMeaningfulMarker = ContextSelection.DetailedEventIds.Contains(ContextEventId)
					|| Coverage.bAssetMarker || Coverage.bSceneRaster || Coverage.bNanite
					|| Coverage.bDepthStage || Coverage.bBranchBoundary || Coverage.bUnresolvedProducer;
				if (bMeaningfulMarker)
				{
					IndexEntry->SetStringField(TEXT("markerHint"),
						CompactMarkerPath(Context.MarkerPath).Left(MaxAgentContextIndexMarkerChars));
				}

				auto SetBoundedEventIds = [&IndexEntry](const TCHAR* Field, const TCHAR* CountField,
					const TArray<uint32>& EventIds)
				{
					TArray<TSharedPtr<FJsonValue>> Values;
					for (int32 Index = 0; Index < EventIds.Num() && Index < MaxAgentContextIndexLinks; ++Index)
					{
						Values.Add(MakeShared<FJsonValueNumber>(EventIds[Index]));
					}
					IndexEntry->SetArrayField(Field, MoveTemp(Values));
					IndexEntry->SetNumberField(CountField, EventIds.Num());
				};
				SetBoundedEventIds(TEXT("producerEventIds"), TEXT("producerEventCount"),
					Coverage.ProducerEventIds);
				SetBoundedEventIds(TEXT("downstreamConsumerEventIds"), TEXT("downstreamConsumerEventCount"),
					Coverage.DownstreamConsumerEventIds);
				ContextIndex.Add(MakeShared<FJsonValueObject>(IndexEntry));
			}
			ContextIndexSummary->SetNumberField(TEXT("total"), ContextIndex.Num());
			ContextIndexSummary->SetNumberField(TEXT("selectedForDetail"), ContextCount);
			ContextIndexSummary->SetNumberField(TEXT("referencedPixelWriters"), PixelWriterContextCount);
			ContextIndexSummary->SetNumberField(TEXT("assetMarkers"), AssetMarkerContextCount);
			ContextIndexSummary->SetNumberField(TEXT("sceneRaster"), SceneRasterContextCount);
			ContextIndexSummary->SetNumberField(TEXT("nanite"), NaniteContextCount);
			ContextIndexSummary->SetNumberField(TEXT("depth"), DepthContextCount);
			ContextIndexSummary->SetNumberField(TEXT("chainBoundaries"), BoundaryContextCount);
			Root->SetStringField(TEXT("deterministicEventContextSelectionPolicy"),
				TEXT("causal-coverage: critical + confirmed writers + asset markers + scene raster + Nanite + depth + boundaries + depth diversity"));
			Root->SetObjectField(TEXT("deterministicEventContextIndexSummary"), ContextIndexSummary);
			Root->SetArrayField(TEXT("deterministicEventContextIndex"), MoveTemp(ContextIndex));
			Root->SetNumberField(TEXT("deterministicEventContextCount"), EventContexts.Num());
			Root->SetNumberField(TEXT("deterministicEventContextsIncluded"), ContextCount);
			Root->SetBoolField(TEXT("agentEvidenceIsFullTrace"), false);
			Root->SetStringField(TEXT("fullTraceSnapshotPath"), FullTraceSnapshotPath);
			Root->SetStringField(TEXT("fullTraceRecordsJsonlPath"), FullTraceJsonlPath);
			Root->SetNumberField(TEXT("fullTraceRecordCount"), FullTraceRecordCount);
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
					&& PendingShaderDebugByRequest.IsEmpty()
					&& BudgetDeferredResourcePixelHistoryRequests.IsEmpty()
					&& BudgetDeferredEventContextDepths.IsEmpty());
			Root->SetNumberField(TEXT("deterministicContextFailureCount"), FailedEventContextIds.Num());
			Root->SetNumberField(TEXT("shaderDebugFailureCount"), FailedShaderDebugIds.Num());
			Root->SetNumberField(TEXT("resourcePixelHistoryFailureCount"), FailedResourcePixelHistoryKeys.Num());
			Root->SetNumberField(TEXT("resourcePixelHistoryQueryCount"), ResourcePixelHistoryQueriesSubmitted);
			Root->SetNumberField(TEXT("resourcePixelHistoryBranchCount"), GetDiscoveredResourceHistoryCount());
			Root->SetNumberField(TEXT("resourcePixelHistoryBudgetDeferredCount"),
				GetBudgetDeferredResourceHistoryCount());
			Root->SetNumberField(TEXT("resourcePixelHistoryBudgetDeferredCandidateCount"),
				BudgetDeferredResourcePixelHistoryRequests.Num());
			Root->SetNumberField(TEXT("resourcePixelHistoryQueryLimit"), ResourcePixelHistoryQueryLimit);
			Root->SetNumberField(TEXT("deterministicContextBudgetDeferredCount"),
				BudgetDeferredEventContextDepths.Num());
			Root->SetNumberField(TEXT("deterministicContextLimit"), DeterministicContextLimit);
			Root->SetBoolField(TEXT("automaticDeepTraceEnabled"), true);
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

		void ResumeAgentAfterEventContext(uint32 EventId)
		{
			if (!bAgentRunning || !AgentPendingEventId.IsSet() || AgentPendingEventId.GetValue() != EventId)
				return;
			AgentPendingEventId.Reset();
			AddAgentMessage(TEXT("user"), TEXT("TOOL_RESULT\n") + BuildAgentEventObservation(EventId));
			SendAgentTurn();
		}

		TArray<FRenderTrailResultLane> BuildDeterministicResultLanes() const
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

		void MarkReplaySynchronizationPending()
		{
			bReplaySynchronizationPending = true;
			SetStatus(TEXT("已选点；正在等待同步：ReplayController → 目标 RT → Pixel History。"));
			SetAgentStatus(TEXT("分析已排队，等待 Replay 完整同步；同步完成后才会读取事件上下文和 Shader Debug。"));
		}

		void CancelQueuedWorkerRequestsForNewAnalysisGeneration(const TCHAR* Reason)
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

		void ResetSamples()
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

		void PollWorkerPipes()
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

		bool TryDispatchNextWorkerRequest()
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

		bool SendWorkerRequest(const FString& Command, const FString& RequestId,
			TFunctionRef<void(const TSharedRef<FJsonObject>&)> Populate, int32 Priority = 0)
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

		void QueryPixel(int32 X, int32 Y)
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

		void RemoveResourceTraceBoundary(FEventContextEvidence& Context, const FString& TraceKey,
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

		void ScheduleResourcePixelHistory(FEventContextEvidence& Context,
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

		int32 ScheduleExecutedTextureAccessHistories(FEventContextEvidence& Context)
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

		void EnrichResourcePixelHistoryFromShaderTrace(const FEventContextEvidence& Context,
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

		void StoreResourcePixelHistory(const TSharedRef<FJsonObject>& Message)
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

		void ScheduleProducerEventContexts(const FEventContextEvidence& Context)
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

		void EnsureRelevantEventContexts()
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

		void EnsureCandidateShaderDebug()
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

		void TryResumeAgentAfterDeterministicContexts()
		{
			if (bAgentWaitingForDeterministicContexts && !HasPendingCriticalDeterministicQueries())
			{
				bAgentWaitingForDeterministicContexts = false;
				StartAgentAnalysis();
			}
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
					if (!Context->TraceStopReason.IsEmpty())
					{
						Report += FString::Printf(TEXT("- Focused trace stop: %s (scene source Pipeline/Shader evidence retained)\n"),
							*Context->TraceStopReason);
					}
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
							FString MappingConfidence;
							FString TracePurpose;
							FString CoordinateEvidence;
							double X = 0.0;
							double Y = 0.0;
							double SampleIndex = 0.0;
							double TotalEvents = 0.0;
							double SelectedWriterEventId = 0.0;
							double DominatedWriterCount = 0.0;
							History->TryGetStringField(TEXT("resourceName"), ResourceName);
							History->TryGetStringField(TEXT("shaderBinding"), ShaderBinding);
							History->TryGetStringField(TEXT("branchStatus"), BranchStatus);
							History->TryGetStringField(TEXT("coordinateMapping"), Mapping);
							History->TryGetStringField(TEXT("mappingConfidence"), MappingConfidence);
							History->TryGetStringField(TEXT("tracePurpose"), TracePurpose);
							History->TryGetStringField(TEXT("coordinateEvidence"), CoordinateEvidence);
							History->TryGetNumberField(TEXT("x"), X);
							History->TryGetNumberField(TEXT("y"), Y);
							History->TryGetNumberField(TEXT("sample"), SampleIndex);
							History->TryGetNumberField(TEXT("totalEvents"), TotalEvents);
							History->TryGetNumberField(TEXT("selectedWriterEventId"), SelectedWriterEventId);
							History->TryGetNumberField(TEXT("dominatedWriterCount"), DominatedWriterCount);
							Report += FString::Printf(TEXT("- [%s] %s/%s pixel=(%d,%d) sample=%d mapping=%s confidence=%s events=%d selectedWriter=EID %u visibleAlternatives=%d status=%s\n"),
								TracePurpose.IsEmpty() ? TEXT("unknown") : *TracePurpose,
								*ResourceName, ShaderBinding.IsEmpty() ? TEXT("unknown-binding") : *ShaderBinding,
								static_cast<int32>(X), static_cast<int32>(Y), static_cast<int32>(SampleIndex),
								Mapping.IsEmpty() ? TEXT("unknown") : *Mapping,
								MappingConfidence.IsEmpty() ? TEXT("unknown") : *MappingConfidence,
								static_cast<int32>(TotalEvents), static_cast<uint32>(SelectedWriterEventId),
								static_cast<int32>(DominatedWriterCount),
								BranchStatus.IsEmpty() ? TEXT("unknown") : *BranchStatus);
							if (!CoordinateEvidence.IsEmpty())
							{
								Report += FString::Printf(TEXT("  coordinate evidence: %s\n"), *CoordinateEvidence);
							}

							const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
							if (History->TryGetArrayField(TEXT("eventSummaries"), EventSummaries) && EventSummaries)
							{
								Report += TEXT("  Returned Pixel History events (complete; passed/changed draws first)\n");
								auto AppendHistoryEvents = [&Report, EventSummaries](bool bPassedOrChangedGroup)
								{
									for (const TSharedPtr<FJsonValue>& EventValue : *EventSummaries)
									{
										const TSharedPtr<FJsonObject> Event = EventValue.IsValid() ? EventValue->AsObject() : nullptr;
										if (!Event.IsValid())
										{
											continue;
										}
										double EventId = 0.0;
										double PassedFragments = 0.0;
										double RejectedFragments = 0.0;
										double PrimitiveId = 0.0;
										bool bChanged = false;
										FString Action;
										FString ActionKind;
										FString MarkerPath;
										Event->TryGetNumberField(TEXT("eventId"), EventId);
										Event->TryGetNumberField(TEXT("passedFragments"), PassedFragments);
										Event->TryGetNumberField(TEXT("rejectedFragments"), RejectedFragments);
										Event->TryGetNumberField(TEXT("lastPrimitiveId"), PrimitiveId);
										Event->TryGetBoolField(TEXT("changedTextureValue"), bChanged);
										Event->TryGetStringField(TEXT("action"), Action);
										Event->TryGetStringField(TEXT("actionKind"), ActionKind);
										Event->TryGetStringField(TEXT("markerPath"), MarkerPath);
										const bool bPassedOrChanged = PassedFragments > 0.0 || bChanged;
										if (bPassedOrChanged != bPassedOrChangedGroup)
										{
											continue;
										}
										TArray<FString> FailureReasons;
										const TArray<TSharedPtr<FJsonValue>>* Failures = nullptr;
										if (Event->TryGetArrayField(TEXT("failureReasons"), Failures) && Failures)
										{
											for (const TSharedPtr<FJsonValue>& Failure : *Failures)
											{
												if (Failure.IsValid() && Failure->Type == EJson::String)
												{
													FailureReasons.Add(Failure->AsString());
												}
											}
										}
										Report += FString::Printf(
											TEXT("    %s EID %u [%s] passed=%d rejected=%d changed=%s primitive=%u action=%s\n      marker=%s%s\n"),
											bPassedOrChanged ? TEXT("[PASS/WRITE]") : TEXT("[REJECTED]"),
											static_cast<uint32>(EventId), ActionKind.IsEmpty() ? TEXT("unknown") : *ActionKind,
											static_cast<int32>(PassedFragments), static_cast<int32>(RejectedFragments),
											bChanged ? TEXT("true") : TEXT("false"), static_cast<uint32>(PrimitiveId),
											Action.IsEmpty() ? TEXT("<unnamed>") : *Action, *CompactMarkerPath(MarkerPath),
											FailureReasons.IsEmpty()
												? TEXT("")
												: *FString::Printf(TEXT("\n      failures=%s"), *FString::Join(FailureReasons, TEXT(","))));
									}
								};
								AppendHistoryEvents(true);
								AppendHistoryEvents(false);
							}
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

			const FAgentContextCoverageSelection ContextSelection = BuildAgentContextCoverageSelection();
			if (!ContextSelection.DetailedEventIds.IsEmpty())
			{
				Report += FString::Printf(TEXT("\nAgent causal-coverage detail selection (%d/%d contexts; full index is included in Agent evidence)\n"),
					ContextSelection.DetailedEventIds.Num(), EventContexts.Num());
				for (const uint32 EventId : ContextSelection.DetailedEventIds)
				{
					const FEventContextEvidence& Context = EventContexts.FindChecked(EventId);
					const FAgentContextCoverageEvidence& Coverage =
						ContextSelection.CoverageByEventId.FindChecked(EventId);
					const FString Roles = FString::Join(GetAgentContextCoverageRoles(Coverage), TEXT(","));
					const TArray<FString>* Reasons = ContextSelection.SelectionReasons.Find(EventId);
					Report += FString::Printf(TEXT("- depth=%d causalDistance=%s EID %u [%s] %s\n  roles=%s；selectedBecause=%s\n  marker/pass: %s\n  pixelEvidence: passed=%d rejected=%d changed=%s\n  shader: %s%s；inputs=%d；outputs=%d；resource/sample branches=%d；pipeline=%s\n"),
						Coverage.ReverseDepth,
						Coverage.CausalDistance == MAX_int32 ? TEXT("unlinked")
							: *FString::FromInt(Coverage.CausalDistance),
						EventId, *Context.ActionKind, *Context.Action, *Roles,
						Reasons ? *FString::Join(*Reasons, TEXT(",")) : TEXT("coverage-score-fill"),
						*CompactMarkerPath(Context.MarkerPath),
						Coverage.PassedFragments, Coverage.RejectedFragments,
						Coverage.bChangedTextureValue ? TEXT("true") : TEXT("false"),
						*Context.ShaderStage,
						Context.ShaderEntry.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("/%s"), *Context.ShaderEntry),
						Context.Inputs.Num(), Context.Outputs.Num(), Context.ResourcePixelHistories.Num(),
						Context.PipelineState.IsValid() ? TEXT("available") : TEXT("unavailable"));
				}
			}

			const FString HistoryCoverage = BuildHistoryCoverageText();
			Report += TEXT("\n") + HistoryCoverage + TEXT("\n");
			if (!FullTraceSnapshotPath.IsEmpty())
			{
				Report += FString::Printf(
					TEXT("\n全量追踪（未经过 Agent 裁剪）\n- 实时原始记录：%s\n- 完整快照：%s\n- 当前记录：%d（请求 %d / 响应 %d）\n- 注意：Agent 只读取最多 %d/%d 个上下文；它的回答不是全量追踪。\n"),
					*FullTraceJsonlPath, *FullTraceSnapshotPath, FullTraceRecordCount,
					FullTraceRequestCount, FullTraceResponseCount,
					ContextSelection.DetailedEventIds.Num(), EventContexts.Num());
			}
			if (!BudgetDeferredResourcePixelHistoryRequests.IsEmpty()
				|| !BudgetDeferredEventContextDepths.IsEmpty())
			{
				Summary += FString::Printf(TEXT(" 自动深追已达到安全上限：仍有 %d 个资源组（%d 个候选坐标）和 %d 个 producer 上下文被预算截断；这不表示没有更早原因。"),
					GetBudgetDeferredResourceHistoryCount(), BudgetDeferredResourcePixelHistoryRequests.Num(),
					BudgetDeferredEventContextDepths.Num());
			}

			Report += FString::Printf(TEXT("\nConfidence: %s. Applies only to the observed GPU chain, not design intent or engine-side cause.\n"),
				ReadySamples.Num() >= 2 ? TEXT("medium") : TEXT("low"));
			SetReportCards(Summary, CausalPath, Suspects, Report);
		}
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
