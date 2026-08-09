#include "RenderTrailAnalyzerEvidence.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace UE::RenderTrail::Private
{
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderTrailTracePolicyTest,
		"RenderTrail.Analyzer.Trace.FocusedPolicy",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FRenderTrailTracePolicyTest::RunTest(const FString& Parameters)
	{
		TArray<FEventSummaryEvidence> Events;
		FEventSummaryEvidence ChangedWriter;
		ChangedWriter.EventId = 10;
		ChangedWriter.ActionKind = TEXT("draw");
		ChangedWriter.PassedFragments = 1;
		ChangedWriter.bChangedTextureValue = true;
		Events.Add(ChangedWriter);

		FEventSummaryEvidence SameValueWriter;
		SameValueWriter.EventId = 11;
		SameValueWriter.ActionKind = TEXT("draw");
		SameValueWriter.PassedFragments = 1;
		SameValueWriter.bChangedTextureValue = false;
		Events.Add(SameValueWriter);

		TestEqual(TEXT("Value-changing writer dominates a later same-value pass"),
			SelectDominatingWriterSummaryIndex(Events, 20, TEXT("color")), 0);

		Events[0].bChangedTextureValue = false;
		TestEqual(TEXT("Latest passed writer is the explicit fallback"),
			SelectDominatingWriterSummaryIndex(Events, 20, TEXT("color")), 1);

		TArray<FEventSummaryEvidence> GeometryEvents;
		FEventSummaryEvidence DepthReset;
		DepthReset.EventId = 1;
		DepthReset.ActionKind = TEXT("clear");
		DepthReset.MarkerPath = TEXT("ClearDepthStencil (SceneDepthZ)");
		DepthReset.PassedFragments = 1;
		DepthReset.bChangedTextureValue = true;
		GeometryEvents.Add(DepthReset);
		FEventSummaryEvidence MeshDepthWriter;
		MeshDepthWriter.EventId = 2;
		MeshDepthWriter.ActionKind = TEXT("draw");
		MeshDepthWriter.MarkerPath = TEXT("PrePass > /Game/Meshes/SM_Cylinder");
		MeshDepthWriter.PassedFragments = 1;
		MeshDepthWriter.bChangedTextureValue = true;
		GeometryEvents.Add(MeshDepthWriter);
		FEventSummaryEvidence StencilReset;
		StencilReset.EventId = 3;
		StencilReset.ActionKind = TEXT("clear");
		StencilReset.MarkerPath = TEXT("ClearStencil (SceneDepthZ)");
		StencilReset.PassedFragments = 1;
		StencilReset.bChangedTextureValue = true;
		GeometryEvents.Add(StencilReset);
		TestEqual(TEXT("Stencil-only clear does not hide the latest depth-owning Mesh draw"),
			SelectDominatingWriterSummaryIndex(GeometryEvents, 10, TEXT("geometry")), 1);

		FEventSummaryEvidence LaterDepthReset = DepthReset;
		LaterDepthReset.EventId = 4;
		GeometryEvents.Add(LaterDepthReset);
		TestEqual(TEXT("A real depth clear remains an ownership reset boundary"),
			SelectDominatingWriterSummaryIndex(GeometryEvents, 10, TEXT("geometry")), 3);

		TestEqual(TEXT("Editor selection resources are overlay evidence"),
			ClassifyResourceTracePurpose(TEXT("Editor.Primitives.Color"), TEXT("SelectionOutlineTexture")),
			FString(TEXT("overlay")));
		TestEqual(TEXT("Depth and visibility resources are geometry evidence"),
			ClassifyResourceTracePurpose(TEXT("SceneDepthZ"), TEXT("SceneDepthTexture")),
			FString(TEXT("geometry")));
		TestEqual(TEXT("Scene color resources remain color evidence"),
			ClassifyResourceTracePurpose(TEXT("SceneColor"), TEXT("ColorTexture")),
			FString(TEXT("color")));

		TestTrue(TEXT("BasePass draw is a bounded scene-source stop"),
			IsSceneSourceEvent(TEXT("draw"), TEXT("BasePass > /Game/Meshes/SM_Cylinder")));
		TestFalse(TEXT("Final composite is not mistaken for the scene source"),
			IsSceneSourceEvent(TEXT("draw"), TEXT("PostProcessing > CompositeEditorPrimitives")));

		const FString ReplayKey = BuildReplayPixelHistoryKey(7, FIntPoint(12, 34), 1, 2, 3, 4);
		TestEqual(TEXT("Replay cache key contains only replay-affecting inputs"),
			ReplayKey, FString(TEXT("7:12:34:1:2:3:4")));

		auto MakeHistory = [](const FString& Purpose, const FString& Resource, uint32 Writer,
			int32 Sample, const FString& Status)
		{
			const TSharedRef<FJsonObject> History = MakeShared<FJsonObject>();
			History->SetStringField(TEXT("tracePurpose"), Purpose);
			History->SetStringField(TEXT("resourceName"), Resource);
			History->SetNumberField(TEXT("selectedWriterEventId"), Writer);
			History->SetNumberField(TEXT("sample"), Sample);
			History->SetStringField(TEXT("branchStatus"), Status);
			History->SetStringField(TEXT("mappingConfidence"), TEXT("confirmed-executed-values"));
			History->SetNumberField(TEXT("collapsedShaderAccessCount"), 2);
			return MakeShared<FJsonValueObject>(History);
		};
		TMap<uint32, FEventContextEvidence> LaneContexts;
		TMap<uint32, int32> LaneDepths;
		FEventContextEvidence ColorConsumer;
		ColorConsumer.EventId = 100;
		ColorConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("color"), TEXT("Tonemap"), 90, 0,
			TEXT("continued-to-dominating-writer")));
		ColorConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("color"), TEXT("Tonemap"), 90, 1,
			TEXT("continued-to-dominating-writer")));
		ColorConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("color"), TEXT("CombineLUTs"), 0, 0,
			TEXT("adaptive-footprint-continued")));
		ColorConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("color"), TEXT("CombineLUTs"), 0, 0,
			TEXT("no-modification-before-consumer")));
		LaneContexts.Add(ColorConsumer.EventId, ColorConsumer);
		LaneDepths.Add(ColorConsumer.EventId, 1);
		FEventContextEvidence GeometryConsumer;
		GeometryConsumer.EventId = 110;
		GeometryConsumer.ResourcePixelHistories.Add(MakeHistory(TEXT("geometry"), TEXT("SceneDepthZ"), 2, 0,
			TEXT("continued-to-dominating-writer")));
		LaneContexts.Add(GeometryConsumer.EventId, GeometryConsumer);
		LaneDepths.Add(GeometryConsumer.EventId, 0);

		const TArray<FCausalLaneEvidence> Lanes = BuildCausalLaneEvidence(LaneContexts, LaneDepths);
		TestEqual(TEXT("Color and geometry remain separate causal lanes"), Lanes.Num(), 2);
		const FCausalLaneEvidence* ColorLane = Lanes.FindByPredicate([](const FCausalLaneEvidence& Lane)
		{
			return Lane.TracePurpose == TEXT("color");
		});
		TestNotNull(TEXT("Color lane is present"), ColorLane);
		if (ColorLane)
		{
			TestEqual(TEXT("MSAA samples collapse into one producer branch"), ColorLane->Branches.Num(), 2);
			const FCausalLaneBranchEvidence* TonemapBranch = ColorLane->Branches.FindByPredicate(
				[](const FCausalLaneBranchEvidence& Branch) { return Branch.ResourceName == TEXT("Tonemap"); });
			TestNotNull(TEXT("Tonemap branch is present"), TonemapBranch);
			if (TonemapBranch)
			{
				TestEqual(TEXT("Grouped branch retains both MSAA sample indices"), TonemapBranch->Samples.Num(), 2);
				TestEqual(TEXT("Grouped branch retains raw query count"), TonemapBranch->QueryRecordCount, 2);
			}
			const FCausalLaneBranchEvidence* LutBoundary = ColorLane->Branches.FindByPredicate(
				[](const FCausalLaneBranchEvidence& Branch) { return Branch.ResourceName == TEXT("CombineLUTs"); });
			TestNotNull(TEXT("Adaptive LUT boundary is present"), LutBoundary);
			if (LutBoundary)
			{
				TestEqual(TEXT("Terminal no-modification status replaces intermediate adaptive status"),
					LutBoundary->BranchStatus, FString(TEXT("no-modification-before-consumer")));
			}
		}
		return true;
	}
}

#endif
