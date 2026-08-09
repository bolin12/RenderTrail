#include "RenderTrailAnalyzerResultView.h"

#include "Brushes/SlateColorBrush.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

namespace UE::RenderTrail::Private
{
	namespace
	{
		FLinearColor StateColor(ERenderTrailResultNodeState State)
		{
			switch (State)
			{
			case ERenderTrailResultNodeState::Confirmed: return FLinearColor(0.18f, 0.58f, 0.36f, 1.0f);
			case ERenderTrailResultNodeState::Candidate: return FLinearColor(0.76f, 0.48f, 0.12f, 1.0f);
			case ERenderTrailResultNodeState::Blocked: return FLinearColor(0.46f, 0.50f, 0.56f, 1.0f);
			default: return FLinearColor(0.22f, 0.48f, 0.72f, 1.0f);
			}
		}

		FString StateLabel(ERenderTrailResultNodeState State)
		{
			switch (State)
			{
			case ERenderTrailResultNodeState::Confirmed: return TEXT("✓ 已证明");
			case ERenderTrailResultNodeState::Candidate: return TEXT("◇ 候选");
			case ERenderTrailResultNodeState::Blocked: return TEXT("■ 断点");
			default: return TEXT("· 信息");
			}
		}

		ERenderTrailResultNodeState InferNodeState(const FString& Text)
		{
			if (Text.Contains(TEXT("中断")) || Text.Contains(TEXT("断点")) || Text.Contains(TEXT("不可用"))
				|| Text.Contains(TEXT("暂停")) || Text.Contains(TEXT("未确认")))
			{
				return ERenderTrailResultNodeState::Blocked;
			}
			if (Text.Contains(TEXT("候选")) || Text.Contains(TEXT("继续反向追踪")))
			{
				return ERenderTrailResultNodeState::Candidate;
			}
			if (Text.Contains(TEXT("写入")) || Text.Contains(TEXT("P1")))
			{
				return ERenderTrailResultNodeState::Confirmed;
			}
			return ERenderTrailResultNodeState::Information;
		}
	}

	void SRenderTrailAnalyzerResultView::Construct(const FArguments& Args)
	{
		ChildSlot
		[
			SAssignNew(ContentBox, SVerticalBox)
		];
		ShowMessage(TEXT("选择一个像素并读取 Pixel History 后，这里会显示形成链条。"));
	}

	void SRenderTrailAnalyzerResultView::ShowMessage(const FString& Message)
	{
		if (!ContentBox.IsValid())
		{
			return;
		}
		ContentBox->ClearChildren();
		ContentBox->AddSlot().AutoHeight()
		[
			MakeSummaryCard(TEXT("当前状态"), Message, TEXT("每次只分析一个像素；没有证据时不会补猜测。"))
		];
	}

