#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

namespace UE::RenderTrail::Private
{
	enum class ERenderTrailResultNodeState : uint8
	{
		Confirmed,
		Candidate,
		Blocked,
		Information
	};

	struct FRenderTrailResultFact
	{
		FString Label;
		FString Value;
	};

	struct FRenderTrailResultNode
	{
		ERenderTrailResultNodeState State = ERenderTrailResultNodeState::Information;
		FString Title;
		FString Subtitle;
	};

	struct FRenderTrailResultLane
	{
		FString Kind;
		FString Title;
		FString Summary;
		TArray<FRenderTrailResultNode> Nodes;
	};

	struct FRenderTrailAgentResultViewModel
	{
		FString Question;
		FString Finding;
		FString Answer;
		FString PixelLabel;
		FString FinalColor;
		FString Confidence;
		bool bHasColorTransition = false;
		FLinearColor BeforeColor = FLinearColor::Black;
		FLinearColor AfterColor = FLinearColor::Black;
		FString BeforeColorText;
		FString AfterColorText;
		FString ColorDeltaText;
		TArray<FRenderTrailResultFact> Facts;
		TArray<FRenderTrailResultNode> Chain;
		TArray<FRenderTrailResultLane> Lanes;
		FString ProcessText;
		FString PipelineText;
		FString ShaderText;
		FString UnknownText;
		FString RawReport;
	};

	class SRenderTrailAnalyzerResultView final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SRenderTrailAnalyzerResultView) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& Args);
		void ShowMessage(const FString& Message);
		void SetDeterministicReport(const FString& Summary, const FString& CausalPath, const FString& Suspects);
		void SetAgentResult(const FRenderTrailAgentResultViewModel& Model);

	private:
		TSharedRef<SWidget> MakeSummaryCard(const FString& Eyebrow, const FString& Heading, const FString& Body) const;
		TSharedRef<SWidget> MakeFactCard(const FRenderTrailResultFact& Fact) const;
		TSharedRef<SWidget> MakeChainNode(const FRenderTrailResultNode& Node) const;
		TSharedRef<SWidget> MakeExpandableEvidence(const FString& Title, const FString& Body, bool bInitiallyExpanded = false) const;
		TSharedRef<SWidget> MakeColorSwatch(const FLinearColor& Color, const FString& Label) const;
		void AddChain(const TArray<FRenderTrailResultNode>& Nodes, const FString& Title = TEXT("最终 RT 写入"));
		void AddLane(const FRenderTrailResultLane& Lane);

		TSharedPtr<SVerticalBox> ContentBox;
	};
}
