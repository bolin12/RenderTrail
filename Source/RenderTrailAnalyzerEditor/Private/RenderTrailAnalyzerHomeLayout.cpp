#include "RenderTrailAnalyzerHomeInternal.h"

namespace UE::RenderTrail::Private
{
	SAnalyzerHome::~SAnalyzerHome()
	{
		CancelAgentRun();
		StopWorker();
		ReleasePreview();
	}

	void SAnalyzerHome::Construct(const FArguments& Args)
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

	TSharedRef<SWidget> SAnalyzerHome::BuildRootLayout(const FString& InitialCapture)
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

	TSharedRef<SWidget> SAnalyzerHome::BuildCaptureToolbar(const FString& InitialCapture)
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

	TSharedRef<SWidget> SAnalyzerHome::BuildImagePanel()
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

	TSharedRef<SWidget> SAnalyzerHome::BuildInspectorPanel()
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

	TSharedRef<SWidget> SAnalyzerHome::BuildAgentComposer()
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

	FReply SAnalyzerHome::ShowOverviewPage()
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

	FReply SAnalyzerHome::ShowEvidencePage()
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

	void SAnalyzerHome::OpenCapture(const FString& CapturePath)
	{
		if (!CapturePathBox.IsValid())
		{
			return;
		}
		CapturePathBox->SetText(FText::FromString(FPaths::ConvertRelativePathToFull(CapturePath)));
		StartWorker();
	}

	void SAnalyzerHome::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
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
}