	void SRenderTrailAnalyzerResultView::SetDeterministicReport(
		const FString& Summary, const FString& CausalPath, const FString& Suspects)
	{
		if (!ContentBox.IsValid())
		{
			return;
		}
		ContentBox->ClearChildren();
		ContentBox->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
		[
			MakeSummaryCard(TEXT("规则分析"), Summary, TEXT("结论仅来自当前 P1 的确定性 RenderDoc 证据。"))
		];

		TArray<FString> Lines;
		CausalPath.ParseIntoArrayLines(Lines, true);
		TArray<FRenderTrailResultNode> Nodes;
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line == TEXT("↓") || Line.StartsWith(TEXT("↓ ")))
			{
				continue;
			}
			FRenderTrailResultNode Node;
			Node.State = InferNodeState(Line);
			Node.Title = Line;
			Nodes.Add(MoveTemp(Node));
			if (Nodes.Num() >= 8)
			{
				break;
			}
		}
		AddChain(Nodes);

		if (!Suspects.IsEmpty())
		{
			ContentBox->AddSlot().AutoHeight().Padding(0, 10, 0, 0)
			[
				MakeExpandableEvidence(TEXT("下一步检查 / 尚未证明"), Suspects)
			];
		}
	}

	void SRenderTrailAnalyzerResultView::SetAgentResult(const FRenderTrailAgentResultViewModel& Model)
	{
		if (!ContentBox.IsValid())
		{
			return;
		}
		ContentBox->ClearChildren();
		const FString ResultMeta = FString::Printf(TEXT("%s · 最终颜色 %s · 置信度 %s"),
			Model.PixelLabel.IsEmpty() ? TEXT("当前 P1") : *Model.PixelLabel,
			Model.FinalColor.IsEmpty() ? TEXT("unknown") : *Model.FinalColor,
			Model.Confidence.IsEmpty() ? TEXT("unknown") : *Model.Confidence);
		ContentBox->AddSlot().AutoHeight().Padding(0, 0, 0, 9)
		[
			MakeSummaryCard(Model.Question.IsEmpty() ? TEXT("Agent 结论") : Model.Question,
				Model.Finding.IsEmpty() ? TEXT("没有生成结论") : Model.Finding,
				ResultMeta)
		];

		if (Model.bHasColorTransition)
		{
			ContentBox->AddSlot().AutoHeight().Padding(0, 0, 0, 9)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(FLinearColor(0.045f, 0.055f, 0.07f, 1.0f))
				.Padding(9.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						MakeColorSwatch(Model.BeforeColor, Model.BeforeColorText)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(9, 0)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("→")))
						.ColorAndOpacity(FLinearColor(0.66f, 0.72f, 0.80f))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						MakeColorSwatch(Model.AfterColor, Model.AfterColorText)
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12, 0, 0, 0)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Model.ColorDeltaText))
						.AutoWrapText(true)
						.ColorAndOpacity(FLinearColor(0.78f, 0.83f, 0.90f))
					]
				]
			];
		}

		if (!Model.Facts.IsEmpty())
		{
			for (int32 RowStart = 0; RowStart < Model.Facts.Num(); RowStart += 3)
			{
				TSharedRef<SHorizontalBox> FactRow = SNew(SHorizontalBox);
				const int32 RowEnd = FMath::Min(RowStart + 3, Model.Facts.Num());
				for (int32 Index = RowStart; Index < RowEnd; ++Index)
				{
					FactRow->AddSlot().FillWidth(1.0f).Padding(Index == RowStart ? 0.0f : 3.0f, 0,
						Index + 1 == RowEnd ? 0.0f : 3.0f, 0)
					[
						MakeFactCard(Model.Facts[Index])
					];
				}
				ContentBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)[FactRow];
			}
		}

		AddChain(Model.Chain, TEXT("最终 RT 写入"));
		for (const FRenderTrailResultLane& Lane : Model.Lanes)
		{
			AddLane(Lane);
		}

		if (!Model.UnknownText.IsEmpty())
		{
			ContentBox->AddSlot().AutoHeight().Padding(0, 9, 0, 0)
			[
				MakeExpandableEvidence(TEXT("■ 证据缺口"), Model.UnknownText, true)
			];
		}
		if (!Model.ProcessText.IsEmpty())
		{
			ContentBox->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
			[
				MakeExpandableEvidence(TEXT("最终 RT 直接过程（Agent）"), Model.ProcessText)
			];
		}
		if (!Model.PipelineText.IsEmpty())
		{
			ContentBox->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
			[
				MakeExpandableEvidence(TEXT("Pipeline 固定功能状态"), Model.PipelineText)
			];
		}
		if (!Model.ShaderText.IsEmpty())
		{
			ContentBox->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
			[
				MakeExpandableEvidence(TEXT("Shader / 执行证据"), Model.ShaderText)
			];
		}
		if (!Model.Answer.IsEmpty())
		{
			ContentBox->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
			[
				MakeExpandableEvidence(TEXT("Agent 详细回答"), Model.Answer)
			];
		}
		if (!Model.RawReport.IsEmpty())
		{
			ContentBox->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
			[
				MakeExpandableEvidence(TEXT("原始完整报告（可复制）"), Model.RawReport)
			];
		}
	}

	TSharedRef<SWidget> SRenderTrailAnalyzerResultView::MakeSummaryCard(
		const FString& Eyebrow, const FString& Heading, const FString& Body) const
	{
		return SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(FLinearColor(0.055f, 0.12f, 0.18f, 1.0f))
			.Padding(11.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Eyebrow))
					.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 10))
					.ColorAndOpacity(FLinearColor(0.38f, 0.74f, 1.0f))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Heading))
					.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
					.AutoWrapText(true)
					.ColorAndOpacity(FLinearColor(0.90f, 0.94f, 0.98f))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Body))
					.AutoWrapText(true)
					.ColorAndOpacity(FLinearColor(0.64f, 0.71f, 0.80f))
				]
			];
	}

	TSharedRef<SWidget> SRenderTrailAnalyzerResultView::MakeFactCard(const FRenderTrailResultFact& Fact) const
	{
		return SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(FLinearColor(0.055f, 0.06f, 0.075f, 1.0f))
			.Padding(8.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Fact.Label))
					.ColorAndOpacity(FLinearColor(0.54f, 0.61f, 0.70f))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Fact.Value.IsEmpty() ? TEXT("unknown") : Fact.Value))
					.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 10))
					.AutoWrapText(true)
					.ColorAndOpacity(FLinearColor(0.85f, 0.89f, 0.94f))
				]
			];
	}

	TSharedRef<SWidget> SRenderTrailAnalyzerResultView::MakeChainNode(const FRenderTrailResultNode& Node) const
	{
		const FLinearColor Accent = StateColor(Node.State);
		return SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(Accent)
			.Padding(FMargin(3, 0, 0, 0))
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(FLinearColor(0.045f, 0.05f, 0.065f, 1.0f))
				.Padding(8.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Node.Title))
							.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 10))
							.AutoWrapText(true)
							.ColorAndOpacity(FLinearColor(0.86f, 0.90f, 0.95f))
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
						[
							SNew(STextBlock)
							.Text(FText::FromString(StateLabel(Node.State)))
							.ColorAndOpacity(Accent)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, Node.Subtitle.IsEmpty() ? 0 : 3, 0, 0)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Node.Subtitle))
						.AutoWrapText(true)
						.Visibility(Node.Subtitle.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
						.ColorAndOpacity(FLinearColor(0.60f, 0.67f, 0.75f))
					]
				]
			];
	}

	TSharedRef<SWidget> SRenderTrailAnalyzerResultView::MakeExpandableEvidence(
		const FString& Title, const FString& Body, bool bInitiallyExpanded) const
	{
		return SNew(SExpandableArea)
			.InitiallyCollapsed(!bInitiallyExpanded)
			.AllowAnimatedTransition(false)
			.BorderBackgroundColor(FLinearColor(0.085f, 0.095f, 0.115f, 1.0f))
			.BodyBorderBackgroundColor(FLinearColor(0.035f, 0.04f, 0.05f, 1.0f))
			.HeaderContent()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Title))
				.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 10))
				.ColorAndOpacity(FLinearColor(0.78f, 0.84f, 0.91f))
			]
			.BodyContent()
			[
				SNew(SMultiLineEditableText)
				.Text(FText::FromString(Body.IsEmpty() ? TEXT("暂无证据") : Body))
				.AutoWrapText(true)
				.IsReadOnly(true)
				.AllowContextMenu(true)
				.ClearTextSelectionOnFocusLoss(false)
			];
	}

	TSharedRef<SWidget> SRenderTrailAnalyzerResultView::MakeColorSwatch(
		const FLinearColor& Color, const FString& Label) const
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(28.0f)
				.HeightOverride(28.0f)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
					.BorderBackgroundColor(Color)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
				.ColorAndOpacity(FLinearColor(0.72f, 0.78f, 0.85f))
			];
	}

	void SRenderTrailAnalyzerResultView::AddChain(const TArray<FRenderTrailResultNode>& Nodes, const FString& Title)
	{
		if (!ContentBox.IsValid() || Nodes.IsEmpty())
		{
			return;
		}
		ContentBox->AddSlot().AutoHeight().Padding(0, 1, 0, 6)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Title))
			.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11))
			.ColorAndOpacity(FLinearColor(0.78f, 0.84f, 0.91f))
		];
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			ContentBox->AddSlot().AutoHeight()[MakeChainNode(Nodes[Index])];
			if (Index + 1 < Nodes.Num())
			{
				ContentBox->AddSlot().AutoHeight().HAlign(HAlign_Left).Padding(14, 1)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("↓")))
					.ColorAndOpacity(FLinearColor(0.42f, 0.48f, 0.56f))
				];
			}
		}
	}

	void SRenderTrailAnalyzerResultView::AddLane(const FRenderTrailResultLane& Lane)
	{
		if (!ContentBox.IsValid())
		{
			return;
		}
		TSharedRef<SVerticalBox> LaneBox = SNew(SVerticalBox);
		LaneBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(Lane.Title.IsEmpty() ? Lane.Kind : Lane.Title))
			.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11))
			.ColorAndOpacity(FLinearColor(0.72f, 0.84f, 0.96f))
		];
		if (!Lane.Summary.IsEmpty())
		{
			LaneBox->AddSlot().AutoHeight().Padding(0, 3, 0, 6)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Lane.Summary))
				.AutoWrapText(true)
				.ColorAndOpacity(FLinearColor(0.60f, 0.68f, 0.77f))
			];
		}
		for (const FRenderTrailResultNode& Node : Lane.Nodes)
		{
			LaneBox->AddSlot().AutoHeight().Padding(0, 0, 0, 3)[MakeChainNode(Node)];
		}
		ContentBox->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(FLinearColor(0.038f, 0.045f, 0.058f, 1.0f))
			.Padding(9.0f)
			[
				LaneBox
			]
		];
	}
}
